#include "forever_module/tier2_session.h"

// SKELETON ONLY -- see header comment. No real boost::interprocess implementation yet.

namespace forever_module {

Tier2Session::Tier2Session() = default;
Tier2Session::~Tier2Session() = default;

WorldTickSnapshotView const& Tier2Session::ReadLatestSnapshot()
{
    // TODO: read Tier2RegionHeader::published_snapshot_buffer/published_snapshot_tick atomically,
    // populate lastSnapshot_ from the corresponding buffer in shared memory (or return a
    // non-owning view once the real offset-addressing accessors exist, per the header TODO).
    return lastSnapshot_;
}

void Tier2Session::PublishActions(std::vector<BotAction> const& /*actions*/)
{
    // TODO: write into this module's unpublished ActionQueue buffer, then atomically update
    // published_action_tick/published_action_buffer in the Tier2RegionHeader.
}

} // namespace forever_module
