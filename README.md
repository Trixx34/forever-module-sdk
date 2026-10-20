# forever-module-sdk

SDK for building external, process-isolated modules for the ForeverBots / Forever Modules TrinityCore fork's module system. A
module is a separate program, in its own git repository, that extends the core (event hooks, actions on players, bot AI) without
modifying core source. It talks to the core over a local socket (Tier 1) and, for bots, through a shared-memory region (Tier 2).

## Design docs

The design docs live in the core repository, not in this one: `doc/module-system/` in
[Trixx34/Trinity-Forever-Modules](https://github.com/Trixx34/Trinity-Forever-Modules) (formerly ForeverBots), branch
`foreverModules`. Start with `00-investigation.md`, then `03-phase1-protocol-design.md` for the wire contract and
`11-sdk-scope.md` for this SDK's scope and versioning.

## What the SDK gives a module

**Tier 1 (`ModuleClient`, protobuf over a local socket)**
- `Connect`, `Handshake`, `Run`, `Shutdown`: connect to the endpoint the core passes at spawn, and run the event loop.
- `OnConfig`: the `Module.<name>.<Key>` entries from the core's config, with the prefix removed. Re-delivered on a config reload.
- `OnNotify` / `OnRequest`: event hooks, for example `PlayerScript.OnLootItem` (the item looted, its class, quality, sell price).
- `AddTimer`: periodic callbacks on the thread that runs `Run()`.
- Actions the core carries out on the world thread, answered asynchronously: `SellItem`, `SendPlayerMessage`, `ShareQuestItem`
  (give a looted quest item to the party members who need it), and the module store (`StoreGet`, `StoreSet`, `StoreDelete`,
  `StoreList`): rows private to the module.
- Bot control: `RequestControl` (spawn or reclaim a bot), `ReleaseControl`, `OnControlRevoked`.

**Tier 2 (`Tier2Session`, shared memory)**
- A snapshot of every controlled bot each tick (position, health, power, level, combat state, the result of its last action,
  cooldowns, party state, nearest vendor, map, and the nearby units, 16 per bot by default), read as a `WorldTickSnapshotView`.
- Actions published back for the next tick (`PublishActions`): move, follow, attack, cast, stop, rest, vendor, train, group
  (party), and loot. The core re-checks everything it is asked to do.

## Status and versioning

Two independent axes (see `doc/module-system/11-sdk-scope.md` in the core repo):

- **`protocol_version`** (`include/forever_module/protocol_version.h`, `kProtocolVersion`) is the wire contract.
  Core exact-matches it at handshake. Only bumps on breaking changes.
- **Tier 2 layout version** (`kTier2LayoutVersion` in `tier2_layout.h`) is the shared-memory layout. A module built
  against another layout is refused at region attach. Each layout bump is listed in `CHANGELOG.md`; adding an action type or a Tier 1
  action does not bump it. `tier2_session.cpp` pins the sizes of the shared structs, so a field added without a bump fails to compile.
- **SDK package version** (`kSdkPackageVersion`, currently `"0.1.0"`) is this repo's own release version.

The core and the SDK must agree on the protocol and the layout. The core keeps a byte-identical copy of `tier2_layout.h`
(`src/server/game/Modules/ModuleTier2Layout.h`).

## Building

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build

Requires Protobuf (with `protoc`) and Boost (system component) development packages available to CMake's `find_package`. Builds
on Linux and Windows. `tier1_roundtrip_test` runs a fake core against the client; `tier2_attach_test` checks the shared-memory
region, the snapshot reader and the action publisher.

## Using it from a module

    cmake -S . -B build -DFOREVER_MODULE_SDK_SOURCE=/path/to/forever-module-sdk

or leave `FOREVER_MODULE_SDK_SOURCE` unset and the module's CMake fetches this repo from GitHub (`main`). Modules that need
features not yet on `main` point `FOREVER_MODULE_SDK_SOURCE` at a checkout of the branch that has them.

Modules built on it: [MOD-Forever-Bots](https://github.com/Trixx34/MOD-Forever-Bots) (bot AI),
[MOD-auto-vendor-trash](https://github.com/Trixx34/MOD-auto-vendor-trash) (sells looted grey items),
[MOD-quest-group-loot](https://github.com/Trixx34/MOD-quest-group-loot) (shares quest items with the party).

## Layout

```
proto/                    Tier 1 wire schema (.proto): handshake, events, actions, control
include/forever_module/   Public headers: protocol_version.h, tier2_layout.h, module_client.h, tier2_session.h
src/                      ModuleClient and Tier2Session
cmake/GenerateProto.cmake protoc codegen wiring (output under build/generated/forever_module/)
examples/minimal-module/  Smallest possible module using this SDK
tests/                    tier1_roundtrip_test, tier2_attach_test
CHANGELOG.md              Protocol, layout and additive changes
```

## License

TBD.
