// Smallest possible module using forever_module_sdk: connects to core, handshakes, and logs when the
// world starts. It is the fork point for the core repo's "Hello Module" walkthrough. Tier 2 and bot
// control are not in this SDK version, so this module uses Tier 1 only.

#include <iostream>

#include "forever_module/module_client.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: minimal_module <tier1-endpoint>\n";
        return 1;
    }

    forever_module::ModuleManifest manifest;
    manifest.module_name = "minimal-module";
    manifest.module_version = "0.1.0";
    manifest.tier1_hooks = {"WorldScript.OnStartup"};

    forever_module::ModuleClient client(std::move(manifest));

    if (!client.Connect(argv[1])) {
        std::cerr << "minimal-module: failed to connect to " << argv[1] << "\n";
        return 1;
    }

    if (!client.Handshake()) {
        std::cerr << "minimal-module: handshake failed\n";
        return 1;
    }

    client.OnNotify("WorldScript.OnStartup", [](forever_module::EventNotify const&) {
        std::cout << "minimal-module: world started\n";
    });

    client.Run();
    return 0;
}
