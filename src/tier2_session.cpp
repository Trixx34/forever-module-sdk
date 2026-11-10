// SPDX-License-Identifier: GPL-2.0-or-later
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

// The shared structs are the wire format the core writes. These are their sizes at layout 25 (LP64). A field added without a layout bump
// changes a size and fails here, rather than reading the wrong bytes at run time. Update them in the same commit as the layout version.
static_assert(sizeof(BotSnapshot) == 1856, "BotSnapshot changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(NearbyUnit) == 136, "NearbyUnit changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(GroundEffect) == 24, "GroundEffect changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(Telegraph) == 48, "Telegraph changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(ProbeResult) == 32, "ProbeResult changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(PartyDebuff) == 12, "PartyDebuff changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(PartyMember) == 176, "PartyMember changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(LooseMob) == 24, "LooseMob changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(PartyView) == 320, "PartyView changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(Tier2Section) == 24, "Tier2Section changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(Tier2RegionHeader) == 304, "Tier2RegionHeader changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(WorldTickSnapshotHeader) == 24, "WorldTickSnapshotHeader changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(ActionQueueHeader) == 16, "ActionQueueHeader changed size: bump kTier2LayoutVersion and update this check");
static_assert(sizeof(BotAction) == 40, "BotAction changed size: bump kTier2LayoutVersion and update this check");
// Layout 25 offsets: the append-only rule keeps every earlier field where it was (ground_effects grew in place, which is the one break of this layout).
static_assert(offsetof(NearbyUnit, aggro_radius) == 128, "NearbyUnit layout 25");
static_assert(offsetof(BotSnapshot, ground_effects) == 904 && offsetof(BotSnapshot, results) == 1460 && offsetof(BotSnapshot, party_view_index) == 1508 &&
              offsetof(BotSnapshot, telegraphs) == 1528 && offsetof(BotSnapshot, probes) == 1720, "BotSnapshot layout 25");
static_assert(offsetof(PartyMember, debuffs) == 96 && offsetof(PartyView, loose_mobs) == 112 && offsetof(Tier2RegionHeader, sections) == 112,
              "party view layout 25");

namespace {

// Where one section of a snapshot buffer lies, as the core wrote it (its stride may be wider than this SDK's struct: the growth rule).
struct SectionSpan
{
    std::size_t offset = 0;
    std::size_t stride = 0;
    std::uint32_t count = 0;
};

// Copies `count` elements of the core's `stride` into `out`. The same stride is one copy; a wider one (a newer core) copies the prefix of each.
template <typename T>
void CopySection(std::vector<T>& out, std::byte const* first, std::size_t stride, std::size_t count)
{
    if (stride == sizeof(T))
    {
        auto const* typed = reinterpret_cast<T const*>(first);
        out.assign(typed, typed + count);
        return;
    }
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        std::memcpy(static_cast<void*>(&out[i]), first + i * stride, sizeof(T));
}

} // namespace

struct Tier2Session::Region
{
    Region(bi::shared_memory_object&& shm, bi::mapped_region&& map)
        : shm(std::move(shm)), map(std::move(map))
    {
    }

    bi::shared_memory_object shm;
    bi::mapped_region map;
    Tier2RegionHeader* header = nullptr;

    // The geometry as resolved at Attach (from the header's own numbers, checked against the helpers for a same-layout core).
    std::size_t snapshotOffset = 0;   // the first snapshot buffer
    std::size_t snapshotBytes = 0;    // one snapshot buffer
    std::size_t actionOffset = 0;     // the first action buffer
    std::size_t actionBytes = 0;      // one action buffer
    std::size_t actionHeaderBytes = 0;
    std::size_t actionStride = 0;
    SectionSpan bots;
    SectionSpan nearby;
    SectionSpan partyViews;
    SectionSpan partyMembers;

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
        // The growth rule (tier2_layout.h, kTier2LayoutBase): an older core is refused; a newer one is read when this SDK's layout is at or above
        // the base the core declares.
        bool const sameLayout = header->layout_version == kTier2LayoutVersion;
        if (header->layout_version < kTier2LayoutVersion || (!sameLayout && header->layout_base > kTier2LayoutVersion))
        {
            error = "Tier 2 layout mismatch: core wrote version " + std::to_string(header->layout_version) + " (base " +
                    std::to_string(header->layout_base) + "), this SDK reads version " + std::to_string(kTier2LayoutVersion);
            return nullptr;
        }

