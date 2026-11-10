// SPDX-License-Identifier: GPL-2.0-or-later
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
inline constexpr std::uint32_t kTier2LayoutVersion = 25;

// Layout 25, the growth rule. From this layout on, a change to the shared shapes is one of two kinds:
//   - an APPEND: new fields at the end of a struct (its old fields keep their offsets), a new array at the end of the snapshot
//     buffer (a new Tier2Section kind), or new enum values. kTier2LayoutVersion goes up; kTier2LayoutBase stays.
//   - a BREAK: a field moved, removed, widened, or an array in the middle of a struct resized. Both numbers go up to the new layout.
// The core writes both into Tier2RegionHeader. A module built against layout M reads a region of layout N when
// kTier2LayoutBase(N) <= M <= N: it reads each section with the element stride the section table gives (the core's sizeof), so
// the tail it does not know is skipped, and a section it does not know is ignored. A region older than the module (N < M)
// is refused: the module would read fields the core never wrote. kProtocolVersion is still bumped with every layout change
// (the Tier 1 handshake refuses a mismatch; relaxing that is a separate decision). The procedure for adding a field is in
// forever-ops docs/122-layout-25-design.md.
inline constexpr std::uint32_t kTier2LayoutBase = 25;

// Small, fixed cap -- see doc/module-system/07-phase5-reference-module-design.md, this value is
// still open pending real measurement from a spell-casting test module.
// Layout 24: 16 slots (was 8), for rotations with many abilities.
inline constexpr std::size_t kMaxTrackedCooldowns = 16;

// Layout 24: one aura the bot itself put on a unit (a DOT, a debuff, a CC). spell_id 0 marks an unused slot. remaining_ms is 0xFFFFFFFF for an aura with no duration.
inline constexpr std::size_t kMaxOwnDebuffs = 4;
struct OwnDebuff {
    std::uint32_t spell_id = 0;
    std::uint32_t remaining_ms = 0;
};

// Layout 24: bits of NearbyUnit::state_flags.
enum NearbyStateFlag : std::uint32_t {
    kNearbyCcLocked = 1u << 0,        // a crowd-control aura holds it (stun, fear, sheep, root, sleep...)
    kNearbyFleeing = 1u << 1,
    kNearbyTauntImmune = 1u << 2,
    kNearbyStunImmune = 1u << 3,
    kNearbyHasMagicBuff = 1u << 4,    // a dispellable magic buff (purge, tranquilizing shot)
    kNearbyEnraged = 1u << 5,
    kNearbySnared = 1u << 6,
    kNearbyEvading = 1u << 7,
    kNearbyInLosOfBot = 1u << 8,      // only meaningful with kNearbyLosKnown
    kNearbyLosKnown = 1u << 9,        // the core computed line of sight for this unit (instance bots, hostile units within 40 yd); else bit 8 is 0 and means unknown
    kNearbyPetOrGuardian = 1u << 10,
    kNearbyTotem = 1u << 11,
};

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
    // Layout 13: auras on this unit. aura_mask is a coarse summary the core encodes so a module can tell
    // "has a DOT" / "has a poison" without filling a per-unit aura array; aura_count is the number of
    // auras the core chose to report on it. Mask bit assignment is core-defined and fixed per layout.
    std::uint32_t aura_count = 0;
    std::uint32_t aura_mask = 0;
    // Layout 23: a player unit's best equipped item level (0 for a creature, or unknown), and 1 when the player is on an active quest the bot
    // is on too (shared_objective; the bot's quest log is in its snapshot). Both are 0 for a unit that is not a player.
    std::uint32_t item_level = 0;
    std::uint8_t shared_objective = 0;
    // Layout 24: who the unit is fighting, what it casts, what kind of unit it is. All 0 when unknown or not filled (the core's Bot.Snapshot.* switches).
    // victim_ref: the unit it is attacking (a unit reference, as guid), 0 for none. threat_top_ref: the unit at the top of its threat list (0 for none or
    // when Bot.Snapshot.Threat is off or the bot is not in a dungeon or raid). bot_threat_pct: the bot's threat as a percentage of the top, 255 when the bot
    // is on top (or 100% and above); top_is_tank: threat_top_ref is a member of the bot's group with a tank role.
    std::uint64_t victim_ref = 0;
    std::uint64_t threat_top_ref = 0;
    std::uint32_t cast_spell_id = 0;      // the spell it is casting or channeling, 0 for none
    std::uint32_t cast_remaining_ms = 0;
    std::uint32_t state_flags = 0;        // NearbyStateFlag
    float orientation = 0.0f;
    std::uint8_t bot_threat_pct = 0;
    std::uint8_t top_is_tank = 0;
    std::uint8_t cast_interruptible = 0;
    std::uint8_t creature_type = 0;       // CreatureType (1 beast, 2 dragonkin, 3 demon, 4 elemental, 5 giant, 6 undead, 7 humanoid ...), 0 for a player or unknown
    std::uint8_t creature_rank = 0;       // 0 normal, 1 elite, 2 rare elite, 3 boss, 4 rare
    std::uint8_t raid_mark = 0;           // the raid target icon on it, 1..8, 0 for none
    OwnDebuff my_debuffs[kMaxOwnDebuffs] = {};
    // Layout 25: how close the bot may come before a living creature that can be fought attacks on its own (Creature::GetAggroRange for this bot,
    // in yards; 0 for a player, a dead or friendly unit, or when Bot.Snapshot.AggroRadius is off in the core). Pull spots and patrol waits.
    float aggro_radius = 0.0f;
    std::uint32_t reserved25 = 0;
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
    kBusy = 11,          // a cast: the bot is already casting; try again when it is done (layout 10)
    kMoving = 12,        // a cast: the spell has a cast time and the bot is moving; stop first (layout 10)
    kNoRoute = 13,       // a move: the navmesh has no full route to the destination, so the bot walks to the nearest reachable point
                         // and stops there (the move is still applied). Only sent for kMoveTo.
    kNeedsFocus = 14,    // SDK 0.18: a craft (kSkillUp, kCraft) or a cast needs a spell focus object (an anvil, a forge, a cooking fire) within reach and
                         // there is none: walk to one and try again. Distinct from kRefused (no recipe, no materials), which no walk will cure.
};

// Layout 21: one harmful-looking thing on the ground near a bot: a dynamic object (a spell's persistent area, such as Flamestrike or Blizzard) or an area trigger. The
// core lists the ones within kGroundEffectRangeYards of the bot, the one the bot stands in or nearest to first, up to kMaxGroundEffects. radius is the effect's radius in
// yards (0: not known). kind: 1 dynamic object, 2 area trigger. hostile is 1 when the effect's caster is hostile to the bot (a creature's spell, not a party member's).
// spell_id 0 marks an unused slot. Only filled when Bot.Snapshot.GroundEffects is on in the core. Layout 25: 8 slots (was 4).
inline constexpr std::size_t kMaxGroundEffects = 8;
inline constexpr float kGroundEffectRangeYards = 30.0f;
struct GroundEffect {
    std::uint32_t spell_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = 0.0f;
    std::uint8_t kind = 0;
    std::uint8_t hostile = 0;
    std::uint16_t reserved = 0;
};

