# forever-module-sdk

SDK for building external, process-isolated modules for the ForeverBots/ForeverModules
TrinityCore fork's module system. Lets developers extend the core (hooks, bot AI, custom
behavior) from a separate git repository, without modifying core source.

Full design is documented in the core repo (`D:\Private\ForeverModules`, branch `foreverModules`)
under `doc/module-system/`:

- `00-investigation.md` through `11-sdk-scope.md` — the complete design arc (codebase
  investigation, trust/versioning/build decisions, Tier 1 event-hook + Tier 2 shared-memory
  protocol, core-side module host, build-time discovery, capability approval, reference bot-AI
  acceptance test, and this SDK's own scope).

## Status

**Skeleton.** Directory structure, full `.proto` schemas, and header declarations are in place.
`ModuleClient`/`Tier2Session` method bodies are stubs (see `src/*.cpp`) — no real
`boost::asio`/`boost::interprocess` implementation yet. Not usable to build a working module yet.

## Versioning

Two independent axes (see `doc/module-system/11-sdk-scope.md` in the core repo for the full
rationale):

- **`protocol_version`** (`include/forever_module/protocol_version.h`, `kProtocolVersion`) — the
  Tier 1/Tier 2 wire contract. Exact-matched against core's compiled-in version at handshake.
  Only bumps on breaking changes.
- **SDK package version** (`kSdkPackageVersion`, and this repo's own git tags) — this repo's own
  release version. Can bump freely for bugfixes without changing `protocol_version`.

## Building

```
cmake -S . -B build
cmake --build build
```

Requires Protobuf and Boost (system component) development packages available to CMake's
`find_package`.

## Layout

```
proto/                  Tier 1 wire schema (.proto)
include/forever_module/  Public headers: protocol_version.h, tier2_layout.h, module_client.h, tier2_session.h
src/                     ModuleClient / Tier2Session implementation (currently stubs)
cmake/GenerateProto.cmake  protoc codegen wiring
examples/minimal-module/  Smallest possible module using this SDK
```

## License

TBD.
