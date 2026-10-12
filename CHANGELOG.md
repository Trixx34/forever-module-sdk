# Changelog

## [0.1.0] - unreleased

Phase 1 contract release (wire schema, Tier 2 layout) plus the Tier 1 client. Tier 2 shared memory
and bot control are not included yet (see "Not yet implemented").

- Tier 1 client (`ModuleClient`), POSIX only: Unix domain socket transport, 4-byte little-endian
  length-prefixed `Envelope` frames (max 1 MiB), handshake with timeout, config delivery, notify and
  request/reply dispatch, heartbeat thread, and `Shutdown()` callable from another thread.
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
