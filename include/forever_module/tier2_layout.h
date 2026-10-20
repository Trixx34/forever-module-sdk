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
inline constexpr std::uint32_t kTier2LayoutVersion = 20;

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
    // Layout 13: auras on this unit. aura_mask is a coarse summary the core encodes so a module can tell
    // "has a DOT" / "has a poison" without filling a per-unit aura array; aura_count is the number of
    // auras the core chose to report on it. Mask bit assignment is core-defined and fixed per layout.
    std::uint32_t aura_count = 0;
    std::uint32_t aura_mask = 0;
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
    kCraft = 20,         // craft using a profession spell: spell_id = the profession spell/recipe id,
                         // target_guid = optional unit or object target (0 = self). Refused in combat.
    kGather = 21,        // gather a node (herb, ore, etc.): target_guid = the node/object unit reference,
                         // spell_id = the gathering spell id (optional). Refused in combat.
    kPetCommand = 22,    // command the bot's pet (layout 13). spell_id 1 = attack target_guid, 2 = follow the bot,
                         // 3 = stay; 4 = set stance (spell_id 4, dest_x = the stance value). Refused in combat
                         // and when the bot has no pet (status kInvalidTarget).
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
    kCraft = 27,         // SDK 0.17: make something that raises a secondary skill. spell_id is the skill line, 129 First Aid (bandages from cloth) or 185 Cooking
                         // (food from meat). The core picks, among the recipes the bot knows for that skill, the one that can still raise its skill, that it has the
                         // materials for and that needs the most skill, and casts it on the bot. A recipe that needs a cooking fire when none is near makes the
                         // bot light a campfire first (Basic Campfire); the next kCraft makes the food. kApplied when a cast started; kRefused with a reason when
                         // the bot has no recipe, no materials, or its skill is too high for any recipe it can make; kBusy while another cast runs; kMoving when the
                         // recipe has a cast time and the bot moves. Refused in combat and dead.
    kLastActionType = kCraft, // the highest valid value; PublishActions refuses anything above it
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
