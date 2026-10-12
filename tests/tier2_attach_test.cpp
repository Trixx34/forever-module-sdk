// Tier 2 region, module side: this test plays the core (creates the region with the shared layout header,
// publishes a snapshot), then checks Tier2Session::Attach and ReadLatestSnapshot against it.

#include "forever_module/tier2_session.h"

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <cstdio>
#include <new>
#include <string>
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
    header->published_snapshot_tick.store(10);
    header->published_snapshot_buffer.store(1);
    return shm;
}

} // namespace

int main()
{
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
        }
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
        std::printf("PASS  Tier 2 attach and snapshot read\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
