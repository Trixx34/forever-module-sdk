# 6. Worked example: bots follow the party leader and fight what is near

This guide builds a small but real policy on the Tier 2 API: **a bot that is a party member follows its leader, and fights a hostile unit that comes close.**
It shows the shape a policy should have (pure function, small memory per bot, results read back, no repeated orders) and tests it without a core.

**The code in this guide is not compiled by CI or by the author.** It was checked by reading against `tier2_layout.h`, `tier2_session.h` and `module_client.h`.
Put it in your own repository, not in this one: the SDK's CMake does not reference it.

## What the snapshot gives us, and what it does not

Used fields (all from [docs/03](03-tier2-snapshot-and-actions-reference.md)):

- `group_role` (1 = member) and `group_leader_guid` (a unit reference): who to follow. The leader's **position** is not a snapshot field; we hand the leader's reference
  to `kFollow` and the core does the walking.
- `in_combat`, `threat_target_guid`: whether the bot is fighting and what its target is.
- `NearbyOf(i)`: `guid`, position, `health`, `is_hostile`, `level`, `elite` of units around the bot.
- `life_state`, `casting_spell`, `level`, `pos_x/y/z`, `last_action_*` for state and results.

What it does **not** give us: the party leader's current target. A "assist the leader" rule cannot be written from the snapshot alone (the only place a player's
selected target reaches a module is `PlayerChatEvent.target_ref`, in a chat hook). So this policy uses a simpler rule: fight the bot's own threat target, else the
nearest hostile in range. If you need "attack the leader's target" you must obtain that reference some other way and pass it as `target_guid`.

## The policy

`follow_policy.h`, pure, with the time passed in as the snapshot's `tick_number`:

