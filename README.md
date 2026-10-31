# forever-module-sdk

The SDK for writing **modules** for the Forever server (the ForeverBots / Forever Modules TrinityCore fork). A module is a separate program,
kept in its own git repository, that extends the server without touching the core's source: it reacts to game events, acts on players, and (the
reason the system exists) drives bots. This repository holds the C++ client library, the wire schema and the shared-memory layout that a module and
the core both have to agree on.

If you want to write a module, start with **[docs/01-quick-start.md](docs/01-quick-start.md)**.

## How a module relates to the core

A module is **not** a plugin loaded into the worldserver. It is its own process, started and supervised by the worldserver, and it can crash, stall or be
restarted without taking the world down.

```
 worldserver (core)                                         module process (your program, linked with this SDK)
 ------------------                                         ---------------------------------------------------
 ModuleMgr: scans Modules.Dir, checks approval,    spawn    main(argc, argv): argv[1] is the Tier 1 endpoint
 spawns <name> with the endpoint as argv[1]  ------------>
                                                            ModuleClient::Connect(endpoint)
 Tier 1: a local socket (Unix domain socket)  <==========>  ModuleClient::Handshake()   -> HandshakeResponse + ModuleConfig
   protobuf Envelopes, 4-byte length prefix                 ModuleClient::Run()          -> events, replies, timers, heartbeats
   handshake, config, heartbeats, event hooks,
   actions with replies, bot control, shutdown

 Tier 2: one shared-memory region per module  <==========>  Tier2Session (ModuleClient::AttachTier2())
   core writes a snapshot of the module's bots              ReadLatestSnapshot()  -> bots, nearby units
   core reads the module's published actions                PublishActions(...)   -> move, attack, cast, vendor, ...
```

- **Tier 1** is the control channel. It carries the handshake (name, versions, requested hooks and Tier 2 limits), the module's config, heartbeats, event
  notifications and requests from the core (a player looted an item, a chat line, a chat command), the module's actions on the world with an
  asynchronous reply (sell an item, send a message, buy, auction, trade, talent spending, the module store), bot control (`RequestControl`,
  `ReleaseControl`, `OnControlRevoked`), and the core's `Shutdown`.
- **Tier 2** is the fast path for bot AI. Every world tick (or every `tick_interval` ticks) the core publishes a snapshot of the bots the module controls
  and reads back the actions the module published for it. It is shared memory, not a socket, so a module can drive over a thousand bots without a frame per
  bot. The core re-checks every action it is given; the result of the last one comes back in the next snapshot.

The full picture (startup, approval, heartbeats, restarts, shutdown) is in [docs/02-architecture-and-lifecycle.md](docs/02-architecture-and-lifecycle.md).

## Documentation for module authors

| Guide | What it covers |
|---|---|
| [docs/01-quick-start.md](docs/01-quick-start.md) | From zero to a running module: layout, `CMakeLists.txt`, `module.json`, a `main.cpp`, installing and approving it on a server |
| [docs/02-architecture-and-lifecycle.md](docs/02-architecture-and-lifecycle.md) | Process model, handshake and approval, heartbeats and restarts, the shared-memory region, shutdown, how bots are taken and given back |
| [docs/03-tier2-snapshot-and-actions-reference.md](docs/03-tier2-snapshot-and-actions-reference.md) | Every snapshot field and every `ActionType`, with units, caps, the core switches an action needs, and the status codes |
| [docs/04-configuration-and-packaging.md](docs/04-configuration-and-packaging.md) | `Module.<name>.<Key>` config, `module.json`, data files, versioning, how releases are built and how to ship your own module |
| [docs/05-testing-and-debugging.md](docs/05-testing-and-debugging.md) | Unit-testing logic without a core, the SDK's tests, running against a worldserver, reading logs, common errors, rules of the road |
| [docs/06-writing-a-bot-policy-example.md](docs/06-writing-a-bot-policy-example.md) | A worked policy: bots follow their party leader and fight what is hostile near them |

