# 4. Configuration, versioning and packaging

Checked against the core's `ModuleConfig.cpp`, `ModuleMgr.cpp` and `worldserver.conf.dist`, the SDK (`module_client.h`, `handshake.proto`), the two stock
modules, and the forever-ops repo (`docs/96-versioning.md`, `scripts/build-artifact.sh`, `scripts/lib/package-configs.sh`, the deploy role). Anything
that is only the project's build and deploy practice, rather than something the core or SDK enforces, is marked as such.

## How a module's configuration reaches it

A module cannot read `worldserver.conf` (the `.dist` file says so) and the core does not pass it the `Bot.*` or `Modules.*` keys. The only settings a module
gets are the ones the operator wrote for it:

1. The operator writes keys of the form **`Module.<module-name>.<Key> = value`** in any `.conf` file the worldserver loads. By convention one file per
   module: `worldserver.conf.d/modules.d/<module-name>.conf`. `worldserver.conf.d` is the default folder, relative to the directory the worldserver starts in; every
   `.conf` under it is loaded at startup (`worldserver.conf.dist`, "MODULE SYSTEM SETTINGS"). `<module-name>` is the module's `name`, so for `forever-bots` the
   keys are `Module.forever-bots.MaxBots = 20`.
2. When the module's handshake is accepted the core sends a `ModuleConfig` message (`handshake.proto`: a list of `ConfigEntry{key, value}`) with every key that
   starts with `Module.<name>.` and has something after the prefix, **with the prefix removed**. `ModuleConfigFor` in the core does exactly that; keys of other
   modules and of the core are skipped.
3. The same message is sent again, in full (not a diff), whenever the worldserver reloads its configuration (`ModuleMgr::ResendConfig`, called from
   `World::LoadConfigSettings` when `reload` is true; in stock TrinityCore that is the `.reload config` command). A key the operator removed is simply absent
   from the new map.
4. In the SDK: `ModuleClient::Handshake()` reads the first `ModuleConfig` and stores it. `ModuleClient::OnConfig(callback)` gives you
   `std::unordered_map<std::string, std::string> const&`; if the config has already arrived the callback runs **immediately** inside `OnConfig`, and again on each
   later delivery, on the `Run()` thread. Register `OnConfig` after `Handshake()` and before you need the values, and make the first delivery set everything up.

Consequences to design for:

- **Everything is a string.** Parse and validate every value yourself. A missing key means "use the default"; an invalid one should log a warning and keep the
  default, not throw (a throw out of a callback leaves `Run()`). The stock modules collect warnings in a `std::vector<std::string>` and print them to
  `std::cerr` as `<module>: config: <Key>: <reason>; using <default>.`
- **A reload can change values while the module runs.** Decide which settings may change live (the bots module applies the ramp and the bot cap on reload; its
  telemetry file and test spell need a restart) and say so in the example config's comments. Do not hold references into the map: copy what you need.
- **The callback runs on the `Run()` thread**, while a pump thread (if you have one) is reading the same settings: use a lock, an atomic, or a generation
  counter, as the bots module does for its dungeon files.
- Keys are matched by exact prefix `Module.<name>.`; whether the core's `ConfigMgr` treats key names case-insensitively was not checked, so write them
  exactly as the module reads them.

### A parser worth copying

Both stock modules keep parsing in a separate, core-free file (`bot_settings.cpp`, `vendor_settings.cpp`) so it is unit-tested. This is the shape of the
helper, adapted from `bot_settings.cpp` (not compiled here):

```cpp
#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

// Reads Key as a whole number in [min, max]. A missing key leaves `value` as it is; an invalid one leaves it and records a warning.
void ReadUnsigned(std::unordered_map<std::string, std::string> const& config, char const* key, std::uint32_t min, std::uint32_t max,
                  std::uint32_t& value, std::vector<std::string>& warnings)
{
    auto const it = config.find(key);
    if (it == config.end())
        return;

    std::string const& text = it->second;
    std::uint64_t parsed = 0;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc() || end != text.data() + text.size() || parsed < min || parsed > max)
    {
        warnings.push_back(std::string("Module.my-module.") + key + ": '" + text + "' is not a whole number from " +
                           std::to_string(min) + " to " + std::to_string(max) + "; using " + std::to_string(value) + ".");
        return;
    }
    value = static_cast<std::uint32_t>(parsed);
}
```

