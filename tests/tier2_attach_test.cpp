// SPDX-License-Identifier: GPL-2.0-or-later
// Tier 2 region, module side: this test plays the core (creates the region with the shared layout header,
// publishes a snapshot), then checks Tier2Session::Attach and ReadLatestSnapshot against it.

#include "forever_module/tier2_session.h"

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <unistd.h>

namespace bi = boost::interprocess;
using namespace forever_module;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                           \
    } while (0)

constexpr std::uint32_t kBots = 4, kNearby = 2, kActions = 8;

// Creates a region the way the core does: sized from the bounds, header written (geometry and section table included), snapshot buffer 1 published.
std::unique_ptr<bi::shared_memory_object> CreateRegion(std::string const& name, std::uint32_t layoutVersion,
                                                      std::unique_ptr<bi::mapped_region>& mapping)
{
    bi::shared_memory_object::remove(name.c_str());
    auto shm = std::make_unique<bi::shared_memory_object>(bi::create_only, name.c_str(), bi::read_write);
    shm->truncate(static_cast<bi::offset_t>(Tier2RegionBytes(kBots, kNearby, kActions)));
    mapping = std::make_unique<bi::mapped_region>(*shm, bi::read_write);

    auto* base = static_cast<std::byte*>(mapping->get_address());
    auto* header = new (base) Tier2RegionHeader();
    header->max_bots = kBots;
    header->max_nearby_units = kNearby;
    header->max_actions = kActions;
    header->tick_interval = 2;
    Tier2DescribeRegion(*header);
    header->layout_version = layoutVersion;

    // Core publishes into buffer 1 and flips the index.
    std::byte* snapshotBase = base + Tier2SnapshotBufferOffset(kBots, kNearby, 1);
    auto* snapshot = new (snapshotBase) WorldTickSnapshotHeader();
    snapshot->tick_number = 10;
    snapshot->bot_count = 1;
    snapshot->party_view_count = 1;
    snapshot->party_member_count = 2;
    auto* bot = new (snapshotBase + Tier2BotsSectionOffset()) BotSnapshot();
    bot->guid = 42;
    bot->health = 900;
    bot->max_health = 1000;
    bot->nearby_count = 1;
    bot->ammo_entry = 2512;
    bot->ammo_count = 200;
    bot->ground_effect_count = 1;                       // layout 21
    bot->ground_effects[0].spell_id = 11829;
    bot->ground_effects[0].radius = 5.0f;
    bot->ground_effects[0].kind = 1;
    bot->ground_effects[0].hostile = 1;
    bot->ground_effects[7].spell_id = 22946;            // layout 25: the eighth slot exists
    // Layout 25: the bot's group view, a telegraph and a probe answer.
    bot->party_view_index = 0;
    bot->party_member_index = 1;
    bot->telegraph_count = 1;
    bot->telegraphs[0].caster_ref = MakeUnitRef(8, 1234, 77);
    bot->telegraphs[0].spell_id = 11366; // Pyroblast
    bot->telegraphs[0].kind = 2;
    bot->telegraphs[0].radius = 10.0f;
    bot->telegraphs[0].cone_angle = 60.0f;
    bot->telegraphs[0].interruptible = 1;
    bot->probe_count = 1;
    bot->probe_sequence = 5;
    bot->probes[0].probe_id = 9;
    bot->probes[0].flags = kProbeReachable | kProbeGroundKnown;
    bot->probes[0].path_yards = 12.5f;
    bot->probes[0].sequence = 5;
    bot->role_hint = 2;
    // Nearby units: kNearby slots per bot.
    auto* nearby = new (snapshotBase + Tier2NearbySectionOffset(kBots)) NearbyUnit();
    nearby->guid = MakeUnitRef(8, 1234, 77);
    nearby->is_hostile = true;
    nearby->aggro_radius = 17.5f;
    // The party view and its two members (the pool holds the bot at slot 1).
    auto* view = new (snapshotBase + Tier2PartyViewsSectionOffset(kBots, kNearby)) PartyView();
    view->group_id = 500;
    view->leader_ref = MakeUnitRef(2, 0, 41);
    view->main_tank_ref = MakeUnitRef(2, 0, 41);
    view->member_first = 0;
    view->member_count = 2;
    view->generation = 3;
    view->in_combat = 1;
    view->engaged_count = 2;
    view->loose_count = 1;
    view->loose_mobs[0].unit_ref = MakeUnitRef(8, 1234, 77);
    view->loose_mobs[0].victim_ref = MakeUnitRef(2, 0, 42);
    view->loose_mobs[0].since_ms = 400;
    view->raid_marks[7] = MakeUnitRef(8, 1234, 77); // skull
    auto* members = reinterpret_cast<PartyMember*>(snapshotBase + Tier2PartyMembersSectionOffset(kBots, kNearby));
    new (members) PartyMember();
    members[0].guid = MakeUnitRef(2, 0, 41);
    members[0].role = kPartyRoleTank;
    members[0].member_flags = kPartyMemberLeader | kPartyMemberMainTank;
    members[0].online = 1;
    members[0].health = 1000;
    new (members + 1) PartyMember();
    members[1].guid = MakeUnitRef(2, 0, 42);
    members[1].role = kPartyRoleHealer;
    members[1].is_bot = 1;
    members[1].online = 1;
    members[1].debuff_count = 1;
    members[1].debuffs[0].spell_id = 589;
    members[1].debuffs[0].dispel_type = 1;
    members[1].debuffs[0].remaining_ms = 9000;
    members[1].attacker_count = 1;
    header->published_snapshot_tick.store(10);
    header->published_snapshot_buffer.store(1);
    return shm;
}

} // namespace