The core's own design documents are in the core repository (`doc/module-system/` in
[Trixx34/Trinity-Forever-Modules](https://github.com/Trixx34/Trinity-Forever-Modules)): `03-phase1-protocol-design.md` for the wire contract,
`04-phase2-module-host-design.md` for the host (it is the original design, marked "not yet implemented" in its header, and the code has moved on; the guides
here were checked against the code instead), `11-sdk-scope.md` for this SDK's scope, `15-module-store.md` for the module store.

## Requirements

- A C++20 compiler (the SDK sets `CMAKE_CXX_STANDARD 20`; the public headers use `std::span`).
- CMake 3.20 or newer.
- Protobuf development files **and `protoc`** (the SDK generates its C++ code from `proto/*.proto` at build time). The schema is `proto2`.
- Boost 1.78 or newer, headers only (`find_package(Boost 1.78 REQUIRED)`). Boost.Interprocess backs the Tier 2 region; it is kept out of the public headers.
  The tests also use Boost.Asio.
- Threads.
- On Debian/Ubuntu the packages the project's own build hosts install are `libprotobuf-dev protobuf-compiler libboost-all-dev`.

A module built from this SDK runs on the same host as the worldserver (a local socket plus shared memory). Linux is what the server runs on. The code has a
Windows path (Winsock, `.exe` module names); it is not what the project's Linux pipelines exercise, so treat Windows as unverified here.

## Building and using the SDK

To build the SDK and its tests on their own:

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

To use it from a module, add it as a subdirectory (a local checkout) or fetch it, then link the `forever_module_sdk` target. This is the block the two stock
modules use; it switches off the SDK's own tests:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK can come from a local checkout (FOREVER_MODULE_SDK_SOURCE) or from GitHub.
set(FOREVER_MODULE_SDK_SOURCE "" CACHE PATH "Path to a forever-module-sdk checkout (optional)")
set(FOREVER_MODULE_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(FOREVER_MODULE_SDK_SOURCE)
    add_subdirectory("${FOREVER_MODULE_SDK_SOURCE}" "${CMAKE_BINARY_DIR}/forever-module-sdk")
else()
    include(FetchContent)
    FetchContent_Declare(forever_module_sdk
        GIT_REPOSITORY https://github.com/Trixx34/forever-module-sdk
        GIT_TAG main)
    FetchContent_MakeAvailable(forever_module_sdk)
endif()

find_package(Threads REQUIRED)

add_executable(my-module src/main.cpp)
target_link_libraries(my-module PRIVATE forever_module_sdk Threads::Threads)
```

`forever_module_sdk` is a static library. It exports the include directories of `include/` and of the generated protobuf headers, and links
`protobuf::libprotobuf` and `Threads::Threads` publicly, so a module needs no other include path. `FOREVER_MODULE_SDK_BUILD_EXAMPLES` (default `ON`) also
builds `examples/minimal-module`; set it `OFF` before `add_subdirectory` if you do not want that.

A module must be built against an SDK revision that matches the core it will run with: see the next section. A full, step-by-step setup is in the
[quick start](docs/01-quick-start.md).

## Versioning: three numbers

| Number | Where it is written | What it means | Bumps when |
|---|---|---|---|
| **SDK package version** | `kSdkPackageVersion` in `include/forever_module/protocol_version.h` and `project(... VERSION ...)` in `CMakeLists.txt` (kept equal; `tier1_roundtrip_test` fails when they differ) | This repository's own release number. Informational on the wire (`sdk_package_version` in the handshake). | Pre-1.0 scheme `0.MINOR.PATCH`: MINOR for a protocol or layout change or a new capability, PATCH for a fix. A change that touches only tests or docs bumps nothing (forever-ops `docs/96-versioning.md`). |
| **Protocol version** | `kProtocolVersion` (a string, currently `"20"`) in `protocol_version.h`; `kModuleProtocolVersion` in the core's `ModuleMgr.h`; `protocol_version` in every `module.json` | The wire contract: the Tier 1 schema plus the Tier 2 layout. | A breaking change to the Tier 1 schema or the Tier 2 layout. A new action type or a new Tier 1 action alone has been shipped without a bump (`CHANGELOG.md`, 0.24.0 `kGossip`). |
| **Tier 2 layout version** | `kTier2LayoutVersion` (currently `22`) in `tier2_layout.h`; the core keeps a byte-identical copy, `src/server/game/Modules/ModuleTier2Layout.h` | The exact byte layout of the shared-memory structs. | Any change to those structs. `src/tier2_session.cpp` pins their sizes with `static_assert`, so a field added without a bump does not compile. Every layout bump also bumps the protocol version (the header says so). |

**What happens on a mismatch.** The protocol version is matched **exactly**, as a string, in two places on the core side: when it reads `module.json` at
startup (the module is marked permanently failed and never started), and again against the `HandshakeRequest` (the handshake is rejected with
`protocol_version mismatch: core speaks "20", module declared "19"`). A rejected module is not restarted, because a restart cannot fix it. The layout
version is checked a second time, from the shared memory itself, when the module attaches to the region (`Tier 2 layout mismatch: core wrote version N, this
SDK reads version M`); the attach fails and `tier2_error()` carries that text. The SDK package version is never matched. See
[docs/02](docs/02-architecture-and-lifecycle.md) and [docs/05](docs/05-testing-and-debugging.md).

In practice: build the core, the SDK and every module from revisions that agree on the protocol number, and change `protocol_version` in your
`module.json` whenever you move to an SDK that bumped it. The project's artifact build (forever-ops `scripts/build-artifact.sh`) refuses to package a set
whose protocol numbers differ.

## Repository layout

```
CMakeLists.txt            The forever_module_sdk static library, and the options for the tests and examples
cmake/GenerateProto.cmake protoc wiring (output under <build>/generated/forever_module/)
proto/                    Tier 1 wire schema (proto2): handshake, events, actions, control, envelope
include/forever_module/   Public headers
    protocol_version.h        kProtocolVersion, kSdkPackageVersion
    tier2_layout.h            the shared-memory structs: BotSnapshot, NearbyUnit, BotAction, ActionType, MakeUnitRef, region arithmetic
    tier2_session.h           Tier2Session: Attach, ReadLatestSnapshot, PublishActions
    module_client.h           ModuleClient: the Tier 1 client and the bot-action convenience methods
    talent_spec.h             TalentSpec ids used by SpendBotTalents
src/                      module_client.cpp, tier2_session.cpp
examples/minimal-module/  The smallest module (Tier 1 only, one hook); built by default
tests/                    tier1_roundtrip_test, tier2_attach_test, module_client_test
docs/                     The module author guides listed above
CHANGELOG.md              Protocol, layout and additive changes, newest first
```

## Modules built on it

- [MOD-Forever-Bots](https://github.com/Trixx34/MOD-Forever-Bots): the bot AI. The reference for a complete Tier 2 module: connect and handshake, a pump
  thread that reads snapshots and publishes actions, settings parsed from the delivered config, data files, many pure-logic units with CTest suites.
- [MOD-auto-vendor-trash](https://github.com/Trixx34/MOD-auto-vendor-trash): the smallest real module. Sells looted grey items; Tier 1 only (one hook, one
  action, a config).
- [MOD-quest-group-loot](https://github.com/Trixx34/MOD-quest-group-loot): shares quest items with the party.

## License and contributing

There is no license file in this repository (the earlier README said "TBD"), so none is granted yet; ask the owner before reusing the code outside the
project. Work on a branch other than `main` and `test` and open a pull request: in this project a push to either of those branches moves a product head
and starts the build and deploy pipelines. A change that alters the protocol or the layout has to be made in the SDK and the core together, with a
`CHANGELOG.md` entry and the version bumps described above.