// One active spell cooldown of a bot. spell_id 0 marks an unused slot.
struct SpellCooldown {
    std::uint32_t spell_id = 0;
    std::uint32_t remaining_ms = 0;
};

// Layout 13: one aura on a bot (a buff, debuff, DOT or poison the core reports). remaining_ms is the time
// left; an aura with no duration uses a large sentinel (core's choice, e.g. 0xFFFFFFFF). stacks is the
// stack count (1 when it does not stack). spell_id 0 marks an unused slot.
inline constexpr std::size_t kMaxAuras = 16;

struct AuraEntry {
    std::uint32_t spell_id = 0;
    std::uint32_t remaining_ms = 0;
    std::uint32_t stacks = 0;
};

// Layout 8: one quest in a bot's quest log (M5). status: 1 incomplete, 2 complete (ready to hand in), 3 failed. counts[i] is the
// progress of the quest's i-th objective (a kill count or an item count), 0 for unused objectives.
inline constexpr std::size_t kMaxQuestLog = 8;
inline constexpr std::size_t kMaxQuestObjectives = 4;
struct QuestEntry {
    std::uint32_t quest_id = 0;   // 0 marks an unused slot
    std::uint8_t status = 0;
    std::uint8_t reserved = 0;
    std::uint16_t counts[kMaxQuestObjectives] = {};
};

// Layout 22: one hit the bot took. source_guid is the attacker's unit reference (0 for an environmental source, or one that cannot be packed).
// spell_id is the spell that dealt the damage (0 for a melee or auto attack). amount is the damage as the core applied it. sequence counts
// every hit on the bot since it came online, so the newest hit has the highest sequence.
inline constexpr std::size_t kMaxDamageHits = 8;
struct DamageHit {
    std::uint64_t source_guid = 0;
    std::uint32_t spell_id = 0;
    std::uint32_t amount = 0;
    std::uint32_t sequence = 0;
};

// Layout 24: the result of one applied action, for the ring in BotSnapshot::results. type is the ActionType, status a BotActionStatus (as its integer value).
inline constexpr std::size_t kMaxActionResults = 4;
inline constexpr std::size_t kMaxTargetAuras = 8;
struct ActionResult {
    std::uint8_t type = 0;
    std::uint8_t status = 0;
    std::uint16_t reserved = 0;
    std::uint32_t spell_id = 0;
    std::uint32_t sequence = 0;
};

// Layout 25: an enemy telegraph: a hostile cast in progress, near the bot, whose spell hits an area or a cone, so the bot can step out of it (or kick it)
// before it lands. The core lists up to kMaxTelegraphs of them per bot, the soonest to land first, taken from the casting units of the bot's nearby list.
// kind: 1 a circle of `radius` yards around (x, y, z), the cast's destination; 2 a cone from the caster at (x, y, z), `radius` yards long, `cone_angle`
// degrees wide (the full angle, 0 = the spell system's default of 90) around the caster's `facing`; 3 a circle of `radius` yards around the caster
// itself. interruptible is the NearbyUnit cast_interruptible of the caster. spell_id 0 marks an unused slot. Only filled when Bot.Snapshot.Telegraphs is
// on in the core.
inline constexpr std::size_t kMaxTelegraphs = 4;
struct Telegraph {
    std::uint64_t caster_ref = 0;
    std::uint32_t spell_id = 0;
    std::uint32_t cast_remaining_ms = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = 0.0f;
    float cone_angle = 0.0f;
    float facing = 0.0f;
    std::uint8_t kind = 0;
    std::uint8_t interruptible = 0;
    std::uint16_t reserved = 0;
    std::uint32_t reserved2 = 0;
};

// Layout 25: the answer to a kProbePoint action (the position probe service, plan Phase 5.3): what the core knows about a point the module named.
// probe_id is the action's spell_id (the module's own number for the probe, echoed back); (x, y, z) the point as asked. flags: ProbeFlag bits.
// ground_z is the ground height under the point (with kProbeGroundKnown); path_yards the length of the route from where the bot stood (with
// kProbeReachable or kProbePartialRoute). sequence is the action sequence the probe was answered with (0 marks an unused slot). The bot's snapshot
// keeps the last kMaxProbeResults answers, oldest first (BotSnapshot::probes); probe_sequence is the newest.
inline constexpr std::size_t kMaxProbeResults = 4;
enum ProbeFlag : std::uint32_t {
    kProbeReachable = 1u << 0,      // the navmesh has a full route from the bot to the point
    kProbePartialRoute = 1u << 1,   // a route exists but stops short of the point (path_yards is the part that exists)
    kProbeLosToTarget = 1u << 2,    // the point is in line of sight of the unit target_guid named (only with kProbeLosKnown)
    kProbeLosKnown = 1u << 3,       // a target was named and found, so bit 2 is an answer
    kProbeInLiquid = 1u << 4,       // the point is in water (or another liquid)
    kProbeGroundKnown = 1u << 5,    // ground_z is set
};
struct ProbeResult {
    std::uint32_t probe_id = 0;
    std::uint32_t flags = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float ground_z = 0.0f;
    float path_yards = 0.0f;
    std::uint32_t sequence = 0;
};

// Layout 25: the party view, one block per group shared by every bot of the group (BotSnapshot::party_view_index), not copied into each bot. Its
// members live in one pool of PartyMember entries for the whole snapshot (PartyView::member_first, member_count). Only built when Bot.Snapshot.PartyView
// is on in the core; a bot whose group was not built (switch off, or more groups than the region has slots for) has party_view_index == kNoPartyView.
inline constexpr std::uint32_t kNoPartyView = 0xFFFFFFFFu;
inline constexpr std::size_t kMaxPartyMembers = 40;   // members listed per view: a raid
inline constexpr std::size_t kMaxPartyDebuffs = 6;
inline constexpr std::size_t kMaxLooseMobs = 8;
inline constexpr std::size_t kMaxRaidMarks = 8;

// A harmful aura on a party member. dispel_type: 0 none, 1 magic, 2 curse, 3 disease, 4 poison (the spell's DispelType); the dispellable ones come first,
// then the longest remaining first. remaining_ms 0xFFFFFFFF for an aura with no duration. spell_id 0 marks an unused slot.
struct PartyDebuff {
    std::uint32_t spell_id = 0;
    std::uint32_t remaining_ms = 0;
    std::uint8_t dispel_type = 0;
    std::uint8_t stacks = 0;
    std::uint16_t reserved = 0;
};

// Bits of PartyMember::role: the group's LFG roles, or the main tank flag; for a bot without either, the role its module gave it (SpendBotTalents).
enum PartyRole : std::uint8_t {
    kPartyRoleTank = 1u << 0,
    kPartyRoleHealer = 1u << 1,
    kPartyRoleDamage = 1u << 2,
};