        std::unique_ptr<Tier2Session> session(new Tier2Session());
        session->region_ = std::make_unique<Region>(std::move(shm), std::move(map));
        Region& region = *session->region_;
        region.header = header;

        // The geometry: the header's own numbers. A same-layout core must agree with the helpers, which is the check that the two builds
        // share one header; a newer core is believed, within the mapping.
        std::uint32_t const maxBots = header->max_bots;
        std::uint32_t const maxNearby = header->max_nearby_units;
        std::uint32_t const maxActions = header->max_actions;
        region.snapshotOffset = std::size_t(header->header_bytes);
        region.snapshotBytes = std::size_t(header->snapshot_bytes);
        region.actionOffset = std::size_t(header->action_offset);
        region.actionBytes = std::size_t(header->action_bytes);
        region.actionHeaderBytes = header->action_header_bytes;
        region.actionStride = header->action_stride;
        if (sameLayout &&
            (region.snapshotOffset != Tier2HeaderBytes() || region.snapshotBytes != Tier2SnapshotBufferBytes(maxBots, maxNearby) ||
             region.actionOffset != Tier2ActionBufferOffset(maxBots, maxNearby, maxActions, 0) ||
             region.actionBytes != Tier2ActionBufferBytes(maxActions) || region.actionHeaderBytes != sizeof(ActionQueueHeader) ||
             region.actionStride != sizeof(BotAction) || header->max_party_views != Tier2PartyViewSlots(maxBots) ||
             header->max_party_members != Tier2PartyMemberSlots(maxBots)))
        {
            error = "Tier 2 region '" + regionName + "' has a geometry this SDK's layout " + std::to_string(kTier2LayoutVersion) + " does not give";
            return nullptr;
        }
        if (region.actionHeaderBytes < sizeof(ActionQueueHeader) || region.actionStride < sizeof(BotAction) ||
            region.snapshotOffset < sizeof(Tier2RegionHeader) || region.actionOffset < region.snapshotOffset + 2 * region.snapshotBytes ||
            region.actionBytes < region.actionHeaderBytes + std::size_t(maxActions) * region.actionStride ||
            region.map.get_size() < region.actionOffset + 2 * region.actionBytes)
        {
            error = "Tier 2 region '" + regionName + "' is smaller than its header says";
            return nullptr;
        }

        // The section table: the arrays this SDK knows must be there, each at least as wide as its struct here, and inside the buffer.
        auto resolve = [&](Tier2SectionKind kind, std::size_t elementSize, std::uint32_t wantCount, SectionSpan& span) -> bool
        {
            std::uint32_t const sections = std::min<std::uint32_t>(header->section_count, kMaxTier2Sections);
            for (std::uint32_t i = 0; i < sections; ++i)
            {
                Tier2Section const& section = header->sections[i];
                if (section.kind != static_cast<std::uint32_t>(kind))
                    continue;
                if (section.element_size < elementSize || (sameLayout && section.element_size != elementSize) || section.count != wantCount)
                    return false;
                if (section.offset > region.snapshotBytes ||
                    std::size_t(section.count) * section.element_size > region.snapshotBytes - std::size_t(section.offset))
                    return false;
                span.offset = std::size_t(section.offset);
                span.stride = section.element_size;
                span.count = section.count;
                return true;
            }
            return false;
        };
        if (!resolve(Tier2SectionKind::kBots, sizeof(BotSnapshot), maxBots, region.bots) ||
            !resolve(Tier2SectionKind::kNearbyUnits, sizeof(NearbyUnit), maxBots * maxNearby, region.nearby) ||
            !resolve(Tier2SectionKind::kPartyViews, sizeof(PartyView), header->max_party_views, region.partyViews) ||
            !resolve(Tier2SectionKind::kPartyMembers, sizeof(PartyMember), header->max_party_members, region.partyMembers) ||
            region.bots.offset < sizeof(WorldTickSnapshotHeader))
        {
            error = "Tier 2 region '" + regionName + "' has a section table this SDK cannot read (layout " +
                    std::to_string(header->layout_version) + ")";
            return nullptr;
        }
        return session;
    }
    catch (std::exception const& e)
    {
        // interprocess_exception, and std::bad_alloc from the copies below: a region that cannot be used is reported, not thrown.
        error = "cannot open Tier 2 region '" + regionName + "': " + e.what();
        return nullptr;
    }
}

