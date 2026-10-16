// Tier 2 region, module side: this test plays the core (creates the region with the shared layout header,
// publishes a snapshot), then checks Tier2Session::Attach and ReadLatestSnapshot against it.

#include "forever_module/tier2_session.h"

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <cstdio>
#include <cstddef>
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

// Creates a region the way the core does: sized from the bounds, header written, snapshot buffer 1 published.
std::unique_ptr<bi::shared_memory_object> CreateRegion(std::string const& name, std::uint32_t layoutVersion,
                                                      std::unique_ptr<bi::mapped_region>& mapping)
{
    constexpr std::uint32_t kBots = 4, kNearby = 2, kActions = 8;
    bi::shared_memory_object::remove(name.c_str());
    auto shm = std::make_unique<bi::shared_memory_object>(bi::create_only, name.c_str(), bi::read_write);
    shm->truncate(static_cast<bi::offset_t>(Tier2RegionBytes(kBots, kNearby, kActions)));
    mapping = std::make_unique<bi::mapped_region>(*shm, bi::read_write);

    auto* base = static_cast<std::byte*>(mapping->get_address());
    auto* header = new (base) Tier2RegionHeader();
    header->layout_version = layoutVersion;
    header->max_bots = kBots;
    header->max_nearby_units = kNearby;
    header->max_actions = kActions;
    header->tick_interval = 2;

    // Core publishes into buffer 1 and flips the index.
    auto* snapshot = new (base + Tier2SnapshotBufferOffset(kBots, kNearby, 1)) WorldTickSnapshotHeader();
    snapshot->tick_number = 10;
    snapshot->bot_count = 1;
    auto* bot = new (reinterpret_cast<std::byte*>(snapshot + 1)) BotSnapshot();
    bot->guid = 42;
    bot->health = 900;
    bot->max_health = 1000;
    bot->nearby_count = 1;
    bot->ammo_entry = 2512;
    bot->ammo_count = 200;
    // Nearby units start after the kBots bot slots, kNearby slots per bot.
    auto* nearby = new (reinterpret_cast<std::byte*>(bot + kBots)) NearbyUnit();
    nearby->guid = MakeUnitRef(8, 1234, 77);
    nearby->is_hostile = true;
    header->published_snapshot_tick.store(10);
    header->published_snapshot_buffer.store(1);
    return shm;
}

} // namespace

int main()
{
    // Unit references pack and unpack without loss inside their field widths.
    std::uint64_t const creatureRef = MakeUnitRef(8, 1234567, 0x89ABCDEF);
    CHECK(UnitRefType(creatureRef) == 8);
    CHECK(UnitRefEntry(creatureRef) == 1234567);
    CHECK(UnitRefCounter(creatureRef) == 0x89ABCDEF);
    CHECK(MakeUnitRef(0, 0, 0) == 0);
    CHECK(UnitRefCounter(MakeUnitRef(2, 0, 0x1FFFFFFFFull)) == 0xFFFFFFFFull); // out of range: masked, not corrupted

    std::string const name = "trinity_module_sdk_test_" + std::to_string(::getpid()) + "_tier2";
    std::unique_ptr<bi::mapped_region> mapping;
    auto shm = CreateRegion(name, kTier2LayoutVersion, mapping);

    std::string error;
    auto session = Tier2Session::Attach(name, error);
    CHECK(session != nullptr);
    if (session)
    {
        CHECK(session->MaxBots() == 4);
        CHECK(session->MaxNearbyUnits() == 2);
        CHECK(session->MaxActions() == 8);
        CHECK(session->TickInterval() == 2);

        WorldTickSnapshotView const& view = session->ReadLatestSnapshot();
        CHECK(view.tick_number == 10);
        CHECK(view.bots.size() == 1);
        if (view.bots.size() == 1)
        {
            CHECK(view.bots[0].guid == 42);
            CHECK(view.bots[0].health == 900);
            CHECK(view.bots[0].ammo_entry == 2512);
            CHECK(view.bots[0].ammo_count == 200);
        }
        CHECK(view.NearbyOf(0).size() == 1);
        if (view.NearbyOf(0).size() == 1)
        {
            CHECK(view.NearbyOf(0)[0].guid == MakeUnitRef(8, 1234, 77));
            CHECK(view.NearbyOf(0)[0].is_hostile);
        }
        CHECK(view.NearbyOf(5).empty()); // out of range

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

        BotAction beyond;
        beyond.bot_guid = 42;
        beyond.type = static_cast<ActionType>(static_cast<std::uint32_t>(ActionType::kLastActionType) + 1);
        CHECK(!session->PublishActions({beyond}, actionError));
    }

    // A region written with another layout version is refused, with the reason in the error.
    std::string const badName = name + "_v99";
    std::unique_ptr<bi::mapped_region> badMapping;
    auto badShm = CreateRegion(badName, kTier2LayoutVersion + 98, badMapping);
    std::string badError;
    CHECK(Tier2Session::Attach(badName, badError) == nullptr);
    CHECK(badError.find("layout mismatch") != std::string::npos);

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
