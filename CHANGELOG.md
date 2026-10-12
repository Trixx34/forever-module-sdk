# Changelog

## [0.1.0-skeleton] - unreleased

Initial scaffold. Not a functional release.

- Full `.proto` schemas for Tier 1 (handshake, events, control, envelope) per the design in the
  core repo's `doc/module-system/03-phase1-protocol-design.md`, `04-phase2-module-host-design.md`,
  `09-onaurachange-hook-design.md`.
- `include/forever_module/tier2_layout.h` — Tier 2 shared-memory POD struct declarations
  (`Tier2RegionHeader`, `BotSnapshot`, `NearbyUnit`, `BotAction`, `ActionType`) per
  `doc/module-system/03-phase1-protocol-design.md`.
- `ModuleClient`/`Tier2Session` public API declared per `doc/module-system/11-sdk-scope.md` —
  implementation bodies are stubs (`TODO` markers throughout `src/*.cpp`).
- `protocol_version` (`"1"`) / SDK package version (`"0.1.0-skeleton"`) split established per
  `doc/module-system/11-sdk-scope.md`.
- CMake build wiring (`cmake/GenerateProto.cmake`) for protoc codegen at build time.
- `examples/minimal-module/` — smallest possible module skeleton using this SDK.

### Not yet implemented

- Real `boost::asio` Tier 1 socket/pipe handling (connect, handshake, heartbeat, message framing,
  callback dispatch).
- Real `boost::interprocess` Tier 2 shared-memory attach/read/write and offset-addressing
  accessors for the variable-length `bots[]`/`nearby[]`/`actions[]` arrays.
- `MAX_TRACKED_COOLDOWNS` default value — still open pending a spell-casting test module
  (`doc/module-system/07-phase5-reference-module-design.md`).