// Bits of PartyMember::member_flags: the group's own member flags, plus the leader.
enum PartyMemberFlag : std::uint8_t {
    kPartyMemberAssistant = 1u << 0,
    kPartyMemberMainTank = 1u << 1,
    kPartyMemberMainAssist = 1u << 2,
    kPartyMemberLeader = 1u << 3,
};

// One member of a group, bot or human. guid is the player's unit reference. online is 1 when the player is in the world: only then are the position,
// health, power, combat, cast, pet, debuff and attacker fields filled (the others stay 0). life_state as BotSnapshot's: 0 alive, 1 dead, 2 ghost.
// rez_pending 1 when a resurrect offer waits for the player's answer. transport_entry: the entry of the transport the member stands on (0 none),
// so a bot can ride with its leader. attacker_count: the units attacking the member now (Bot.Snapshot.PartyThreat). debuffs: the first debuff_count
// of debuffs[]. The pet fields follow BotSnapshot's (pet_guid 0 = none).
struct PartyMember {
    std::uint64_t guid = 0;
    std::uint64_t victim_ref = 0;
    std::uint64_t casting_target = 0;
    std::uint64_t pet_guid = 0;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float pos_z = 0.0f;
    std::uint32_t map_id = 0;
    std::uint32_t health = 0;
    std::uint32_t max_health = 0;
    std::uint32_t power = 0;
    std::uint32_t max_power = 0;
    std::uint32_t casting_spell = 0;
    std::uint32_t cast_remaining_ms = 0;
    std::uint32_t pet_health = 0;
    std::uint32_t pet_max_health = 0;
    std::uint32_t transport_entry = 0;
    std::uint8_t class_id = 0;
    std::uint8_t role = 0;            // PartyRole bits
    std::uint8_t subgroup = 0;        // 0..7 in a raid, 0 in a party
    std::uint8_t is_bot = 0;
    std::uint8_t power_type = 0;
    std::uint8_t life_state = 0;
    std::uint8_t rez_pending = 0;
    std::uint8_t member_flags = 0;    // PartyMemberFlag bits
    std::uint8_t online = 0;
    std::uint8_t in_combat = 0;
    std::uint8_t attacker_count = 0;
    std::uint8_t debuff_count = 0;
    PartyDebuff debuffs[kMaxPartyDebuffs] = {};
    std::uint32_t reserved[2] = {};
};

// A creature attacking a member of the group who is not a tank (Bot.Snapshot.PartyThreat): the aggro-loss list. victim_ref is that member; since_ms is
// how long the creature has been on a non-tank (small = a new loss: the module needs no event ring to see one). Mobs on healers come first, then the
// newest. unit_ref 0 marks an unused slot.
struct LooseMob {
    std::uint64_t unit_ref = 0;
    std::uint64_t victim_ref = 0;
    std::uint32_t since_ms = 0;
    std::uint32_t reserved = 0;
};

// One group. group_id is the group's guid counter (0 marks an unused slot). main_tank_ref: the member flagged main tank, else the first member with
// a tank role, else 0; main_assist_ref likewise from the main assist flag. raid_marks[i] is the unit raid target icon i + 1 is on (0 none).
// engaged_count: the creatures attacking any member (capped at 255); loose_count: the first loose_count entries of loose_mobs[] (both need
// Bot.Snapshot.PartyThreat). generation counts the rebuilds of this group's view: an idle group is rebuilt one tick in Bot.Snapshot.PartyViewIdleTicks,
// a group in combat or in an instance every tick, so a module that sees the same generation twice knows the facts are reused.
struct PartyView {
    std::uint64_t group_id = 0;
    std::uint64_t leader_ref = 0;
    std::uint64_t main_tank_ref = 0;
    std::uint64_t main_assist_ref = 0;
    std::uint64_t raid_marks[kMaxRaidMarks] = {};
    std::uint32_t member_first = 0;
    std::uint32_t member_count = 0;
    std::uint32_t generation = 0;
    std::uint8_t is_raid = 0;
    std::uint8_t in_combat = 0;
    std::uint8_t engaged_count = 0;
    std::uint8_t loose_count = 0;
    LooseMob loose_mobs[kMaxLooseMobs] = {};
    std::uint32_t reserved[4] = {};
};

