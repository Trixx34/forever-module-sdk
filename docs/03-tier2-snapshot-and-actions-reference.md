# 3. Tier 2 reference: snapshot and actions

The authority is `include/forever_module/tier2_layout.h` (layout 22 at the time of writing; protocol `"20"`). This page restates its comments as tables, and
adds what the core's `ModuleBotActions.cpp` and `worldserver.conf.dist` say about each action. When this page and the header disagree, the header is right and
this page is stale. Field names, enum names and constants are the real ones, in namespace `forever_module`.

Conventions:

- A **unit reference** (`BotAction::target_guid`, `NearbyUnit::guid`, and every `*_guid` that is a unit in the snapshot) is a 64-bit value built by `MakeUnitRef`
  (below). A **raw character guid counter** is the plain number `BotSnapshot::guid` and `BotAction::bot_guid` use for a bot; Tier 1 calls that name a bot
  (`BuyItemForBot`, `SpendBotTalents`, `BotSpeak`, `BotTrade`) take the same raw counter.
- Distances are in yards, positions in the world's coordinates (`pos_x/y/z`, `orientation` in radians as the core gives it), times in milliseconds, money
  in copper.
- "Layout N" says when a field appeared. A field marked 0 / valid 0 means "none".
- `1 byte flag` fields are `std::uint8_t` and hold 0 or 1.

## The snapshot you read

```cpp
forever_module::Tier2Session* session = client.AttachTier2();
forever_module::WorldTickSnapshotView const& view = session->ReadLatestSnapshot();
// view.tick_number   the server tick this snapshot was published for
// view.bots          std::vector<BotSnapshot>, only the bots this module controls that are online (at most max_bots)
// view.NearbyOf(i)   std::span<NearbyUnit const> for view.bots[i], up to bots[i].nearby_count entries
```

`ReadLatestSnapshot()` copies the whole published snapshot (a vector of `BotSnapshot`, 1136 bytes each at layout 22, plus `max_nearby_units` `NearbyUnit`s (48 bytes) per bot
slot). It is non-blocking and returns the previous complete snapshot if the core was writing during all three attempts; before the first complete copy the
result is empty with `tick_number` 0, and `HasSnapshot()` is `false`.

### BotSnapshot