```cpp
#pragma once

#include <cmath>
#include <cstdint>
#include <iterator>
#include <span>
#include <unordered_map>

#include "forever_module/tier2_layout.h"

namespace follow {

namespace fm = forever_module;

constexpr float kEngageYards = 20.0f;          // a hostile unit this close is fought
constexpr std::uint32_t kMaxLevelAbove = 2;    // not above the bot's level + this
constexpr std::uint64_t kReissueTicks = 20;    // repeat an attack order no more often than this
constexpr std::uint64_t kRetryTicks = 40;      // wait this long after a refusal

// What the policy remembers about one bot between ticks.
struct BotMemory
{
    enum class Mode { kIdle, kFollowing, kFighting };

    Mode mode = Mode::kIdle;
    std::uint64_t followTarget = 0;
    std::uint64_t attackTarget = 0;
    std::uint64_t lastOrderTick = 0;
    std::uint64_t holdUntilTick = 0;                  // no follow order before this tick
    std::uint32_t seenSequence = 0;                   // last_action_sequence already read
    std::unordered_map<std::uint64_t, std::uint64_t> avoid; // unit -> tick until which it is not chosen again
};

inline float Distance(fm::BotSnapshot const& bot, fm::NearbyUnit const& unit)
{
    float const dx = unit.pos_x - bot.pos_x;
    float const dy = unit.pos_y - bot.pos_y;
    float const dz = unit.pos_z - bot.pos_z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The core reports the result of the last action it applied to the bot. Read each result once and react to a refusal:
// a target the core would not attack is left alone for a while, and a follow that failed is retried later.
inline void ReadResult(fm::BotSnapshot const& bot, BotMemory& memory, std::uint64_t tick)
{
    if (bot.last_action_sequence == 0 || bot.last_action_sequence == memory.seenSequence)
        return;
    memory.seenSequence = bot.last_action_sequence;
    if (bot.last_action_status == fm::BotActionStatus::kApplied)
        return;

    auto const type = static_cast<fm::ActionType>(bot.last_action_type);
    if (type == fm::ActionType::kAttackTarget)
    {
        memory.avoid[memory.attackTarget] = tick + kRetryTicks;
        memory.attackTarget = 0;
        memory.mode = BotMemory::Mode::kIdle;
    }
    else if (type == fm::ActionType::kFollow)
    {
        memory.mode = BotMemory::Mode::kIdle;
        memory.holdUntilTick = tick + kRetryTicks;
    }
}

// The nearest living, non-elite hostile unit inside kEngageYards that is not above the bot's level by more than kMaxLevelAbove.
inline fm::NearbyUnit const* PickTarget(fm::BotSnapshot const& bot, std::span<fm::NearbyUnit const> nearby,
                                        BotMemory& memory, std::uint64_t tick)
{
    for (auto it = memory.avoid.begin(); it != memory.avoid.end();)
        it = tick >= it->second ? memory.avoid.erase(it) : std::next(it);

    fm::NearbyUnit const* best = nullptr;
    float bestDistance = kEngageYards;
    for (fm::NearbyUnit const& unit : nearby)
    {
        if (!unit.is_hostile || unit.health == 0 || unit.elite)
            continue;
        if (unit.level > bot.level + kMaxLevelAbove)
            continue;
        if (memory.avoid.contains(unit.guid))
            continue;
        float const distance = Distance(bot, unit);
        if (distance < bestDistance)
        {
            best = &unit;
            bestDistance = distance;
        }
    }
    return best;
}

// Decides at most one action for the bot on this snapshot tick. Returns true and fills `out` when there is one.
inline bool Decide(fm::BotSnapshot const& bot, std::span<fm::NearbyUnit const> nearby, BotMemory& memory,
                   std::uint64_t tick, fm::BotAction& out)
{
    ReadResult(bot, memory, tick);

    // A dead or ghost bot, or one in the middle of a cast, gets no order (a real policy would release the spirit and reclaim the corpse).
    if (bot.life_state != 0 || bot.casting_spell != 0)
        return false;

    // 1. Fight: the bot's own target while it is in combat, else the nearest fair target.
    std::uint64_t target = 0;
    if (bot.in_combat && bot.threat_target_guid != 0)
        target = bot.threat_target_guid;
    else if (fm::NearbyUnit const* unit = PickTarget(bot, nearby, memory, tick))
        target = unit->guid;

    if (target != 0)
    {
        bool const changed = target != memory.attackTarget || memory.mode != BotMemory::Mode::kFighting;
        if (!changed && tick - memory.lastOrderTick < kReissueTicks)
            return false;                       // already fighting it; do not re-order every tick
        memory.mode = BotMemory::Mode::kFighting;
        memory.attackTarget = target;
        memory.lastOrderTick = tick;
        out = fm::BotAction{ bot.guid, fm::ActionType::kAttackTarget, 0, target, 0.0f, 0.0f, 0.0f };
        return true;
    }
    memory.attackTarget = 0;

    // 2. Follow the party leader. Only a member follows (group_role 1); the leader (2) and a bot with no party (0) do not.
    if (bot.group_role != 1 || bot.group_leader_guid == 0)
    {
        memory.mode = BotMemory::Mode::kIdle;
        return false;
    }
    if (tick < memory.holdUntilTick)
        return false;
    if (memory.mode == BotMemory::Mode::kFollowing && memory.followTarget == bot.group_leader_guid)
        return false;                           // already following this leader

    memory.mode = BotMemory::Mode::kFollowing;
    memory.followTarget = bot.group_leader_guid;
    memory.lastOrderTick = tick;
    out = fm::BotAction{ bot.guid, fm::ActionType::kFollow, 0, bot.group_leader_guid, 0.0f, 0.0f, 0.0f };
    return true;
}

} // namespace follow
```

Why it is built this way:

- **One action per bot per tick, at most.** The core remembers only the result of the last action it applied per bot, and a bot's result is what `ReadResult` reacts to.
- **No repeated orders.** `kFollow` and `kAttackTarget` both reset the bot's movement when applied (the core clears the motion master before `MoveFollow`; an attack on a
  target already being meleed is simply `kApplied`). So a follow is sent once per leader, and an attack is renewed only every `kReissueTicks`.
  When the fight ends, `memory.mode` is still `kFighting`, which is why the follow rule sends a fresh `kFollow` afterwards.
- **A refusal is information.** `kInvalidTarget` or `kRefused` for an attack means the core would not attack that unit (friendly, a critter, not attackable, ...): it is put
  on a short avoid list instead of being retried every tick. A failed follow waits `kRetryTicks`.
- **Filters on the nearby list** use real fields: `is_hostile`, `health`, `elite`, `level`. The list is limited to the negotiated `max_nearby_units` and can be several ticks old
  (docs/02), so a unit may already be dead; the core refuses an attack on a dead unit and the avoid list absorbs that.
- It does not depend on the connection, the clock or the SDK library, only on `tier2_layout.h`, so it unit-tests with a plain `main`.

## Wiring it into the pump

In the quick start's timer callback ([docs/01](01-quick-start.md#4-maincpp)), replace the `Walker` loop with:

```cpp
#include "follow_policy.h"
// ...
std::unordered_map<std::uint64_t, follow::BotMemory> memory;
// ... inside the pump callback, after a new snapshot tick was detected:
actions.clear();
for (std::size_t i = 0; i < view.bots.size(); ++i)
{
    fm::BotSnapshot const& bot = view.bots[i];
    fm::BotAction action;
    if (follow::Decide(bot, view.NearbyOf(i), memory[bot.guid], view.tick_number, action))
        actions.push_back(action);
}
// forget bots that are no longer ours (control revoked): erase their entries from `memory`
```

