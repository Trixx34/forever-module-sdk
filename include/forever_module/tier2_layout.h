#pragma once

// Tier 2 shared-memory layout (per-tick bot-AI/combat decision loop).
// See doc/module-system/03-phase1-protocol-design.md (Tier 2 section) and
// doc/module-system/11-sdk-scope.md for design rationale.
//
// IMPORTANT: everything below must stay POD (no pointers, no STL containers) -- these structs are
// placed directly into a boost::interprocess::managed_shared_memory region and read/written from
// two separate processes. Variable-length arrays (bots[], nearby[], actions[]) are NOT declared as
// in-struct arrays here because their length is negotiated per-module at handshake time (see
// Tier2RegionHeader below) -- they are addressed by offset math from the region header at runtime.
// This header only declares the fixed-size per-entry shapes; the actual region layout/offset
// calculation is in the helpers at the end of this file, and Tier2Session reads it through them.
//
// Any change to these shapes is a breaking change: bump kTier2LayoutVersion, and bump
// kProtocolVersion (protocol_version.h) as well, since the layout is part of the wire contract.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace forever_module {

// Explicit layout version, checked by the module when it attaches to the region. Lets a mismatch
// be detected from the shared memory itself, independent of the Tier 1 handshake.
inline constexpr std::uint32_t kTier2LayoutVersion = 6;

// Small, fixed cap -- see doc/module-system/07-phase5-reference-module-design.md, this value is
// still open pending real measurement from a spell-casting test module.
inline constexpr std::size_t kMaxTrackedCooldowns = 8;

struct NearbyUnit {
    std::uint64_t guid = 0;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float pos_z = 0.0f;
    std::uint32_t health = 0;
    std::uint32_t max_health = 0;
    bool is_hostile = false;
    std::uint32_t level = 0;   // layout 4
    std::uint8_t elite = 0;    // 1 for an elite creature or boss (layout 4)
    std::uint8_t casting = 0;  // 1 while the unit is casting a spell (layout 4)
};

// What happened to a bot's action in the last action set the core applied. Reported in the next snapshot.
enum class BotActionStatus : std::uint8_t {
    kNone = 0,           // no action was applied for this bot yet
    kApplied = 1,        // the action was applied (a cast was accepted; it may still fail while it runs)
    kRefused = 2,        // refused for another reason (see the core log)
    kUnknownSpell = 3,   // the spell id does not exist
    kNotKnown = 4,       // the bot has not learned the spell
    kOutOfRange = 5,
    kOnCooldown = 6,
    kNotEnoughPower = 7,
    kNoLineOfSight = 8,
    kInvalidTarget = 9,  // no unit for the target reference, or it is not a valid target
    kNotOnline = 10,     // the bot was not in the world when the action ran
};

// One active spell cooldown of a bot. spell_id 0 marks an unused slot.
struct SpellCooldown {
    std::uint32_t spell_id = 0;
    std::uint32_t remaining_ms = 0;
};

struct BotSnapshot {
    std::uint64_t guid = 0;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float pos_z = 0.0f;
    float orientation = 0.0f;
    std::uint32_t health = 0;
    std::uint32_t max_health = 0;
    // Power of the bot's main resource: mana, rage or energy, given by power_type (layout 4; was mana in layout 3).
    std::uint32_t power = 0;
    std::uint32_t max_power = 0;
    std::uint64_t threat_target_guid = 0;
    // The bot's active cooldowns, closest to expiring first. Unused slots have spell_id 0.
    SpellCooldown spell_cooldowns[kMaxTrackedCooldowns] = {};

    // Combat state (layout 3). in_combat is 1 while the bot is in combat. target_distance is the distance in yards to
    // threat_target_guid's unit, or 0 when there is no target or it is not in the snapshot's view.
    std::uint8_t in_combat = 0;
    float target_distance = 0.0f;

    // Result of the last action set the core applied for this bot (layout 3): the action type, its status, and the spell
    // for a cast. A module reads this in the next snapshot to see what happened.
    std::uint8_t last_action_type = 0;   // ActionType
    BotActionStatus last_action_status = BotActionStatus::kNone;
    std::uint32_t last_action_spell = 0;
    std::uint32_t last_action_sequence = 0; // counts the actions applied to this bot; a new result has a new sequence