std::uint32_t Tier2Session::MaxBots() const
{
    return region_->Header().max_bots;
}

std::uint32_t Tier2Session::MaxNearbyUnits() const
{
    return region_->Header().max_nearby_units;
}

std::uint32_t Tier2Session::MaxActions() const
{
    return region_->Header().max_actions;
}

std::uint32_t Tier2Session::TickInterval() const
{
    return region_->Header().tick_interval;
}

std::uint32_t Tier2Session::LayoutVersion() const
{
    return region_->Header().layout_version;
}

std::uint32_t Tier2Session::MaxPartyViews() const
{
    return region_->Header().max_party_views;
}

std::uint32_t Tier2Session::MaxPartyMembers() const
{
    return region_->Header().max_party_members;
}

WorldTickSnapshotView const& Tier2Session::ReadLatestSnapshot()
{
    Region const& region = *region_;
    Tier2RegionHeader const& header = region.Header();
    std::uint32_t const maxBots = header.max_bots;
    std::uint32_t const maxNearby = header.max_nearby_units;

    // The core writes a snapshot's tick, then its buffer index. A copy is whole when the index and the published tick
    // are the same before and after it: a publish during the copy changes the tick, so two publishes that bring the
    // index back to the same value are still caught. Three attempts. If none is whole, the previous snapshot is kept
    // rather than a mix of two.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        std::uint8_t const buffer = header.published_snapshot_buffer.load(std::memory_order_acquire);
        if (buffer > 1)
            break; // a corrupt index would point outside the region
        std::uint64_t const tick = header.published_snapshot_tick.load(std::memory_order_acquire);

        std::byte const* bufferBase = region.Base() + region.snapshotOffset + buffer * region.snapshotBytes;
        auto const* snapshot = reinterpret_cast<WorldTickSnapshotHeader const*>(bufferBase);

        WorldTickSnapshotView fresh;
        fresh.tick_number = snapshot->tick_number;
        std::uint32_t const count = std::min(snapshot->bot_count, maxBots);
        CopySection(fresh.bots, bufferBase + region.bots.offset, region.bots.stride, count);

        // Nearby units follow the bot array, maxNearby slots for every bot slot the core allotted.
        fresh.max_nearby_units = maxNearby;
        CopySection(fresh.nearby, bufferBase + region.nearby.offset, region.nearby.stride, std::size_t(count) * maxNearby);

        // Layout 25: the party views built this tick and their member pool.
        CopySection(fresh.party_views, bufferBase + region.partyViews.offset, region.partyViews.stride,
                    std::min(snapshot->party_view_count, region.partyViews.count));
        CopySection(fresh.party_members, bufferBase + region.partyMembers.offset, region.partyMembers.stride,
                    std::min(snapshot->party_member_count, region.partyMembers.count));

        if (header.published_snapshot_buffer.load(std::memory_order_acquire) == buffer &&
            header.published_snapshot_tick.load(std::memory_order_acquire) == tick)
        {
            lastSnapshot_ = std::move(fresh);
            haveSnapshot_ = true;
            break;
        }
    }
    return lastSnapshot_;
}

