// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Tier2Session -- the module side of the Tier 2 shared-memory region. See doc/module-system/03-phase1-protocol-design.md
// (Tier 2 section) and doc/module-system/11-sdk-scope.md.
//
// Attach() maps the region the core created for this module (named in HandshakeResponse) and checks its layout
// version. ReadLatestSnapshot() copies the published snapshot. PublishActions() sends this module's bot actions back.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "forever_module/tier2_layout.h"

namespace forever_module {

// Typed, owning copy of the currently published WorldTickSnapshot for this module's bots.
struct WorldTickSnapshotView {
    std::uint64_t tick_number = 0;
    std::vector<BotSnapshot> bots; // only the bots this module controls

    // Nearby units for every bot, max_nearby_units slots per bot in bot order. Use NearbyOf() rather than indexing.
    std::vector<NearbyUnit> nearby;
    std::uint32_t max_nearby_units = 0;

    // Layout 25: the party views the core built this tick and the member pool they index into. Use PartyViewOf() and MembersOf()
    // rather than indexing: a bot's party_view_index is kNoPartyView when its group was not built.
    std::vector<PartyView> party_views;
    std::vector<PartyMember> party_members;

    // The units near bots[botIndex], up to its nearby_count. Empty if the index is out of range.
    std::span<NearbyUnit const> NearbyOf(std::size_t botIndex) const;

    // Layout 25: the group view of bots[botIndex], or nullptr (no group, the core's Bot.Snapshot.PartyView off, out of slots, bad index).
    PartyView const* PartyViewOf(std::size_t botIndex) const;
    // The members of a view, in the group's slot order; empty when the view's range is not inside the pool.
    std::span<PartyMember const> MembersOf(PartyView const& view) const;
    // The bot's own entry in its view, or nullptr.
    PartyMember const* MemberOf(std::size_t botIndex) const;
    // The enemy telegraphs near bots[botIndex], the first telegraph_count. Empty if the index is out of range.
    std::span<Telegraph const> TelegraphsOf(std::size_t botIndex) const;
    // The answered probes of bots[botIndex], oldest first (the first probe_count). Empty if the index is out of range.
    std::span<ProbeResult const> ProbesOf(std::size_t botIndex) const;
};

class Tier2Session {
public:
    // Maps the region named `regionName`. Returns nullptr and sets error if the region is missing, too small, or
    // written with a layout version this SDK does not understand: older than kTier2LayoutVersion, or newer with a
    // layout_base above it (the growth rule in tier2_layout.h). A newer region whose base this SDK meets is read
    // through its section table, with the core's element strides.
    static std::unique_ptr<Tier2Session> Attach(std::string const& regionName, std::string& error);

    ~Tier2Session();

    Tier2Session(Tier2Session const&) = delete;
    Tier2Session& operator=(Tier2Session const&) = delete;

    // Bounds the core granted, read from the region header.
    std::uint32_t MaxBots() const;
    std::uint32_t MaxNearbyUnits() const;
    std::uint32_t MaxActions() const;
    std::uint32_t TickInterval() const;
    // Layout 25: the layout the core writes (kTier2LayoutVersion or newer), and the party slots.
    std::uint32_t LayoutVersion() const;
    std::uint32_t MaxPartyViews() const;
    std::uint32_t MaxPartyMembers() const;

    // True once ReadLatestSnapshot() has copied a whole snapshot. PublishActions() needs one.
    bool HasSnapshot() const { return haveSnapshot_; }

    // Non-blocking. Returns the most recently published snapshot. If the core publishes while it is copied, the copy
    // is retried. If all retries are torn, the previous snapshot is returned, so the result is always one complete
    // snapshot (possibly an older one). Before the first whole copy the result is empty with tick_number 0.
    WorldTickSnapshotView const& ReadLatestSnapshot();

    // Publishes actions for the snapshot most recently read by ReadLatestSnapshot(); the snapshot's tick number is
    // sent with them so the core can tell which snapshot they answer. Returns false, with the reason in error, and
    // publishes nothing if no snapshot has been read yet, if there are more actions than MaxActions(), or if an
    // action has an unknown type. The core checks each bot_guid against its own ownership, so an action for a bot
    // this module does not control is dropped there. Each call replaces the whole action queue for that tick: the
    // core reads one queue per tick, so pass every action for the tick in one call.
    bool PublishActions(std::vector<BotAction> const& actions, std::string& error);

private:
    Tier2Session();

    struct Region;
    std::unique_ptr<Region> region_;
    WorldTickSnapshotView lastSnapshot_;
    bool haveSnapshot_ = false;
};

} // namespace forever_module