The stock modules' conventions for other shapes: booleans are `0`/`1` (the bots module also accepts `true`/`false`); lists are comma-separated
(`ExcludedItemIds = 1,2,3`, spaces around entries tolerated, empty entries ignored); a point is `"x y z"` (`Hub0_1 = 2265.2 269.3 36.5`); a time of day is `HH:MM`
(`BotsStopTime = 06:00`); a path is relative to **the module's working directory** (the child inherits the worldserver's, as far as the spawn call shows) unless
absolute, and the example config says to use an absolute path to be sure.

### The example config file

Ship `config/modules.d/<name>.conf.example` in the module's repository. The project's packaging copies it into the artifact (below) and the operator copies it,
without `.example`, to `worldserver.conf.d/modules.d/<name>.conf`. Start it with a comment that says exactly that, give every key with its range and default, and
keep the optional ones commented out. The two stock files (`MOD-Forever-Bots/config/modules.d/forever-bots.conf.example`,
`MOD-auto-vendor-trash/config/modules.d/auto-vendor-trash.conf.example`) are the model.

## Data files

A module that needs tables (quests, spells, routes) ships them as files and reads them by path. In the project's layout:

- they live in the module's source tree (`MOD-Forever-Bots/data/`), are copied by the artifact build to `modules/forever-bots/data/`, and the deploy installs them
  under `modules_runtime/forever-bots/data/`. The deploy's rsync does **not** delete files already inside a module folder (`delete: false`), so data an operator
  added there survives a deploy;
- the module is told where to find them by **config keys** (the test and prod configs point `QuestFile`, `GrindFile`, `RotationFile` and `DungeonFile` at
  `<service folder>/modules_runtime/forever-bots/data/`), not by a path compiled in. Do the same: a data path is a setting with a documented default;