| Field | Type | Meaning |
|---|---|---|
| `guid` | `uint64` | The bot's raw character guid counter. Use it as `BotAction::bot_guid`. |
| `pos_x`, `pos_y`, `pos_z`, `orientation` | `float` | Position and facing. |
| `health`, `max_health` | `uint32` | Hit points. A ghost has health 1: test `life_state`, not `health`. |
| `power`, `max_power` | `uint32` | The bot's main resource, of kind `power_type`. |
| `power_type` | `uint8` | Layout 4. The core's `Powers` value: 0 mana, 1 rage, 3 energy. |
| `level` | `uint32` | Layout 4. |
| `threat_target_guid` | `uint64` | Unit reference of the bot's current target (0 none). |
| `spell_cooldowns[kMaxTrackedCooldowns]` | `SpellCooldown[8]` | Active cooldowns, closest to expiring first; `spell_id` 0 marks an unused slot. |
| `in_combat` | `uint8` | 1 while in combat. |
| `target_distance` | `float` | Yards to `threat_target_guid`'s unit; 0 with no target or when it is not in the snapshot's view. |
| `last_action_type` | `uint8` (`ActionType`) | Layout 3. The type of the last action the core applied to this bot. |
| `last_action_status` | `BotActionStatus` | Its result. See [Reading the result](#reading-the-result-of-an-action). |
| `last_action_spell` | `uint32` | The spell id, for a cast (the core fills it only for `kCastSpell`). |
| `last_action_sequence` | `uint32` | Counts the actions applied to this bot; a new result has a new sequence. |
| `nearby_count` | `uint32` | Layout 4. Number of valid entries in `NearbyOf(i)`. |
| `group_role` | `uint8` | Layout 5. 0 no party, 1 member, 2 leader. |
| `group_size` | `uint32` | Members in the bot's party. |
| `group_leader_guid` | `uint64` | Unit reference of the party leader (0 with no party). |
| `group_invite_from` | `uint64` | Unit reference of the leader whose invite the bot has not answered (0 none). |
| `vendor_x/y/z`, `vendor_valid` | `float`, `uint8` | Layout 6. Nearest vendor on the bot's own map; `vendor_valid` 0 if the map has none. |
| `vendor_guid`, `repair_guid`, `auctioneer_guid` | `uint64` | Layout 13. Unit references of the NPCs, for `kVendor`, `BuyItem` and `AuctionHouse` calls. |
| `map_id` | `uint32` | Layout 7. The map the bot is on. |
| `quest_count`, `quests[kMaxQuestLog]` | `uint8`, `QuestEntry[8]` | Layout 8. The first `quest_count` entries are the quest log. |
| `repair_x/y/z`, `repair_valid` | `float`, `uint8` | Layout 9. Nearest repairman friendly to the bot on its own map. |
| `durability_pct` | `uint8` | Lowest durability of equipped items in percent; 100 when nothing wears out. |
| `moving` | `uint8` | Layout 10. 1 while the bot moves. |
| `casting_spell`, `cast_remaining_ms` | `uint32` | Spell id of the cast or channel in progress (0 none), and what is left. A cast with a cast time cannot start while moving, and none while another runs. |
| `life_state` | `uint8` | Layout 11. 0 alive, 1 dead (corpse, spirit not released), 2 ghost (released). |
| `corpse_valid`, `corpse_map`, `corpse_x/y/z` | `uint8`, `uint32`, `float` | 1 when the corpse position is set: a ghost's corpse (maybe on another map than `map_id`) or a dead bot's own position. |
| `reclaim_wait_ms` | `uint32` | Ghosts only: time until the corpse may be reclaimed; 0 means now. |
| `resurrect_from` | `uint64` | Unit reference of the player whose resurrect offer is pending (0 none). |
| `aura_count`, `auras[kMaxAuras]` | `uint32`, `AuraEntry[16]` | Layout 13. The first `aura_count` auras the core reports on the bot. |
| `casting_target` | `uint64` | Unit reference of the target of the cast in progress (0 = self). |
| `pet_guid`, `pet_health`, `pet_max_health`, `pet_state` | `uint64`, `uint32` | The bot's pet: unit reference (0 none); `pet_state` 0 none, 1 alive, 2 dead. |
| `copper`, `bag_free_slots` | `uint64`, `uint32` | Money and free bag slots, for buy and hand-in checks. |
| `last_target_lost_reason` | `uint32` | Why the bot lost its last target: 0 none, 1 killed, 2 leashed, 3 despawned, 4 another player. |
| `ammo_entry`, `ammo_count` | `uint32` | Layout 14. The selected projectile (or the core's recommended one for the weapon and level; 0 = no ammo weapon or no data) and how many the bot carries (inventory, not bank). |
| `food_count`, `drink_count` | `uint32` | Layout 15. Carried food and drink the bot may use at its level; `drink_count` is 0 for a class without mana. |
| `ammo_vendor_*`, `food_vendor_*` | `uint8` valid, `float` x/y/z, `uint64` guid | Layout 15. Nearest vendor on the map that sells the ammunition the bot's weapon takes / food or drink it may use. Valid 0: none, or the bot needs none. |
| `health_potion_count`, `mana_potion_count`, `bandage_count` | `uint32` | Layout 16. Carried and usable at the bot's level (mana potions 0 for a class without mana). |
| `goods_vendor_*` | `uint8` valid, `float` x/y/z, `uint64` guid | Layout 16. Nearest reachable vendor selling any of those. |
| `reagent_entry`, `reagent_count`, `reagent_vendor_*` | `uint32`, `uint8`, `float`, `uint64` | Layout 16. The item a refused cast was missing (0 none), how many the bot has, and the nearest reachable vendor of it. |
| `taxi_master_*`, `in_flight` | `uint8`, `float`, `uint64` | Layout 17. Nearest flight master the bot can walk to, and whether the bot is on a flight (it takes no orders but a `kTaxi` that ends it; it lands by itself). |
| `profession_skill[2]`, `profession_value[2]`, `profession_max[2]` | `uint16` | Layout 18. The two primary profession skill lines the core assigned (0: none, an alt, or `Bot.Professions.Enabled` off), and the skill value and maximum in each (0 until learned). |
| `auctioneer_valid`, `auctioneer_x/y/z` | `uint8`, `float` | Layout 20. Nearest auctioneer the bot's faction may use, on its map. `auctioneer_guid` is the unit reference an `AuctionHouse` call names. |
| `ground_effect_count`, `ground_effects[kMaxGroundEffects]` | `uint8`, `GroundEffect[4]` | Layout 21. The first `ground_effect_count` entries. 0 when none, **or when the core's `Bot.Snapshot.GroundEffects` is off (default 0)**. |
| `damage_sequence`, `damage_hit_count`, `damage_hits[kMaxDamageHits]` | `uint32`, `uint8`, `DamageHit[8]` | Layout 22. The last hits the bot took, oldest first (the core keeps the last 8). |

### NearbyUnit

| Field | Type | Meaning |
|---|---|---|
| `guid` | `uint64` | Unit reference (`MakeUnitRef`). Use it as `BotAction::target_guid`. |
| `pos_x/y/z` | `float` | Position. |
| `health`, `max_health` | `uint32` | Hit points. |
| `is_hostile` | `bool` | Hostile to the bot. |
| `level` | `uint32` | Layout 4. |
| `elite` | `uint8` | 1 for an elite creature or boss. |
| `casting` | `uint8` | 1 while the unit casts a spell. |
| `aura_count`, `aura_mask` | `uint32` | Layout 13. Number of auras the core reports, and a coarse summary mask ("has a DOT", "has a poison"). The bit assignment is **core-defined and not documented in the SDK header**; the per-unit aura array does not exist, only the mask. |

How many are listed per bot is the negotiated `max_nearby_units` (default limit 16), chosen by the core (the header does not say in what order, so do not rely on one). The list may be several ticks old (see the nearby refresh note in [docs/02](02-architecture-and-lifecycle.md)).

### The small structs

| Struct | Fields | Notes |
|---|---|---|
| `SpellCooldown` | `spell_id`, `remaining_ms` (`uint32`) | `spell_id` 0 = unused slot. Up to `kMaxTrackedCooldowns` = 8 per bot. |
| `AuraEntry` | `spell_id`, `remaining_ms`, `stacks` (`uint32`) | `spell_id` 0 = unused slot. Up to `kMaxAuras` = 16. `remaining_ms` for an aura with no duration is a large sentinel, "core's choice, e.g. 0xFFFFFFFF". `stacks` is 1 when it does not stack. |
| `QuestEntry` | `quest_id` (`uint32`, 0 = unused), `status` (`uint8`: 1 incomplete, 2 complete and ready to hand in, 3 failed), `counts[kMaxQuestObjectives]` (`uint16[4]`) | `counts[i]` is the progress of objective *i* (a kill or item count), 0 for unused objectives. Up to `kMaxQuestLog` = 8 quests. |
| `GroundEffect` | `spell_id` (0 = unused), `x`, `y`, `z`, `radius`, `kind`, `hostile` | A dynamic object (a spell's persistent area: Flamestrike, Blizzard) or an area trigger within `kGroundEffectRangeYards` = 30 of the bot, the one the bot stands in or nearest first. `radius` yards (0 = not known). `kind`: 1 dynamic object, 2 area trigger. `hostile` 1 when the caster is hostile to the bot. Up to `kMaxGroundEffects` = 4. |
| `DamageHit` | `source_guid` (`uint64`), `spell_id`, `amount`, `sequence` (`uint32`) | `source_guid` is the attacker's unit reference (0 for an environmental source, or one that cannot be packed). `spell_id` 0 = melee or auto attack. `amount` as the core applied it. `sequence` counts every hit on the bot since it came online. Up to `kMaxDamageHits` = 8, oldest first. |

**Reading new damage.** Keep, per bot, the highest `sequence` you have processed; the hits with a higher `sequence` are new. `damage_sequence` is the newest
hit's sequence (0 before the first), so `bot.damage_sequence == seen` means nothing new. If more than 8 hits arrive between two snapshots, the oldest are gone.

### Unit references

```cpp
constexpr std::uint64_t MakeUnitRef(std::uint8_t highType, std::uint32_t entry, std::uint64_t counter);
// bits 56-63 HighGuid type, bits 32-55 creature/object entry, bits 0-31 counter
constexpr std::uint8_t  UnitRefType(std::uint64_t ref);
constexpr std::uint32_t UnitRefEntry(std::uint64_t ref);
constexpr std::uint64_t UnitRefCounter(std::uint64_t ref);
```

- Type **2** is a player (entry 0, counter = the player's database guid). Type **8** is a creature (entry, counter as on the map).
- A counter above 32 bits or an entry above 24 bits cannot be packed; the core refuses such units (`kUnitRefCounterMask`, `kUnitRefEntryMask`).
- Reference **0** means "no unit", and for a spell target, the bot itself.
- To target a player or party leader, use the reference the snapshot gave you (`group_leader_guid`). To name a bot of your own as a unit (for a party invite),
  `MakeUnitRef(2, 0, bot.guid)` is the form the header describes for a player; this was not exercised end to end here.

## Publishing actions

```cpp
struct BotAction {
    std::uint64_t bot_guid;      // the bot (BotSnapshot::guid)
    ActionType    type;
    std::uint32_t spell_id;      // spell, item entry, skill line, map, bitmask or sub-command, depending on type
    std::uint64_t target_guid;   // a unit reference (MakeUnitRef); 0 = self for kCastSpell
    float dest_x, dest_y, dest_z;// a destination, or a count/target number, depending on type
};
bool Tier2Session::PublishActions(std::vector<BotAction> const& actions, std::string& error);
```

`BotAction` is 40 bytes. The three float fields are overloaded by some actions as plain numbers (a count in `dest_x`). `PublishActions` refuses: no snapshot read yet,
more actions than `MaxActions()`, an `ActionType` above `kLastActionType` (28). The core checks ownership and every precondition itself.

### ActionType

All values, from `tier2_layout.h`. "Switch" is the `worldserver.conf` key the core consults (defaults from `worldserver.conf.dist`).

| Value | Name | Arguments | Notes and refusals | Switch |
|---|---|---|---|---|
| 0 | `kCastSpell` | `spell_id` = spell; `target_guid` = target (0 = self) | Result in the snapshot: `kApplied` means the cast was accepted, it may still fail while it runs. `kUnknownSpell`, `kNotKnown`, `kOutOfRange`, `kOnCooldown`, `kNotEnoughPower`, `kNoLineOfSight`, `kInvalidTarget`, `kBusy`, `kMoving`. | none |
| 1 | `kMoveTo` | `dest_x/y/z` | Refused for an invalid map position. `kNoRoute` when the navmesh has no full route: the bot **still walks** to the nearest reachable point. | none |
| 2 | `kAttackTarget` | `target_guid` | Must be a live hostile unit on the bot's map; `kInvalidTarget` otherwise; `kRefused` with a reason if the core's attack call refuses. Repeating it on a target already being meleed is `kApplied`. | none |
| 3 | `kFollow` | `target_guid` | `kInvalidTarget` if the target is not a unit on the bot's map or is the bot. Follows at distance 1.0. | none |
| 4 | `kStop` | none | Stops movement, clears the motion queue. | none |
| 5 | `kRest` | `spell_id` 1 sits, 0 stands | Sitting refused in combat. | none |
| 6 | `kVendor` | `spell_id` = bitmask, see below; `target_guid` = vendor (optional); `dest_x/y/z` = buy targets | Services at an NPC within interaction range. Refused in combat and with no vendor/repairman in range. | buy bits need `Bot.Actions.Supplies` (1) |
| 7 | `kTrain` | none | Learns every spell the trainer in range teaches this bot now. Refused in combat, with no trainer in range, when the trainer teaches nothing, or a profession outside the bot's pair. | none |
| 8 | `kGroup` | `spell_id` 1 invite `target_guid`, 2 accept the pending invite, 3 leave, 4 convert own party to raid, 5 move `target_guid` to subgroup `dest_x` (0 to 7) | Refused in combat. Invite: the target must be a bot **this module controls**, the bot must be (or becomes) the leader, same team, party not full. 4 and 5: leader/assistant only, group must be a raid for 5. | none |
| 9 | `kLoot` | `target_guid` = corpse | Takes every item the bot may and the money. The corpse must be dead, within 30 yards, and one the bot may loot. An empty corpse is not an error. | none |
| 10 | `kQuest` | `spell_id` = `(operation << 24) \| quest id`; operation 1 accept, 2 hand in, 3 abandon | Quest ids must fit 24 bits (`ModuleClient::QuestAccept` etc. refuse larger ones). Accept and hand-in need the giver/ender in range and are refused in combat; abandon needs no NPC. | none |
| 11 | `kTeleport` | `spell_id` = map; `dest_x/y/z` | Scenarios and tests. Refused in combat. A bot in a party enters the party's instance. | **`Bot.Actions.Teleport` (default 0)** |
| 12 | `kGrant` | `spell_id` = item entry; `dest_x` = count (at least 1) | Scenarios and tests. Refused for an unknown item, a count outside 1..stack size, or full bags. | **`Bot.Actions.Grant` (default 0)** |
| 13 | `kAutoGear` | `spell_id` bit 1 = tank (one-hand weapon and shield) | Dresses the bot for its level, uncommon quality at most (`Bot.AutoGear.MaxQuality`). Refused in combat, for a charmed bot, and for an alt. The core's `Bot.Actions.EquipUpgrades` (default 1) gates a second, bag-only mode (wear a carried item that is better); how that mode is encoded in `spell_id` is not in the header, so ask the core's `ModuleBotActions.cpp` (`AutoGearAction`) before using it. | `Bot.Actions.AutoGear` (default 1) |
| 14 | `kReleaseSpirit` | none | Dead bot becomes a ghost at the graveyard. Refused when alive, already a ghost, being teleported, or in combat. | none |
| 15 | `kResurrect` | `target_guid` = resurrecter (`BotSnapshot::resurrect_from`); `spell_id` 0 accepts, 1 declines | Answers a pending resurrect offer. | none |
| 16 | `kReclaimCorpse` | none | A ghost within the reclaim radius of its corpse, once the delay (`reclaim_wait_ms`) is over. | none |
| 17 | `kSummon` | `target_guid` = player; `dest_x` = spread index | A chat order: puts the bot next to that player. Refused for a dead or fighting bot, a fighting speaker, an instance on another map, or when the bot is within 5 yards already. | **`Bot.Chat.Orders.Enabled` (default 0)** |
| 18 | `kRevive` | `target_guid` = player | A chat order: resurrects the dead bot at 35 % health next to the player. Refused on a hardcore realm. | **`Bot.Chat.Orders.Enabled`** |
| 19 | `kMount` | `spell_id` 1 mounts on the fastest learned ground mount, 0 dismounts | Refused in combat. | none |
| 20 | `kCraft` | `spell_id` = the recipe's spell id | A recipe of Alchemy, Blacksmithing, Engineering, Leatherworking, Tailoring, Smelting, First Aid or Cooking the bot knows, cast on the bot. `kNeedsFocus`, `kRefused`, `kNotKnown`, `kBusy`, `kMoving` as in the header. | none |
| 21 | `kGather` | `spell_id` = skill line: 186 Mining, 182 Herbalism, 393 Skinning | Looks for something to gather within 30 yards; starts the bot walking when it is more than 4 yards away (`kMoving`: ask again); in reach, casts. Loot is taken with `kLoot`. `kRefused` with a reason if there is nothing, no skill, combat or dead; `kBusy` while casting. | none |
| 22 | `kPetCommand` | `spell_id` 1 attack `target_guid`, 2 follow the bot, 3 stay, 4 stance (`dest_x` = stance value) | Refused in combat and with no pet (`kInvalidTarget`). | none |
| 23 | `kConsume` | `spell_id` 1 best food, 2 best drink, 3 best health potion, 4 best mana potion, 5 best bandage | Eat/drink: refused in combat and when the bot carries none. 3 and 4 are allowed in combat (potion cooldown applies). 5 is out of combat, the bot stands still while it channels, and is refused while it has "Recently Bandaged". | `Bot.Actions.Supplies` (default 1) |
| 24 | `kTaxi` | `target_guid` = flight master (0 = nearest in reach); `dest_x/y/z` = the point; `spell_id` = the bot's map id | Flies to the flight node nearest the point on the bot's own map. Refused in combat, on a flight, with no master in reach, no flight path, or when the flight would not bring the bot at least `Bot.Taxi.MinGainYards` (600) closer. | `Bot.Taxi.Enabled` (1), `Bot.Taxi.Free` (1) |
| 25 | `kUseObject` | `spell_id` = object entry; `dest_x/y/z` = where it stands; `target_guid` = an **item entry** to use on it (0 = a plain click) | The nearest object of that entry within 30 yards; the bot must be within 8 yards (`kOutOfRange` otherwise). With an item entry the bot is given one if it has none. | **`Bot.Actions.UseObject` (default 0)** |
| 26 | `kSellToVendor` | `spell_id` = item entry; `dest_x` = count (0 = all); `target_guid` = vendor (0 = nearest in reach) | Refused in combat, dead, with no vendor in reach, or when the bot has none of the entry. Items the vendor will not take are kept and reported as `kRefused` with a reason. | none |
| 27 | `kSkillUp` | `spell_id` = skill line: 129 First Aid, 185 Cooking, 164 Blacksmithing, 165 Leatherworking, 171 Alchemy, 186 Smelting, 197 Tailoring, 202 Engineering | The core picks the recipe that can still raise the skill, has materials, and needs the most skill. A recipe needing a cooking fire with none near makes the bot light a campfire first. `kNeedsFocus` when only recipes needing a missing anvil/forge/fire remain. Refused in combat and dead. | none |
| 28 | `kGossip` | `spell_id` = creature entry; `dest_x` = option index in the menu, from 0 | Talks to the nearest live creature of that entry within 30 yards (must be within interaction range, else `kOutOfRange`), out of combat and alive, and picks the option, which starts a scripted event such as an escort. `kInvalidTarget` when no such creature, `kRefused` when no gossip/option or the switch is off. | **`Bot.Actions.Gossip` (default 0)** |

`kLastActionType` (= `kGossip`, 28) is the highest valid value. In the header `kQuest = 10` is declared after `kAutoGear = 13`; the values, not the order of
declaration, are what the wire carries.

#### kVendor bitmask (`spell_id`)

| No. | Value | Meaning |
|---|---|---|
| 1 | `1` | Sell grey items to the vendor. |
| 2 | `2` | Repair at a repairman. |
| 3 | `4` | Equip the best gear in the bags. |
| 4 | `8` | **Buy ammunition** up to `dest_x` items. Needs `Bot.Actions.Supplies`. |
| 5 | `16` | **Buy food** up to `dest_y` items. |
| 6 | `32` | **Buy drink** up to `dest_z` items. |
| 7 | `64` | Buy health potions up to the core's `Bot.Supplies.HealthPotionTarget`. |
| 8 | `128` | Buy mana potions up to `Bot.Supplies.ManaPotionTarget`. |
| 9 | `256` | Buy bandages up to `Bot.Supplies.BandageTarget`. |
| 10 | `512` | Buy the reagent the snapshot names (`reagent_entry`) up to `Bot.Supplies.ReagentTarget` pieces. |
| 11 | `1024` | Also sell white junk with the grey items (spare bags, miscellaneous items, the surplus of trade goods beyond 60 of a kind); never quest items, equipped bags, consumables, reagents, ammunition, weapons or armour. |

The core picks the item from the vendor's stock (ammunition the bot's weapon takes; the best food or drink usable at the bot's level) and buys until the bot
carries the target or its money or bags run out; a vendor without the goods is not an error. The core keeps a money reserve (`Bot.Supplies.ReserveCopperPerLevel`
per level) from every purchase except ammunition. The buy counts for 8/16/32 are clamped to the core's `kMaxSupplyTarget`, a value I did not read. The
vendor is `target_guid` if it names a vendor/repairman in interaction range, else the nearest in range. The buying bits are ignored (the sell/repair part
still runs) when `Bot.Actions.Supplies` is 0.

The `ModuleClient::Vendor(bot, vendor, spellId)` Tier 1 call exists with the same bitmask meanings for 1, 2 and 4, but the header says buying (8 to 32) is a Tier 2
action.

### ModuleClient convenience methods and a mismatch

`ModuleClient::MoveTo`, `AttackTarget`, `Follow`, `Stop`, `Rest`, `Train`, `Loot`, `GroupInvite/Accept/Leave`, `QuestAccept/TurnIn/Abandon`, `Teleport`, `Grant`,
`AutoGear`, `ReleaseSpirit`, `Resurrect`, `ReclaimCorpse`, `Summon`, `Revive`, `Mount`, `Craft`, `Gather`, `PetCommand`, `CastSpell` each build one `BotAction`
(see `src/module_client.cpp`) and queue it; the queue is published after the callbacks of one `Run()` pass, attached to the snapshot most recently read. They
need a snapshot to have been read (`AttachTier2()->ReadLatestSnapshot()`), accept an `onReply` that is **never called** (the result comes in the snapshot), and on
a failure do nothing and set `tier2_error()`. There is no convenience method for `kVendor` buying, `kConsume`, `kTaxi`, `kUseObject`, `kSellToVendor`,
`kSkillUp` or `kGossip`: build the `BotAction` yourself.

**Known mismatch in 0.24.0:** `ModuleClient::Gather(botGuid, targetGuid)` puts its second argument in `target_guid`, but `kGather` (header and core) takes the
**skill line in `spell_id`** and ignores the target. Use a hand-built `BotAction{ bot, ActionType::kGather, 186 /*or 182, 393*/, 0, 0, 0, 0 }` until it is
fixed. (`ModuleClient::Craft` does match: `spell_id` = recipe.)

## Reading the result of an action

The core applies a published set on a later world tick and records, **per bot, the result of the last action it applied** (`last_action_type`,
`last_action_status`, `last_action_spell` for casts, `last_action_sequence`). The next snapshot carries them. Compare `last_action_sequence` with the one you saw
for that bot; a different value is a new result. If you send several actions for one bot in a tick, only the last one's result survives.

`BotActionStatus` (`enum class : uint8_t`):

| Value | Name | Meaning |
|---|---|---|
| 0 | `kNone` | No action was applied for this bot yet. |
| 1 | `kApplied` | Applied (a cast was accepted; it may still fail while it runs). |
| 2 | `kRefused` | Refused for another reason; the core logs the reason (debug level per action, and a once-a-minute info summary of the commonest). |
| 3 | `kUnknownSpell` | The spell id does not exist. |
| 4 | `kNotKnown` | The bot has not learned the spell. |
| 5 | `kOutOfRange` | Out of range. |
| 6 | `kOnCooldown` | On cooldown. |
| 7 | `kNotEnoughPower` | Not enough mana, rage or energy. |
| 8 | `kNoLineOfSight` | No line of sight. |
| 9 | `kInvalidTarget` | No unit for the target reference, or not a valid target. |
| 10 | `kNotOnline` | The bot was not in the world when the action ran. |
| 11 | `kBusy` | A cast: the bot is already casting; try again when it is done. |
| 12 | `kMoving` | A cast: it has a cast time and the bot is moving; stop first. |
| 13 | `kNoRoute` | A move: no full navmesh route; the bot walks to the nearest reachable point and stops (the move is still applied). Only for `kMoveTo`. |
| 14 | `kNeedsFocus` | A craft or cast needs a spell focus object (anvil, forge, cooking fire) within reach and there is none; walk to one and retry. Distinct from `kRefused`. |

The **reason text** of a refusal is not in the snapshot; only the status code is. The reason is in the core's log (`modules` debug lines, and the once-a-minute
`Modules: N action(s) refused ...` summary). Tier 1 actions are different: their `ActionReply` carries `success` and a `reason` string.

## Tier 1 actions in short

These go through `ModuleClient` and are answered later in an `ActionReply` (`success`, `reason`, and per-call fields); the callback runs on the `Run()` thread.
The core runs them on the world thread, 32 per tick.

| Call | Does | Reply fields |
|---|---|---|
| `SellItem(player, item, entry, count)` | Sells one whole stack of an item a player carries (count must equal the stack; the entry must match). | `copper_gained` |
| `SendPlayerMessage(player, text)` | System message to a player. | |
| `ShareQuestItem(player, entry, count, rangeYards=0)` | Gives party members who need it a copy of a quest item the player looted. | `recipients` |
| `Log(level, text)` | A line in the core's `Server.log` tagged with the module; level 0 info, 1 warning, 2 error; 1 to 255 bytes. | |
| `BuyItem(vendor, entry, count)`, `BuyItemForBot(bot, vendor, entry, count)` | Buys from a vendor; the core checks range, sell list, money. | `copper_spent` |
| `Vendor(bot, vendor, spellId)` | Services bitmask 1, 2, 4 at an NPC in range. Refused in combat. | |
| `AuctionHouse...`, `AuctionHouseForBot`, `AuctionBrowseByFilterForBot` | Bid, buyout, list, cancel, browse (by item, or by filter since protocol 19). | `listings`, `item_entry`, `auction_guid` |
| `BotSpeak(bot, kind, player, text)` | A bot says (kSay) or whispers (kWhisper) 1 to 255 bytes. | |
| `BotTrade(bot, op, slot, item, copper)` | Drives a bot's trade window (kBegin, kSetItem, kClearItem, kSetGold, kAccept, kCancel, kListBags). | `bag_items` |
| `SpendBotTalents(bot, role[, spec])` | Spends the bot's talent points by profile (role kTank/kHealer/kDps; spec from `talent_spec.h`, 1 to 28). Out of combat, level 10 or more. | |
| `StoreGet/Set/Delete/List` | The module store; see [docs/02](02-architecture-and-lifecycle.md). | `found`, `value`, `keys` |

`Bot.Trade.Enabled` (default 0) and `Bot.Alt.Enabled` (alts via `SpawnParams.alt_name`) are in `worldserver.conf.dist`; which Tier 1 calls each one gates was not traced for
this page, so check the core before relying on them being on.

## Common patterns

Each is a `BotAction` to put into the vector you pass to `PublishActions`. `b` is the `BotSnapshot`.

```cpp
using namespace forever_module;

// Move: walk to a point on the bot's current map. Re-issue only when the bot stopped (b.moving == 0) or the goal changed.
BotAction{ b.guid, ActionType::kMoveTo, 0, 0, x, y, z };

// Attack: target is a NearbyUnit::guid. The bot swings; it casts only when you send kCastSpell.
BotAction{ b.guid, ActionType::kAttackTarget, 0, unit.guid, 0, 0, 0 };

// Follow: the party leader's reference from the snapshot, or any unit reference.
BotAction{ b.guid, ActionType::kFollow, 0, b.group_leader_guid, 0, 0, 0 };

// Rest: sit when out of combat and low; stand with spell_id 0 (refused sitting in combat).
BotAction{ b.guid, ActionType::kRest, 1, 0, 0, 0, 0 };

// Vendor: sell greys, repair and equip, at the vendor the snapshot names (walk there first; interaction range is checked by the core).
BotAction{ b.guid, ActionType::kVendor, 1 | 2 | 4, b.vendor_guid, 0, 0, 0 };
// ...and buy 200 arrows, 20 food, 20 drink on top (bits 8, 16, 32): the counts go in dest_x, dest_y, dest_z.
BotAction{ b.guid, ActionType::kVendor, 8 | 16 | 32, b.food_vendor_guid, 200, 20, 20 };

// Quest: accept quest 783 from a giver in range; hand in is operation 2, abandon 3.
BotAction{ b.guid, ActionType::kQuest, (1u << 24) | 783u, 0, 0, 0, 0 };

// Group: invite another bot you control (target is its unit reference), accept, leave.
BotAction{ b.guid, ActionType::kGroup, 1, otherBotRef, 0, 0, 0 };
BotAction{ other.guid, ActionType::kGroup, 2, 0, 0, 0, 0 };

// Teleport (only with Bot.Actions.Teleport = 1): map 0, x y z.
BotAction{ b.guid, ActionType::kTeleport, 0, 0, x, y, z };

// Use an object (only with Bot.Actions.UseObject = 1): a door whose entry is 17153, standing at x y z, within 8 yards.
BotAction{ b.guid, ActionType::kUseObject, 17153, 0, x, y, z };

// Gossip (only with Bot.Actions.Gossip = 1): creature entry 3678, first menu option.
BotAction{ b.guid, ActionType::kGossip, 3678, 0, 0, 0, 0 };
```

These are brace-initialisations of the aggregate `BotAction` in declaration order (`bot_guid, type, spell_id, target_guid, dest_x, dest_y, dest_z`). The numbers
(783, 17153, 3678) are placeholders, not checked against the game data.

Rules every pattern shares: send one action per bot per tick unless you do not need the results; do not repeat an order every tick that the bot is already
carrying out (`MoveTo`, `Follow` and `Attack` are not free: `MoveTo` and `Follow` clear the motion queue and restart); check `in_combat`, `life_state`,
`casting_spell` and `moving` before issuing something the core refuses in those states; and treat any status other than `kApplied` as information, not
an error. The stock bots module backs off a repeatedly refused move with a "move gate" for exactly this reason.