// Layout 25: the section table in Tier2RegionHeader: where each array of a snapshot buffer lies, with the element stride the core wrote (its sizeof)
// and the number of slots. offset is from the start of each snapshot buffer. kind 0 marks an unused entry. A module reads the sections it knows by
// kind and ignores the rest (the growth rule above kTier2LayoutBase).
inline constexpr std::size_t kMaxTier2Sections = 8;
enum class Tier2SectionKind : std::uint32_t {
    kNone = 0,
    kBots = 1,          // BotSnapshot[max_bots]
    kNearbyUnits = 2,   // NearbyUnit[max_bots * max_nearby_units]
    kPartyViews = 3,    // PartyView[max_party_views]
    kPartyMembers = 4,  // PartyMember[max_party_members]
};
struct Tier2Section {
    std::uint32_t kind = 0;          // Tier2SectionKind
    std::uint32_t element_size = 0;  // the stride, in bytes
    std::uint32_t count = 0;         // slots in the section
    std::uint32_t reserved = 0;
    std::uint64_t offset = 0;
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
    // Layout 7: the map the bot is on (M7 hubs). A module picks the hubs of its bot's map from this.
    std::uint32_t map_id = 0;
    // Layout 8: the bot's quest log (M5), the first quest_count entries of quests[].
    std::uint8_t quest_count = 0;
    QuestEntry quests[kMaxQuestLog] = {};
    // Layout 9: the nearest repairman (friendly to the bot, on its own map; repair_valid is 0 when there is none) and the lowest
    // durability of the bot's equipped items in percent (100 when it wears nothing that wears out).
    float repair_x = 0.0f;
    float repair_y = 0.0f;
    float repair_z = 0.0f;
    std::uint8_t repair_valid = 0;
    std::uint8_t durability_pct = 100;
    // Layout 10: what the bot is doing, so a module does not try what is certain to fail. moving is 1 while the bot is moving. casting_spell is the
    // spell id of the cast or channel in progress (0 when none) and cast_remaining_ms what is left of it (a cast with a cast time cannot be
    // started while moving, and no cast can be started while another is in progress).
    std::uint8_t moving = 0;
    std::uint32_t casting_spell = 0;
    std::uint32_t cast_remaining_ms = 0;
    // Layout 11: death state. life_state is 0 alive, 1 dead (corpse, spirit not released), 2 ghost (released). A ghost has health 1, so
    // test life_state, not health. corpse_valid is 1 when corpse_map and corpse_x/y/z are set (a ghost: where its corpse lies, which can be
    // another map than map_id; a dead bot: its own position). reclaim_wait_ms is the time until the corpse may be reclaimed (0 = now; ghosts
    // only). resurrect_from is the unit reference of the player whose resurrect offer is pending (0 none).
    std::uint8_t life_state = 0;
    std::uint8_t corpse_valid = 0;
    std::uint32_t corpse_map = 0;
    float corpse_x = 0.0f;
    float corpse_y = 0.0f;
    float corpse_z = 0.0f;
    std::uint32_t reclaim_wait_ms = 0;
    std::uint64_t resurrect_from = 0;
    // Layout 13: policy facts the module currently has to infer (see the forever-bots report
    // core-sdk-requests §2). Appended at the end of the struct on purpose: earlier fields keep their
    // meaning so the layout history reads in order.
    //
    // auras: every aura the core reports on the bot, the first aura_count entries of auras[] (an
    // unused slot has spell_id 0). Replaces the module's DOT and buff timers.
    std::uint32_t aura_count = 0;
    AuraEntry auras[kMaxAuras] = {};
    // The target of the cast in progress (a unit reference, 0 = self): heal claims and assist targeting.
    std::uint64_t casting_target = 0;
    // The bot's own pet (a unit reference, 0 = none). pet_state is 0 none, 1 alive, 2 dead.
    std::uint64_t pet_guid = 0;
    std::uint32_t pet_health = 0;
    std::uint32_t pet_max_health = 0;
    std::uint32_t pet_state = 0;
    // Guids for the interactions that used to be position-only: vendor buying (BuyItem) and repair
    // need the NPC's guid, not just a position taken from nearby units.
    std::uint64_t vendor_guid = 0;
    std::uint64_t repair_guid = 0;
    std::uint64_t auctioneer_guid = 0;
    // Money in copper and free bag slots, for buy and hand-in pre-checks.
    std::uint64_t copper = 0;
    std::uint32_t bag_free_slots = 0;
    // Why the bot lost its last target, so a fight_end reason=target_gone can be split into the four
    // causes the module cannot tell apart: 0 none, 1 killed, 2 leashed, 3 despawned, 4 another player.
    std::uint32_t last_target_lost_reason = 0;
    // Layout 14: ammo state for the equipped ranged weapon. ammo_entry is the selected projectile, or when
    // none is selected the core's recommended compatible projectile for this weapon/level; 0 means no
    // ammo-using ranged weapon or no compatible projectile data. ammo_count is the carried-inventory
    // quantity of ammo_entry (not bank); 0 means the bot needs a supply.
    std::uint32_t ammo_entry = 0;
    std::uint32_t ammo_count = 0;
    // Layout 15: supplies (bots buy and use what their play needs).
    // food_count: the carried food the bot can eat (health food, summed over its stacks). drink_count: the carried drink it can use, 0 for a
    // class without mana. Only items the bot may use at its level are counted.
    std::uint32_t food_count = 0;
    std::uint32_t drink_count = 0;
    // The nearest vendor on the bot's map that sells ammunition this bot's ranged weapon takes, and the nearest that sells food or drink it
    // may use (position, valid flag and the unit reference a kVendor action names). Valid 0: the map has none, or the bot needs none.
    std::uint8_t ammo_vendor_valid = 0;
    std::uint8_t food_vendor_valid = 0;
    float ammo_vendor_x = 0.0f;
    float ammo_vendor_y = 0.0f;
    float ammo_vendor_z = 0.0f;
    float food_vendor_x = 0.0f;
    float food_vendor_y = 0.0f;
    float food_vendor_z = 0.0f;
    std::uint64_t ammo_vendor_guid = 0;
    std::uint64_t food_vendor_guid = 0;
    // Layout 16: potions and bandages. health_potion_count and mana_potion_count: the carried potions that restore health or mana and that the
    // bot may use at its level (mana potions: 0 for a class without mana). bandage_count: the carried bandages it may use.
    std::uint32_t health_potion_count = 0;
    std::uint32_t mana_potion_count = 0;
    std::uint32_t bandage_count = 0;
    // The nearest vendor on the bot's map that sells any of those the bot can use (health potions, mana potions for a mana user, bandages),
    // reachable on foot as far as the core can tell. Valid 0: the map has none.
    std::uint8_t goods_vendor_valid = 0;
    float goods_vendor_x = 0.0f;
    float goods_vendor_y = 0.0f;
    float goods_vendor_z = 0.0f;
    std::uint64_t goods_vendor_guid = 0;
    // Reagents (layout 16): the item a spell the bot tried to cast was missing (the core notes it when a cast is refused for lack of reagents), how
    // many of it the bot carries, and the nearest reachable vendor that sells it. reagent_entry 0: no reagent is missing.
    std::uint32_t reagent_entry = 0;
    std::uint32_t reagent_count = 0;
    std::uint8_t reagent_vendor_valid = 0;
    float reagent_vendor_x = 0.0f;
    float reagent_vendor_y = 0.0f;
    float reagent_vendor_z = 0.0f;
    std::uint64_t reagent_vendor_guid = 0;
    // Layout 17: flight points. The nearest flight master on the bot's map that the bot can walk to (position, valid flag and the unit reference a kTaxi action
    // names), and whether the bot is on a flight now (it takes no orders but a kTaxi that ends it; it lands by itself).
    std::uint8_t taxi_master_valid = 0;
    std::uint8_t in_flight = 0;
    float taxi_master_x = 0.0f;
    float taxi_master_y = 0.0f;
    float taxi_master_z = 0.0f;
    std::uint64_t taxi_master_guid = 0;
    // Layout 18: professions. The two primary professions the core assigned to the bot (skill line ids, see BotProfessions.h: 0 when the bot has none, which is an
    // alt, or Bot.Professions.Enabled is off) and its skill value and maximum in each (0 until it has learned the profession).
    std::uint16_t profession_skill[2] = { 0, 0 };
    std::uint16_t profession_value[2] = { 0, 0 };
    std::uint16_t profession_max[2] = { 0, 0 };
    // Layout 20: the auctioneer's position on the bot's map (the nearest one the bot's faction may use, valid 0 when the map has none), so the
    // module can walk to it. auctioneer_guid, already in the snapshot, is the unit reference an AuctionHouse action names.
    std::uint8_t auctioneer_valid = 0;
    float auctioneer_x = 0.0f;
    float auctioneer_y = 0.0f;
    float auctioneer_z = 0.0f;
    // Layout 21: the ground effects near the bot (see GroundEffect), the first ground_effect_count entries of ground_effects[]. 0 when none, or when the core's
    // Bot.Snapshot.GroundEffects is off.
    std::uint8_t ground_effect_count = 0;
    GroundEffect ground_effects[kMaxGroundEffects] = {};
    // Layout 22: the last hits the bot took, the first damage_hit_count entries of damage_hits[], oldest first (the core keeps the last
    // kMaxDamageHits). damage_sequence is the sequence of the newest hit (0 before the first); a module that has seen it already has no new hits.
    std::uint32_t damage_sequence = 0;
    std::uint8_t damage_hit_count = 0;
    DamageHit damage_hits[kMaxDamageHits] = {};
    // Layout 23: 1 when the bot stands in a capital city (its zone allows the Trade channel, AreaFlags::AllowTradeChannel), so city inspection
    // can run. guild_guid is the guild the bot is in (0 with no guild).
    std::uint8_t in_capital = 0;
    std::uint64_t guild_guid = 0;
    // Layout 23: the best equipped item level of the bot (CityInspect compares it with the player's; 0 unknown).
    std::uint32_t item_level = 0;
    // Layout 24: the time left on the global cooldown the bot is under (0 when it is free), the bot's movement state, the transport it stands on,
    // the nearest banker, the auras on the bot's current target, and a ring of the last action results.
    // movement_flags: bit 0 swimming (in water), bit 1 flying, bit 2 falling, bit 3 on a transport. transport_guid: the entry of the transport
    // (a ship, zeppelin or elevator, a game object) the bot is on, 0 for none.
    std::uint32_t gcd_remaining_ms = 0;
    std::uint32_t movement_flags = 0;
    std::uint64_t transport_guid = 0;
    // The nearest banker on the bot's map that it can walk to (position and valid flag, and the unit reference), like auctioneer_*.
    std::uint8_t banker_valid = 0;
    float banker_x = 0.0f;
    float banker_y = 0.0f;
    float banker_z = 0.0f;
    std::uint64_t banker_guid = 0;
    // The first target_aura_count entries of target_auras[]: the auras on the bot's current target (threat_target_guid's unit), when it is in the view.
    std::uint8_t target_aura_count = 0;
    AuraEntry target_auras[kMaxTargetAuras] = {};
    // The results of the last actions the core applied for this bot, newest first; sequence 0 marks an unused entry. last_action_* mirrors results[0].
    ActionResult results[kMaxActionResults] = {};
    // Layout 25: the bot's group view and its own entry in it (indexes into the party view and party member sections, kNoPartyView for none: no
    // group, or Bot.Snapshot.PartyView off), the enemy telegraphs near it (the first telegraph_count of telegraphs[]), the answers to its position
    // probes (the first probe_count of probes[], oldest first; probe_sequence is the newest answer's sequence, 0 before the first), and role_hint:
    // the role its module last gave the core for its talents (0 none, 1 tank, 2 healer, 3 damage), so a module restart finds the roles again.
    std::uint32_t party_view_index = kNoPartyView;
    std::uint32_t party_member_index = kNoPartyView;
    std::uint8_t telegraph_count = 0;
    std::uint8_t probe_count = 0;
    std::uint8_t role_hint = 0;
    std::uint8_t reserved25 = 0;
    std::uint32_t probe_sequence = 0;
    Telegraph telegraphs[kMaxTelegraphs] = {};
    ProbeResult probes[kMaxProbeResults] = {};
    std::uint32_t reserved25b[2] = {};
    // NearbyUnit entries for this bot live in the region's flexible nearby-units area,
    // offset-addressed using Tier2RegionHeader::max_nearby_units; Tier2Session exposes them through NearbyOf().
};