    // Layout 4: the bot's level and the type of its main power (the Powers value: 0 mana, 1 rage, 3 energy).
    std::uint32_t level = 0;
    std::uint8_t power_type = 0;
    std::uint32_t nearby_count = 0;
    // Layout 5: the bot's party (M8). group_role is 0 for no party, 1 for a member, 2 for the leader. group_leader_guid is the
    // unit reference of the party leader (0 with no party). group_invite_from is the unit reference of the leader whose invite
    // the bot has not answered (0 when none). group_size is the number of members in the bot's party.
    std::uint8_t group_role = 0;
    std::uint32_t group_size = 0;
    std::uint64_t group_leader_guid = 0;
    std::uint64_t group_invite_from = 0;
    // Layout 6: the nearest vendor to the bot on its own map (M6), from the core's creature spawns. vendor_valid is 0 when
    // the map has no vendor. The position is in the bot's map, so a walk to it stays on that map.
    float vendor_x = 0.0f;
    float vendor_y = 0.0f;
    float vendor_z = 0.0f;
    std::uint8_t vendor_valid = 0;
    // NearbyUnit entries for this bot live in the region's flexible nearby-units area,
    // offset-addressed using Tier2RegionHeader::max_nearby_units; Tier2Session exposes them through NearbyOf().
};

// WorldTickSnapshot is a conceptual view over the region, not a single fixed struct -- its bots[]
// array length is max_bots from the header. Tier2Session exposes it as WorldTickSnapshotView
// rather than exposing raw offset math to module authors.
struct WorldTickSnapshotHeader {
    std::uint64_t tick_number = 0;
    std::uint32_t bot_count = 0;
};

enum class ActionType : std::uint32_t {
    kCastSpell = 0,
    kMoveTo = 1,
    kAttackTarget = 2,
    kFollow = 3,
    kStop = 4,
    kRest = 5,   // spell_id 1 sits down to rest, 0 stands up. Refused while the bot is in combat (M2).
    kVendor = 6, // services at an NPC within interaction range (M3). spell_id is a bitmask: 1 sell grey items to a vendor,
                 // 2 repair at a repairman, 4 equip the best gear in the bags. Refused in combat.
    kTrain = 7,  // learn every spell the trainer in interaction range teaches this bot now (M4). Refused in combat.
    kGroup = 8,  // party actions (M8). spell_id 1 invites the bot named by target_guid, 2 accepts the bot's pending invite,
                 // 3 leaves the bot's party. Refused in combat.
};

// A unit as one 64-bit value (BotAction::target_guid, NearbyUnit::guid). See MakeUnitRef.
inline constexpr std::uint64_t kUnitRefCounterMask = 0xFFFFFFFFull;
inline constexpr std::uint64_t kUnitRefEntryMask = 0xFFFFFFull;

// Packs a unit reference: HighGuid type in bits 56-63, creature/object entry in bits 32-55, counter in bits 0-31.
// Counters above 32 bits and entries above 24 bits cannot be packed; the core refuses those units. Type 2 is a
// player (entry 0, counter = the player's database guid). Type 8 is a creature (entry, counter as on the map).
// A reference of 0 means no unit, or the bot itself for a spell target.
constexpr std::uint64_t MakeUnitRef(std::uint8_t highType, std::uint32_t entry, std::uint64_t counter)
{
    return (std::uint64_t(highType) << 56) | ((std::uint64_t(entry) & kUnitRefEntryMask) << 32) |
           (counter & kUnitRefCounterMask);
}

constexpr std::uint8_t UnitRefType(std::uint64_t ref)
{
    return std::uint8_t(ref >> 56);
}

constexpr std::uint32_t UnitRefEntry(std::uint64_t ref)
{
    return std::uint32_t((ref >> 32) & kUnitRefEntryMask);
}

constexpr std::uint64_t UnitRefCounter(std::uint64_t ref)
{
    return ref & kUnitRefCounterMask;
}

struct BotAction {
    std::uint64_t bot_guid = 0;
    ActionType type = ActionType::kStop;
    std::uint32_t spell_id = 0;
    std::uint64_t target_guid = 0; // unit reference (MakeUnitRef); 0 = self for kCastSpell
    float dest_x = 0.0f;
    float dest_y = 0.0f;
    float dest_z = 0.0f;
};

// ActionQueue is likewise a conceptual view -- actions[] length is max_actions from the header.
struct ActionQueueHeader {
    std::uint64_t tick_number = 0; // which snapshot this responds to (may lag, see doc/module-system/03)
    std::uint32_t action_count = 0;
};