// Layout 25: the new fields stay where the core writes them. The core's ModuleTier2.cpp pins the same sizes; offsetof is checked here so a reordered field fails
// to compile on both sides.
static_assert(sizeof(NearbyUnit) == 136 && offsetof(NearbyUnit, victim_ref) == 56 && offsetof(NearbyUnit, my_debuffs) == 96 && offsetof(NearbyUnit, aggro_radius) == 128,
              "NearbyUnit layout 25");
static_assert(sizeof(OwnDebuff) == 8 && sizeof(ActionResult) == 12 && sizeof(Telegraph) == 48 && sizeof(ProbeResult) == 32 && sizeof(PartyDebuff) == 12 &&
              sizeof(LooseMob) == 24 && sizeof(Tier2Section) == 24, "layout 25 element sizes");
static_assert(kMaxTrackedCooldowns == 16 && kMaxTargetAuras == 8 && kMaxActionResults == 4 && kMaxOwnDebuffs == 4, "layout 24 counts");
static_assert(kMaxGroundEffects == 8 && kMaxTelegraphs == 4 && kMaxProbeResults == 4 && kMaxPartyMembers == 40 && kMaxPartyDebuffs == 6 && kMaxLooseMobs == 8 &&
              kMaxRaidMarks == 8 && kMaxTier2Sections == 8, "layout 25 counts");
static_assert(sizeof(BotSnapshot) == 1856 && offsetof(BotSnapshot, ground_effects) == 904 && offsetof(BotSnapshot, results) == 1460 &&
              offsetof(BotSnapshot, party_view_index) == 1508 && offsetof(BotSnapshot, party_member_index) == 1512 && offsetof(BotSnapshot, telegraph_count) == 1516 &&
              offsetof(BotSnapshot, probe_sequence) == 1520 && offsetof(BotSnapshot, telegraphs) == 1528 && offsetof(BotSnapshot, probes) == 1720,
              "BotSnapshot layout 25");
static_assert(sizeof(PartyMember) == 176 && offsetof(PartyMember, transport_entry) == 80 && offsetof(PartyMember, class_id) == 84 && offsetof(PartyMember, debuffs) == 96,
              "PartyMember layout 25");
static_assert(sizeof(PartyView) == 320 && offsetof(PartyView, raid_marks) == 32 && offsetof(PartyView, member_first) == 96 && offsetof(PartyView, loose_mobs) == 112,
              "PartyView layout 25");