// WorldTickSnapshot is a conceptual view over the region, not a single fixed struct -- its bots[]
// array length is max_bots from the header. Tier2Session exposes it as WorldTickSnapshotView
// rather than exposing raw offset math to module authors.
struct WorldTickSnapshotHeader {
    std::uint64_t tick_number = 0;
    std::uint32_t bot_count = 0;
    // Layout 25: the party views and party members the core filled in this buffer (the sections hold max_party_views and max_party_members slots).
    std::uint32_t party_view_count = 0;
    std::uint32_t party_member_count = 0;
    std::uint32_t reserved25 = 0;
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
                 // Layout 15, buying (needs Bot.Actions.Supplies in the core): 8 buy ammunition, 16 buy food, 32 buy drink. The core picks the
                 // item from the vendor's stock (ammunition the bot's ranged weapon takes; the best food or drink the bot may use at its level)
                 // and buys until the bot carries the target count or its money or bags run out: dest_x is the ammunition target, dest_y the
                 // food target, dest_z the drink target (item counts; 0 = none of that kind). A vendor without the goods is not an error.
                 // Layout 16: 512 buys the reagent the snapshot names (reagent_entry) up to Bot.Supplies.ReagentTarget pieces.
                 // Layout 16: 64 buy health potions, 128 buy mana potions, 256 buy bandages. Their target counts are the core's settings
                 // (Bot.Supplies.HealthPotionTarget, ManaPotionTarget, BandageTarget), not the action's. The core keeps back a money reserve
                 // (Bot.Supplies.ReserveCopperPerLevel per level) from every purchase except ammunition.
                 // SDK 0.16: 1024 also sells white junk with the grey items of bit 1 (or alone): spare bags, miscellaneous items, and the surplus of trade
                 // goods (a bot keeps 60 pieces of each kind, for First Aid, Cooking or the auction house). Never a quest item, an equipped bag, a
                 // consumable, a reagent, ammunition, a weapon or armour. Meant for a bot whose bags are nearly full.
    kTrain = 7,  // learn every spell the trainer in interaction range teaches this bot now (M4). Refused in combat.
    kGroup = 8,  // party actions (M8). spell_id 1 invites the bot named by target_guid, 2 accepts the bot's pending invite,
                 // 3 leaves the bot's party. Refused in combat.
                 // 40-man raid support (doc 108): spell_id 4 converts the bot's own party to a raid (leader/assistant only, idempotent
                 // once it already is one); spell_id 5 moves the player named by target_guid into the subgroup number given in dest_x
                 // (0-7, leader/assistant only, the group must already be a raid). No new wire field: both reuse the existing generic
                 // spell_id/target_guid/dest_x of this one ActionType, the same way kVendor and kPetCommand already overload spell_id.

