# 5. Testing and debugging

Checked against the SDK's `tests/`, `src/`, the core's `ModuleSession.cpp`, `ModuleMgr.cpp`, `ModuleBotActions.cpp`, and the stock modules. Code blocks here are
not compiled.

## Test the decisions without the core

The most useful habit in the stock modules: **the policy is a pure function of a snapshot**. It takes `BotSnapshot` (and its nearby units) and returns
`BotAction`s; it never touches a socket, shared memory, the clock (time is a parameter) or global state. Then a test builds a snapshot by hand, calls the
policy, and checks the actions. That needs the SDK only for the plain structs in `tier2_layout.h`.

How MOD-Forever-Bots is arranged (see its `CMakeLists.txt`):

- `src/main.cpp` is thin: it connects, parses config, starts the pump, calls `Run()`.
- Everything else is small static libraries with no dependency on the connection: `forever_bots_policy` (`bot_policy.cpp`, `combat_rotation.cpp`,
  `supply_policy.cpp`, ...), `forever_bots_settings` (config parsing), `forever_bots_store` (the `bot/<guid>` keys), `forever_bots_files` (rotating files), and so on.
  `forever_bots_policy` links `forever_module_sdk` only for the headers.
- Each library has test executables registered with `add_test`: `policy_test`, `move_gate_test`, `settings_test`, `bot_store_test`, `roster_test`, `rotation_test`,
  `scale_test` (the policy at the 1500-bot ceiling), and more. `ctest --test-dir build` runs them all. The artifact build runs them too and refuses to package when one
  fails.
