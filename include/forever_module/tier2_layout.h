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
// calculation is implementation work for Tier2Session (TODO).

#include <cstdint>
#include <atomic>

namespace forever_module {

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
};

struct BotSnapshot {
    std::uint64_t guid = 0;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float pos_z = 0.0f;
    float orientation = 0.0f;
    std::uint32_t health = 0;
    std::uint32_t max_health = 0;
    std::uint32_t mana = 0;
    std::uint32_t max_mana = 0;
    std::uint64_t threat_target_guid = 0;
    std::uint32_t spell_cooldowns[kMaxTrackedCooldowns] = {}; // ms remaining
    std::uint32_t nearby_count = 0;
    // NearbyUnit entries for this bot live in the region's flexible nearby-units area,
    // offset-addressed using Tier2RegionHeader::max_nearby_units -- TODO: accessor helpers.
};

// WorldTickSnapshot is a conceptual view over the region, not a single fixed struct -- its bots[]
// array length is max_bots from the header. TODO: provide a typed view/accessor in Tier2Session
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
};

struct BotAction {
    std::uint64_t bot_guid = 0;
    ActionType type = ActionType::kStop;
    std::uint32_t spell_id = 0;
    std::uint64_t target_guid = 0;
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
// (HandshakeRequest.requested_max_*) and fixed for the lifetime of the module's Tier 2 session.
struct Tier2RegionHeader {
    std::uint32_t max_bots = 0;
    std::uint32_t max_nearby_units = 0;
    std::uint32_t max_actions = 0;
    std::uint32_t tick_interval = 1;

    std::atomic<std::uint64_t> published_snapshot_tick{0};
    std::atomic<std::uint8_t> published_snapshot_buffer{0}; // 0 or 1

    std::atomic<std::uint64_t> published_action_tick{0};
    std::atomic<std::uint8_t> published_action_buffer{0}; // 0 or 1

    // Followed in memory by, each sized from the bounds above (TODO: offset calculation helpers):
    //   WorldTickSnapshotHeader + BotSnapshot[max_bots] + NearbyUnit[max_bots * max_nearby_units], x2 buffers
    //   ActionQueueHeader + BotAction[max_actions], x2 buffers
};

} // namespace forever_module
