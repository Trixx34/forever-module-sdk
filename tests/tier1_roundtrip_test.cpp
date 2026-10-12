// Tier 1 round-trip test: a fake core on a Unix socket drives ModuleClient through the real wire
// protocol (length-prefixed Envelope frames). Covers accept + events + heartbeat + core Shutdown,
// protocol rejection, and Shutdown() called from another thread.

#include "forever_module/module_client.h"
#include "forever_module/protocol_version.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <boost/asio.hpp>

using namespace forever_module;
using boost::asio::local::stream_protocol;
using namespace std::chrono_literals;

namespace {

using Failures = std::vector<std::string>;

#define EXPECT_IN(fails, cond)                                                             \
    do {                                                                                    \
        if (!(cond))                                                                        \
            (fails).push_back(std::string(__FILE__) + ":" + std::to_string(__LINE__) +      \
                              " expected: " #cond);                                         \
    } while (0)

// Test-side framing (independent of the client's implementation, so it checks the wire format).
bool ReadEnvelope(stream_protocol::socket& socket, Envelope& envelope)
{
    boost::system::error_code ec;
    std::array<std::uint8_t, 4> header{};
    boost::asio::read(socket, boost::asio::buffer(header), ec);
    if (ec)
        return false;

    std::uint32_t const length = static_cast<std::uint32_t>(header[0]) |
                                 (static_cast<std::uint32_t>(header[1]) << 8) |
                                 (static_cast<std::uint32_t>(header[2]) << 16) |
                                 (static_cast<std::uint32_t>(header[3]) << 24);
    if (length == 0 || length > kMaxFrameBytes)
        return false;

    std::string body(length, '\0');
    boost::asio::read(socket, boost::asio::buffer(body), ec);
    if (ec)
        return false;
    return envelope.ParseFromString(body);
}

bool WriteEnvelope(stream_protocol::socket& socket, Envelope const& envelope)
{
    std::string body;
    if (!envelope.SerializeToString(&body))
        return false;

    auto const length = static_cast<std::uint32_t>(body.size());
    std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(length & 0xFF),
        static_cast<std::uint8_t>((length >> 8) & 0xFF),
        static_cast<std::uint8_t>((length >> 16) & 0xFF),
        static_cast<std::uint8_t>((length >> 24) & 0xFF),
    };
    std::array<boost::asio::const_buffer, 2> buffers{
        boost::asio::buffer(header),
        boost::asio::buffer(body),
    };
    boost::system::error_code ec;
    boost::asio::write(socket, buffers, ec);
    return !ec;
}

std::string SocketPath(char const* tag)
{
    return (std::filesystem::temp_directory_path() /
            ("fm-test-" + std::to_string(::getpid()) + "-" + tag + ".sock")).string();
}

ModuleManifest TestManifest()
{
    ModuleManifest manifest;
    manifest.module_name = "test-module";
    manifest.module_version = "0.0.1";
    manifest.tier1_hooks = {"PlayerScript.OnLogin", "CommandScript"};
    manifest.heartbeat_interval = 20ms;
    return manifest;
}

// Fake core for the accept scenario. Sends config, a notify, and a request; expects the reply, then
// waits for heartbeats. If sendShutdownEnvelope is true it ends with a core Shutdown, otherwise it
// waits for the module to finish (runReturned) so the module-side Shutdown() path is what ends Run().
void RunAcceptingCore(stream_protocol::acceptor& acceptor, bool sendShutdownEnvelope,
                      std::atomic<bool>& heartbeatsSeen, std::atomic<bool>& runReturned,
                      Failures& fails)
{
    stream_protocol::socket socket(acceptor.get_executor());
    acceptor.accept(socket);

    Envelope in;
    EXPECT_IN(fails, ReadEnvelope(socket, in));
    EXPECT_IN(fails, in.payload_case() == Envelope::kHandshakeRequest);
    if (in.payload_case() == Envelope::kHandshakeRequest)
    {
        HandshakeRequest const& req = in.handshake_request();
        EXPECT_IN(fails, req.module_name() == "test-module");
        EXPECT_IN(fails, req.protocol_version() == kProtocolVersion);
        EXPECT_IN(fails, req.sdk_package_version() == kSdkPackageVersion);
        EXPECT_IN(fails, req.tier1_hooks_size() == 2);
        EXPECT_IN(fails, req.requested_max_bots() == 0);
    }

    Envelope out;
    HandshakeResponse* response = out.mutable_handshake_response();
    response->set_accepted(true);
    response->set_module_id(7);
    response->set_negotiated_max_bots(16);
    response->set_negotiated_tick_interval(2);
    EXPECT_IN(fails, WriteEnvelope(socket, out));

    out.Clear();
    ConfigEntry* entry = out.mutable_module_config()->add_values();
    entry->set_key("greeting");
    entry->set_value("hello");
    EXPECT_IN(fails, WriteEnvelope(socket, out));

    out.Clear();
    EventNotify* notify = out.mutable_event_notify();
    notify->set_hook_name("PlayerScript.OnLogin");
    notify->mutable_player_login()->set_player_guid(42);
    EXPECT_IN(fails, WriteEnvelope(socket, out));

    out.Clear();
    out.set_message_id(99);
    EventRequest* request = out.mutable_event_request();
    request->set_message_id(99);
    request->set_hook_name("CommandScript");
    request->mutable_command()->set_command_text("ping");
    EXPECT_IN(fails, WriteEnvelope(socket, out));

    // Skip heartbeats until the reply to our request arrives.
    bool gotReply = false;
    while (ReadEnvelope(socket, in))
    {
        if (in.payload_case() == Envelope::kHeartbeat)
            continue;
        gotReply = in.payload_case() == Envelope::kEventReply;
        break;
    }
    EXPECT_IN(fails, gotReply);
    if (gotReply)
    {
        EXPECT_IN(fails, in.message_id() == 99);
        EXPECT_IN(fails, in.event_reply().message_id() == 99);
        EXPECT_IN(fails, in.event_reply().command().success());
        EXPECT_IN(fails, in.event_reply().command().result_text() == "pong");
    }

    // Wait for two heartbeats to confirm the module-side heartbeat thread is running.
    int heartbeats = 0;
    while (heartbeats < 2 && ReadEnvelope(socket, in))
    {
        if (in.payload_case() == Envelope::kHeartbeat)
            ++heartbeats;
    }
    EXPECT_IN(fails, heartbeats == 2);
    heartbeatsSeen = true;

    if (sendShutdownEnvelope)
    {
        out.Clear();
        out.mutable_shutdown()->set_reason("test");
        EXPECT_IN(fails, WriteEnvelope(socket, out));
    }
    else
    {
        // Keep the connection open until the module has returned from Run(), so the only thing that
        // can end Run() is the module's own Shutdown().
        while (!runReturned)
            std::this_thread::sleep_for(5ms);
    }

    boost::system::error_code ec;
    socket.shutdown(stream_protocol::socket::shutdown_both, ec);
    socket.close(ec);
}

// Runs the accept scenario end to end. Returns the failure list.
Failures RunAcceptScenario(char const* tag, bool sendShutdownEnvelope)
{
    Failures clientFails;
    Failures coreFails;

    std::string const path = SocketPath(tag);
    ::unlink(path.c_str());

    boost::asio::io_context io;
    stream_protocol::acceptor acceptor(io, stream_protocol::endpoint(path));

    std::atomic<bool> heartbeatsSeen{false};
    std::atomic<bool> runReturned{false};
    std::thread core([&]
    {
        RunAcceptingCore(acceptor, sendShutdownEnvelope, heartbeatsSeen, runReturned, coreFails);
    });

    ModuleClient client(TestManifest());
    EXPECT_IN(clientFails, client.Connect(path));
#ifdef __linux__
    int deathSignal = 0;
    ::prctl(PR_GET_PDEATHSIG, &deathSignal);
    EXPECT_IN(clientFails, deathSignal == SIGTERM);
#endif
    EXPECT_IN(clientFails, client.Handshake(2000ms));
    EXPECT_IN(clientFails, client.rejection_reason().empty());
    EXPECT_IN(clientFails, client.module_id() == 7);
    EXPECT_IN(clientFails, client.negotiated_bounds().max_bots == 16);
    EXPECT_IN(clientFails, client.negotiated_bounds().tick_interval == 2);
    EXPECT_IN(clientFails, client.tier2_region_name().empty());
    EXPECT_IN(clientFails, client.AttachTier2() == nullptr);

    // Config arrived during the handshake; OnConfig must deliver it immediately.
    bool configOk = false;
    client.OnConfig([&](std::unordered_map<std::string, std::string> const& cfg)
    {
        auto it = cfg.find("greeting");
        configOk = it != cfg.end() && it->second == "hello";
    });
    EXPECT_IN(clientFails, configOk);

    bool loginOk = false;
    client.OnNotify("PlayerScript.OnLogin", [&](EventNotify const& notify)
    {
        loginOk = notify.player_login().player_guid() == 42;
    });

    client.OnRequest("CommandScript", [](EventRequest const& request) -> EventReply
    {
        EventReply reply;
        reply.mutable_command()->set_success(request.command().command_text() == "ping");
        reply.mutable_command()->set_result_text("pong");
        return reply;
    });

    std::thread shutdownFromOutside;
    if (!sendShutdownEnvelope)
    {
        shutdownFromOutside = std::thread([&]
        {
            while (!heartbeatsSeen)
                std::this_thread::sleep_for(1ms);
            client.Shutdown();
        });
    }

    client.Run();
    runReturned = true;

    if (shutdownFromOutside.joinable())
        shutdownFromOutside.join();
    core.join();

    EXPECT_IN(clientFails, loginOk);

    Failures all = clientFails;
    all.insert(all.end(), coreFails.begin(), coreFails.end());
    return all;
}

Failures RunRejectScenario()
{
    Failures clientFails;
    Failures coreFails;

    std::string const path = SocketPath("reject");
    ::unlink(path.c_str());

    boost::asio::io_context io;
    stream_protocol::acceptor acceptor(io, stream_protocol::endpoint(path));

    std::thread core([&]
    {
        stream_protocol::socket socket(io);
        acceptor.accept(socket);

        Envelope in;
        EXPECT_IN(coreFails, ReadEnvelope(socket, in));

        Envelope out;
        HandshakeResponse* response = out.mutable_handshake_response();
        response->set_accepted(false);
        response->set_reason("protocol_version mismatch: core speaks \"1\", module declared \"0\"");
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
    });

    ModuleClient client(TestManifest());
    EXPECT_IN(clientFails, client.Connect(path));
    EXPECT_IN(clientFails, !client.Handshake(2000ms));
    EXPECT_IN(clientFails, client.rejection_reason().find("protocol_version mismatch") != std::string::npos);

    core.join();

    Failures all = clientFails;
    all.insert(all.end(), coreFails.begin(), coreFails.end());
    return all;
}

// Loot + action scenario: core sends a PlayerScript.OnLootItem notify, the module answers it with a
// SellItem action and, from that action's reply callback, a SendPlayerMessage action. The fake core
// checks both requests and answers them, then sends Shutdown.
Failures RunLootScenario()
{
    Failures clientFails;
    Failures coreFails;

    std::string const path = SocketPath("loot");
    ::unlink(path.c_str());

    boost::asio::io_context io;
    stream_protocol::acceptor acceptor(io, stream_protocol::endpoint(path));

    std::thread core([&]
    {
        stream_protocol::socket socket(acceptor.get_executor());
        acceptor.accept(socket);

        Envelope in;
        Envelope out;
        EXPECT_IN(coreFails, ReadEnvelope(socket, in));
        EXPECT_IN(coreFails, in.payload_case() == Envelope::kHandshakeRequest);
        if (in.payload_case() == Envelope::kHandshakeRequest)
        {
            EXPECT_IN(coreFails, in.handshake_request().tier1_hooks_size() == 1);
            EXPECT_IN(coreFails, in.handshake_request().tier1_hooks(0) == "PlayerScript.OnLootItem");
        }
        out.mutable_handshake_response()->set_accepted(true);
        out.mutable_handshake_response()->set_module_id(9);
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        out.Clear();
        out.mutable_module_config(); // empty config: no keys
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        out.Clear();
        EventNotify* notify = out.mutable_event_notify();
        notify->set_hook_name("PlayerScript.OnLootItem");
        PlayerItemLootedEvent* loot = notify->mutable_player_item_looted();
        loot->set_player_guid(42);
        loot->set_item_guid(5001);
        loot->set_item_entry(2589);
        loot->set_count(3);
        loot->set_quality(0);
        loot->set_item_class(7);
        loot->set_sell_price(20);
        loot->set_item_name("Tattered Cloth");
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        // First action: the sale. Heartbeats may arrive in between; skip them.
        bool gotSell = false;
        while (ReadEnvelope(socket, in))
        {
            if (in.payload_case() == Envelope::kHeartbeat)
                continue;
            gotSell = in.payload_case() == Envelope::kActionRequest && in.action_request().has_sell_item();
            break;
        }
        EXPECT_IN(coreFails, gotSell);
        std::uint64_t sellId = 0;
        if (gotSell)
        {
            sellId = in.action_request().message_id();
            SellItemAction const& sell = in.action_request().sell_item();
            EXPECT_IN(coreFails, in.message_id() == sellId);
            EXPECT_IN(coreFails, sell.player_guid() == 42);
            EXPECT_IN(coreFails, sell.item_guid() == 5001);
            EXPECT_IN(coreFails, sell.item_entry() == 2589);
            EXPECT_IN(coreFails, sell.count() == 3);

            out.Clear();
            out.set_message_id(sellId);
            ActionReply* reply = out.mutable_action_reply();
            reply->set_message_id(sellId);
            reply->set_success(true);
            reply->set_copper_gained(60);
            EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        }

        // Second action: the message, sent from the sell reply callback.
        bool gotMessage = false;
        while (ReadEnvelope(socket, in))
        {
            if (in.payload_case() == Envelope::kHeartbeat)
                continue;
            gotMessage = in.payload_case() == Envelope::kActionRequest && in.action_request().has_send_message();
            break;
        }
        EXPECT_IN(coreFails, gotMessage);
        if (gotMessage)
        {
            std::uint64_t const messageId = in.action_request().message_id();
            EXPECT_IN(coreFails, messageId != sellId);
            EXPECT_IN(coreFails, in.action_request().send_message().player_guid() == 42);
            EXPECT_IN(coreFails, in.action_request().send_message().text() == "sold for 60 copper");

            out.Clear();
            out.set_message_id(messageId);
            ActionReply* reply = out.mutable_action_reply();
            reply->set_message_id(messageId);
            reply->set_success(true);
            EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        }

        out.Clear();
        out.mutable_shutdown()->set_reason("test");
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        boost::system::error_code ec;
        socket.shutdown(stream_protocol::socket::shutdown_both, ec);
        socket.close(ec);
    });

    ModuleManifest manifest;
    manifest.module_name = "loot-module";
    manifest.module_version = "0.0.1";
    manifest.tier1_hooks = {"PlayerScript.OnLootItem"};
    manifest.heartbeat_interval = 20ms;

    ModuleClient client(manifest);
    EXPECT_IN(clientFails, client.Connect(path));
    EXPECT_IN(clientFails, client.Handshake(2000ms));

    bool lootNameOk = false;
    bool sellReplied = false;
    bool messageReplied = false;
    bool messageSucceeded = false;
    std::uint64_t copperGained = 0;

    client.OnNotify("PlayerScript.OnLootItem", [&](EventNotify const& notify)
    {
        PlayerItemLootedEvent const& loot = notify.player_item_looted();
        if (loot.quality() != 0 || loot.item_class() == 12 || loot.sell_price() == 0)
            return;
        lootNameOk = loot.item_name() == "Tattered Cloth";
        client.SellItem(loot.player_guid(), loot.item_guid(), loot.item_entry(), loot.count(),
                        [&](ActionReply const& reply)
        {
            sellReplied = reply.success();
            copperGained = reply.copper_gained();
            client.SendPlayerMessage(42, "sold for 60 copper", [&](ActionReply const& message)
            {
                messageReplied = true;
                messageSucceeded = message.success();
            });
        });
    });

    client.Run();
    core.join();

    EXPECT_IN(clientFails, lootNameOk);
    EXPECT_IN(clientFails, sellReplied);
    EXPECT_IN(clientFails, copperGained == 60);
    EXPECT_IN(clientFails, messageReplied);
    EXPECT_IN(clientFails, messageSucceeded);

    Failures all = clientFails;
    all.insert(all.end(), coreFails.begin(), coreFails.end());
    return all;
}

void Report(char const* name, Failures const& fails, int& failedScenarios)
{
    if (fails.empty())
    {
        std::printf("PASS  %s\n", name);
        return;
    }
    ++failedScenarios;
    std::printf("FAIL  %s\n", name);
    for (std::string const& f : fails)
        std::printf("      %s\n", f.c_str());
}

} // namespace

int main()
{
    int failedScenarios = 0;
    Report("accept, events, heartbeat, core Shutdown", RunAcceptScenario("accept", true), failedScenarios);
    Report("reject on protocol mismatch", RunRejectScenario(), failedScenarios);
    Report("module-side Shutdown() ends Run()", RunAcceptScenario("shutdown", false), failedScenarios);
    Report("loot notify answered with sell and message actions", RunLootScenario(), failedScenarios);
    return failedScenarios == 0 ? 0 : 1;
}
