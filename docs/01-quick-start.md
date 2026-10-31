# 1. Quick start: a running "hello" module

This guide takes you from an empty directory to a module the worldserver starts, approves, and that walks a bot around. It uses only names that exist in
this SDK's headers and in the core's module host. **The code blocks in this guide are not compiled by CI or by the author** (no build was run while
writing it; they were checked by reading against `module_client.h`, `tier2_session.h` and `tier2_layout.h`). `examples/minimal-module/` is the one example
in this repository that CI does build. If a snippet does not compile, the headers win.

Read [docs/02](02-architecture-and-lifecycle.md) afterwards for what happens behind each step.

## 0. What you need

- The requirements in the [README](../README.md#requirements) (C++20, CMake 3.20, protobuf with `protoc`, Boost headers 1.78+).
- A checkout of this SDK, at a revision whose `kProtocolVersion` (in `include/forever_module/protocol_version.h`) equals the `kModuleProtocolVersion` of the
  worldserver you will run it on (currently `"20"` on both).
- A worldserver with `Modules.Enabled = 1` (section 5) to see it run.

## 1. Directory layout

A module is one repository, one executable, one manifest. The two stock modules are laid out like this, and so should yours be:

```
hello-bot/
  CMakeLists.txt
  module.json                           the manifest the core reads (section 3)
  src/main.cpp
  config/modules.d/hello-bot.conf.example   example settings (the artifact build packs these; see docs/04)
```

Keep the policy (the decisions) in plain functions in their own files and `main.cpp` thin, so the policy can be unit-tested without a core
([docs/05](05-testing-and-debugging.md)). The quick start puts everything in `main.cpp` to stay short.

## 2. CMakeLists.txt

This is the pattern of MOD-Forever-Bots and MOD-auto-vendor-trash. The module's version is written once, in `module.json`, and the build reads it into the
manifest through the `FOREVER_MODULE_VERSION` macro (`string(JSON ...)` needs CMake 3.19+).

```cmake
cmake_minimum_required(VERSION 3.20)
project(hello_bot LANGUAGES CXX)

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

# The module version is written once, in module.json; the build reads it into the manifest.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/module.json" FOREVER_MODULE_JSON)
string(JSON FOREVER_MODULE_VERSION GET "${FOREVER_MODULE_JSON}" version)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module.json")

# The executable must be named exactly like the module (section 4).
add_executable(hello-bot src/main.cpp)
target_compile_definitions(hello-bot PRIVATE FOREVER_MODULE_VERSION="${FOREVER_MODULE_VERSION}")
target_link_libraries(hello-bot PRIVATE forever_module_sdk Threads::Threads)
```

Build it with `cmake -S . -B build -DFOREVER_MODULE_SDK_SOURCE=/path/to/forever-module-sdk && cmake --build build`.
Linking `forever_module_sdk` brings in the SDK include directory, the generated protobuf headers, `protobuf::libprotobuf` and Threads; Boost stays
private to the SDK.

Note that the stock modules are built by the project's pipeline, not by hand (see [docs/04](04-configuration-and-packaging.md)); a hand build is for
your own development.

## 3. module.json

The core reads `module.json` from the module's folder (`ParseModuleManifest` in the core's `ModuleManifestInfo.cpp`). Only three fields are required.

