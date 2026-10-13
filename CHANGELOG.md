# Changelog

## Layout 5 (M8, breaking for layout 4 modules)

- `BotSnapshot` gains `group_role`, `group_size`, `group_leader_guid` and `group_invite_from`: the bot's party (M8).
- `ActionType::kGroup` (8): invite, accept, or leave a party (`spell_id` 1, 2, 3).
- Protocol unchanged. A module built against layout 4 is refused at region attach, since the layout version differs.

## Unreleased (no protocol or layout change)

- `ActionType::kVendor` (6): sell grey items, repair and equip the best gear, at an NPC within interaction range (M3).
  `spell_id` is a bitmask (1 sell, 2 repair, 4 equip). Old cores refuse it as an unknown type.
- `ActionType::kTrain` (7): learn the spells the trainer in range teaches this bot now (M4).
- `ActionType::kRest` (5): sit down to rest (`spell_id` 1) or stand up (`spell_id` 0). Old cores refuse it as an unknown
  action type, so it is additive.
- `ModuleClient::AddTimer(period, callback)`: periodic callbacks on the thread that runs `Run()`. `Run()` waits with
  `select()` up to the next timer when timers are registered, and blocks on the read as before when there are none.
  Covered by `tier1_roundtrip_test`.

## Protocol 4

- Tier 2 layout 4: `BotSnapshot::mana`/`max_mana` are `power`/`max_power` (the main resource, see `power_type`). New
  `BotSnapshot::level` and `power_type`. `NearbyUnit` gains `level`, `elite` and `casting`.

## Protocol 3

- Tier 2 layout 3: `BotSnapshot` gains combat state (`in_combat`, `target_distance`) and the result of the last action for
  the bot (`last_action_type`, `last_action_status`, `last_action_spell`). `BotActionStatus` lists the statuses.

## Protocol 2

- Tier 2 layout 2: `BotSnapshot::spell_cooldowns` is now `SpellCooldown[]` (spell id and remaining milliseconds).
  Unused slots have spell id 0.

## [0.1.0] - unreleased

Phase 1 contract release (wire schema, Tier 2 layout) plus the Tier 1 client. Tier 2 shared memory
and bot control are not included yet (see "Not yet implemented").

- Tier 1 client (`ModuleClient`), POSIX only: Unix domain socket transport, 4-byte little-endian
  length-prefixed `Envelope` frames (max 1 MiB), handshake with timeout, config delivery, notify and
  request/reply dispatch, heartbeat thread, and `Shutdown()` callable from another thread.
- Schema is proto2, not proto3: the core bundles protobuf 2.6.1, which parses only proto2 and has no
  `map<>`. `ModuleConfig` uses a repeated `ConfigEntry` with the same field number and wire encoding as
  the proto3 map. The wire format is unchanged, so `protocol_version` stays `"1"`.
- Tests: `tests/tier1_roundtrip_test.cpp` drives the client against a fake core over the real wire
  format. Covers accept + events + heartbeat + core `Shutdown`, protocol rejection, and module-side
  `Shutdown()`. Run with `ctest` (disable with `-DFOREVER_MODULE_SDK_BUILD_TESTS=OFF`).

- Tier 1 `.proto` schemas (handshake, events, control, envelope) for the v1 hook subset defined in
  the core repo's `doc/module-system/03-phase1-protocol-design.md`.
  - `HandshakeRequest.sdk_package_version` (informational) and `HandshakeResponse.negotiated_max_*`
    / `negotiated_tick_interval` (the Tier 2 bounds core actually granted).
  - `events.proto` covers only the v1 hooks: `PlayerScript::OnLogin/OnLogout`,
    `AccountScript::OnAccountLogin`, `WorldScript::OnStartup/OnShutdown`, `CommandScript`.
- `include/forever_module/tier2_layout.h` Tier 2 shared-memory POD structs, with explicit
  `Tier2RegionHeader::layout_version` (`kTier2LayoutVersion = 1`) and compile-time checks that the
  shared atomics are lock-free and the structs are POD.
- `protocol_version` (`"1"`) / SDK package version (`"0.1.0"`) split, per
  `doc/module-system/11-sdk-scope.md`.
- Build fix: generated protobuf sources are emitted to `generated/forever_module/`, matching the
  `forever_module/*.pb.h` include paths.
- `ModuleClient`/`Tier2Session` public API declared; implementation bodies are stubs.
- `examples/minimal-module/` smallest module skeleton.

### Not yet implemented

- Real `boost::asio` Tier 1 socket/pipe handling (connect, handshake, heartbeat, message framing,
  callback dispatch).
- Real `boost::interprocess` Tier 2 shared-memory attach/read/write and offset-addressing
  accessors for the variable-length `bots[]`/`nearby[]`/`actions[]` arrays.
- `MAX_TRACKED_COOLDOWNS` default value, still open pending a spell-casting test module
  (`doc/module-system/07-phase5-reference-module-design.md`).