std::span<NearbyUnit const> WorldTickSnapshotView::NearbyOf(std::size_t botIndex) const
{
    if (botIndex >= bots.size())
        return {};
    std::size_t const count = std::min<std::size_t>(bots[botIndex].nearby_count, max_nearby_units);
    if (botIndex * max_nearby_units + count > nearby.size())
        return {};
    return std::span<NearbyUnit const>(nearby.data() + botIndex * max_nearby_units, count);
}

PartyView const* WorldTickSnapshotView::PartyViewOf(std::size_t botIndex) const
{
    if (botIndex >= bots.size())
        return nullptr;
    std::uint32_t const index = bots[botIndex].party_view_index;
    if (index == kNoPartyView || index >= party_views.size())
        return nullptr;
    return &party_views[index];
}

std::span<PartyMember const> WorldTickSnapshotView::MembersOf(PartyView const& view) const
{
    if (view.member_first > party_members.size() || view.member_count > party_members.size() - view.member_first)
        return {};
    return std::span<PartyMember const>(party_members.data() + view.member_first, view.member_count);
}

PartyMember const* WorldTickSnapshotView::MemberOf(std::size_t botIndex) const
{
    if (botIndex >= bots.size())
        return nullptr;
    std::uint32_t const index = bots[botIndex].party_member_index;
    if (index == kNoPartyView || index >= party_members.size())
        return nullptr;
    return &party_members[index];
}

std::span<Telegraph const> WorldTickSnapshotView::TelegraphsOf(std::size_t botIndex) const
{
    if (botIndex >= bots.size())
        return {};
    BotSnapshot const& bot = bots[botIndex];
    return std::span<Telegraph const>(bot.telegraphs, std::min<std::size_t>(bot.telegraph_count, kMaxTelegraphs));
}

std::span<ProbeResult const> WorldTickSnapshotView::ProbesOf(std::size_t botIndex) const
{
    if (botIndex >= bots.size())
        return {};
    BotSnapshot const& bot = bots[botIndex];
    return std::span<ProbeResult const>(bot.probes, std::min<std::size_t>(bot.probe_count, kMaxProbeResults));
}

bool Tier2Session::PublishActions(std::vector<BotAction> const& actions, std::string& error)
{
    Region const& region = *region_;
    Tier2RegionHeader& header = region.Header();

    // The actions answer a snapshot, and their tick is that snapshot's tick: without one read, the core could not match them.
    if (!haveSnapshot_)
    {
        error = "no snapshot has been read yet: call ReadLatestSnapshot() before PublishActions()";
        return false;
    }
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
    if (published > 1)
    {
        error = "corrupt action buffer index " + std::to_string(published);
        return false;
    }
    std::uint8_t const target = published ^ 1;
    std::byte* base = region.MutableBase() + region.actionOffset + target * region.actionBytes;

    // The queue header and each action are written with the core's sizes (the growth rule): the tail a newer core knows and this SDK does not reads
    // as zero there. A same-layout core has the same sizes, and the actions are one memcpy.
    std::memset(base, 0, region.actionHeaderBytes);
    auto* queue = new (base) ActionQueueHeader();
    queue->tick_number = lastSnapshot_.tick_number;
    queue->action_count = static_cast<std::uint32_t>(actions.size());
    std::byte* slots = base + region.actionHeaderBytes;
    if (region.actionStride == sizeof(BotAction))
    {
        if (!actions.empty())
            std::memcpy(static_cast<void*>(slots), actions.data(), actions.size() * sizeof(BotAction));
    }
    else
    {
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            std::memset(slots + i * region.actionStride, 0, region.actionStride);
            std::memcpy(static_cast<void*>(slots + i * region.actionStride), &actions[i], sizeof(BotAction));
        }
    }

    header.published_action_tick.store(lastSnapshot_.tick_number, std::memory_order_release);
    header.published_action_buffer.store(target, std::memory_order_release);
    return true;
}

} // namespace forever_module
