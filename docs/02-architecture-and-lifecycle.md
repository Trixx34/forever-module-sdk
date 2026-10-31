# 2. Architecture and lifecycle

This guide describes what the worldserver (the "core") and a module do, in order, from startup to shutdown. Every statement was checked against the core's
`src/server/game/Modules/` (`ModuleMgr.cpp`, `ModuleSession.cpp`, `ModuleTier2.*`, `ModuleBotOwnership.*`), the SDK's `src/` and the headers, at the revisions
current when this was written (protocol `"20"`, layout `22`). The core's `doc/module-system/04-phase2-module-host-design.md` is the original design and is
partly out of date; where it differs, the code is described here. Where the code does not settle a point, it is said so.

## Process model

- The worldserver runs a `ModuleMgr` singleton. It owns, per module: a `ModuleSession` (a thread that listens on a Unix domain socket and serves the
  Tier 1 connection), the module's child process, and the Tier 2 shared-memory region.
- A module is a **separate process** with its own address space. It can be restarted by the core. It cannot call into the core; everything it does goes
  through Tier 1 requests or Tier 2 actions, and the core re-checks each of them on the world thread.
- The world thread runs `ModuleMgr::OnWorldTick(tick)` on every world tick. That is where Tier 2 snapshots are written and Tier 2 actions are read and applied,
  where queued Tier 1 actions and control requests are run, and where results are recorded.
- **Tier 1**: protobuf `Envelope`s (`proto/envelope.proto`), each framed as a 4-byte little-endian length followed by the serialized message. A frame of
  zero bytes, or larger than `kMaxFrameBytes` (1 MiB), is a protocol error. The socket path is `<Modules.Dir>/<name>.sock`.
- **Tier 2**: one POSIX shared-memory object per module, named `trinity_module_<module id>_tier2`, created by the core with the size computed by
  `Tier2RegionBytes(maxBots, maxNearbyUnits, maxActions)` from `tier2_layout.h`. The module id is assigned by the core when it loads the folder (in the sorted order of
  the folders in `Modules.Dir`), so it can change between worldserver starts if modules are added or removed.

## Startup order

1. The worldserver starts. If `Modules.Enabled = 0` (the default) the module system is off. Otherwise `ModuleMgr::LoadModules` reads the config and the
   approval store, then scans `Modules.Dir` (default `"modules_runtime"`), folders in name order. A missing directory is not an error ("nothing to load").
2. For each folder, `ProcessModuleDirectory`:
   1. the name must be letters, digits, `_` and `-`; otherwise skipped with a warning;
   2. `module.json` must exist and parse, with `name`, `version`, `protocol_version` present; its `name` must equal the folder name; otherwise skipped (with a
      warning, or an error for a parse failure);
   3. **protocol check**: `protocol_version` must equal the core's `kModuleProtocolVersion` as a string, else the module is marked **permanently failed**
      (`protocol_version mismatch: core speaks "20", module declared "19"`) and never started;
   4. **approval gate** (below): not approved means status `pending approval`, not started;
   5. the executable `<Modules.Dir>/<name>/<name>` (`.exe` on Windows) must be a regular file, else permanently failed (`executable not found`);
   6. `SpawnModule`.