static_assert(sizeof(Tier2RegionHeader) == 304 && offsetof(Tier2RegionHeader, layout_base) == 52 && offsetof(Tier2RegionHeader, header_bytes) == 72 &&
              offsetof(Tier2RegionHeader, sections) == 112, "Tier2RegionHeader layout 25");
static_assert(sizeof(WorldTickSnapshotHeader) == 24 && offsetof(WorldTickSnapshotHeader, party_view_count) == 12, "WorldTickSnapshotHeader layout 25");
static_assert(Tier2HeaderBytes() == 304 && Tier2PartyViewSlots(2500) == 1251 && Tier2PartyMemberSlots(2500) == 5000, "layout 25 region arithmetic");
static_assert(kTier2LayoutVersion == 25 && kTier2LayoutBase == 25, "layout 25");
static_assert(static_cast<std::uint32_t>(ActionType::kTransport) == 32 && static_cast<std::uint32_t>(ActionType::kStopCast) == 33 &&
              static_cast<std::uint32_t>(ActionType::kProbePoint) == 34 && ActionType::kLastActionType == ActionType::kProbePoint, "layout 25 action types");

int main()
{
    // Unit references pack and unpack without loss inside their field widths.
    std::uint64_t const creatureRef = MakeUnitRef(8, 1234567, 0x89ABCDEF);
    CHECK(UnitRefType(creatureRef) == 8);
    CHECK(UnitRefEntry(creatureRef) == 1234567);
    CHECK(UnitRefCounter(creatureRef) == 0x89ABCDEF);
    CHECK(MakeUnitRef(0, 0, 0) == 0);
    CHECK(UnitRefCounter(MakeUnitRef(2, 0, 0x1FFFFFFFFull)) == 0xFFFFFFFFull); // out of range: masked, not corrupted

    // The region arithmetic at the module's own bounds (2500 bots, 32 nearby units): one snapshot buffer holds the bots, their nearby units, the party
    // views and the member pool, each section a multiple of 8 bytes from the start.
    {
        constexpr std::uint32_t bots = 2500, nearby = 32;
        CHECK(Tier2NearbySectionOffset(bots) == 24 + std::size_t(bots) * 1856);
        CHECK(Tier2PartyViewsSectionOffset(bots, nearby) == Tier2NearbySectionOffset(bots) + std::size_t(bots) * nearby * 136);
        CHECK(Tier2PartyMembersSectionOffset(bots, nearby) == Tier2PartyViewsSectionOffset(bots, nearby) + 1251u * 320);
        CHECK(Tier2SnapshotBufferBytes(bots, nearby) >= Tier2PartyMembersSectionOffset(bots, nearby) + 5000u * 176);
        CHECK(Tier2PartyViewsSectionOffset(bots, nearby) % 8 == 0 && Tier2PartyMembersSectionOffset(bots, nearby) % 8 == 0);
    }

    std::string const name = "trinity_module_sdk_test_" + std::to_string(::getpid()) + "_tier2";
    std::unique_ptr<bi::mapped_region> mapping;
    auto shm = CreateRegion(name, kTier2LayoutVersion, mapping);

    std::string error;
    auto session = Tier2Session::Attach(name, error);
    CHECK(session != nullptr);
    if (!session)
        std::printf("attach failed: %s\n", error.c_str());
    if (session)
    {
        CHECK(session->MaxBots() == 4);
        CHECK(session->MaxNearbyUnits() == 2);
        CHECK(session->MaxActions() == 8);
        CHECK(session->TickInterval() == 2);
        CHECK(session->LayoutVersion() == kTier2LayoutVersion);
        CHECK(session->MaxPartyViews() == Tier2PartyViewSlots(4) && session->MaxPartyMembers() == Tier2PartyMemberSlots(4));

        WorldTickSnapshotView const& view = session->ReadLatestSnapshot();
        CHECK(view.tick_number == 10);
        CHECK(view.bots.size() == 1);
        if (view.bots.size() == 1)
        {
            CHECK(view.bots[0].guid == 42);
            CHECK(view.bots[0].health == 900);
            CHECK(view.bots[0].ammo_entry == 2512);
            CHECK(view.bots[0].ammo_count == 200);
            CHECK(view.bots[0].ground_effect_count == 1 && view.bots[0].ground_effects[0].spell_id == 11829 && view.bots[0].ground_effects[0].hostile == 1);
            CHECK(view.bots[0].ground_effects[7].spell_id == 22946);
            CHECK(view.bots[0].role_hint == 2);
            // Layout 25: telegraphs and probes through the view helpers.
            CHECK(view.TelegraphsOf(0).size() == 1 && view.TelegraphsOf(0)[0].spell_id == 11366 && view.TelegraphsOf(0)[0].kind == 2 &&
                  view.TelegraphsOf(0)[0].cone_angle == 60.0f && view.TelegraphsOf(0)[0].interruptible == 1);
            CHECK(view.ProbesOf(0).size() == 1 && view.ProbesOf(0)[0].probe_id == 9 && (view.ProbesOf(0)[0].flags & kProbeReachable) != 0 &&
                  view.ProbesOf(0)[0].path_yards == 12.5f && view.bots[0].probe_sequence == 5);
        }
        CHECK(view.NearbyOf(0).size() == 1);
        if (view.NearbyOf(0).size() == 1)
        {
            CHECK(view.NearbyOf(0)[0].guid == MakeUnitRef(8, 1234, 77));
            CHECK(view.NearbyOf(0)[0].is_hostile);
            CHECK(view.NearbyOf(0)[0].aggro_radius == 17.5f);
        }
        CHECK(view.NearbyOf(5).empty()); // out of range
        CHECK(view.TelegraphsOf(5).empty() && view.ProbesOf(5).empty() && view.PartyViewOf(5) == nullptr && view.MemberOf(5) == nullptr);

        // Layout 25: the party view the bot points at, its members from the pool, and the bot's own entry.
        CHECK(view.party_views.size() == 1 && view.party_members.size() == 2);
        PartyView const* party = view.PartyViewOf(0);
        CHECK(party != nullptr);
        if (party)
        {
            CHECK(party->group_id == 500 && party->leader_ref == MakeUnitRef(2, 0, 41) && party->main_tank_ref == MakeUnitRef(2, 0, 41));
            CHECK(party->generation == 3 && party->in_combat == 1 && party->engaged_count == 2 && party->loose_count == 1);
            CHECK(party->loose_mobs[0].unit_ref == MakeUnitRef(8, 1234, 77) && party->loose_mobs[0].victim_ref == MakeUnitRef(2, 0, 42) &&
                  party->loose_mobs[0].since_ms == 400);
            CHECK(party->raid_marks[7] == MakeUnitRef(8, 1234, 77));
            std::span<PartyMember const> const members = view.MembersOf(*party);
            CHECK(members.size() == 2);
            if (members.size() == 2)
            {
                CHECK(members[0].guid == MakeUnitRef(2, 0, 41) && members[0].role == kPartyRoleTank && (members[0].member_flags & kPartyMemberLeader) != 0);
                CHECK(members[1].guid == MakeUnitRef(2, 0, 42) && members[1].is_bot == 1 && members[1].debuff_count == 1 &&
                      members[1].debuffs[0].spell_id == 589 && members[1].debuffs[0].dispel_type == 1 && members[1].attacker_count == 1);
            }
            PartyMember const* self = view.MemberOf(0);
            CHECK(self != nullptr && self == &view.party_members[1]);
            // A view whose range runs past the pool gives no members.
            PartyView broken = *party;
            broken.member_first = 1;
            broken.member_count = 5;
            CHECK(view.MembersOf(broken).empty());
        }

        // Actions answer the snapshot just read (tick 10). The first publish writes action buffer 1, since buffer 0
        // is the one published at region creation.
        BotAction move;
        move.bot_guid = 42;
        move.type = ActionType::kMoveTo;
        move.dest_x = 1.5f;
        BotAction stop;
        stop.bot_guid = 42;
        stop.type = ActionType::kStop;

        auto const* base = static_cast<std::byte const*>(mapping->get_address());
        auto const* header = reinterpret_cast<Tier2RegionHeader const*>(base);
        std::string actionError;
        CHECK(session->PublishActions({move, stop}, actionError));
        CHECK(header->published_action_buffer.load() == 1);
        CHECK(header->published_action_tick.load() == 10);

        auto const* queue = reinterpret_cast<ActionQueueHeader const*>(
            base + Tier2ActionBufferOffset(4, 2, 8, 1));
        CHECK(queue->tick_number == 10);
        CHECK(queue->action_count == 2);
        auto const* sent = reinterpret_cast<BotAction const*>(queue + 1);
        CHECK(sent[0].bot_guid == 42 && sent[0].type == ActionType::kMoveTo && sent[0].dest_x == 1.5f);
        CHECK(sent[1].type == ActionType::kStop);

        // The next publish flips back to buffer 0.
        CHECK(session->PublishActions({stop}, actionError));
        CHECK(header->published_action_buffer.load() == 0);
        auto const* queue0 = reinterpret_cast<ActionQueueHeader const*>(
            base + Tier2ActionBufferOffset(4, 2, 8, 0));
        CHECK(queue0->action_count == 1);

        // Too many actions, or an unknown type, is refused and the published buffer does not change.
        std::vector<BotAction> tooMany(9, stop);
        CHECK(!session->PublishActions(tooMany, actionError));
        CHECK(actionError.find("too many actions") != std::string::npos);
        BotAction unknown;
        unknown.bot_guid = 42;
        unknown.type = static_cast<ActionType>(99);
        CHECK(!session->PublishActions({unknown}, actionError));
        CHECK(actionError.find("unknown action type") != std::string::npos);
        CHECK(header->published_action_buffer.load() == 0);

        // Every type up to kGroup is known: a rest or a party action is accepted, not refused as unknown.
        BotAction party;
        party.bot_guid = 42;
        party.type = ActionType::kGroup;
        party.spell_id = 3;
        CHECK(session->PublishActions({party}, actionError));

        BotAction loot;
        loot.bot_guid = 42;
        loot.type = ActionType::kLoot;
        CHECK(session->PublishActions({loot}, actionError));

        BotAction quest;
        quest.bot_guid = 42;
        quest.type = ActionType::kQuest;
        quest.spell_id = (1u << 24) | 7u;
        CHECK(session->PublishActions({quest}, actionError));

        BotAction teleport;
        teleport.bot_guid = 42;
        teleport.type = ActionType::kTeleport;
        teleport.spell_id = 36;
        CHECK(session->PublishActions({teleport}, actionError));

        BotAction grant;
        grant.bot_guid = 42;
        grant.type = ActionType::kGrant;
        grant.spell_id = 2092;
        grant.dest_x = 1.0f;
        CHECK(session->PublishActions({grant}, actionError));

        BotAction gear;
        gear.bot_guid = 42;
        gear.type = ActionType::kAutoGear;
        gear.spell_id = 1;
        CHECK(session->PublishActions({gear}, actionError));

        BotAction release;
        release.bot_guid = 42;
        release.type = ActionType::kReleaseSpirit;
        CHECK(session->PublishActions({release}, actionError));

        BotAction revive;
        revive.bot_guid = 42;
        revive.type = ActionType::kResurrect;
        revive.target_guid = MakeUnitRef(2, 0, 7);
        CHECK(session->PublishActions({revive}, actionError));

        BotAction reclaim;
        reclaim.bot_guid = 42;
        reclaim.type = ActionType::kReclaimCorpse;
        CHECK(session->PublishActions({reclaim}, actionError));

        // Layout 25: the three new action types are inside the layout.
        BotAction probe;
        probe.bot_guid = 42;
        probe.type = ActionType::kProbePoint;
        probe.spell_id = 9;
        probe.dest_x = 10.0f;
        probe.dest_y = 20.0f;
        probe.dest_z = 30.0f;
        probe.target_guid = MakeUnitRef(8, 1234, 77);
        BotAction stopCast;
        stopCast.bot_guid = 42;
        stopCast.type = ActionType::kStopCast;
        BotAction transport;
        transport.bot_guid = 42;
        transport.type = ActionType::kTransport;
        transport.spell_id = 3;
        CHECK(session->PublishActions({probe, stopCast, transport}, actionError));

        BotAction beyond;
        beyond.bot_guid = 42;
        beyond.type = static_cast<ActionType>(static_cast<std::uint32_t>(ActionType::kLastActionType) + 1);
        CHECK(!session->PublishActions({beyond}, actionError));
    }

    // A session that has read no snapshot cannot publish: the actions would have no tick to answer. The first whole
    // read then makes it publish-ready.
    {
        std::string freshError;
        auto fresh = Tier2Session::Attach(name, freshError);
        CHECK(fresh != nullptr);
        if (fresh)
        {
            std::string publishError;
            CHECK(!fresh->HasSnapshot());
            CHECK(!fresh->PublishActions({}, publishError));
            CHECK(publishError.find("no snapshot has been read yet") != std::string::npos);
            CHECK(fresh->ReadLatestSnapshot().tick_number == 10);
            CHECK(fresh->HasSnapshot());
        }
    }

    // Corrupt indexes: a snapshot index past the two buffers is not followed, and an action index past them is refused.
    {
        auto* header = static_cast<Tier2RegionHeader*>(mapping->get_address());
        header->published_snapshot_buffer.store(7);
        std::string corruptError;
        auto corrupt = Tier2Session::Attach(name, corruptError);
        CHECK(corrupt != nullptr);
        if (corrupt)
        {
            CHECK(corrupt->ReadLatestSnapshot().bots.empty());
            CHECK(!corrupt->HasSnapshot());

            header->published_snapshot_buffer.store(1);
            CHECK(corrupt->ReadLatestSnapshot().tick_number == 10);

            header->published_action_buffer.store(5);
            std::string actionError;
            CHECK(!corrupt->PublishActions({}, actionError));
            CHECK(actionError.find("corrupt action buffer index") != std::string::npos);
            header->published_action_buffer.store(0);
        }
    }

    // Layout 25: a section table the SDK cannot read is refused (a bot stride narrower than the struct), and so is a same-layout core whose
    // geometry differs from the helpers (a header that lies about its snapshot buffer size).
    {
        auto* header = static_cast<Tier2RegionHeader*>(mapping->get_address());
        std::uint32_t const stride = header->sections[0].element_size;
        header->sections[0].element_size = stride - 8;
        std::string narrowError;
        CHECK(Tier2Session::Attach(name, narrowError) == nullptr);
        CHECK(narrowError.find("section table") != std::string::npos);
        header->sections[0].element_size = stride;

        std::uint64_t const bytes = header->snapshot_bytes;
        header->snapshot_bytes = bytes + 16;
        std::string geometryError;
        CHECK(Tier2Session::Attach(name, geometryError) == nullptr);
        CHECK(geometryError.find("geometry") != std::string::npos);
        header->snapshot_bytes = bytes;
        CHECK(Tier2Session::Attach(name, geometryError) != nullptr);
    }

    // The party slot counts are the core's choice: a region with 7 view slots (the default for 4 bots is 3) is attached, the SDK reads 7 and the
    // member pool the header gives (max(2 * bots, 2 * views) = 14), not a formula of max_bots; a header whose pool disagrees is refused.
    {
        std::string const viewsName = name + "_views";
        constexpr std::uint32_t views = 7;
        static_assert(Tier2PartyMemberSlots(kBots, views) == 14 && Tier2PartyMemberSlots(kBots, 1) == 8, "member pool follows the view count");
        std::size_t const total = Tier2RegionBytes(kBots, kNearby, kActions, views, Tier2PartyMemberSlots(kBots, views));
        bi::shared_memory_object::remove(viewsName.c_str());
        bi::shared_memory_object viewsShm(bi::create_only, viewsName.c_str(), bi::read_write);
        viewsShm.truncate(static_cast<bi::offset_t>(total));
        bi::mapped_region viewsMap(viewsShm, bi::read_write);
        auto* header = new (viewsMap.get_address()) Tier2RegionHeader();
        header->max_bots = kBots;
        header->max_nearby_units = kNearby;
        header->max_actions = kActions;
        header->tick_interval = 1;
        header->max_party_views = views;
        Tier2DescribeRegion(*header);
        CHECK(header->max_party_views == views && header->max_party_members == 14);
        CHECK(header->sections[2].count == views && header->sections[3].count == 14);
        CHECK(header->snapshot_bytes == Tier2SnapshotBufferBytes(kBots, kNearby, views, 14));
        std::string viewsError;
        auto custom = Tier2Session::Attach(viewsName, viewsError);
        CHECK(custom != nullptr);
        if (!custom)
            std::printf("attach with custom view slots failed: %s\n", viewsError.c_str());
        if (custom)
            CHECK(custom->MaxPartyViews() == views && custom->MaxPartyMembers() == 14);
        header->max_party_members = 13; // a pool the helper does not derive from 7 views
        CHECK(Tier2Session::Attach(viewsName, viewsError) == nullptr);
        CHECK(viewsError.find("geometry") != std::string::npos);
        bi::shared_memory_object::remove(viewsName.c_str());
    }

    // A region written with another layout version is refused, with the reason in the error: an older core always, a newer one when its
    // base is above this SDK's layout.
    std::string const badName = name + "_v99";
    std::unique_ptr<bi::mapped_region> badMapping;
    auto badShm = CreateRegion(badName, kTier2LayoutVersion + 98, badMapping);
    static_cast<Tier2RegionHeader*>(badMapping->get_address())->layout_base = kTier2LayoutVersion + 98;
    std::string badError;
    CHECK(Tier2Session::Attach(badName, badError) == nullptr);
    CHECK(badError.find("layout mismatch") != std::string::npos);
    static_cast<Tier2RegionHeader*>(badMapping->get_address())->layout_version = kTier2LayoutVersion - 1;
    static_cast<Tier2RegionHeader*>(badMapping->get_address())->layout_base = kTier2LayoutVersion - 1;
    CHECK(Tier2Session::Attach(badName, badError) == nullptr);
    CHECK(badError.find("layout mismatch") != std::string::npos);

    // The growth rule: a newer core (layout + 1, base kTier2LayoutBase) that appended 8 bytes to BotSnapshot, NearbyUnit, ActionQueueHeader and
    // BotAction is read with its strides, and the actions are written with them. The region is laid out here by hand, the way such a core would.
    {
        std::string const newerName = name + "_newer";
        constexpr std::size_t botStride = sizeof(BotSnapshot) + 8, nearbyStride = sizeof(NearbyUnit) + 8;
        constexpr std::size_t actionHeaderBytes = sizeof(ActionQueueHeader) + 8, actionStride = sizeof(BotAction) + 8;
        std::size_t const botsOffset = sizeof(WorldTickSnapshotHeader);
        std::size_t const nearbyOffset = botsOffset + kBots * botStride;
        std::size_t const viewsOffset = nearbyOffset + kBots * kNearby * nearbyStride;
        std::size_t const membersOffset = viewsOffset + Tier2PartyViewSlots(kBots) * sizeof(PartyView);
        std::size_t const snapshotBytes = tier2_detail::AlignUp(membersOffset + Tier2PartyMemberSlots(kBots) * sizeof(PartyMember));
        std::size_t const actionBytes = tier2_detail::AlignUp(actionHeaderBytes + kActions * actionStride);
        std::size_t const headerBytes = Tier2HeaderBytes();
        std::size_t const total = headerBytes + 2 * snapshotBytes + 2 * actionBytes;

        bi::shared_memory_object::remove(newerName.c_str());
        bi::shared_memory_object newerShm(bi::create_only, newerName.c_str(), bi::read_write);
        newerShm.truncate(static_cast<bi::offset_t>(total));
        bi::mapped_region newerMap(newerShm, bi::read_write);
        auto* base = static_cast<std::byte*>(newerMap.get_address());
        std::memset(base, 0, total);
        auto* header = new (base) Tier2RegionHeader();
        header->max_bots = kBots;
        header->max_nearby_units = kNearby;
        header->max_actions = kActions;
        header->tick_interval = 1;
        Tier2DescribeRegion(*header);
        header->layout_version = kTier2LayoutVersion + 1;
        header->snapshot_bytes = snapshotBytes;
        header->action_offset = headerBytes + 2 * snapshotBytes;
        header->action_bytes = actionBytes;
        header->action_header_bytes = std::uint32_t(actionHeaderBytes);
        header->action_stride = std::uint32_t(actionStride);
        header->sections[0].element_size = std::uint32_t(botStride);
        header->sections[1].element_size = std::uint32_t(nearbyStride);
        header->sections[1].offset = nearbyOffset;
        header->sections[2].offset = viewsOffset;
        header->sections[3].offset = membersOffset;
        // Buffer 0 published: one bot at the wider stride, two nearby units, no party views.
        std::byte* snapshotBase = base + headerBytes;
        auto* snapshot = new (snapshotBase) WorldTickSnapshotHeader();
        snapshot->tick_number = 77;
        snapshot->bot_count = 2;
        BotSnapshot first;
        first.guid = 1;
        first.nearby_count = 2;
        BotSnapshot second;
        second.guid = 2;
        second.health = 5;
        std::memcpy(snapshotBase + botsOffset, &first, sizeof(first));
        std::memcpy(snapshotBase + botsOffset + botStride, &second, sizeof(second));
        NearbyUnit unitA;
        unitA.guid = MakeUnitRef(8, 1, 1);
        NearbyUnit unitB;
        unitB.guid = MakeUnitRef(8, 2, 2);
        std::memcpy(snapshotBase + nearbyOffset, &unitA, sizeof(unitA));
        std::memcpy(snapshotBase + nearbyOffset + nearbyStride, &unitB, sizeof(unitB));
        header->published_snapshot_tick.store(77);
        header->published_snapshot_buffer.store(0);

        std::string newerError;
        auto newer = Tier2Session::Attach(newerName, newerError);
        CHECK(newer != nullptr);
        if (!newer)
            std::printf("attach to the newer layout failed: %s\n", newerError.c_str());
        if (newer)
        {
            CHECK(newer->LayoutVersion() == kTier2LayoutVersion + 1);
            WorldTickSnapshotView const& view = newer->ReadLatestSnapshot();
            CHECK(view.tick_number == 77 && view.bots.size() == 2);
            if (view.bots.size() == 2)
            {
                CHECK(view.bots[0].guid == 1 && view.bots[1].guid == 2 && view.bots[1].health == 5);
                CHECK(view.NearbyOf(0).size() == 2 && view.NearbyOf(0)[1].guid == MakeUnitRef(8, 2, 2));
                CHECK(view.PartyViewOf(0) == nullptr);
            }
            BotAction stop;
            stop.bot_guid = 2;
            stop.type = ActionType::kStop;
            BotAction move;
            move.bot_guid = 1;
            move.type = ActionType::kMoveTo;
            move.dest_y = 3.0f;
            std::string publishError;
            CHECK(newer->PublishActions({stop, move}, publishError));
            std::byte const* queue = base + header->action_offset + 1 * actionBytes;
            CHECK(reinterpret_cast<ActionQueueHeader const*>(queue)->tick_number == 77);
            CHECK(reinterpret_cast<ActionQueueHeader const*>(queue)->action_count == 2);
            BotAction sentStop;
            BotAction sentMove;
            std::memcpy(&sentStop, queue + actionHeaderBytes, sizeof(sentStop));
            std::memcpy(&sentMove, queue + actionHeaderBytes + actionStride, sizeof(sentMove));
            CHECK(sentStop.bot_guid == 2 && sentStop.type == ActionType::kStop);
            CHECK(sentMove.bot_guid == 1 && sentMove.type == ActionType::kMoveTo && sentMove.dest_y == 3.0f);
        }
        newer.reset();
        bi::shared_memory_object::remove(newerName.c_str());
    }

    // A region that does not exist is refused.
    std::string missingError;
    CHECK(Tier2Session::Attach(name + "_missing", missingError) == nullptr);
    CHECK(!missingError.empty());

    session.reset();
    mapping.reset();
    shm.reset();
    badMapping.reset();
    badShm.reset();
    bi::shared_memory_object::remove(name.c_str());
    bi::shared_memory_object::remove(badName.c_str());

    if (g_failures == 0)
    {
        std::printf("PASS  Tier 2 attach, snapshot read and action publish\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
