#pragma once

// Tier2Session -- the per-tick shared-memory decision loop surface a module author writes
// against. See doc/module-system/03-phase1-protocol-design.md (Tier 2 section) and
// doc/module-system/11-sdk-scope.md.
//
// SKELETON ONLY: tier2_session.cpp stubs these out. No real boost::interprocess shared-memory
// attach/read/write yet, no offset math into the region described in tier2_layout.h.

#include <cstdint>
#include <vector>

#include "forever_module/tier2_layout.h"

namespace forever_module {

// Typed, read-only view over the currently-published WorldTickSnapshot for this module's owned
// bots. Backed by the shared-memory region attached in Tier2Session -- TODO: real offset-based
// accessors instead of this placeholder owning-copy approach.
struct WorldTickSnapshotView {
    std::uint64_t tick_number = 0;
    std::vector<BotSnapshot> bots; // TODO: should be a non-owning view into shared memory, not a copy
};

class Tier2Session {
public:
    Tier2Session();
    ~Tier2Session();

    Tier2Session(Tier2Session const&) = delete;
    Tier2Session& operator=(Tier2Session const&) = delete;

    // Non-blocking: returns whichever snapshot buffer is currently published (may be the same one
    // as the last call if core hasn't written a new one yet per the module's tick_interval).
    WorldTickSnapshotView const& ReadLatestSnapshot();

    // Writes to this module's unpublished action buffer and flips the atomic publish index.
    // Non-blocking -- core's tick loop reads whatever is published whenever it gets to it
    // (doc/module-system/02-phase0-decisions.md §Tier 2 blocking policy: non-blocking,
    // latest-available).
    void PublishActions(std::vector<BotAction> const& actions);

private:
    WorldTickSnapshotView lastSnapshot_;
    // TODO: boost::interprocess::managed_shared_memory handle, Tier2RegionHeader*, offset
    // calculations for the bots/nearby/actions flexible arrays.
};

} // namespace forever_module
