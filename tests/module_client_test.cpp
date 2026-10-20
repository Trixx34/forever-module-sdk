// ModuleClient without a core: the Tier 2 convenience methods refuse what they cannot send, and tier2_error() says why.

#include "forever_module/module_client.h"

#include <cstdio>
#include <string>

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
