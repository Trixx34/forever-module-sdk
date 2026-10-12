# forever-module-sdk

SDK for building external, process-isolated modules for the ForeverBots/ForeverModules
TrinityCore fork's module system. Lets developers extend the core (hooks, bot AI, custom
behavior) from a separate git repository, without modifying core source.

## Design docs

The design docs live in the core repository, not in this one:
`doc/module-system/` in [Trixx34/ForeverBots](https://github.com/Trixx34/ForeverBots) (branch
`foreverModules`). Start with `00-investigation.md`, then `03-phase1-protocol-design.md` for the wire
contract and `11-sdk-scope.md` for this SDK's scope and versioning.

## Status

**Phase 1 (contract) complete, v0.1.0, plus the Tier 1 client.** The `.proto` schemas and Tier 2
layout are final for protocol `"1"`. `ModuleClient` implements Tier 1 on POSIX (see the CHANGELOG).
`Tier2Session` and bot control (`RequestControl`/`ReleaseControl`) are still stubs, so
this release is not yet usable to run a working module.

## Versioning

Two independent axes (see `doc/module-system/11-sdk-scope.md` in the core repo for the full
rationale):

- **`protocol_version`** (`include/forever_module/protocol_version.h`, `kProtocolVersion`, currently
  `"1"`) is the Tier 1/Tier 2 wire contract. Core exact-matches it at handshake. Only bumps on
  breaking changes.
- **SDK package version** (`kSdkPackageVersion`, currently `"0.1.0"`, and this repo's git tags as
  `v<version>`) is this repo's own release version. It can bump freely for bugfixes without changing
  `protocol_version`.

## Building

```
cmake -S . -B build
cmake --build build
```

Requires Protobuf (with `protoc`) and Boost (system component) development packages available to
CMake's `find_package`.

## Layout

```
proto/                  Tier 1 wire schema (.proto)
include/forever_module/  Public headers: protocol_version.h, tier2_layout.h, module_client.h, tier2_session.h
src/                     ModuleClient / Tier2Session implementation (currently stubs)
cmake/GenerateProto.cmake  protoc codegen wiring (output under build/generated/forever_module/)
examples/minimal-module/  Smallest possible module using this SDK
```

## License

TBD.