```json
{
  "name": "hello-bot",
  "version": "0.1.0",
  "protocol_version": "20",
  "tier2": {
    "participates": true,
    "requested_max_bots": 4,
    "requested_max_nearby_units": 4,
    "requested_max_actions": 8,
    "requested_tick_interval": 1
  }
}
```

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | The module's name. It must equal the **folder name** under `Modules.Dir` (a mismatch skips the module with a warning) and consist of letters, digits, `_` and `-` only. It is also the executable's file name and the `Module.<name>.` prefix of its config keys. |
| `version` | yes | The module's own version, informational. A change of it needs no re-approval. Pre-1.0 scheme `0.MINOR.PATCH` ([docs/04](04-configuration-and-packaging.md)). |
| `protocol_version` | yes | A string. Must equal the core's `kModuleProtocolVersion` exactly, or the module is marked permanently failed and never started. |
| `sdk_package_version` | no | Read by the core, informational only. The stock modules omit it; the SDK sends its own `kSdkPackageVersion` in the handshake anyway. |
| `tier1.hooks` | no | A list of Tier 1 hook names the module subscribes to. The core accepts only these: `PlayerScript.OnLogin`, `PlayerScript.OnLogout`, `AccountScript.OnAccountLogin`, `WorldScript.OnStartup`, `WorldScript.OnShutdown`, `CommandScript`, `PlayerScript.OnLootItem`, `PlayerScript.OnAuctionResult`, `PlayerScript.OnChat`, `PlayerScript.OnBotTrade` (the `SupportedTier1Hooks` list in the core's `ModuleMgr.cpp`). Any other name is refused at the handshake. |
| `tier2.participates` | no (default `false`) | `true` if the module wants a shared-memory region (a snapshot of its bots and an action queue). Without it the module has no bots to drive. |
| `tier2.requested_max_bots` | no (default 0) | The most bots the module may control at once. |
| `tier2.requested_max_nearby_units` | no (default 0) | Nearby units listed per bot in each snapshot. |
| `tier2.requested_max_actions` | no (default 0) | The most actions the module may publish in one tick. |
| `tier2.requested_tick_interval` | no (default 1) | Snapshot cadence in server ticks: 1 is every tick. |

**These are what the operator approves.** The core hashes the sorted, de-duplicated hook list and the Tier 2 block (`participates` and the four `requested_*`
numbers); name, version and SDK version are not hashed. Change any hashed value and the module is not started until the operator approves again.

**`module.json` and the manifest in your code are two declarations of the same thing.** The core uses `module.json` to decide approval and to cap the grant,
and uses the `HandshakeRequest` your code sends (`ModuleManifest`) to negotiate. Keep them identical. At the handshake the core refuses a hook the approval
does not contain (`Tier 1 hook was not approved: ...`) and a Tier 2 request when approval says no Tier 2. The granted bounds are the minimum of what the
code asks, the worldserver's `Modules.Tier2.*` limits, and the approved `module.json` values. For the tick interval, the approved `requested_tick_interval` is
also a ceiling on what the code may ask for (read from `NegotiateTier2`). The SDK then refuses a grant that is above what the code requested, or that has no
bots or no actions (`core granted Tier 2 bounds outside the requested limits`).

## 4. main.cpp

The module is started as `<executable> <endpoint>`: `argv[1]` is the path of the Unix domain socket the core listens on. The program connects, does the
handshake, registers its callbacks and timers, and calls `Run()`, which blocks until the core sends `Shutdown` or the connection is lost.

What this one does: it asks the core for `Bots` bots (default 1, from the module's config, at most 4), then every new snapshot tick it sends each idle bot
that is out of combat to the next corner of an 8-yard square around where it first appeared.

```cpp
// hello-bot: asks the core for a few bots and walks each one around a small square.
// Usage: hello-bot <tier1-endpoint>. The core passes the endpoint when it spawns the module.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "forever_module/module_client.h"
#include "forever_module/tier2_session.h"

namespace fm = forever_module;

namespace {

constexpr float kSide = 8.0f;                         // yards
constexpr std::uint64_t kOrderEveryTicks = 20;        // do not re-order a walking bot more often than this
constexpr float kOffsetX[4] = { kSide, kSide, 0.0f, 0.0f };
constexpr float kOffsetY[4] = { 0.0f, kSide, kSide, 0.0f };

// What the module remembers about one bot.
struct Walker
{
    bool known = false;
    float homeX = 0.0f, homeY = 0.0f, homeZ = 0.0f;
    int corner = 0;
    std::uint64_t lastOrderTick = 0;
    std::uint32_t seenSequence = 0;
};

// The policy: pure, no I/O. Appends the actions this bot should get on this snapshot tick.
void Decide(fm::BotSnapshot const& bot, Walker& walker, std::uint64_t tick, std::vector<fm::BotAction>& out)
{
    if (!walker.known)
    {
        walker.known = true;
        walker.homeX = bot.pos_x;
        walker.homeY = bot.pos_y;
        walker.homeZ = bot.pos_z;
    }
    if (bot.life_state != 0 || bot.in_combat || bot.moving)
        return;
    if (tick - walker.lastOrderTick < kOrderEveryTicks)
        return;

    fm::BotAction action;
    action.bot_guid = bot.guid;                       // the raw character guid counter, as in BotSnapshot::guid
    action.type = fm::ActionType::kMoveTo;
    action.dest_x = walker.homeX + kOffsetX[walker.corner];
    action.dest_y = walker.homeY + kOffsetY[walker.corner];
    action.dest_z = walker.homeZ;
    out.push_back(action);

    walker.corner = (walker.corner + 1) % 4;
    walker.lastOrderTick = tick;
}

// The core reports what it did with the last action it applied to a bot in the next snapshot. Say so when it was not applied.
void ReportResult(fm::BotSnapshot const& bot, Walker& walker)
{
    if (bot.last_action_sequence == 0 || bot.last_action_sequence == walker.seenSequence)
        return;
    walker.seenSequence = bot.last_action_sequence;
    if (bot.last_action_status != fm::BotActionStatus::kApplied)
        std::cerr << "hello-bot: bot " << bot.guid << " action type " << static_cast<int>(bot.last_action_type)
                  << " not done, status " << static_cast<int>(bot.last_action_status) << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: hello-bot <tier1-endpoint>\n";
        return 1;
    }

    // What we tell the core at the handshake. Keep it identical to module.json.
    fm::ModuleManifest manifest;
    manifest.module_name = "hello-bot";
    manifest.module_version = FOREVER_MODULE_VERSION;   // from module.json, see CMakeLists.txt
    manifest.tier2_participates = true;
    manifest.requested_max_bots = 4;
    manifest.requested_max_nearby_units = 4;
    manifest.requested_max_actions = 8;
    manifest.requested_tick_interval = 1;

    fm::ModuleClient client(std::move(manifest));
    if (!client.Connect(argv[1]))
    {
        std::cerr << "hello-bot: could not connect to " << argv[1] << "\n";
        return 1;
    }
    if (!client.Handshake())
    {
        std::cerr << "hello-bot: handshake failed: " << client.rejection_reason() << "\n";
        return 1;
    }

    // Config: the Module.hello-bot.<Key> entries, prefix removed. The core sends it right after the handshake, so this callback runs at once; it runs
    // again, with the full current config, whenever the core reloads it.
    std::uint32_t wanted = 1;
    client.OnConfig([&wanted](std::unordered_map<std::string, std::string> const& config)
    {
        auto const it = config.find("Bots");
        if (it == config.end())
            return;
        try
        {
            wanted = static_cast<std::uint32_t>(std::min<unsigned long>(std::stoul(it->second), 4));
        }
        catch (std::exception const&)
        {
            std::cerr << "hello-bot: config: Bots = '" << it->second << "' is not a number; keeping " << wanted << "\n";
        }
    });

    // The shared-memory region the core created for us (bounds granted at the handshake).
    fm::Tier2Session* session = client.AttachTier2();
    if (!session)
    {
        std::cerr << "hello-bot: no Tier 2 region: " << client.tier2_error() << "\n";
        return 1;
    }
    std::cerr << "hello-bot: granted " << client.negotiated_bounds().max_bots << " bots, " << client.negotiated_bounds().max_actions
              << " actions per tick, tick interval " << client.negotiated_bounds().tick_interval << "\n";

    // Everything below runs on the thread that calls Run(): the callbacks and the timers.
    int granted = 0;
    int pending = 0;
    client.OnControlRevoked([&granted](std::uint64_t botGuid, std::string const& reason)
    {
        --granted;
        std::cerr << "hello-bot: control of bot " << botGuid << " revoked: " << reason << "\n";
    });

    // Ask for bots one at a time until we have as many as configured (and as many as the core granted room for).
    client.AddTimer(std::chrono::seconds(2), [&]
    {
        std::uint32_t const limit = std::min(wanted, client.negotiated_bounds().max_bots);
        if (granted + pending >= static_cast<int>(limit))
            return;
        fm::SpawnParams params;
        params.set_race(1);
        params.set_class_id(1);
        ++pending;
        client.RequestControl(0, params, [&](fm::ControlResponse const& reply)
        {
            --pending;
            if (reply.granted())
            {
                ++granted;
                std::cerr << "hello-bot: got bot " << reply.bot_guid() << "\n";
            }
            else
                std::cerr << "hello-bot: spawn refused: " << reply.reason() << "\n";
        });
    });

    // The pump: poll for a new snapshot, decide, publish once per snapshot tick.
    std::unordered_map<std::uint64_t, Walker> walkers;
    std::vector<fm::BotAction> actions;
    bool seenTick = false;
    std::uint64_t lastTick = 0;
    client.AddTimer(std::chrono::milliseconds(20), [&]
    {
        fm::WorldTickSnapshotView const& view = session->ReadLatestSnapshot();
        if (!session->HasSnapshot() || (seenTick && view.tick_number == lastTick))
            return;
        seenTick = true;
        lastTick = view.tick_number;

        actions.clear();
        for (fm::BotSnapshot const& bot : view.bots)
        {
            Walker& walker = walkers[bot.guid];
            ReportResult(bot, walker);
            Decide(bot, walker, view.tick_number, actions);
        }
        if (actions.empty())
            return;

        std::string error;
        if (!session->PublishActions(actions, error))
            std::cerr << "hello-bot: could not publish actions: " << error << "\n";
    });

    client.Run();          // returns when the core sends Shutdown, or the connection is lost
    return 0;
}
```

Points to notice, each from the headers:

- `Connect` returns `false` with no reason. On Linux it also refuses (returns `false`) if the process was re-parented to pid 1, because it uses
  `prctl(PR_SET_PDEATHSIG, SIGTERM)` to die with the worldserver; a module started from a shell works, one double-forked into the background does not.
  `Handshake` returns `false` and `rejection_reason()` says why (for example the core's `protocol_version mismatch: ...` text).
- `OnConfig` is invoked immediately if the config already arrived during `Handshake()`. Each delivery is the **full** current config, not a diff.
- `AttachTier2()` returns `nullptr` if the core granted no region (the module did not ask, or was not approved for Tier 2) or the attach failed;
  `tier2_error()` says which.
- `ReadLatestSnapshot()` never blocks; it returns the newest complete snapshot, and the same one again until the core publishes the next. Comparing
  `tick_number` is how a module publishes **once per snapshot**: the core takes the newest action set it sees for a tick, and two sets between two reads leave
  only the second visible.
- `PublishActions` takes the **whole** queue for the tick. It needs one snapshot to have been read (the tick number goes with the actions), refuses more
  actions than `MaxActions()` and an action type above `ActionType::kLastActionType`. The core drops an action for a bot the module does not control.
- All callbacks and timers run on the `Run()` thread, and the heartbeat is sent by the SDK's own thread, so a slow callback does not stop heartbeats.
  A real module with expensive decisions puts the pump on its own thread; see [docs/05](05-testing-and-debugging.md).
- This quick start publishes with `Tier2Session::PublishActions` directly. Do not mix that with the `ModuleClient` bot-action methods (`MoveTo`, `Follow`,
  ...) in one module: both publish a whole queue, and the second would replace the first for that tick.

## 5. Install it on a server

The worldserver finds modules by scanning a directory. Everything below is on the machine that runs the worldserver; paths are relative to the directory the
worldserver starts in unless stated otherwise.

1. **Enable the module system** in `worldserver.conf`: `Modules.Enabled = 1` (default 0). `Modules.Dir` defaults to `"modules_runtime"`.
2. **Put the module in `Modules.Dir`**, in a folder named after it:

   ```
   modules_runtime/hello-bot/hello-bot        the executable (must be named exactly like the module; the Windows name is hello-bot.exe)
   modules_runtime/hello-bot/module.json
   ```

   The core runs `<Modules.Dir>/<name>/<name> <Modules.Dir>/<name>.sock`. A folder without `module.json`, with a name that is not letters, digits, `_` and `-`,
   or whose `module.json` `name` differs from the folder name, is skipped with a warning. A missing executable marks the module permanently failed
   (`executable not found`). The project's deploy installs modules with an rsync into `modules_runtime/` and keeps data files already there; for a hand
   install, copy the files and make the binary executable by the account that runs the worldserver.
3. **Give it a config file**, optional, at `worldserver.conf.d/modules.d/hello-bot.conf` (`worldserver.conf.d` is the default folder, relative to the
   directory the worldserver starts in, per `worldserver.conf.dist`; the core loads every `.conf` under it at startup). Every key must be written as `Module.<name>.<Key>`:

   ```
   Module.hello-bot.Bots = 2
   ```

   The module receives `Bots = 2`. See [docs/04](04-configuration-and-packaging.md).
4. **Approve it.** The core starts a module only if the capabilities in its `module.json` match what an operator approved ("deny by default"). On the
   worldserver console (or with the `RBAC_PERM_COMMAND_MODULE` permission):

   ```
   module list
   module approve hello-bot
   ```

   `module approve` reads the module's current `module.json` and records its capability hash and version in `approved_modules.json`, next to
   `worldserver.conf`. The reply says **"Takes effect at the next module load"**: modules are loaded when the worldserver starts (I found no reload
   command), so restart the worldserver after approving. Until then `module list` shows `hello-bot 0.1.0 [pending approval] never approved; run 'module
   approve hello-bot' to approve version 0.1.0`. A later change of hooks or Tier 2 numbers shows `capabilities changed since version ...` and needs a new
   approval. `module info hello-bot` shows the status, process id, heartbeats and restarts. For a throwaway dev setup `Modules.AutoApproveLocal = 1` skips
   the gate entirely; the core warns that this is not for production.
5. **Bot limits.** Spawning a bot is subject to the core's `Bot.*` limits (for example `Bot.Limit.MaxBots`); a refused spawn reaches the module as
   `granted = false` with a `reason`.

## 6. See it working

The core logs under the `modules` logger. After a restart you should see, in `Server.log` (exact wording from `ModuleMgr.cpp`):

```
started 'hello-bot' (pid 12345), waiting for its handshake.
module 1: Tier 2 region 'trinity_module_1_tier2' (bots 4, nearby 4, actions 8, tick interval 1).
'hello-bot' handshake accepted (module id 1).
```

The module's own `std::cerr` lines (`hello-bot: granted 4 bots, ...`, `hello-bot: got bot 42`) go wherever the worldserver's stderr goes: the module is a child
process of the worldserver and, as far as the core's spawn call shows, inherits its standard streams (inferred from `boost::process::child` defaults, not
checked on a running server). Under systemd that would be the service's journal. To get a line
into the core's `Server.log`, tagged with the module's name, use `client.Log(0, "text")` (level 0 info, 1 warning, 2 error; 1 to 255 bytes).

Then log in with a client and look for the bot walking its square, or check `module info hello-bot` for the `heartbeats:` counter moving.

If it does not start, [docs/05](05-testing-and-debugging.md) lists the messages and what they mean.

## 7. Where next

- Calls available on `ModuleClient` (events, sell, buy, auction, trade, store, talents): `include/forever_module/module_client.h`.
- Every snapshot field and every action: [docs/03](03-tier2-snapshot-and-actions-reference.md).
- A policy with state and groups: [docs/06](06-writing-a-bot-policy-example.md).
- The smallest Tier 1 module that works against a real core, `auto-vendor-trash` (about 100 lines of `main.cpp`), and the full Tier 2 reference,
  `MOD-Forever-Bots` (`src/main.cpp`, function `PumpTier2` for the pump).