- A test is a plain `main()` with a tiny `CHECK` macro that counts failures and prints `FAIL file:line: expression` (the same macro as in the SDK's own tests),
  returning non-zero if any failed. No test framework is needed.

A test for the policy of the quick start would look like this:

```cpp
#include <cstdio>
#include <vector>
#include "forever_module/tier2_layout.h"

namespace fm = forever_module;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main()
{
    fm::BotSnapshot bot;               // default-initialised: alive, out of combat, not moving
    bot.guid = 42;
    bot.pos_x = 100.0f; bot.pos_y = 200.0f; bot.pos_z = 30.0f;

    Walker walker;                     // the policy's own state, from the module
    std::vector<fm::BotAction> out;
    Decide(bot, walker, /*tick*/ 100, out);
    CHECK(out.size() == 1);
    CHECK(out[0].type == fm::ActionType::kMoveTo && out[0].bot_guid == 42);

    out.clear();
    bot.in_combat = 1;                 // in combat: no walking order
    Decide(bot, walker, 200, out);
    CHECK(out.empty());

    return failures == 0 ? 0 : 1;
}
```

with `add_executable(policy_test tests/policy_test.cpp)`, `target_link_libraries(policy_test PRIVATE hello_policy)` and `add_test(NAME policy COMMAND policy_test)`.
For nearby units, build a `std::vector<fm::NearbyUnit>` and pass a `std::span` (what `WorldTickSnapshotView::NearbyOf` returns).

What to put under test: every branch of "which action does this snapshot get", the state kept between ticks (cool-downs, claims, "do not repeat the order"), the
reaction to each `BotActionStatus` (a refused or `kNoRoute` move must back off, not repeat), config parsing (defaults, bounds, bad values give a warning and
the default), and any text-file loader (feed it a string, not a path). The bots module also has a 1500-bot `scale_test` that guards the cost of one decision pass.

## The SDK's own tests, as templates

`ctest --test-dir build` in the SDK runs three programs (`tests/CMakeLists.txt`):

| CTest name | Program | What it shows |
|---|---|---|
| `tier1_roundtrip` | `tests/tier1_roundtrip_test.cpp` | A **fake core** written with Boost.Asio local sockets: it accepts a connection, reads the `HandshakeRequest`, answers `HandshakeResponse` and `ModuleConfig`, sends events, checks heartbeats (the test uses `heartbeat_interval = 20ms`) and sends `Shutdown`; its header also lists the protocol rejection and `Shutdown()` from another thread. Also compares `FOREVER_SDK_CMAKE_VERSION` with `kSdkPackageVersion`. Copy it to test your module's Tier 1 behavior end to end. |
| `tier2_attach` | `tests/tier2_attach_test.cpp` | Plays the core for Tier 2: creates a shared-memory object with `Tier2RegionBytes(...)`, writes a `Tier2RegionHeader` and a snapshot with `new (...) BotSnapshot()`, publishes (flips the buffer and the tick), then checks `Tier2Session::Attach`, `ReadLatestSnapshot` and `PublishActions` (including too many actions and an unknown action type), and attaches to a region written with a wrong layout version (`CreateRegion` takes the layout version as a parameter). The way to test a pump thread against a region you control. |
| `module_client_tier2` | `tests/module_client_test.cpp` | `ModuleClient` with no core: the talent table, the 24-bit quest id check, the refusals of the convenience methods when no region is granted (`tier2_error()` text). |

A module can reuse the idea: a test that creates a region, publishes a hand-made snapshot with a few bots, runs the pump code for a few iterations, reads the
action buffer back, and compares. That is the closest to the real thing that runs without a worldserver. (These tests use POSIX shared memory and run on Linux.)

## Running against a local worldserver

You need a worldserver built from the Forever core with the module system on, and the same protocol number as your SDK checkout. Steps:

1. `Modules.Enabled = 1`; put your module in `modules_runtime/<name>/` ([docs/01](01-quick-start.md)).
2. Approve it (`module approve <name>`, then restart), or set `Modules.AutoApproveLocal = 1` on a throwaway development server only.
3. Switch on what your actions need: `Bot.Actions.Teleport`, `Bot.Actions.Grant`, `Bot.Actions.UseObject`, `Bot.Actions.Gossip`, `Bot.Chat.Orders.Enabled`,
   `Bot.Snapshot.GroundEffects` are all **0** in `worldserver.conf.dist`; `Bot.Actions.AutoGear`, `Bot.Actions.EquipUpgrades`, `Bot.Actions.Supplies`,
   `Bot.Taxi.Enabled` are 1. Without a switch the action is refused with a reason such as `Bot.Actions.Teleport is off`.
4. Start the worldserver. It spawns the module itself; you cannot usefully start the module by hand, because the core creates the socket path only while it waits for
   the connection and passes it as `argv[1]`.
5. Watch the module's `stderr` and the core's logs (below). Use the console: `module list`, `module info <name>` (status, pid, heartbeat count, restart count, last
   failure).
6. To iterate quickly, rebuild the module and restart the worldserver; the stock project has a helper for its test server (`scripts/dev-module-deploy.sh`, which installs a
   forever-bots binary and data folder and restarts the sim) but it is specific to that server and to forever-bots.

The project's rule: a module is built and tested by the Jenkins pipeline on the NAS, not by hand on the servers; a push to a product repo's `main` or `test` branch
triggers it. Work on a branch of your own module's repository.

## Reading logs

| Where | What |
|---|---|
| The module's `std::cerr` | Your own lines, plus the SDK's: `forever_module: lost the connection to core` (Run ended without `Shutdown`), `forever_module: dropped N bot actions: <reason>` (a convenience-method queue that `PublishActions` refused). The module is a child of the worldserver and, as far as the spawn call shows, inherits its stdout and stderr (not checked on a running server); under systemd that is the service's journal. |
| The core's `Server.log`, logger `modules` | Lifecycle: `started 'x' (pid N), waiting for its handshake.`, `'x' handshake accepted (module id N).`, `module N: Tier 2 region '...' (bots .., nearby .., actions .., tick interval ..).`, `'x' failed ...`, `'x' is unresponsive: ...`, `'x' restart N scheduled in M ms.`, `'x' permanently failed: ...`, `module N spawned bot G.`, `'x': released N bot(s) on session end (...)`. Debug level: `module N: action on bot G refused (status): reason.`, `... sent an action for bot G, which it does not control; dropped.` |
| The core, `server.worldserver`, info | `Modules: N action(s) refused in the last minute; the commonest reasons:` then up to twelve `Modules:   COUNT x type T status S: reason` lines, once a minute. This is how you find out why 1500 bots' actions fail without turning on debug. |
| `Client.Log(level, text)` | Puts your line in the core's `Server.log`, tagged with the module. 1 to 255 bytes. |
| Console | `module list`, `module info <name>`, `module approve <name>`. |

The stock bots module logs one line a minute of its own cost: `forever-bots: pump: N bots, decide avg X us, max Y us over Z ticks, W actions per tick`. Do the
same; it turns "the server lags" into a number.

## Common errors and what they mean

| You see | Meaning and fix |
|---|---|
| `protocol_version mismatch: core speaks "20", module declared "19"` (core log; `rejection_reason()`; or at load: `not loaded`) | Exact string match failed. `module.json` `protocol_version` (checked at load) and the `kProtocolVersion` the SDK sent (checked at the handshake) must both equal the core's. Rebuild against an SDK with the core's number and update `module.json`. Permanent: the core does not retry. |
| `Tier 2 layout mismatch: core wrote version N, this SDK reads version M` (`tier2_error()` after `AttachTier2()`) | The region was written by a core with a different `kTier2LayoutVersion`. Same cause and fix as above; this check is independent of the handshake. A layout bump is meant to bump the protocol too (the header says so), and the protocol check usually fails first; you reach this message when the protocol numbers agree but the layouts do not. |
| `module.json must set name, version and protocol_version`, `malformed module.json: ...` | The manifest is not valid JSON or lacks a required field. |
| `'x': module.json name 'y' does not match the directory name, skipping.` | The folder, the executable and `name` must be the same string. |
| `'x' is not a valid module directory name` | Letters, digits, `_` and `-` only. |
| `'x' <ver> is pending approval and was not started.` / `never approved; run 'module approve x'` / `capabilities changed since version V: ...` | The approval gate. Run `module approve x` and restart. `capabilities changed` lists what differs (hooks added, Tier 2 numbers). |
| `approval store unreadable: ...` | `approved_modules.json` exists but cannot be parsed; no module starts and `module approve` refuses. Fix or move the file. |
| `executable not found: <path>` | The binary is not at `<Modules.Dir>/<name>/<name>` (or is not a regular file). Permanent. |
| `unsupported Tier 1 hook: X` | `X` is not in the core's list (see [docs/01](01-quick-start.md#3-modulejson)). |
| `Tier 1 hook was not approved: X`, `Tier 2 was not approved for this module` | The code's manifest asks for something `module.json` (as approved) does not declare. Keep the two equal; re-approve after changing `module.json`. |
| `module name mismatch: core spawned "x", handshake declared "y"` | `ModuleManifest::module_name` differs from the folder name. |
| `core granted Tier 2 bounds outside the requested limits` (`rejection_reason()`) | The grant had zero bots or actions, or exceeded what the code requested. Check `requested_*` in both places and the core's `Modules.Tier2.*`. |
| `no HandshakeResponse within timeout` / `module did not connect within the handshake timeout` | The module did not connect or answer within `Modules.HandshakeTimeoutMs` (5000). After `Modules.MaxHandshakeAttempts` (3) the module is permanently failed. On Linux `Connect()` also returns false if the parent is pid 1 (the process was daemonized away from the worldserver). |
| `'x' is unresponsive: no heartbeat within the heartbeat timeout` | No `Heartbeat` frame for `Modules.HeartbeatTimeoutMs` (15000). The module hung as a whole process, or `heartbeat_interval` was set above the timeout, or `Run()` is not running. A restart follows. |
| `crash loop: restarted 5 time(s) within 10 minutes, giving up` | Permanent failure; fix the crash and restart the worldserver. |
| `max_bots exceeded, request a larger bound at handshake` (a `ControlResponse.reason`) | The module controls its negotiated `max_bots` already. Raise `requested_max_bots` in the manifest **and** `module.json` (and re-approve), and mind `Modules.Tier2.MaxBots`. Spawns also fail with the core's own `Bot.Limit.MaxBots` reason, and are paced by `Bot.Login.MaxPerTick` and `Bot.Login.MaxPerSecond`. |
| `already controlled by module X`, `character not found`, `Tier 2 was not granted to this module`, `bot control is not available on this core`, `core is busy, try again later` | Control request refusals, from `ExecuteControlRequest` and the queue limits (docs/02). |
| `no snapshot has been read yet: call ReadLatestSnapshot() before PublishActions()`, `too many actions: N, the core granted M`, `unknown action type N` (`PublishActions` error string) | Publish after reading a snapshot, no more than `MaxActions()` per tick, and no type above `kLastActionType`. |
| `more bot actions queued than the core granted (N)`, `quest id N does not fit in 24 bits`, `no Tier 2 region was granted at handshake` (`tier2_error()`) | The convenience methods refuse these and do nothing. |
| Action status `kRefused` with `bot is in combat`, `no vendor or repairman within interaction range`, `no trainer within interaction range`, `Bot.Actions.X is off`, `party is full`, `quest is not in the log`, ... | The core re-checked and said no. Statuses are in the next snapshot; reasons are in the core's refusal summary. See [docs/03](03-tier2-snapshot-and-actions-reference.md). |
| Every action seems ignored | Check that you read a snapshot first, that `bot_guid` is a bot **this module** controls (a foreign bot is dropped and recorded `kRefused`), that the bot is online (`kNotOnline`), and that you publish at most once per tick (a second set in the same world tick overwrites the first). |

## Rules of the road

1. **Never block the thread that calls `Run()`.** Replies to your Tier 1 actions, control responses, hooks, timers and `OnConfig` all arrive on it. An `EventRequest`
   (a chat command) is waited for by the core for `Modules.CommandRequestTimeoutMs` (200 ms) only; the handler must return promptly. File and network
   I/O, sleeps and long computations belong on your own thread, handing results back through a lock-protected queue. Heartbeats come from the SDK's own thread, so
   a blocked `Run()` thread will not get you restarted, but it will stall everything else the module does.
2. **Keep the decision pass short.** The core hands you a snapshot every `tick_interval` server ticks; a pass that takes longer than the interval means you act on
   stale snapshots and publish late. The project's measurements for forever-bots on the sim, given by the owner and not re-measured for this guide: an average of
   0.5 to 1.2 ms per tick for 40 to 90 bots, worst cases of 25 to 40 ms. The 16.6 s tick of 2026-10-26 that led to a restart is documented in forever-ops `docs/99-todo.md`
   (see [docs/02](02-architecture-and-lifecycle.md#liveness-heartbeats-timeouts-restarts)). Put a per-pass time check in your tests (the bots module's `scale_test` does,
   with a wide ceiling configurable through `FOREVER_BOTS_TICK_MS`).
3. **Publish at most once per snapshot tick, and always the whole queue.** The core reads one action set per tick; a second publish between two world ticks replaces
   the first. Do not mix `PublishActions` and the `ModuleClient` bot-action methods.
4. **Do not repeat what is already happening.** A `MoveTo` or `Follow` each tick clears and restarts the bot's movement; an unreachable `MoveTo` is refused with
   `kNoRoute` every tick. The move gate in the bots module exists because 140 bots once sent refused moves 5000 times a minute. Remember the last order per bot, wait
   for `last_action_sequence` to change, and back off when the status is not `kApplied`.
5. **Treat the snapshot as the only truth, and as slightly old.** Nearby units can be several ticks old (the staggered refresh), a target can die between the
   snapshot and the action, and the core re-checks everything. Handle every `BotActionStatus`; never assume `kApplied` for an action you did not read back.
6. **Stay within the grant.** Size everything from `negotiated_bounds()`; respect `max_bots` and `max_actions`. Control requests are paced by the core (32 a world
   tick); ramp up spawns instead of asking for hundreds at once.
7. **Do not trust your own memory across restarts.** The core can restart you at any time; keep what you need to continue in the module store and read it back at
   start ([docs/02](02-architecture-and-lifecycle.md#taking-bots-and-giving-them-back)). Exit promptly on `Shutdown`: the core terminates a process that is still
   running 2 s later.
8. **Thread safety of `Tier2Session`:** use it from one thread. `ModuleClient::AttachTier2()` returns the one session the client owns; the convenience methods
   use it from the `Run()` thread, and `main.cpp` of forever-bots passes it to a pump thread and uses none of them.
9. **Log sparingly.** At 1500 bots, one line per bot per tick is a flood. The bots module logs a refusal once per action and, for the commonest harmless
   ones (vendor, trainer, craft, gather out of range), at most once per bot every ten minutes.