    kLoot = 9,   // loot the corpse named by target_guid: every item the bot may take, and the money (M7). The corpse must be
                 // dead, within 30 yards, and one the bot has the right to loot; an empty or partly looted corpse is not an error.
    kTeleport = 11, // put the bot at (map = spell_id, dest_x, dest_y, dest_z) (scenarios, doc 19). The core refuses it unless
                    // Bot.Actions.Teleport is on, and refuses it in combat. A bot in a party enters the party's instance.
    kGrant = 12,    // add the item with entry spell_id to the bot's bags, dest_x items (at least 1) (scenarios). Refused unless the core's
                    // Bot.Actions.Grant is on. Followed by a kVendor equip, it dresses a bot for a test.
    kAutoGear = 13, // dress the bot for its level (doc 19): the core picks, for each equipment slot, the best item the bot's class can use at
                    // its level (uncommon quality at most), adds it to the bags and equips it. spell_id bit 1 = a tank (one-hand weapon
                    // and shield). Refused in combat and unless the core's Bot.Actions.AutoGear is on (default on).
    kQuest = 10, // quest actions at an NPC within interaction range (M5). spell_id = (operation << 24) | quest id, with operation 1 =
                 // accept the quest from a giver, 2 = hand it in at an ender (the core picks the reward item), 3 = abandon it
                 // (no NPC needed). Accept and hand-in are refused in combat.
    kReleaseSpirit = 14, // release the spirit of the dead bot (layout 11): the corpse becomes a ghost at the graveyard. No arguments. Refused
                         // when the bot is alive, already a ghost, being teleported, or in combat.
    kResurrect = 15,     // answer a pending resurrect offer (layout 11). target_guid = the resurrecter's player unit ref (BotSnapshot::resurrect_from);
                         // spell_id 0 accepts, 1 declines.
    kReclaimCorpse = 16, // reclaim the corpse (layout 11): a ghost within the corpse reclaim radius of its corpse, once the reclaim delay is over.
    kSummon = 17,        // a chat order (group of the target): put the bot next to the player target_guid names, a spread position given by the index in
                         // dest_x. Refused for a dead or fighting bot, a speaker who is fighting, an instance involved on another map, or when the bot is
                         // within 5 yards already. Needs Bot.Chat.Orders.Enabled in the core.
    kRevive = 18,        // a chat order: resurrect the dead bot (corpse or ghost) at 35% health and put it next to the player target_guid names (as kSummon).
                         // Refused on a hardcore realm. Needs Bot.Chat.Orders.Enabled.
    kMount = 19,         // spell_id 1 mounts the bot on its fastest learned ground mount, 0 dismounts it. Refused in combat.
    kCraft = 20,         // SDK 0.18: craft one named recipe: spell_id = the recipe's spell id (a recipe of Alchemy, Blacksmithing, Engineering, Leatherworking,
                         // Tailoring, Smelting, First Aid or Cooking that the bot knows), cast on the bot itself. kNeedsFocus when its anvil, forge or fire is not
                         // near; kRefused (with a reason) for an unknown recipe, missing materials, combat or death; kNotKnown when the bot has not learned it;
                         // kBusy while another cast runs; kMoving for a recipe with a cast time while the bot moves.
    kGather = 21,        // SDK 0.18: gather with a gathering skill. spell_id is the skill line: 186 Mining, 182 Herbalism or 393 Skinning. The core looks for what the
                         // bot can gather within 30 yards (a vein or herb of a level its skill allows, or a corpse it can skin) and, when it is more than 4 yards
                         // away, starts the bot walking to it (kMoving: ask again); in reach it casts the gathering spell, and the loot is taken with kLoot as for a
                         // kill. kRefused with a reason when there is nothing to gather, the bot has no such skill, or it is in combat or dead; kBusy while casting.
    kPetCommand = 22,    // command the bot's pet (layout 13). spell_id 1 = attack target_guid, 2 = follow the bot,
                         // 3 = stay; 4 = set stance (spell_id 4, dest_x = the stance value). Refused when the bot has no pet
                         // (status kInvalidTarget); 1-3 are allowed in combat too (redirecting the pet is often needed mid-fight),
                         // only 4 (the stance) is out-of-combat only.
    kConsume = 23,       // layout 15: eat or drink. spell_id 1 uses the best food the bot carries, 2 the best drink. The bot sits while it
                         // restores, as for a player. Refused in combat, when the bot carries none, and unless Bot.Actions.Supplies is on.
                         // Layout 16: 3 uses the best health potion, 4 the best mana potion (both also allowed in combat; the game's potion
                         // cooldown applies and is answered as a cooldown refusal), 5 uses the best bandage on the bot (out of combat, the bot stands
                         // still while it channels; refused while it carries "Recently Bandaged").
    kTaxi = 24,          // layout 17: fly from the flight master the bot stands at (target_guid names it; 0 = the nearest in reach) to the flight node nearest to
                         // (dest_x, dest_y, dest_z) on the bot's own map (spell_id = that map id, which must be the bot's). Every bot knows every flight node and flies free
                         // (Bot.Taxi.Enabled, Bot.Taxi.Free in the core). Refused in combat, on a flight, with no flight master in reach, with no flight path (the faction's
                         // nodes only), or when the flight would not bring the bot at least Bot.Taxi.MinGainYards closer to the point than it is.
    kUseObject = 25,     // layout 19: use a game object near the bot, as a click would: a door, a lever, an altar, a chest. spell_id is the object's entry; dest_x, dest_y,
                         // dest_z where it stands (only checked to find the right one among several of an entry: the nearest to the bot within 30 yards is used). The
                         // bot must be within 8 yards of it (status kOutOfRange otherwise: walk closer). target_guid, when not 0, is an ITEM ENTRY to use on the object
                         // instead of a click (a key, a charge of powder): the bot is given one if it has none, and casts the item's spell at the object. Refused unless
                         // Bot.Actions.UseObject is on in the core (for scenarios and tests), in combat, dead, or with no such object in reach.
    kSellToVendor = 26,  // layout 20: sell items of one entry from the bags to a vendor in reach. spell_id is the item entry; dest_x the count
                         // to sell (0 = every one the bot carries of it). target_guid, when not 0, names the vendor NPC (BotSnapshot::vendor_guid is the
                         // one to use); 0 takes the nearest vendor in reach. Refused in combat, dead, with no vendor in reach, or when the bot has none
                         // of the entry. Items the vendor will not take are kept and reported as kRefused with a reason.
    kSkillUp = 27,         // SDK 0.17: make something that raises a crafting skill. spell_id is the skill line, 129 First Aid (bandages from cloth) or 185 Cooking
                         // (food from meat); SDK 0.18 adds 164 Blacksmithing, 165 Leatherworking, 171 Alchemy, 186 Smelting, 197 Tailoring and 202 Engineering
                         // (Enchanting needs an item to work on and is not offered). The core picks, among the recipes the bot knows for that skill, the one that can still raise its skill, that it has the
                         // materials for and that needs the most skill, and casts it on the bot. A recipe that needs a cooking fire when none is near makes the
                         // bot light a campfire first (Basic Campfire); the next kSkillUp makes the food. kApplied when a cast started; kRefused with a reason when
                         // the bot has no recipe, no materials, or its skill is too high for any recipe it can make; kBusy while another cast runs; kMoving when the
                         // recipe has a cast time and the bot moves. Refused in combat and dead. SDK 0.18: a recipe whose focus (anvil, forge, fire) is not near is
                         // not picked when another is possible; when only such recipes remain the answer is kNeedsFocus, and the module walks to a focus object.
    kGossip = 28,          // SDK 0.24: talk to a friendly creature and pick one of its gossip options, as a player does at an escort or quest NPC. spell_id is the
                         // creature's entry (the nearest live one within 30 yards is used); dest_x is the option's place in the menu it shows, counted from 0 in menu order.
                         // The bot must be within interaction range (status kOutOfRange otherwise: walk closer), out of combat and alive. The core opens the creature's menu
                         // (its script's hello, or the database menu) and picks the option, so a scripted escort or event starts. kApplied when an option was picked;
                         // kInvalidTarget with no such creature in reach; kRefused with a reason when the creature has no gossip, no option at that place, or the core's
                         // Bot.Actions.Gossip is off (a scenario tool, like Bot.Actions.UseObject).
    kRespecTalents = 29,   // talent respec: unlearn every talent point the bot has spent, pay the core's reset cost (its gold), then spend the points again by
                         // the profile for spec_id/role (dest_x: 1 tank, 2 healer, 3 dps, 0 none; spell_id: a TalentSpec, 0 none), as SpendBotTalents does.
                         // Uses the core's talent reset cost (GetNextResetTalentsCost), so a bot short of gold is refused (kRefused, "not enough gold for the
                         // respec"), nothing is unlearned, and nothing is paid. Refused in combat and dead, below level 10, and unless the core's
                         // Bot.Talents.Respec is on (default off). kApplied when the reset is done; the reason is empty when the points were spent again,
                         // else it says the spend was refused and the points stay unspent (SpendBotTalents spends them later, never a second reset).
    kEmote = 30,           // SDK 0.25 (protocol 21): a bot emote. The Tier 1 request is ModuleClient::BotEmote (EmoteAction); this value reserves the number for core.
    kLfgJoin = 31,         // SDK 0.25 (protocol 21): a bot joins the dungeon finder. The Tier 1 request is ModuleClient::BotLfgJoin (LfgJoinAction).
    kTransport = 32,       // layout 25: RESERVED for the transport design (MOD-Forever-Bots doc/transports.md): spell_id 1 board the transport with entry target_guid,
                           // 2 leave it, 3 join the leader's transport. The core answers kRefused with a reason until it is implemented; the number is fixed here so
                           // the module can carry the code. Needs Bot.Actions.Transport in the core.
    kStopCast = 33,        // layout 25: stop the bot's cast or channel in progress (an overheal cancel, a kick that is no longer needed). No arguments. kRefused
                           // when nothing is being cast. Needs Bot.Actions.StopCast in the core.
    kProbePoint = 34,      // layout 25: the position probe (plan Phase 5.3): ask what the core knows about the point (dest_x, dest_y, dest_z) on the bot's map:
                           // whether the bot has a route to it and how long, the ground height, water, and (target_guid, when not 0) whether the point is in line
                           // of sight of that unit. spell_id is the module's own probe id, echoed back. The answer is the next snapshot's BotSnapshot::probes
                           // (ProbeResult); the action result is kApplied, or kBusy when the core's per-tick probe budget (Bot.Probe.PerTick) is used up: ask
                           // again next tick. Needs Bot.Actions.Probe in the core. Costs a path calculation and a line-of-sight check: ration them.
    kLastActionType = kProbePoint, // the highest valid value; PublishActions refuses anything above it
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

