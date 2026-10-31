// SPDX-License-Identifier: GPL-2.0-or-later
// ModuleClient without a core: the Tier 2 convenience methods refuse what they cannot send, and tier2_error() says why.

#include "forever_module/module_client.h"
#include "forever_module/talent_spec.h"

#include <cstdio>
#include <string>
#include <string_view>

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

} // namespace

int main()
{
    // The talent spec table (protocol 18): rows in id order, every spec of the nine classes, a key unique inside its class, and the lookups agree.
    {
        CHECK(kTalentSpecs.size() == 28 && kLastTalentSpec == 28);
        CHECK(GetTalentSpecInfo(TalentSpec::kNone) == nullptr);
        CHECK(GetTalentSpecInfo(static_cast<TalentSpec>(kLastTalentSpec + 1)) == nullptr);
        for (std::uint32_t id = 1; id <= kLastTalentSpec; ++id)
        {
            TalentSpecInfo const* info = GetTalentSpecInfo(static_cast<TalentSpec>(id));
            CHECK(info && static_cast<std::uint32_t>(info->Spec) == id);
            if (!info)
                continue;
            CHECK(TalentSpecFromKey(info->ClassId, info->Key) == info->Spec);
            for (std::uint32_t other = id + 1; other <= kLastTalentSpec; ++other)
            {
                TalentSpecInfo const* o = GetTalentSpecInfo(static_cast<TalentSpec>(other));
                CHECK(!(o->ClassId == info->ClassId && std::string_view(o->Key) == info->Key));
            }
        }
        CHECK(TalentSpecFromKey(3, "bm") == TalentSpec::kHunterBm);
        CHECK(TalentSpecFromKey(1, "prot") == TalentSpec::kWarriorProt && TalentSpecFromKey(2, "prot") == TalentSpec::kPaladinProt);
        CHECK(TalentSpecFromKey(7, "resto") == TalentSpec::kShamanResto && TalentSpecFromKey(11, "resto") == TalentSpec::kDruidResto);
        CHECK(TalentSpecFromKey(11, "feral_cat") == TalentSpec::kDruidFeralCat && TalentSpecFromKey(11, "feral_tank") == TalentSpec::kDruidFeralTank);
        CHECK(TalentSpecFromKey(3, "prot") == TalentSpec::kNone);                                       // not a hunter spec
        CHECK(TalentSpecFromKey(3, "dps") == TalentSpec::kNone && TalentSpecFromKey(3, "tank") == TalentSpec::kNone && TalentSpecFromKey(5, "healer") == TalentSpec::kNone); // roles are not specs
        CHECK(TalentSpecFromKey(6, "bm") == TalentSpec::kNone);                                         // class 6 (death knight) does not exist here
    }

    // A quest id must fit in the 24 bits under the operation byte. Anything larger is refused, not sent with its
    // high bits merged into the operation.
    {
        ModuleClient client{ModuleManifest{}};
        client.QuestAccept(42, 0x01000000u);
        CHECK(client.tier2_error().find("24 bits") != std::string::npos);
    }
    {
        ModuleClient client{ModuleManifest{}};
        client.QuestTurnIn(42, 0xFFFFFFFFu);
        CHECK(client.tier2_error().find("24 bits") != std::string::npos);
    }
    {
        ModuleClient client{ModuleManifest{}};
        client.QuestAbandon(42, 0x00FFFFFFu); // the largest id that fits is not refused for its size
        CHECK(client.tier2_error().find("24 bits") == std::string::npos);
    }

    // With no Tier 2 region granted, a bot action is refused and the reason is recorded, not dropped silently.
    {
        ModuleClient client{ModuleManifest{}};
        CHECK(client.AttachTier2() == nullptr);
        CHECK(client.tier2_error().find("no Tier 2 region") != std::string::npos);
        client.QuestAccept(42, 7);
        CHECK(client.tier2_error().find("no Tier 2 region") != std::string::npos);
    }

    if (g_failures == 0)
    {
        std::printf("PASS  ModuleClient Tier 2 refusals\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