`view.NearbyOf(i)` returns `std::span<NearbyUnit const>`, exactly what `Decide` takes. `PublishActions` is called once with `actions` if it is not empty, as before. The manifest
needs `requested_max_nearby_units` of at least a few (the quick start asks for 4; 16 is the stock bots module's number) and `requested_max_actions` at least the bot count, in the
code **and** in `module.json`.

## Forming a party (sketch, not run)

The follower rule needs bots in a party. The stock bots module forms parties with `kGroup`: the leader invites, each member accepts, on later ticks (invite and accept are
refused in combat, and a pending invite shows in the target's `group_invite_from`). In terms of the API:

```cpp
// tick N: the leader (a bot this module controls) invites a member. The target is a unit reference of the other bot (type 2 = player, entry 0, counter = its guid).
BotAction{ leader.guid, ActionType::kGroup, 1, MakeUnitRef(2, 0, member.guid), 0, 0, 0 };
// tick N+k: the member sees group_invite_from != 0 and accepts.
BotAction{ member.guid, ActionType::kGroup, 2, 0, 0, 0, 0 };
```

The core's checks for the invite are: the target must be a player unit that is a bot this module controls, not already in a party or invited, same team; the inviter must be the
leader (it starts a party if it has none), and the party must not be full. After the accept, `group_role` becomes 1 for the member and 2 for the leader, and the follow rule
starts. That `MakeUnitRef(2, 0, guid)` is the right form for a bot is inferred from the header's description and the core's reason texts; it was not run.

## Testing it without a core

```cpp
#include <cstdio>
#include <vector>
#include "follow_policy.h"

namespace fm = forever_module;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static fm::NearbyUnit Mob(std::uint64_t guid, float x, std::uint32_t level)
{
    fm::NearbyUnit unit;
    unit.guid = guid; unit.pos_x = x; unit.health = 100; unit.max_health = 100; unit.is_hostile = true; unit.level = level;
    return unit;
}

int main()
{
    fm::BotSnapshot bot;
    bot.guid = 7; bot.level = 10; bot.group_role = 1; bot.group_leader_guid = fm::MakeUnitRef(2, 0, 99);
    follow::BotMemory memory;
    fm::BotAction action;

    // A party member with nothing around follows the leader, once.
    CHECK(follow::Decide(bot, {}, memory, 100, action) && action.type == fm::ActionType::kFollow && action.target_guid == bot.group_leader_guid);
    CHECK(!follow::Decide(bot, {}, memory, 101, action));                     // not repeated

    // A fair hostile unit 10 yards away is attacked; a unit three levels above is not.
    std::vector<fm::NearbyUnit> mobs = { Mob(1, 10.0f, 11), Mob(2, 5.0f, 14) };
    CHECK(follow::Decide(bot, mobs, memory, 102, action) && action.type == fm::ActionType::kAttackTarget && action.target_guid == 1);

    // The core refused the attack: the unit is avoided for a while, and the bot goes back to following.
    bot.last_action_type = static_cast<std::uint8_t>(fm::ActionType::kAttackTarget);
    bot.last_action_status = fm::BotActionStatus::kInvalidTarget;
    bot.last_action_sequence = 5;
    CHECK(follow::Decide(bot, mobs, memory, 103, action) && action.type == fm::ActionType::kFollow);

    return failures == 0 ? 0 : 1;
}
```

Link this against nothing but the SDK's include directory (or `forever_module_sdk`, which supplies it), register it with `add_test`, and run it with `ctest`.

## Where to go from here

- Rest when out of combat and hurt: `kRest` with `spell_id` 1 sits (refused in combat) and 0 stands up. Skip it while the bot follows (sitting stops it).
- Cast: `kCastSpell` with `spell_id` and `target_guid`; read `spell_cooldowns[]`, `power`, `casting_spell` and the status (`kOnCooldown`, `kNotEnoughPower`, `kMoving`, `kBusy`) before the next.
- Loot: after a kill, `kLoot` with the corpse's unit reference (within 30 yards).
- Supplies, vendors, quests, trainers, travel: the stock bots module (`src/supply_policy.cpp`, `src/errand_policy.cpp`, `src/quest_planner.cpp`, `src/taxi_policy.cpp`) is the
  reference, each as a pure policy with its own `*_test.cpp`.