    // Layout 25: the oldest layout a module may be built against to read this region (kTier2LayoutBase, see the growth rule), the party slots
    // (the core's choice, Tier2PartyViewSlots(max_bots) by default and Tier2PartyMemberSlots(max_bots, views); a module reads them here, not from a formula), the region geometry as the core
    // laid it out (so a module built against an older layout of the same base needs none of the helpers below: header_bytes is where the first
    // snapshot buffer starts, snapshot_bytes the size of one, action_offset where the first action buffer starts and action_bytes the size of one;
    // action_header_bytes and action_stride are the core's sizeof(ActionQueueHeader) and sizeof(BotAction), which the module writes actions with),
    // and the section table: the first section_count entries of sections[] say where each array of a snapshot buffer lies and with what stride.
    std::uint8_t reserved_header[3] = {};
    std::uint32_t layout_base = kTier2LayoutBase;
    std::uint32_t max_party_views = 0;
    std::uint32_t max_party_members = 0;
    std::uint32_t section_count = 0;
    std::uint32_t action_header_bytes = 0;
    std::uint64_t header_bytes = 0;
    std::uint64_t snapshot_bytes = 0;
    std::uint64_t action_offset = 0;
    std::uint64_t action_bytes = 0;
    std::uint32_t action_stride = 0;
    std::uint32_t reserved_header2 = 0;
    Tier2Section sections[kMaxTier2Sections] = {};

    // Followed in memory by, each sized from the bounds above (offsets: see the helpers below):
    //   WorldTickSnapshotHeader + BotSnapshot[max_bots] + NearbyUnit[max_bots * max_nearby_units]
    //     + PartyView[max_party_views] + PartyMember[max_party_members], x2 buffers
    //   ActionQueueHeader + BotAction[max_actions], x2 buffers
};

// Compile-time checks on the parts of the layout that would silently break across processes.
// Atomics in shared memory must be lock-free, or the two processes can't safely share them.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "published_*_tick must be lock-free across processes");
static_assert(std::atomic<std::uint8_t>::is_always_lock_free, "published_*_buffer must be lock-free across processes");

static_assert(std::is_standard_layout_v<NearbyUnit> && std::is_trivially_copyable_v<NearbyUnit>,
              "NearbyUnit must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<GroundEffect> && std::is_trivially_copyable_v<GroundEffect>,
              "GroundEffect must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<AuraEntry> && std::is_trivially_copyable_v<AuraEntry>,
              "AuraEntry must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<BotSnapshot> && std::is_trivially_copyable_v<BotSnapshot>,
              "BotSnapshot must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<WorldTickSnapshotHeader> && std::is_trivially_copyable_v<WorldTickSnapshotHeader>,
              "WorldTickSnapshotHeader must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<BotAction> && std::is_trivially_copyable_v<BotAction>,
              "BotAction must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<ActionQueueHeader> && std::is_trivially_copyable_v<ActionQueueHeader>,
              "ActionQueueHeader must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<Telegraph> && std::is_trivially_copyable_v<Telegraph>,
              "Telegraph must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<ProbeResult> && std::is_trivially_copyable_v<ProbeResult>,
              "ProbeResult must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<PartyMember> && std::is_trivially_copyable_v<PartyMember>,
              "PartyMember must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<PartyView> && std::is_trivially_copyable_v<PartyView>,
              "PartyView must be POD for shared-memory use");
static_assert(std::is_standard_layout_v<Tier2Section> && std::is_trivially_copyable_v<Tier2Section>,
              "Tier2Section must be POD for shared-memory use");

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

// Layout 25: the party slots of a region. The core chooses the view count (Modules.Tier2.MaxPartyViews; 0 = the default below) and writes it into
// the header with the member pool that follows from it; the module reads both from the header (Tier2RegionHeader::max_party_views,
// max_party_members), so no handshake field is needed and the two sides cannot disagree. Default views: every bot's group has at least two
// members, so at most max_bots / 2 groups of bots, plus one for a bot grouped with humans only. Members: every bot once plus as many humans, and
// never fewer than two per view. A group beyond the slots is not built (its bots read kNoPartyView); a member beyond them is left out of its view.
constexpr std::uint32_t Tier2PartyViewSlots(std::uint32_t maxBots)
{
    return maxBots / 2 + 1;
}

constexpr std::uint32_t Tier2PartyMemberSlots(std::uint32_t maxBots, std::uint32_t maxPartyViews)
{
    return maxBots * 2 > maxPartyViews * 2 ? maxBots * 2 : maxPartyViews * 2;
}

constexpr std::uint32_t Tier2PartyMemberSlots(std::uint32_t maxBots)
{
    return Tier2PartyMemberSlots(maxBots, Tier2PartyViewSlots(maxBots));
}