- a missing data file should mean "that feature is off" with a warning, as the bots module does ("without them the module runs without quests, grind spots and
  rotations"), not a crash;
- read data files at startup, and again on a config reload if you want them live; read them off the pump thread.

## module.json

The fields are in [docs/01](01-quick-start.md#3-modulejson). Rules that matter for packaging:

- The `name` equals the folder name and the executable name, and is `[A-Za-z0-9_-]+`.
- The `protocol_version` must equal the core's, and is the first thing to update when the SDK's protocol number moves.
- The `version` is the **only** place a module version is written; read it into the build with `string(JSON ... GET ... version)` as the stock modules do (see
  the quick start), so the binary, the manifest and `module list` agree.
- The hooks and the `tier2` block are the module's capabilities. They are hashed for approval; keep them equal to the `ModuleManifest` in `main.cpp`.

## Versioning

From forever-ops `docs/96-versioning.md` (project policy, not enforced by the core): every component is `0.MINOR.PATCH` until the owner declares it stable for
players (`1.0.0`).

| Bump | When |
|---|---|
| MINOR (`0.16.0` to `0.17.0`, PATCH back to 0) | a new feature, a changed behavior, a new setting or command, a protocol or layout change. Before 1.0 a breaking change is a MINOR bump. |
| PATCH (`0.16.1`) | a bug fix, a data update, a log or doc change that does not change what the component does |

- Bump **in the same commit as the change** (or the one that finishes it) and say the new version in the commit message. A change that touches only tests or
  docs bumps nothing.
- A module's version is informational: the core approves by capabilities, so a version bump never needs re-approval; a change of hooks or Tier 2 numbers does.
- The SDK's own version is in two places that must stay equal (`kSdkPackageVersion` and `project(VERSION)`); a module's is in one (`module.json`).
- The protocol number is separate: it is what the core and the module have to agree on. Bump a module's `protocol_version` when you adopt an SDK that bumped it
  (see the [README](../README.md#versioning-three-numbers)).
- Pointing a module at the SDK: the stock `CMakeLists.txt` fetches the SDK's `main` unless `FOREVER_MODULE_SDK_SOURCE` is set. Reproducible builds should
  set the variable (the project's pipeline does, to the SDK revision it is building) or pin `GIT_TAG` to a commit.

## How releases are built and packaged (project practice)

The pieces below are forever-ops, not the SDK; they are described so you know what a module has to look like to be packaged, and what is hard-wired.

- **`scripts/build-artifact.sh --channel test|main`** builds the core, the SDK and the modules from git worktrees of the channel's branch tips (`test` builds the
  `test` branches; `main` builds the core's `master` and the others' `main`, which is what prod runs). Before building it checks that the protocol number is the
  same in the SDK (`kProtocolVersion`), the core (`kModuleProtocolVersion`) and every module's `module.json`; if they differ, it refuses (`the protocol numbers differ;
  a core and a module built from these refuse each other at the handshake`). It builds the SDK and runs its tests (`ctest`), then for each module runs
  `cmake -S <module> -B ... -DFOREVER_MODULE_SDK_SOURCE=<sdk worktree> -DCMAKE_BUILD_TYPE=Release`, builds and runs that module's `ctest`. A module therefore needs a
  `enable_testing()` and test targets that pass without a core.
- **The artifact** is `/srv/forever/artifacts/<id>/` with this layout:

  ```
  bin/        worldserver, bnetserver
  lib/        the core's shared libraries
  scripts/
  modules/
    forever-bots/        forever-bots (0755), module.json (0644), data/
    auto-vendor-trash/   auto-vendor-trash (0755), module.json (0644)
  sql/        base, updates, custom
  config/     defaults for people to read and copy; NEVER deployed (see below)
  MANIFEST.json, SHA256SUMS
  ```

  So the packaged module layout is `modules/<name>/<name>` plus `modules/<name>/module.json` (plus its data), the same shape the core scans in
  `modules_runtime/<name>/`.
- **`config/`** (built by `scripts/lib/package-configs.sh`): `worldserver.conf(.dist)`, `bnetserver.conf(.dist)`, a README, and `modules.d/<module>.conf.example`
  copied from each module's `config/modules.d/*`. The script warns when a module has no `config/modules.d`. The deploy role never installs `config/`; the live
  config of each server is kept on the server (`ops-private`, `worldserver.conf.d/modules.d/*.conf`) and is not overwritten by a deploy.
- **Deploy**: Ansible rsyncs `modules/` into `<service folder>/modules_runtime/` (owner and mode set for the service user, existing files not deleted). It does
  not run `module approve`; an operator does, once, and again after a capability change. `approved_modules.json` lives next to `worldserver.conf` (it is in the
  server's `conf` folder, which the backup procedure lists), so it survives deploys.
- The Windows package has the same `config/` layout (forever-ops `docs/112-windows-package.md`).
- Deploys to prod run only from the `main` channel and need a confirmation; a push to a product repo's `main` or `test` branch starts the pipelines. Develop on
  a branch.

## Shipping your own module alongside the stock ones

**What the core needs** is only a folder `modules_runtime/<name>/` with the executable and `module.json`, the module approved, and optionally
`worldserver.conf.d/modules.d/<name>.conf`. Any number of modules can run side by side; they get separate sockets (`<Modules.Dir>/<name>.sock`), regions and
bot sets, and a bot belongs to one module at a time (a request for a bot another module controls is refused with `already controlled by module <name>`).

**By hand** (your own server, no pipeline): build the module against the SDK revision that matches your core, copy the executable and `module.json` into
`modules_runtime/<name>/`, copy its config, run `module approve <name>`, restart the worldserver.

**In the project's pipeline**: the module list is hard-wired. `scripts/build-artifact.sh` names `MOD-Forever-Bots` and `MOD-auto-vendor-trash` in `REPOS`, in the
build loop, in the protocol check, in the `mkdir`/`install` lines that make `modules/<name>/`, in the library check, and in `MANIFEST.json`'s `"modules"` list;
`jenkins/ci/build-modules.sh` and `jenkins/windows/modules.ps1` list them too. Adding a module to the packaged server means editing those places in forever-ops
(a repo entry, a build-and-test step, an `install` of its binary and `module.json`, a `package_configs` argument for its example config), and then it
is delivered like the others. I did not find a plugin mechanism that picks up extra modules without those edits. MOD-quest-group-loot is the existing
example of a module that is deliberately **not** part of the builds (forever-ops `docs/96-versioning.md`: "not part of the builds; left as it was").

**Checklist for a module repository**

1. `module.json` with `name`, `version`, `protocol_version`, hooks, tier2 numbers equal to the code's manifest.
2. `CMakeLists.txt` as in the quick start, executable named like the module, `enable_testing()` and tests that run without a core.
3. `config/modules.d/<name>.conf.example` with every key documented.
4. Pure logic and parsing in files with no SDK dependency, so they build in `ctest` anywhere ([docs/05](05-testing-and-debugging.md)).
5. A `README.md` with the module's purpose, its keys, and the `Bot.Actions.*` switches it needs on the core ([docs/03](03-tier2-snapshot-and-actions-reference.md)).
6. The version bumped with each change; the capabilities changed only deliberately, because they need a new approval.