// Lives at the start of the shared-memory region. Bounds are negotiated at handshake
// (HandshakeResponse.negotiated_max_*, which may be lower than HandshakeRequest.requested_max_*)
// and fixed for the lifetime of the module's Tier 2 session.
struct Tier2RegionHeader {
    std::uint32_t layout_version = kTier2LayoutVersion;
    std::uint32_t max_bots = 0;
    std::uint32_t max_nearby_units = 0;
    std::uint32_t max_actions = 0;
    std::uint32_t tick_interval = 1;

    std::atomic<std::uint64_t> published_snapshot_tick{0};
    std::atomic<std::uint8_t> published_snapshot_buffer{0}; // 0 or 1

    std::atomic<std::uint64_t> published_action_tick{0};
    std::atomic<std::uint8_t> published_action_buffer{0}; // 0 or 1

    // Followed in memory by, each sized from the bounds above (offsets: see the helpers below):
    //   WorldTickSnapshotHeader + BotSnapshot[max_bots] + NearbyUnit[max_bots * max_nearby_units], x2 buffers
    //   ActionQueueHeader + BotAction[max_actions], x2 buffers
};

// Compile-time checks on the parts of the layout that would silently break across processes.
// Atomics in shared memory must be lock-free, or the two processes can't safely share them.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "published_*_tick must be lock-free across processes");
static_assert(std::atomic<std::uint8_t>::is_always_lock_free, "published_*_buffer must be lock-free across processes");

static_assert(std::is_standard_layout_v<NearbyUnit> && std::is_trivially_copyable_v<NearbyUnit>,
              "NearbyUnit must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<BotSnapshot> && std::is_trivially_copyable_v<BotSnapshot>,
              "BotSnapshot must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<WorldTickSnapshotHeader> && std::is_trivially_copyable_v<WorldTickSnapshotHeader>,
              "WorldTickSnapshotHeader must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<BotAction> && std::is_trivially_copyable_v<BotAction>,
              "BotAction must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<ActionQueueHeader> && std::is_trivially_copyable_v<ActionQueueHeader>,
              "ActionQueueHeader must be POD for shared-memory use");

static_assert(sizeof(bool) == 1, "NearbyUnit::is_hostile assumes a 1-byte bool");

// Region arithmetic. Core creates the region with these sizes and the module maps it with the same
// ones; keep this block identical to the core's ModuleTier2Layout.h copy.
namespace tier2_detail
{
constexpr std::size_t kAlign = alignof(std::max_align_t);

constexpr std::size_t AlignUp(std::size_t value)
{
    return (value + kAlign - 1) / kAlign * kAlign;
}
} // namespace tier2_detail

// Byte offsets from the start of the region. Snapshot buffers come first, then action buffers.
constexpr std::size_t Tier2HeaderBytes()
{
    return tier2_detail::AlignUp(sizeof(Tier2RegionHeader));
}

constexpr std::size_t Tier2SnapshotBufferBytes(std::uint32_t maxBots, std::uint32_t maxNearbyUnits)
{
    return tier2_detail::AlignUp(sizeof(WorldTickSnapshotHeader) +
                                 std::size_t(maxBots) * sizeof(BotSnapshot) +
                                 std::size_t(maxBots) * maxNearbyUnits * sizeof(NearbyUnit));
}

constexpr std::size_t Tier2ActionBufferBytes(std::uint32_t maxActions)
{
    return tier2_detail::AlignUp(sizeof(ActionQueueHeader) + std::size_t(maxActions) * sizeof(BotAction));
}

constexpr std::size_t Tier2RegionBytes(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint32_t maxActions)
{
    return Tier2HeaderBytes() + 2 * Tier2SnapshotBufferBytes(maxBots, maxNearbyUnits) + 2 * Tier2ActionBufferBytes(maxActions);
}

constexpr std::size_t Tier2SnapshotBufferOffset(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint8_t buffer)
{
    return Tier2HeaderBytes() + buffer * Tier2SnapshotBufferBytes(maxBots, maxNearbyUnits);
}

constexpr std::size_t Tier2ActionBufferOffset(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint32_t maxActions,
                                              std::uint8_t buffer)
{
    return Tier2HeaderBytes() + 2 * Tier2SnapshotBufferBytes(maxBots, maxNearbyUnits) +
           buffer * Tier2ActionBufferBytes(maxActions);
}

} // namespace forever_module