// Offsets of the sections inside one snapshot buffer (the core writes the same numbers into the header's section table). Every struct here is a
// multiple of 8 bytes long, so each section starts 8-aligned when the buffer does. The forms without the party slots use the defaults above.
constexpr std::size_t Tier2BotsSectionOffset()
{
    return sizeof(WorldTickSnapshotHeader);
}

constexpr std::size_t Tier2NearbySectionOffset(std::uint32_t maxBots)
{
    return Tier2BotsSectionOffset() + std::size_t(maxBots) * sizeof(BotSnapshot);
}

constexpr std::size_t Tier2PartyViewsSectionOffset(std::uint32_t maxBots, std::uint32_t maxNearbyUnits)
{
    return Tier2NearbySectionOffset(maxBots) + std::size_t(maxBots) * maxNearbyUnits * sizeof(NearbyUnit);
}

constexpr std::size_t Tier2PartyMembersSectionOffset(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint32_t maxPartyViews)
{
    return Tier2PartyViewsSectionOffset(maxBots, maxNearbyUnits) + std::size_t(maxPartyViews) * sizeof(PartyView);
}

constexpr std::size_t Tier2PartyMembersSectionOffset(std::uint32_t maxBots, std::uint32_t maxNearbyUnits)
{
    return Tier2PartyMembersSectionOffset(maxBots, maxNearbyUnits, Tier2PartyViewSlots(maxBots));
}

constexpr std::size_t Tier2SnapshotBufferBytes(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint32_t maxPartyViews,
                                               std::uint32_t maxPartyMembers)
{
    return tier2_detail::AlignUp(Tier2PartyMembersSectionOffset(maxBots, maxNearbyUnits, maxPartyViews) +
                                 std::size_t(maxPartyMembers) * sizeof(PartyMember));
}

constexpr std::size_t Tier2SnapshotBufferBytes(std::uint32_t maxBots, std::uint32_t maxNearbyUnits)
{
    return Tier2SnapshotBufferBytes(maxBots, maxNearbyUnits, Tier2PartyViewSlots(maxBots), Tier2PartyMemberSlots(maxBots));
}

constexpr std::size_t Tier2ActionBufferBytes(std::uint32_t maxActions)
{
    return tier2_detail::AlignUp(sizeof(ActionQueueHeader) + std::size_t(maxActions) * sizeof(BotAction));
}

constexpr std::size_t Tier2RegionBytes(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint32_t maxActions, std::uint32_t maxPartyViews,
                                       std::uint32_t maxPartyMembers)
{
    return Tier2HeaderBytes() + 2 * Tier2SnapshotBufferBytes(maxBots, maxNearbyUnits, maxPartyViews, maxPartyMembers) +
           2 * Tier2ActionBufferBytes(maxActions);
}

constexpr std::size_t Tier2RegionBytes(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint32_t maxActions)
{
    return Tier2RegionBytes(maxBots, maxNearbyUnits, maxActions, Tier2PartyViewSlots(maxBots), Tier2PartyMemberSlots(maxBots));
}

constexpr std::size_t Tier2SnapshotBufferOffset(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint32_t maxPartyViews,
                                                std::uint32_t maxPartyMembers, std::uint8_t buffer)
{
    return Tier2HeaderBytes() + buffer * Tier2SnapshotBufferBytes(maxBots, maxNearbyUnits, maxPartyViews, maxPartyMembers);
}

constexpr std::size_t Tier2SnapshotBufferOffset(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint8_t buffer)
{
    return Tier2SnapshotBufferOffset(maxBots, maxNearbyUnits, Tier2PartyViewSlots(maxBots), Tier2PartyMemberSlots(maxBots), buffer);
}

constexpr std::size_t Tier2ActionBufferOffset(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint32_t maxActions,
                                              std::uint32_t maxPartyViews, std::uint32_t maxPartyMembers, std::uint8_t buffer)
{
    return Tier2HeaderBytes() + 2 * Tier2SnapshotBufferBytes(maxBots, maxNearbyUnits, maxPartyViews, maxPartyMembers) +
           buffer * Tier2ActionBufferBytes(maxActions);
}

constexpr std::size_t Tier2ActionBufferOffset(std::uint32_t maxBots, std::uint32_t maxNearbyUnits, std::uint32_t maxActions,
                                              std::uint8_t buffer)
{
    return Tier2ActionBufferOffset(maxBots, maxNearbyUnits, maxActions, Tier2PartyViewSlots(maxBots), Tier2PartyMemberSlots(maxBots), buffer);
}

// Layout 25: writes the geometry and the section table of a region into its header, from the bounds already set in it (max_bots,
// max_nearby_units, max_actions, and max_party_views when the core chose one: 0 takes the default, and the member pool follows the view count).
// The core calls it once after it set the bounds; a test that plays the core does the same. layout_version is left alone (a test writes a
// wrong one on purpose).
inline void Tier2DescribeRegion(Tier2RegionHeader& header)
{
    std::uint32_t const bots = header.max_bots;
    std::uint32_t const nearby = header.max_nearby_units;
    header.layout_base = kTier2LayoutBase;
    if (header.max_party_views == 0)
        header.max_party_views = Tier2PartyViewSlots(bots);
    header.max_party_members = Tier2PartyMemberSlots(bots, header.max_party_views);
    std::uint32_t const views = header.max_party_views;
    std::uint32_t const members = header.max_party_members;
    header.header_bytes = Tier2HeaderBytes();
    header.snapshot_bytes = Tier2SnapshotBufferBytes(bots, nearby, views, members);
    header.action_offset = Tier2ActionBufferOffset(bots, nearby, header.max_actions, views, members, 0);
    header.action_bytes = Tier2ActionBufferBytes(header.max_actions);
    header.action_header_bytes = std::uint32_t(sizeof(ActionQueueHeader));
    header.action_stride = std::uint32_t(sizeof(BotAction));
    header.section_count = 4;
    header.sections[0] = Tier2Section{ std::uint32_t(Tier2SectionKind::kBots), std::uint32_t(sizeof(BotSnapshot)), bots, 0,
                                       std::uint64_t(Tier2BotsSectionOffset()) };
    header.sections[1] = Tier2Section{ std::uint32_t(Tier2SectionKind::kNearbyUnits), std::uint32_t(sizeof(NearbyUnit)), bots * nearby, 0,
                                       std::uint64_t(Tier2NearbySectionOffset(bots)) };
    header.sections[2] = Tier2Section{ std::uint32_t(Tier2SectionKind::kPartyViews), std::uint32_t(sizeof(PartyView)), views, 0,
                                       std::uint64_t(Tier2PartyViewsSectionOffset(bots, nearby)) };
    header.sections[3] = Tier2Section{ std::uint32_t(Tier2SectionKind::kPartyMembers), std::uint32_t(sizeof(PartyMember)), members, 0,
                                       std::uint64_t(Tier2PartyMembersSectionOffset(bots, nearby, views)) };
    for (std::size_t i = 4; i < kMaxTier2Sections; ++i)
        header.sections[i] = Tier2Section{};
}

} // namespace forever_module