3. `SpawnModule` builds the session settings (handshake timeout, heartbeat timeout, expected protocol and name, the list of hooks the core supports, the approved
   capabilities, the Tier 2 negotiation hook, and a snapshot of the module's config), starts listening on the socket (failure to bind is permanent), sets the
   status to `starting`, and starts the child with `executable endpoint` as its command line. The resource limits `Modules.ResourceLimits.MemoryMB` and
   `CpuPercent` (256 and 25 in `worldserver.conf.dist`, and the design doc's Job Object / cgroup plan) are **not read anywhere** in the core's `src/`
   (`git grep` finds them only in the `.dist` file), so a module is not memory- or CPU-limited by the host today.

## Approval

The core is "deny by default". A module starts only when the capabilities in its `module.json` hash to what an operator approved.

- **Hashed**: the sorted, unique `tier1.hooks`, and the Tier 2 block (`participates`, `requested_max_bots`, `requested_max_nearby_units`,
  `requested_max_actions`, `requested_tick_interval`) as a canonical JSON, SHA-256. **Not hashed**: name, version, SDK version. So a bug-fix release needs no
  re-approval; asking for a new hook or more bots does.
- **Where**: `approved_modules.json`, in the directory of `worldserver.conf` (so it survives a rebuild of `Modules.Dir`). Written by the console command
  `module approve <name>` (needs `RBAC_PERM_COMMAND_MODULE`; console allowed). `module list` and `module info <name>` show the state. Approval takes effect at the
  next module load, which is the next worldserver start as far as I found.
- **Unreadable store**: if `approved_modules.json` exists but cannot be read or parsed, no module is started (status `pending approval`, reason `approval store
  unreadable: ...`) and `module approve` is refused, so a good store is not overwritten.
- `Modules.AutoApproveLocal = 1` skips the whole gate. Development only; the core logs a warning per module.

## Handshake

The module connects (`ModuleClient::Connect`) and sends a `HandshakeRequest` (`ModuleClient::Handshake`, default timeout 5000 ms). The core waits at most
`Modules.HandshakeTimeoutMs` (default 5000) from the moment it starts waiting for the connection, for both the connect and the request.

The core checks, in this order, and answers with the first failure as `HandshakeResponse{accepted=false, reason=...}`:

| Order | Check | Reason text |
|---|---|---|
| 1 | request decodes | `malformed HandshakeRequest` (no response is sent) |
| 2 | `protocol_version` equals the core's | `protocol_version mismatch: core speaks "20", module declared "19"` |
| 3 | `module_name` equals the folder name the core spawned | `module name mismatch: core spawned "x", handshake declared "y"` |
| 4 | Tier 2 requested and the core can offer it | `Tier 2 is not available on this core` |
| 5 | Tier 2 requested and approved | `Tier 2 was not approved for this module` |
| 6 | every hook is in the core's supported list, then in the approved list | `unsupported Tier 1 hook: X`, `Tier 1 hook was not approved: X` |
| 7 | the region can be created | `Tier 2 region: <error>` |

A rejection marks the module permanently failed: the core stops the process and does not retry. On success the core replies `accepted = true` with the
`module_id`, and, if Tier 2 was requested, `tier2_region_name` and the **negotiated** bounds, then immediately sends a `ModuleConfig`. The SDK's
`Handshake()` returns only after it has read both (`expected ModuleConfig after HandshakeResponse` otherwise).

**Negotiated bounds** (`NegotiateTier2`): each bound is the minimum of the module's request, the worldserver limit (`Modules.Tier2.MaxBots` 1500,
`MaxNearbyUnits` 16, `MaxActions` 3000, `MaxTickInterval` 100) and, when `module.json` declares Tier 2, the approved `requested_*` values. A tick interval of 0 is
treated as 1. The core may grant less than requested, never more; the SDK refuses a grant above the request, or with no bots or no actions. A module must size
everything from `client.negotiated_bounds()`, not from what it asked.

If the module asked for Tier 2 and the previous region of this module id still exists, it is released first, and `Create` replaces a leftover shared-memory
object of the same name.

## Region creation, attach and layout checks

1. The core creates the shared-memory object, sizes it with `Tier2RegionBytes`, and constructs a `Tier2RegionHeader` in it with `layout_version =
   kTier2LayoutVersion` and the negotiated bounds. It uses a plain `shared_memory_object` rather than a managed region (a documented deviation from the design).
2. The module calls `ModuleClient::AttachTier2()` (or `Tier2Session::Attach(name, error)`). `Attach` opens the object read-write (`open_only`), maps it, and
   fails with these messages in `error`:
   - `cannot open Tier 2 region '<name>': <boost error>`: the object does not exist (for example the core restarted the session) or cannot be opened;
   - `Tier 2 region '<name>' is too small to hold a header`;
   - `Tier 2 layout mismatch: core wrote version N, this SDK reads version M`: the module was built against another layout. This is the check that does not
     depend on the Tier 1 handshake;
   - `Tier 2 region '<name>' is smaller than its header says`.
3. `AttachTier2()` itself also fails with `no Tier 2 region was granted at handshake` when the handshake returned no region name.
4. The structs are plain data (no pointers), so both processes read the same bytes. `tier2_session.cpp` has `static_assert`s on their sizes (at layout 22:
   `BotSnapshot` 1136, `NearbyUnit` 48, `GroundEffect` 24, `Tier2RegionHeader` 56, `WorldTickSnapshotHeader` 16, `ActionQueueHeader` 16, `BotAction` 40 bytes).

### How snapshots and actions move

- The region holds a header and **two** snapshot buffers and **two** action buffers. The core writes the snapshot into the buffer that is not published, then
  flips `published_snapshot_buffer` and stores `published_snapshot_tick`; the module's `ReadLatestSnapshot()` copies the published buffer and checks that the
  index and the tick did not change during the copy (three attempts; if all are torn it returns the previous complete snapshot). Actions work the same way in
  the other direction (`published_action_buffer`, `published_action_tick`).
- The core publishes a snapshot when `SnapshotDue(tick)`, i.e. on the ticks given by the negotiated `tick_interval`, containing **only the bots this module
  controls** that are online, limited to the negotiated `max_bots` and, per bot, `max_nearby_units` nearby units. The nearby list is refreshed on a stagger:
  each bot on one tick in `Modules.Tier2.NearbyRefreshTicks` (clamped to 0..16; 0 = auto: one in ceil(bots / 400), so 1 up to 400 bots), which makes a nearby list up to that many
  ticks old.
- On each world tick the core also calls `ReadNewActions`: a set is **new** if its buffer index or its tick differs from the last one returned. If a module
  publishes twice between two world ticks, only the second set is seen, so publish **at most once per snapshot tick**. Actions answer a snapshot tick that may
  lag; the core does not discard a set for being old.
- `PublishActions` replaces the whole queue; pass every action for the tick in one call. For each action, in order, the core: drops it if the bot is not owned by
  this module (recorded as `kRefused`), records `kNotOnline` if the bot is not in the world, otherwise applies it and records the status. Only the **last** action
  applied to a bot is remembered: the bot's `last_action_type`, `last_action_status`, `last_action_spell` (casts only) and `last_action_sequence` in the next
  snapshot describe that one. Send one action per bot per tick if you need to know each result.
- Refusals are counted by action type, status and reason and the commonest twelve are logged once a minute at info level (`Modules: N action(s) refused in the
  last minute; the commonest reasons:` in `server.worldserver`); each refusal is also logged at debug level under `modules`.

## Liveness: heartbeats, timeouts, restarts

- The SDK sends a `Heartbeat` (with an increasing `sequence`) every `ModuleManifest::heartbeat_interval`, default **5000 ms**, from an internal thread started
  by `Run()`. Because it is a separate thread, a callback or timer that takes long on the `Run()` thread does **not** by itself stop heartbeats. Heartbeats
  stop when `Run()` returns, or when the whole process cannot run (stopped, swapped out, starved of CPU).
- The core's session treats only `Heartbeat` frames as liveness. If none arrives within `Modules.HeartbeatTimeoutMs` (default **15000**) of the previous one
  (or of the session starting to serve), it reports a heartbeat timeout: `'<name>' is unresponsive: no heartbeat within the heartbeat timeout`. Keep
  `heartbeat_interval` well below that timeout. `Modules.HeartbeatIntervalMs` (5000) is in `worldserver.conf.dist` but nothing in the core reads it; only the timeout is enforced.
- **What happens next** (`OnSessionEvent`): the module's bots are released from it (ownership is dropped), the process is terminated and waited for, the Tier 2
  region is destroyed, and a restart is scheduled.
- **Restart policy**:
  - A failure **before** the handshake completed (no connect, no request in time, crash during start) counts against `Modules.MaxHandshakeAttempts` (default 3).
    After that many, the module is permanently failed.
  - A failure **after** a successful handshake (crash, disconnect, heartbeat timeout) uses a rolling budget: at most `Modules.MaxRestartAttempts` (default 5)
    restarts within a fixed **10-minute** window. A sixth within the window marks the module permanently failed (`crash loop: restarted 5 time(s) within 10
    minutes, giving up`).
  - The delay before restart *n* is `Modules.RestartBackoffMs` (default 1000) times 2^(n-1), capped at 60 s. The supervisor thread polls every 50 ms.
  - A permanently failed module stays down until the worldserver is restarted (there is no restart command; the design lists admin tooling as deferred).
  - The restarted module gets a fresh handshake, a **new** region and the current config. Everything it kept in memory is gone; see "Taking bots" below for how
    a module gets its bots back.
- **The 16.6 s incident** (forever-ops `docs/99-todo.md`, 2026-10-26 08:51): one decision tick of forever-bots took 16.6 s, the core found both modules
  unresponsive and restarted them, including `auto-vendor-trash`, which does almost nothing. The todo's reading is that the *host* is the likely cause, since both
  modules stalled: the test server had 2.3 GB of memory available, 1.8 GB of swap in use, and the kernel was logging `[TTM] Buffer eviction failed` (2 190 lines
  that day). That fits the code above, where a slow decision loop alone does not stop the SDK's heartbeat thread; but the cause is the todo's diagnosis ("watch
  for a repeat"), not something this guide could verify.

## Shutdown

- **Core stopping** (`ModuleMgr::Unload`): it stops new restarts, then tells each session to stop, which sends a Tier 1 `Shutdown{reason="server stopping"}` to the
  module (best effort, 1 s write timeout), then waits up to **2000 ms** (`ShutdownGrace`) per process for it to exit and terminates it if it has not.
- **In the module**: `ModuleClient::Run()` returns when it dispatches `Shutdown`, when `ModuleClient::Shutdown()` is called (thread-safe), or when the
  connection is lost (it prints `forever_module: lost the connection to core` to stderr). `main` should then return promptly. Join your own threads before
  returning, and stop them first if they use the `Tier2Session` (the session is owned by the `ModuleClient`).
- **Core crash**: on Linux, `Connect()` sets `prctl(PR_SET_PDEATHSIG, SIGTERM)`, so the kernel stops the module when the worldserver dies. The SDK also ignores
  `SIGPIPE`, so a write to a closed socket is an error return, not a kill.
- **Region and socket**: when a session ends the core closes and removes its socket path; the region is released with the module's descriptor, and its
  destructor removes the shared-memory name.

## Taking bots and giving them back

Bots belong to at most one module. The core keeps the map bot guid to module (`ModuleBotOwnership`); the module sees only the bots it owns, in its snapshot.

| Call | What the core does |
|---|---|
| `RequestControl(0, SpawnParams, cb)` | **Spawn** a new bot and give it to the module. Refused with `Tier 2 was not granted to this module` (no region), `bot control is not available on this core`, `max_bots exceeded, request a larger bound at handshake` (the module is at the negotiated `max_bots`), or the spawn's own reason (the core's `Bot.Limit.*` and login limits, for example). On success the reply has `granted`, `bot_guid`, `race`, `class_id`, and the core activates (logs in) the bot. |
| `RequestControl(guid, {}, cb)` | **Reclaim** an existing bot. Refused with `character not found`, `already controlled by module <other>`, or `max_bots exceeded ...`. On success the core activates the bot. |
| `ReleaseControl(guid)` | Give a bot back. No reply. For an alt the core logs the character out ("an alt logs out when its owner lets it go"); for other bots what the backend does is not visible in `ModuleMgr`, so treat it as unspecified. |
| `OnControlRevoked(cb)` | The core took a bot away (deleted, logged out, GM). `cb(botGuid, reason)`; stop using the guid. Nothing is sent back. |

Facts about the transport of these calls:

- A control request is queued for the **world thread** and answered asynchronously: each of the two queues (control requests, and Tier 1 actions such as sells, store calls and `Log`) runs at most **32** requests per world tick and holds at
  most **256** waiting (`MaxActionsPerTick`, `MaxPendingActions`). Past that the reply is `core is busy, try again later` (for a release, which always fits, never). A module that
  spawns hundreds of bots must pace its requests; the stock bots module uses a ramp (`BotsInitial`, `BotsStep`, ...).
- When a module's **session ends for any reason other than a normal accept** (crash, heartbeat timeout, shutdown), the core drops all its ownership
  (`DropModule`, logged as `released N bot(s) on session end`). It does **not** send `ControlRevoked`, since the module cannot receive it. Whether the bots
  are logged out or stay online as ordinary bots is decided by the bot backend, which is outside the module host; this guide does not claim either.
- After a restart the module's memory is empty, so a module that must continue with the same bots has to remember them itself. The convention of the stock
  bots module (a module convention, **not** something the core does): for each controlled bot it writes a row `bot/<guid>` in the **module store** (value
  `"<race> <class>"`), and at start it calls `StoreList("bot/")`, then for each key `StoreGet` and `RequestControl(guid, {}, ...)` to reclaim it, deleting rows
  whose reclaim is refused as stale. See `MOD-Forever-Bots/src/bot_store.h`.

### The module store

Key/value rows in the world database (table `module_store`), private to the module: the core takes the module from the session, so a module cannot name another
module's rows. Calls: `StoreGet(key, cb)`, `StoreSet(key, value, cb)`, `StoreDelete(key, cb)`, `StoreList(prefix, cb)`. A key is 1 to 128 printable ASCII
characters without spaces, a value at most 1024 bytes, a module at most 1000 rows, a list at most 1000 keys, sorted. The reply sets `success`, and `found`,
`value` or `keys`; a refusal has `success = false` and a `reason`. The core runs these on the world thread and waits for the database, and reads a write back
before replying. (Limits from `doc/module-system/15-module-store.md` in the core repo.)

## Tier 1 events

A module subscribes to hooks by listing them in the manifest; the core delivers an `EventNotify` (fire and forget) or an `EventRequest` (the core waits for an
`EventReply` up to `Modules.CommandRequestTimeoutMs`, default 200 ms, and reports "module did not respond" on timeout). `ModuleClient::OnNotify(hook, cb)`
and `OnRequest(hook, cb)` register the handlers, keyed by the hook name; an `EventRequest` with no handler is answered with an empty reply so the core does
not wait. The payload messages are in `proto/events.proto` (for example `PlayerItemLootedEvent`, `PlayerChatEvent`, `CommandRequestPayload`). Registering a
handler for a hook you did not list in the manifest receives nothing, and listing a hook the core does not support fails the handshake.
