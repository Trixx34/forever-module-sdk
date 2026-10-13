#include "forever_module/tier2_session.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <span>

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

namespace bi = boost::interprocess;

namespace forever_module {

struct Tier2Session::Region
{
    Region(bi::shared_memory_object&& shm, bi::mapped_region&& map)
        : shm(std::move(shm)), map(std::move(map))
    {
    }

    bi::shared_memory_object shm;
    bi::mapped_region map;
    Tier2RegionHeader* header = nullptr;

    Tier2RegionHeader& Header() const { return *header; }
    std::byte const* Base() const { return static_cast<std::byte const*>(map.get_address()); }
    std::byte* MutableBase() const { return static_cast<std::byte*>(map.get_address()); }
};

Tier2Session::Tier2Session() = default;
Tier2Session::~Tier2Session() = default;

std::unique_ptr<Tier2Session> Tier2Session::Attach(std::string const& regionName, std::string& error)
{
    try
    {
        bi::shared_memory_object shm(bi::open_only, regionName.c_str(), bi::read_write);
        bi::mapped_region map(shm, bi::read_write);

        if (map.get_size() < sizeof(Tier2RegionHeader))
        {
            error = "Tier 2 region '" + regionName + "' is too small to hold a header";
            return nullptr;
        }

        auto* header = static_cast<Tier2RegionHeader*>(map.get_address());
        if (header->layout_version != kTier2LayoutVersion)
        {
            error = "Tier 2 layout mismatch: core wrote version " + std::to_string(header->layout_version) +
                    ", this SDK reads version " + std::to_string(kTier2LayoutVersion);
            return nullptr;
        }

        std::size_t const expected = Tier2RegionBytes(header->max_bots, header->max_nearby_units, header->max_actions);
        if (map.get_size() < expected)
        {
            error = "Tier 2 region '" + regionName + "' is smaller than its header says";
            return nullptr;
        }

        std::unique_ptr<Tier2Session> session(new Tier2Session());
        session->_region = std::make_unique<Region>(std::move(shm), std::move(map));
        session->_region->header = header;
        return session;
    }
    catch (bi::interprocess_exception const& e)
    {
        error = "cannot open Tier 2 region '" + regionName + "': " + e.what();
        return nullptr;
    }
}

std::uint32_t Tier2Session::MaxBots() const
{
    return _region->Header().max_bots;
}

std::uint32_t Tier2Session::MaxNearbyUnits() const
{
    return _region->Header().max_nearby_units;
}

std::uint32_t Tier2Session::MaxActions() const
{
    return _region->Header().max_actions;
}

std::uint32_t Tier2Session::TickInterval() const
{
    return _region->Header().tick_interval;
}

WorldTickSnapshotView const& Tier2Session::ReadLatestSnapshot()
{
    Region const& region = *_region;
    Tier2RegionHeader const& header = region.Header();
    std::uint32_t const maxBots = header.max_bots;
    std::uint32_t const maxNearby = header.max_nearby_units;

    // Copy the published buffer. If the core flipped the index during the copy, the buffer it was writing may
    // have been overwritten, so copy again. Three attempts; after that keep the last copy.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        std::uint8_t const buffer = header.published_snapshot_buffer.load(std::memory_order_acquire);
        auto const* snapshot = reinterpret_cast<WorldTickSnapshotHeader const*>(
            region.Base() + Tier2SnapshotBufferOffset(maxBots, maxNearby, buffer));

        _lastSnapshot.tick_number = snapshot->tick_number;
        std::uint32_t const count = std::min(snapshot->bot_count, maxBots);
        auto const* bots = reinterpret_cast<BotSnapshot const*>(snapshot + 1);
        _lastSnapshot.bots.assign(bots, bots + count);

        // Nearby units follow the bot array, maxNearby slots for every bot slot the core allotted.
        auto const* nearby = reinterpret_cast<NearbyUnit const*>(bots + maxBots);
        _lastSnapshot.max_nearby_units = maxNearby;
        _lastSnapshot.nearby.assign(nearby, nearby + std::size_t(count) * maxNearby);

        if (header.published_snapshot_buffer.load(std::memory_order_acquire) == buffer)
            break;
    }
    return _lastSnapshot;
}

std::span<NearbyUnit const> WorldTickSnapshotView::NearbyOf(std::size_t botIndex) const
{
    if (botIndex >= bots.size())
        return {};
    std::size_t const count = std::min<std::size_t>(bots[botIndex].nearby_count, max_nearby_units);
    return std::span<NearbyUnit const>(nearby.data() + botIndex * max_nearby_units, count);
}

bool Tier2Session::PublishActions(std::vector<BotAction> const& actions, std::string& error)
{
    Tier2RegionHeader& header = _region->Header();

    if (actions.size() > header.max_actions)
    {
        error = "too many actions: " + std::to_string(actions.size()) + ", the core granted " +
                std::to_string(header.max_actions);
        return false;
    }
    for (BotAction const& action : actions)
    {
        if (static_cast<std::uint32_t>(action.type) > static_cast<std::uint32_t>(ActionType::kLastActionType))
        {
            error = "unknown action type " + std::to_string(static_cast<std::uint32_t>(action.type));
            return false;
        }
    }

    // This module is the only writer, so the published index changes only here. Write the buffer the core is not
    // reading, then publish the tick before the index: a core that sees the new index also sees the new tick.
    std::uint8_t const published = header.published_action_buffer.load(std::memory_order_acquire);
    std::uint8_t const target = published ^ 1;
    std::byte* base = _region->MutableBase() + Tier2ActionBufferOffset(header.max_bots, header.max_nearby_units,
                                                                        header.max_actions, target);

    auto* queue = new (base) ActionQueueHeader();
    queue->tick_number = _lastSnapshot.tick_number;
    queue->action_count = static_cast<std::uint32_t>(actions.size());
    if (!actions.empty())
        std::memcpy(static_cast<void*>(queue + 1), actions.data(), actions.size() * sizeof(BotAction));

    header.published_action_tick.store(_lastSnapshot.tick_number, std::memory_order_release);
    header.published_action_buffer.store(target, std::memory_order_release);
    return true;
}

} // namespace forever_module
