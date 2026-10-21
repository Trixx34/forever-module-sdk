# Changelog

## SDK package 0.19.0 (2026-10-21)

- Layout 21, protocol 16: `GroundEffect` (spell id, position, radius, kind, hostile flag) and `BotSnapshot::ground_effect_count` / `ground_effects[kMaxGroundEffects = 4]`, the harmful-looking
  things on the ground within 30 yards of a bot: a dynamic object (a spell's persistent area: Flamestrike, Blizzard, a poison cloud) or an area trigger. The one the bot stands in or
  nearest to comes first. The core fills them only when `Bot.Snapshot.GroundEffects` is on. `sizeof(BotSnapshot)` is now 936 (it was 840), `sizeof(GroundEffect)` 24. A breaking
  change (the shared structs grew), so `kTier2LayoutVersion` 21 and `protocol_version` `"16"`; every module must be rebuilt (module.json `protocol_version` `"16"`).
  `kLastActionType` stays `kSkillUp`.

## SDK package 0.18.0 (2026-10-21)

- `BotActionStatus::kNeedsFocus` (14): a craft or a cast needs a spell focus object (anvil, forge, cooking fire) within reach and there is none; the module walks to one and
  tries again. Distinct from `kRefused`, which no walk cures.
- `ActionType::kCraft` (20) is implemented: craft one named recipe (`spell_id` = the recipe's spell id).
- `ActionType::kGather` (21) is implemented: `spell_id` is a gathering skill line (186 Mining, 182 Herbalism, 393 Skinning); the core finds a node or a skinnable corpse within
  30 yards, walks the bot to it and casts the gathering spell.
- `ActionType::kSkillUp` (27) accepts 164 Blacksmithing, 165 Leatherworking, 171 Alchemy, 186 Smelting, 197 Tailoring and 202 Engineering besides 129 and 185, and no longer
  picks a recipe whose focus is not near while another is possible. New capabilities, so a MINOR bump; no layout change: `protocol_version` stays `"15"`,
  `kTier2LayoutVersion` stays `20`. `kLastActionType` stays `kSkillUp`.

## SDK package 0.17.0 (2026-10-20)

- `ActionType::kSkillUp` (27): make something that raises a secondary skill. `spell_id` is the skill line, 129 First Aid (bandages from cloth) or 185 Cooking (food
  from meat); the core picks the recipe (one the bot knows, that can still raise its skill, that it has the materials for, that needs the most skill) and casts it,
  lighting a campfire first when a cooking recipe needs one. `kLastActionType` is now `kSkillUp`. (`kCraft` (20), craft a recipe the module names, stays reserved and is not implemented.) A new capability, so a MINOR bump; no layout or wire change:
  `protocol_version` stays `"15"`, `kTier2LayoutVersion` stays `20`.

## SDK package 0.16.0 (2026-10-20)

- `ActionType::kVendor` gets bit 1024: sell white junk with the grey items of bit 1, or alone: spare bags, miscellaneous items and the surplus of trade goods (a bot keeps
  60 pieces of each kind for First Aid, Cooking or the auction house). Never a quest item, an equipped bag, a consumable, a reagent, ammunition, a weapon or armour. For a
  bot whose bags are nearly full. A new capability, so a MINOR bump;
  no layout or wire change: `protocol_version` stays `"15"`, `kTier2LayoutVersion` stays `20`.

## SDK package 0.15.0 (2026-10-20)

- The package version was still the first release number, `0.1.0`, while the SDK had gone through 15 protocol revisions (layout 20). Each of those was a breaking
  change, which is a MINOR bump while the SDK is pre-1.0, so the version is now `0.15.0` (`kSdkPackageVersion` and the project VERSION; the round trip test checks they
  agree). The scheme is in the forever-ops docs, `96-versioning.md`. No wire or layout change: `protocol_version` stays `"15"`, `kTier2LayoutVersion` stays `20`.

## Layout 20 / protocol 15 (auction house and vendor sales)

- `BotSnapshot` gains `auctioneer_valid`, `auctioneer_x`, `auctioneer_y` and `auctioneer_z`: the nearest auctioneer the bot's faction may use, on the bot's map, so the module can walk to the house. `auctioneer_guid` (layout 13) is the unit reference an AuctionHouse action names.
- `ActionType::kSellToVendor` (26): sell items of one entry from the bags to a vendor in reach (`spell_id` = item entry, `dest_x` = count, 0 = all; `target_guid` = vendor, 0 = nearest). `kLastActionType` is now `kSellToVendor`.
- `AuctionListingEvent` from a kBrowse now carries the browsing bot (`player_guid`, the request's `player_guid` field 9) and the request's `message_id` (`request_id`); the core filled neither before.

## Layout 19 / protocol 14 (use an object)

- `ActionType::kUseObject` (25): click a door, lever or altar by its entry, or use an item on it (`target_guid` = item entry). Needs `Bot.Actions.UseObject` in the core. `kLastActionType` is now `kUseObject`.

## Layout 18 / protocol 13 (professions)

- `BotSnapshot` gains `profession_skill[2]`, `profession_value[2]` and `profession_max[2]`: the two primary professions the core assigned to the bot and its skill in each. Only combinations that
  make sense are assigned (two gathering professions, or Mining + Engineering, Mining + Blacksmithing, Skinning + Leatherworking, Alchemy + Herbalism, Tailoring + Enchanting).

## Layout 17 / protocol 12 (flight points)

- `BotSnapshot` gains `taxi_master_*` (the nearest flight master the bot can walk to: valid flag, x/y/z, unit reference) and `in_flight`.
- `ActionType::kTaxi` (24): fly from a flight master to the node nearest to a point on the bot's map. Bots know every node and fly free. `kLastActionType` is now `kTaxi`.

## Layout 16 / protocol 11 (bot supplies 2: potions and bandages)

- `BotSnapshot` gains `health_potion_count`, `mana_potion_count`, `bandage_count` and the nearest reachable vendor that sells any of them
  (`goods_vendor_*`).
- `ActionType::kVendor` buying: `spell_id` bit 64 health potions, 128 mana potions, 256 bandages (counts from the core's settings).
- `ActionType::kConsume`: `spell_id` 3 health potion, 4 mana potion, 5 bandage.
- Reagents: `reagent_entry`, `reagent_count` and `reagent_vendor_*` name the item a refused cast was missing (the core notes it when a cast
  fails for lack of reagents) and the nearest reachable vendor that sells it; `kVendor` bit 512 buys it.
- The core keeps back a money reserve from purchases (everything except ammunition) and checks that the vendors it reports can be reached on foot.

## Layout 15 / protocol 10 (bot supplies: ammunition, food and drink)

- `BotSnapshot` gains `food_count`, `drink_count` (what the bot carries and may use) and the nearest vendor that sells its ammunition and the nearest
  that sells food or drink it may use (`ammo_vendor_*`, `food_vendor_*`: valid flag, x/y/z, unit reference).
- `ActionType::kVendor` gains buying: `spell_id` bit 8 buys ammunition, 16 food, 32 drink, with the target counts in `dest_x`, `dest_y`, `dest_z`.
  The core chooses the items from the vendor's stock.
- `ActionType::kConsume` (23): eat (`spell_id` 1) or drink (2) the best food or drink the bot carries. `kLastActionType` is now `kConsume`.
- The core now fills `ammo_entry` and `ammo_count` (declared in layout 14 but never set).

## Layout 14 / protocol 9 (bot speech and bot trade)

Tier 1 only; the Tier 2 layout is unchanged (still 14).

- `ActionRequest` gains `bot_speak` (20, `BotSpeakAction`: a bot's /say or a whisper to a player) and `bot_trade` (21, `BotTradeAction`: begin, set
  item, clear item, set gold, accept, cancel, list bags). `ActionReply` gains `bag_items` (12, `TradeBagItem`) for the list-bags reply.
- `EventNotify` gains `bot_trade` (19, `BotTradeEvent`: a trade request, the window opening, the partner changing or accepting, a cancel, a
  completion). `server_stats` stays 18.
- `ModuleClient::BotSpeak` and `ModuleClient::BotTrade`.

## Layout 14 / protocol 8 (ammo replenishment and economy actor identity)

- `BotSnapshot` gains `ammo_entry` (active or core-recommended compatible projectile) and `ammo_count` (carried inventory, not bank).
- `BuyItemAction` and `AuctionHouseAction` gain `player_guid`, the controlled bot UnitRef whose money/items are used.
- `AuctionListingEvent` gains `player_guid` and `request_id` so concurrent browse results can be correlated to the requesting bot/action.

## Layout 13 / protocol 7 (auras, pets, vendor guids, and policy facts)

- `BotSnapshot` gains `aura_count` and `auras[kMaxAuras]` (16, `AuraEntry`: spell_id, remaining_ms, stacks) — the aura
  list replaces the module's DOT and buff timers. `NearbyUnit` gains `aura_count` and `aura_mask` (a coarse summary, the
  bit assignment is core-defined and fixed per layout).
- `BotSnapshot` gains `casting_target` (the target of the cast in progress, 0 = self) for heal claims and assist targeting.
- `BotSnapshot` gains `pet_guid`, `pet_health`, `pet_max_health` and `pet_state` (0 none, 1 alive, 2 dead), and
  `ActionType` gains `kPetCommand` (22): spell_id 1 = attack target_guid, 2 = follow the bot, 3 = stay, 4 = stance
  (stance in dest_x). `kLastActionType` is now `kPetCommand`. **Note: kPetCommand is 22, not 20 — kCraft (20) and
  kGather (21) were added in layout 12.**
- `BotSnapshot` gains `vendor_guid`, `repair_guid` and `auctioneer_guid`, so vendor buying, repair and the auction house
  no longer have to take an NPC's guid from the nearby list.
- `BotSnapshot` gains `copper` (money in copper) and `bag_free_slots`, for buy and hand-in pre-checks.
- `BotSnapshot` gains `last_target_lost_reason` (0 none, 1 killed, 2 leashed, 3 despawned, 4 another player), so a
  fight_end reason=target_gone can be split into its causes.
- `ActionReply` gains `copper_spent` (BuyItem: money taken from the player) and `item_entry` / `auction_guid`
  (AuctionHouse kList: the auction core created).
- `PlayerScript.OnAuctionResult` (`AuctionListingEvent`, events.proto field 17): one event per matching auction of a
  browse, carrying auction_guid, item_entry, count, bid, buyout, time_left_min, seller, query_item and total_matches.
- `ModuleClient::PetCommand(botGuid, cmd, targetGuid)` publishes the new `kPetCommand`. `BuyItem` and `AuctionHouse`
  are Tier 1 actions and were already in protocol 6.
- `kProtocolVersion` bumped from "6" to "7". A module built against layout 12 is refused at region attach.

## Layout 12 / protocol 6 (crafting and gathering)

- `ActionType` gains `kCraft` (20) and `kGather` (21); `kLastActionType` is now `kGather`.
  `kProtocolVersion` bumped from "5" to "6". A module built against layout 11 is refused at region attach.

## Layout 11 / protocol 5 (dead-bot recovery, breaking for layout 10 modules)

- `BotSnapshot` gains `life_state` (0 alive, 1 dead, 2 ghost), `corpse_valid`, `corpse_map`, `corpse_x/y/z`, `reclaim_wait_ms` and `resurrect_from`.
  A ghost has health 1: test `life_state`, not `health`.
- `ActionType` gains `kReleaseSpirit` (14, refused in combat), `kResurrect` (15, `target_guid` = resurrecter ref, `spell_id` 0 accept / 1 decline),
  `kReclaimCorpse` (16) and `kLastActionType`. `PublishActions` accepts every type up to `kReclaimCorpse`.
- `kProtocolVersion` bumped from "4" to "5" (core `kModuleProtocolVersion` too). A module manifest declaring protocol "4" is refused.
  A module built against layout 10 is refused at region attach.
## Unreleased: server stats event

- Tier 1 `ServerStatsEvent` (events.proto, `EventNotify.server_stats` field 16), delivered to modules that list the hook
  `WorldScript.OnStats`: players online, queued sessions, uptime and world update diff (mean, max, ticks). Bot figures are the bots module's to report. Sent every `Modules.Stats.IntervalSec` seconds (off when 0). Additive; an old core refuses the hook name at handshake.
  Protocol version unchanged.

## Unreleased (no protocol or layout change)

- `ActionType` gains `kSummon` (17), `kRevive` (18) and `kMount` (19); `kLastActionType` is now `kMount`. Chat orders of the bots module; the core refuses
  them unless `Bot.Chat.Orders.Enabled` is on. `SpawnParams.alt_name`/`owner_guid` and `ControlResponse.race`/`class_id` (alts as bots). Additive.

- Tier 1 hook `PlayerScript.OnChat` with `PlayerChatEvent` (events.proto field 16): the lines a group leader says in party or raid chat to a group that has
  a bot in it, and whispers to a bot. Additive; modules that do not subscribe are unaffected.

- Tier 1 `ShareQuestItemAction` (actions.proto field 16, `ModuleClient::ShareQuestItem`) and `ActionReply.recipients` (field 8): give a quest
  item a player looted to the party members who still need it. Additive. Old cores answer it as an unknown action.
- `ActionType::kLoot` (9): loot the corpse named by `target_guid`. Old cores refuse it as an unknown action type, so it is additive.
  `PublishActions` accepts every type up to `kLoot`.
- Build fix for Boost 1.87 and newer: `boost::asio::socket_base::receive_timeout` and the timed
  `basic_socket::wait(wait_type, duration, error_code)` were both removed from Asio, so the SDK did not
  compile against 1.90. The handshake receive timeout is now set through an equivalent `SO_RCVTIMEO` option
  spelled out in `module_client.cpp`, and the timed wait uses `poll(2)` on the socket's native descriptor,
  which is what Asio did internally. Behaviour and wire protocol unchanged; the declared Boost minimum (1.78)
  still holds and now also builds on 1.90.
- Build fix: `ModuleClient::AuctionHouse` set the proto's `AuctionHouseAction.op` as a `uint32_t`, but the
  field is the typed `AHOp` enum, so the call did not compile.

## Unreleased: log action

- Tier 1 `LogAction` (actions.proto field 17, `ModuleClient::Log(level, text)`): a module writes a line to the core's Server.log, tagged
  with its name. Additive; an old core answers it as an unknown action. Protocol version unchanged.

## Layout 10 (cast state, breaking for layout 9 modules)

- `BotSnapshot` gains `moving`, `casting_spell` and `cast_remaining_ms`. `BotActionStatus` gains `kBusy` (a cast refused: the bot is
  already casting) and `kMoving` (a cast refused: the spell has a cast time and the bot is moving).
- Protocol unchanged. A module built against layout 9 is refused at region attach, since the layout version differs.

## Unreleased: scenario actions

- `ActionType::kTeleport` (11), `kGrant` (12) and `kAutoGear` (13), for scenarios (doc 19 of the core): put a bot at a map position, add an
  item to its bags, and dress it for its level. Additive; an old core refuses them as unknown action types. The core accepts the first two
  only with `Bot.Actions.Teleport` / `Bot.Actions.Grant` on. `PublishActions` accepts every type up to `kAutoGear`.

## Unreleased: placement in SpawnParams

- `SpawnParams` gains `place`, `map_id`, `pos_x`, `pos_y`, `pos_z` (control.proto fields 5 to 9): where the bot is put when it logs in.
  Additive; an old core ignores them and spreads the bot as before.

## Layout 9 (repair, breaking for layout 8 modules)

- `BotSnapshot` gains `repair_x`, `repair_y`, `repair_z`, `repair_valid` (the nearest friendly repairman on the bot's map) and
  `durability_pct` (the lowest equipped durability in percent). The vendor in `vendor_*` is now also one friendly to the bot.
- Protocol unchanged. A module built against layout 8 is refused at region attach, since the layout version differs.

## Layout 8 (M5 quests, breaking for layout 7 modules)

- `BotSnapshot` gains `quest_count` and `quests[8]` (`QuestEntry`: quest id, status, up to 4 objective counts): the bot's quest log.
- `ActionType::kQuest` (10): accept (1), hand in (2) or abandon (3) a quest; `spell_id = (operation << 24) | quest id`. The core finds the
  NPC in interaction range and picks the reward item. `PublishActions` accepts every type up to `kQuest`.
- Protocol unchanged. A module built against layout 7 is refused at region attach, since the layout version differs.

## Layout 7 (M7 hubs, breaking for layout 6 modules)

- `BotSnapshot` gains `map_id`: the map the bot is on, so a module can pick the hubs of that map.
- Protocol unchanged. A module built against layout 6 is refused at region attach, since the layout version differs.

## Layout 6 (M6, breaking for layout 5 modules)

- `BotSnapshot` gains `vendor_x`, `vendor_y`, `vendor_z` and `vendor_valid`: the nearest vendor to the bot on its own map.
  The core finds it, since a bot only sees units within 40 yards. Travel to vendors no longer needs a position per race.
- Protocol unchanged. A module built against layout 5 is refused at region attach, since the layout version differs.

## Layout 5 (M8, breaking for layout 4 modules)

- `BotSnapshot` gains `group_role`, `group_size`, `group_leader_guid` and `group_invite_from`: the bot's party (M8).
- `ActionType::kGroup` (8): invite, accept, or leave a party (`spell_id` 1, 2, 3).
- Protocol unchanged. A module built against layout 4 is refused at region attach, since the layout version differs.

## Unreleased (no protocol or layout change)

- `ActionType::kVendor` (6): sell grey items, repair and equip the best gear, at an NPC within interaction range (M3).
  `spell_id` is a bitmask (1 sell, 2 repair, 4 equip). Old cores refuse it as an unknown type.
- `ActionType::kTrain` (7): learn the spells the trainer in range teaches this bot now (M4).
- `ActionType::kRest` (5): sit down to rest (`spell_id` 1) or stand up (`spell_id` 0). Old cores refuse it as an unknown
  action type, so it is additive.
- `ModuleClient::AddTimer(period, callback)`: periodic callbacks on the thread that runs `Run()`. `Run()` waits with
  `select()` up to the next timer when timers are registered, and blocks on the read as before when there are none.
  Covered by `tier1_roundtrip_test`.

## Protocol 4

- Tier 2 layout 4: `BotSnapshot::mana`/`max_mana` are `power`/`max_power` (the main resource, see `power_type`). New
  `BotSnapshot::level` and `power_type`. `NearbyUnit` gains `level`, `elite` and `casting`.

## Protocol 3

- Tier 2 layout 3: `BotSnapshot` gains combat state (`in_combat`, `target_distance`) and the result of the last action for
  the bot (`last_action_type`, `last_action_status`, `last_action_spell`). `BotActionStatus` lists the statuses.

## Protocol 2

- Tier 2 layout 2: `BotSnapshot::spell_cooldowns` is now `SpellCooldown[]` (spell id and remaining milliseconds).
  Unused slots have spell id 0.

## [0.1.0] - unreleased

Phase 1 contract release (wire schema, Tier 2 layout) plus the Tier 1 client. Tier 2 shared memory
and bot control are not included yet (see "Not yet implemented").

- Tier 1 client (`ModuleClient`), POSIX only: Unix domain socket transport, 4-byte little-endian
  length-prefixed `Envelope` frames (max 1 MiB), handshake with timeout, config delivery, notify and
  request/reply dispatch, heartbeat thread, and `Shutdown()` callable from another thread.
- Schema is proto2, not proto3: the core bundles protobuf 2.6.1, which parses only proto2 and has no
  `map<>`. `ModuleConfig` uses a repeated `ConfigEntry` with the same field number and wire encoding as
  the proto3 map. The wire format is unchanged, so `protocol_version` stays `"1"`.
- Tests: `tests/tier1_roundtrip_test.cpp` drives the client against a fake core over the real wire
  format. Covers accept + events + heartbeat + core `Shutdown`, protocol rejection, and module-side
  `Shutdown()`. Run with `ctest` (disable with `-DFOREVER_MODULE_SDK_BUILD_TESTS=OFF`).

- Tier 1 `.proto` schemas (handshake, events, control, envelope) for the v1 hook subset defined in
  the core repo's `doc/module-system/03-phase1-protocol-design.md`.
  - `HandshakeRequest.sdk_package_version` (informational) and `HandshakeResponse.negotiated_max_*`
    / `negotiated_tick_interval` (the Tier 2 bounds core actually granted).
  - `events.proto` covers only the v1 hooks: `PlayerScript::OnLogin/OnLogout`,
    `AccountScript::OnAccountLogin`, `WorldScript::OnStartup/OnShutdown`, `CommandScript`.
- `include/forever_module/tier2_layout.h` Tier 2 shared-memory POD structs, with explicit
  `Tier2RegionHeader::layout_version` (`kTier2LayoutVersion = 1`) and compile-time checks that the
  shared atomics are lock-free and the structs are POD.
- `protocol_version` (`"1"`) / SDK package version (`"0.1.0"`) split, per
  `doc/module-system/11-sdk-scope.md`.
- Build fix: generated protobuf sources are emitted to `generated/forever_module/`, matching the
  `forever_module/*.pb.h` include paths.
- `ModuleClient`/`Tier2Session` public API declared; implementation bodies are stubs.
- `examples/minimal-module/` smallest module skeleton.

### Not yet implemented

- Real `boost::asio` Tier 1 socket/pipe handling (connect, handshake, heartbeat, message framing,
  callback dispatch).
- Real `boost::interprocess` Tier 2 shared-memory attach/read/write and offset-addressing
  accessors for the variable-length `bots[]`/`nearby[]`/`actions[]` arrays.
- `MAX_TRACKED_COOLDOWNS` default value, still open pending a spell-casting test module
  (`doc/module-system/07-phase5-reference-module-design.md`).
