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
        EXPECT_IN(fails, std::string(kSdkPackageVersion) == FOREVER_SDK_CMAKE_VERSION); // CMakeLists.txt and protocol_version.h agree
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

// Control scenario: the module asks core to spawn a bot; core grants bot 777, revokes it, and the module
// releases it back. The fake core checks each message and ends with Shutdown.
Failures RunControlScenario()
{
    Failures clientFails;
    Failures coreFails;

    std::string const path = SocketPath("control");
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
        out.mutable_handshake_response()->set_accepted(true);
        out.mutable_handshake_response()->set_module_id(10);
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        out.Clear();
        out.mutable_module_config();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        // Spawn request.
        std::uint64_t requestId = 0;
        bool gotSpawn = false;
        while (ReadEnvelope(socket, in))
        {
            if (in.payload_case() == Envelope::kHeartbeat)
                continue;
            gotSpawn = in.payload_case() == Envelope::kControlRequest;
            break;
        }
        EXPECT_IN(coreFails, gotSpawn);
        if (gotSpawn)
        {
            requestId = in.message_id();
            EXPECT_IN(coreFails, in.control_request().bot_guid() == 0);
            EXPECT_IN(coreFails, in.control_request().spawn_params().race() == 1);
            EXPECT_IN(coreFails, in.control_request().spawn_params().level() == 80);
            EXPECT_IN(coreFails, in.control_request().spawn_params().place() && in.control_request().spawn_params().map_id() == 36);
            EXPECT_IN(coreFails, in.control_request().spawn_params().pos_x() == -16.5f && in.control_request().spawn_params().pos_z() == 61.8f);

            out.Clear();
            out.set_message_id(requestId);
            out.mutable_control_response()->set_granted(true);
            out.mutable_control_response()->set_bot_guid(777);
            EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        }

        // Core takes the bot back; the module answers with a release.
        out.Clear();
        out.mutable_control_revoked()->set_bot_guid(777);
        out.mutable_control_revoked()->set_reason("bot deleted");
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        bool gotRelease = false;
        while (ReadEnvelope(socket, in))
        {
            if (in.payload_case() == Envelope::kHeartbeat)
                continue;
            gotRelease = in.payload_case() == Envelope::kReleaseControl;
            break;
        }
        EXPECT_IN(coreFails, gotRelease);
        if (gotRelease)
            EXPECT_IN(coreFails, in.release_control().bot_guid() == 777);

        out.Clear();
        out.mutable_shutdown()->set_reason("test");
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        boost::system::error_code ec;
        socket.shutdown(stream_protocol::socket::shutdown_both, ec);
        socket.close(ec);
    });

    ModuleManifest manifest;
    manifest.module_name = "control-module";
    manifest.module_version = "0.0.1";
    manifest.heartbeat_interval = 20ms;

    ModuleClient client(manifest);
    EXPECT_IN(clientFails, client.Connect(path));
    EXPECT_IN(clientFails, client.Handshake(2000ms));

    bool granted = false;
    std::uint64_t grantedGuid = 0;
    bool revoked = false;
    std::string revokedReason;

    client.OnControlRevoked([&](std::uint64_t botGuid, std::string const& reason)
    {
        revoked = botGuid == grantedGuid;
        revokedReason = reason;
        client.ReleaseControl(botGuid);
    });

    SpawnParams params;
    params.set_race(1);
    params.set_level(80);
    params.set_place(true);
    params.set_map_id(36);
    params.set_pos_x(-16.5f);
    params.set_pos_y(-383.0f);
    params.set_pos_z(61.8f);
    client.RequestControl(0, params, [&](ControlResponse const& response)
    {
        granted = response.granted();
        grantedGuid = response.bot_guid();
    });

    client.Run();
    core.join();

    EXPECT_IN(clientFails, granted);
    EXPECT_IN(clientFails, grantedGuid == 777);
    EXPECT_IN(clientFails, revoked);
    EXPECT_IN(clientFails, revokedReason == "bot deleted");

    Failures all = clientFails;
    all.insert(all.end(), coreFails.begin(), coreFails.end());
    return all;
}

// Fake core for the timer scenario: handshake and an empty config, then 200 ms of silence while the module's timer
// runs, then Shutdown.
void RunTimerCore(stream_protocol::acceptor& acceptor, Failures& fails)
{
    stream_protocol::socket socket(acceptor.get_executor());
    acceptor.accept(socket);

    Envelope in;
    EXPECT_IN(fails, ReadEnvelope(socket, in));
    EXPECT_IN(fails, in.payload_case() == Envelope::kHandshakeRequest);

    Envelope out;
    HandshakeResponse* response = out.mutable_handshake_response();
    response->set_accepted(true);
    response->set_module_id(9);
    EXPECT_IN(fails, WriteEnvelope(socket, out));

    out.Clear();
    out.mutable_module_config();
    EXPECT_IN(fails, WriteEnvelope(socket, out));

    std::this_thread::sleep_for(200ms);

    out.Clear();
    out.mutable_shutdown()->set_reason("test");
    EXPECT_IN(fails, WriteEnvelope(socket, out));

    boost::system::error_code ec;
    socket.shutdown(stream_protocol::socket::shutdown_both, ec);
    socket.close(ec);
}

// A 20 ms timer runs on the thread that calls Run() while the core is silent, and Run() still returns on Shutdown.
Failures RunTimerScenario()
{
    Failures clientFails;
    Failures coreFails;

    std::string const path = SocketPath("timer");
    ::unlink(path.c_str());

    boost::asio::io_context io;
    stream_protocol::acceptor acceptor(io, stream_protocol::endpoint(path));
    std::thread core([&] { RunTimerCore(acceptor, coreFails); });

    ModuleClient client(TestManifest());
    EXPECT_IN(clientFails, client.Connect(path));
    EXPECT_IN(clientFails, client.Handshake(2000ms));

    int fires = 0;
    bool onRunThread = true;
    std::thread::id const runThread = std::this_thread::get_id();
    client.AddTimer(20ms, [&]
    {
        ++fires;
        onRunThread = onRunThread && std::this_thread::get_id() == runThread;
    });

    client.Run();
    core.join();

    // 200 ms at 20 ms per fire is about ten fires; allow scheduling slack either way.
    EXPECT_IN(clientFails, fires >= 5);
    EXPECT_IN(clientFails, fires <= 20);
    EXPECT_IN(clientFails, onRunThread);
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

Failures RunEconomyActorScenario()
{
    Failures clientFails;
    Failures coreFails;
    std::string const path = SocketPath("economy-actor");
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
        out.mutable_handshake_response()->set_accepted(true);
        out.mutable_handshake_response()->set_module_id(12);
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        out.Clear();
        out.mutable_module_config();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        out.Clear();
        out.mutable_event_notify()->set_hook_name("WorldScript.OnStartup");
        out.mutable_event_notify()->mutable_world_startup();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        auto readAction = [&](Envelope& envelope)
        {
            while (ReadEnvelope(socket, envelope))
                if (envelope.payload_case() == Envelope::kActionRequest)
                    return true;
            return false;
        };
        bool const gotBuy = readAction(in);
        EXPECT_IN(coreFails, gotBuy);
        if (gotBuy)
        {
            EXPECT_IN(coreFails, in.action_request().has_buy_item());
            BuyItemAction const& buy = in.action_request().buy_item();
            EXPECT_IN(coreFails, buy.player_guid() == 42);
            EXPECT_IN(coreFails, buy.vendor_guid() == 9001 && buy.item_entry() == 2512 && buy.count() == 200);
            std::uint64_t const id = in.action_request().message_id();
            out.Clear();
            out.set_message_id(id);
            ActionReply* reply = out.mutable_action_reply();
            reply->set_message_id(id);
            reply->set_success(true);
            reply->set_copper_spent(100);
            reply->set_item_entry(2512);
            EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        }

        bool const gotBrowse = readAction(in);
        EXPECT_IN(coreFails, gotBrowse);
        if (gotBrowse)
        {
            EXPECT_IN(coreFails, in.action_request().has_auction_house());
            AuctionHouseAction const& ah = in.action_request().auction_house();
            std::uint64_t const id = in.action_request().message_id();
            EXPECT_IN(coreFails, ah.player_guid() == 42);
            EXPECT_IN(coreFails, ah.op() == AuctionHouseAction::kBrowse && ah.auctioneer_guid() == 9002 && ah.item_entry() == 2512);
            out.Clear();
            out.set_message_id(id);
            ActionReply* reply = out.mutable_action_reply();
            reply->set_message_id(id);
            reply->set_success(true);
            EXPECT_IN(coreFails, WriteEnvelope(socket, out));
            out.Clear();
            EventNotify* notify = out.mutable_event_notify();
            notify->set_hook_name("PlayerScript.OnAuctionResult");
            AuctionListingEvent* listing = notify->mutable_player_auction_result();
            listing->set_player_guid(42);
            listing->set_request_id(id);
            listing->set_auction_guid(777);
            listing->set_item_entry(2512);
            listing->set_count(200);
            listing->set_query_item(2512);
            listing->set_total_matches(1);
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
    manifest.module_name = "economy-module";
    manifest.module_version = "0.0.1";
    manifest.tier1_hooks = {"WorldScript.OnStartup", "PlayerScript.OnAuctionResult"};
    manifest.heartbeat_interval = 20ms;
    ModuleClient client(manifest);
    std::uint64_t browseId = 0;
    bool gotBuyReply = false;
    bool gotBrowseReply = false;
    bool gotListing = false;
    client.OnNotify("WorldScript.OnStartup", [&](EventNotify const&)
    {
        client.BuyItemForBot(42, 9001, 2512, 200, [&](ActionReply const& reply)
        {
            gotBuyReply = reply.success() && reply.copper_spent() == 100 && reply.item_entry() == 2512;
        });
        browseId = client.AuctionHouseForBot(42, ModuleClient::AuctionHouseOp::kBrowse,
                                             9002, 0, 0, 2512, 0, 0, 0, [&](ActionReply const& reply)
        {
            gotBrowseReply = reply.success();
        });
    });
    client.OnNotify("PlayerScript.OnAuctionResult", [&](EventNotify const& notify)
    {
        AuctionListingEvent const& listing = notify.player_auction_result();
        gotListing = listing.player_guid() == 42 && listing.request_id() == browseId &&
                     listing.auction_guid() == 777 && listing.item_entry() == 2512 && listing.count() == 200;
    });
    EXPECT_IN(clientFails, client.Connect(path));
    EXPECT_IN(clientFails, client.Handshake(2000ms));
    client.Run();
    EXPECT_IN(clientFails, gotBuyReply);
    EXPECT_IN(clientFails, gotBrowseReply);
    EXPECT_IN(clientFails, gotListing);
    core.join();
    clientFails.insert(clientFails.end(), coreFails.begin(), coreFails.end());
    return clientFails;
}

} // namespace

// Share scenario: core sends a quest item loot notify, the module answers it with a ShareQuestItem action, and the fake core replies
// with the number of recipients.
Failures RunShareScenario()
{
    Failures clientFails;
    Failures coreFails;

    std::string const path = SocketPath("share");
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
        out.mutable_handshake_response()->set_accepted(true);
        out.mutable_handshake_response()->set_module_id(11);
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        out.Clear();
        out.mutable_module_config();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        out.Clear();
        EventNotify* notify = out.mutable_event_notify();
        notify->set_hook_name("PlayerScript.OnLootItem");
        PlayerItemLootedEvent* loot = notify->mutable_player_item_looted();
        loot->set_player_guid(42);
        loot->set_item_guid(6001);
        loot->set_item_entry(3000);
        loot->set_count(1);
        loot->set_quality(1);
        loot->set_item_class(12); // a quest item
        loot->set_item_name("Wolf Pelt");
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        bool gotShare = false;
        while (ReadEnvelope(socket, in))
        {
            if (in.payload_case() == Envelope::kHeartbeat)
                continue;
            gotShare = in.payload_case() == Envelope::kActionRequest && in.action_request().has_share_quest_item();
            break;
        }
        EXPECT_IN(coreFails, gotShare);
        if (gotShare)
        {
            std::uint64_t const id = in.action_request().message_id();
            ShareQuestItemAction const& share = in.action_request().share_quest_item();
            EXPECT_IN(coreFails, share.player_guid() == 42);
            EXPECT_IN(coreFails, share.item_entry() == 3000);
            EXPECT_IN(coreFails, share.count() == 1);
            EXPECT_IN(coreFails, share.range_yards() == 80);

            out.Clear();
            out.set_message_id(id);
            ActionReply* reply = out.mutable_action_reply();
            reply->set_message_id(id);
            reply->set_success(true);
            reply->set_recipients(2);
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
    manifest.module_name = "share-module";
    manifest.module_version = "0.0.1";
    manifest.tier1_hooks = {"PlayerScript.OnLootItem"};
    manifest.heartbeat_interval = 20ms;

    ModuleClient client(manifest);
    EXPECT_IN(clientFails, client.Connect(path));
    EXPECT_IN(clientFails, client.Handshake(2000ms));

    bool replied = false;
    std::uint32_t recipients = 0;
    client.OnNotify("PlayerScript.OnLootItem", [&](EventNotify const& notify)
    {
        PlayerItemLootedEvent const& loot = notify.player_item_looted();
        if (loot.item_class() != 12)
            return;
        client.ShareQuestItem(loot.player_guid(), loot.item_entry(), loot.count(), 80, [&](ActionReply const& reply)
        {
            replied = reply.success();
            recipients = reply.recipients();
        });
    });

    client.Run();
    core.join();

    EXPECT_IN(clientFails, replied);
    EXPECT_IN(clientFails, recipients == 2);

    Failures all = clientFails;
    all.insert(all.end(), coreFails.begin(), coreFails.end());
    return all;
}

// Bot speech and bot trade: the module whispers as a bot, lists the bot's tradeable bags (a reply with items), puts one in a trade slot,
// and receives a trade event carrying what the partner offers.
Failures RunBotTradeScenario()
{
    Failures clientFails;
    Failures coreFails;
    std::string const path = SocketPath("bot-trade");
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
        out.mutable_handshake_response()->set_accepted(true);
        out.mutable_handshake_response()->set_module_id(13);
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        out.Clear();
        out.mutable_module_config();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        out.Clear();
        out.mutable_event_notify()->set_hook_name("WorldScript.OnStartup");
        out.mutable_event_notify()->mutable_world_startup();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        auto readAction = [&](Envelope& envelope)
        {
            while (ReadEnvelope(socket, envelope))
                if (envelope.payload_case() == Envelope::kActionRequest)
                    return true;
            return false;
        };
        auto reply = [&](std::uint64_t id, bool withBags)
        {
            out.Clear();
            out.set_message_id(id);
            ActionReply* r = out.mutable_action_reply();
            r->set_message_id(id);
            r->set_success(true);
            if (withBags)
            {
                TradeBagItem* a = r->add_bag_items();
                a->set_item_guid(1001);
                a->set_entry(2589);
                a->set_count(20);
                a->set_name("Linen Cloth");
                TradeBagItem* b = r->add_bag_items();
                b->set_item_guid(1002);
                b->set_entry(6948);
                b->set_count(1);
                b->set_name("Hearthstone");
            }
            EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        };

        bool const gotSpeak = readAction(in);
        EXPECT_IN(coreFails, gotSpeak);
        if (gotSpeak)
        {
            EXPECT_IN(coreFails, in.action_request().has_bot_speak());
            BotSpeakAction const& speak = in.action_request().bot_speak();
            EXPECT_IN(coreFails, speak.bot_guid() == 42 && speak.kind() == 1 && speak.player_guid() == 7 && speak.text() == "Hello");
            reply(in.action_request().message_id(), false);
        }

        bool const gotList = readAction(in);
        EXPECT_IN(coreFails, gotList);
        if (gotList)
        {
            EXPECT_IN(coreFails, in.action_request().has_bot_trade());
            BotTradeAction const& trade = in.action_request().bot_trade();
            EXPECT_IN(coreFails, trade.op() == BotTradeAction::kListBags && trade.bot_guid() == 42);
            reply(in.action_request().message_id(), true);
        }

        bool const gotSet = readAction(in);
        EXPECT_IN(coreFails, gotSet);
        if (gotSet)
        {
            EXPECT_IN(coreFails, in.action_request().has_bot_trade());
            BotTradeAction const& trade = in.action_request().bot_trade();
            EXPECT_IN(coreFails, trade.op() == BotTradeAction::kSetItem && trade.bot_guid() == 42 && trade.slot() == 0 &&
                                     trade.item_guid() == 1001);
            reply(in.action_request().message_id(), false);
        }

        out.Clear();
        EventNotify* notify = out.mutable_event_notify();
        notify->set_hook_name("PlayerScript.OnBotTrade");
        BotTradeEvent* event = notify->mutable_bot_trade();
        event->set_bot_guid(42);
        event->set_partner_guid(7);
        event->set_status(3);
        event->set_partner_copper(150);
        TradeOfferedItem* offered = event->add_offered();
        offered->set_slot(0);
        offered->set_entry(2589);
        offered->set_count(5);
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        out.Clear();
        out.mutable_shutdown()->set_reason("test");
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        boost::system::error_code ec;
        socket.shutdown(stream_protocol::socket::shutdown_both, ec);
        socket.close(ec);
    });

    ModuleManifest manifest;
    manifest.module_name = "bot-trade-module";
    manifest.module_version = "0.0.1";
    manifest.tier1_hooks = {"WorldScript.OnStartup", "PlayerScript.OnBotTrade"};
    manifest.heartbeat_interval = 20ms;
    ModuleClient client(manifest);
    bool gotSpeakReply = false;
    bool gotBags = false;
    bool gotSetReply = false;
    bool gotEvent = false;
    client.OnNotify("WorldScript.OnStartup", [&](EventNotify const&)
    {
        client.BotSpeak(42, ModuleClient::SpeakKind::kWhisper, 7, "Hello", [&](ActionReply const& r) { gotSpeakReply = r.success(); });
        client.BotTrade(42, ModuleClient::BotTradeOp::kListBags, 0, 0, 0, [&](ActionReply const& r)
        {
            gotBags = r.success() && r.bag_items_size() == 2 && r.bag_items(0).item_guid() == 1001 && r.bag_items(0).entry() == 2589 &&
                      r.bag_items(0).count() == 20 && r.bag_items(0).name() == "Linen Cloth" && r.bag_items(1).name() == "Hearthstone";
            client.BotTrade(42, ModuleClient::BotTradeOp::kSetItem, 0, 1001, 0, [&](ActionReply const& r2) { gotSetReply = r2.success(); });
        });
    });
    client.OnNotify("PlayerScript.OnBotTrade", [&](EventNotify const& notify)
    {
        BotTradeEvent const& event = notify.bot_trade();
        gotEvent = event.bot_guid() == 42 && event.partner_guid() == 7 && event.status() == 3 && event.partner_copper() == 150 &&
                   event.offered_size() == 1 && event.offered(0).slot() == 0 && event.offered(0).entry() == 2589 && event.offered(0).count() == 5;
    });
    EXPECT_IN(clientFails, client.Connect(path));
    EXPECT_IN(clientFails, client.Handshake(2000ms));
    client.Run();
    core.join();

    EXPECT_IN(clientFails, gotSpeakReply);
    EXPECT_IN(clientFails, gotBags);
    EXPECT_IN(clientFails, gotSetReply);
    EXPECT_IN(clientFails, gotEvent);

    Failures all = clientFails;
    all.insert(all.end(), coreFails.begin(), coreFails.end());
    return all;
}

// SpendBotTalents (protocol 18): the old call sends no spec, the new one sends role and spec, kNone leaves the field out.
Failures RunTalentsScenario()
{
    Failures clientFails;
    Failures coreFails;
    std::string const path = SocketPath("talents");
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
        out.mutable_handshake_response()->set_accepted(true);
        out.mutable_handshake_response()->set_module_id(14);
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        out.Clear();
        out.mutable_module_config();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        out.Clear();
        out.mutable_event_notify()->set_hook_name("WorldScript.OnStartup");
        out.mutable_event_notify()->mutable_world_startup();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        auto readAction = [&](Envelope& envelope)
        {
            while (ReadEnvelope(socket, envelope))
                if (envelope.payload_case() == Envelope::kActionRequest)
                    return true;
            return false;
        };
        auto reply = [&](std::uint64_t id, bool success)
        {
            out.Clear();
            out.set_message_id(id);
            ActionReply* r = out.mutable_action_reply();
            r->set_message_id(id);
            r->set_success(success);
            if (!success)
                r->set_reason("in combat");
            EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        };

        // 1: the protocol 17 call (role only): no spec on the wire.
        bool got = readAction(in);
        EXPECT_IN(coreFails, got);
        if (got)
        {
            EXPECT_IN(coreFails, in.action_request().has_spend_bot_talents());
            SpendBotTalentsAction const& talents = in.action_request().spend_bot_talents();
            EXPECT_IN(coreFails, talents.bot_guid() == 42 && talents.role() == 3 && !talents.has_spec());
            reply(in.action_request().message_id(), true);
        }
        // 2: role and spec (hunter Beast Mastery = 7).
        got = readAction(in);
        EXPECT_IN(coreFails, got);
        if (got)
        {
            EXPECT_IN(coreFails, in.action_request().has_spend_bot_talents());
            SpendBotTalentsAction const& talents = in.action_request().spend_bot_talents();
            EXPECT_IN(coreFails, talents.bot_guid() == 43 && talents.role() == 3 && talents.has_spec() && talents.spec() == 7);
            reply(in.action_request().message_id(), true);
        }
        // 3: healer role with the druid Restoration spec (28), answered with a refusal.
        got = readAction(in);
        EXPECT_IN(coreFails, got);
        if (got)
        {
            EXPECT_IN(coreFails, in.action_request().has_spend_bot_talents());
            SpendBotTalentsAction const& talents = in.action_request().spend_bot_talents();
            EXPECT_IN(coreFails, talents.bot_guid() == 44 && talents.role() == 2 && talents.spec() == 28);
            reply(in.action_request().message_id(), false);
        }

        out.Clear();
        out.mutable_shutdown()->set_reason("test");
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        boost::system::error_code ec;
        socket.shutdown(stream_protocol::socket::shutdown_both, ec);
        socket.close(ec);
    });

    ModuleManifest manifest;
    manifest.module_name = "talents-module";
    manifest.module_version = "0.0.1";
    manifest.tier1_hooks = {"WorldScript.OnStartup"};
    manifest.heartbeat_interval = 20ms;
    ModuleClient client(manifest);
    bool gotOld = false;
    bool gotSpec = false;
    bool gotRefusal = false;
    client.OnNotify("WorldScript.OnStartup", [&](EventNotify const&)
    {
        client.SpendBotTalents(42, ModuleClient::TalentRole::kDps, [&](ActionReply const& r) { gotOld = r.success(); });
        client.SpendBotTalents(43, ModuleClient::TalentRole::kDps, TalentSpec::kHunterBm, [&](ActionReply const& r) { gotSpec = r.success(); });
        client.SpendBotTalents(44, ModuleClient::TalentRole::kHealer, TalentSpec::kDruidResto,
                               [&](ActionReply const& r) { gotRefusal = !r.success() && r.reason() == "in combat"; });
    });
    EXPECT_IN(clientFails, client.Connect(path));
    EXPECT_IN(clientFails, client.Handshake(2000ms));
    client.Run();
    core.join();

    EXPECT_IN(clientFails, gotOld);
    EXPECT_IN(clientFails, gotSpec);
    EXPECT_IN(clientFails, gotRefusal);

    Failures all = clientFails;
    all.insert(all.end(), coreFails.begin(), coreFails.end());
    return all;
}

// Auction browse by filter (protocol 19): the filter fields reach the core, the listings come back in the reply with the new fields, and a
// buyout the core did not place answers success = false with a reason.
Failures RunAuctionBuyScenario()
{
    Failures clientFails;
    Failures coreFails;
    std::string const path = SocketPath("auctionbuy");
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
        out.mutable_handshake_response()->set_accepted(true);
        out.mutable_handshake_response()->set_module_id(15);
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        out.Clear();
        out.mutable_module_config();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        out.Clear();
        out.mutable_event_notify()->set_hook_name("WorldScript.OnStartup");
        out.mutable_event_notify()->mutable_world_startup();
        EXPECT_IN(coreFails, WriteEnvelope(socket, out));

        auto readAction = [&](Envelope& envelope)
        {
            while (ReadEnvelope(socket, envelope))
                if (envelope.payload_case() == Envelope::kActionRequest)
                    return true;
            return false;
        };

        // 1: the filter browse.
        bool got = readAction(in);
        EXPECT_IN(coreFails, got);
        if (got)
        {
            EXPECT_IN(coreFails, in.action_request().has_auction_house());
            AuctionHouseAction const& ah = in.action_request().auction_house();
            EXPECT_IN(coreFails, ah.op() == AuctionHouseAction::kBrowse && ah.player_guid() == 42 && ah.auctioneer_guid() == 9002);
            EXPECT_IN(coreFails, ah.inventory_type() == 7 && ah.min_level() == 5 && ah.max_level() == 15 && ah.max_results() == 8 &&
                                     ah.max_buyout() == 4000 && ah.only_upgrades() && !ah.has_item_entry());
            std::uint64_t const id = in.action_request().message_id();
            out.Clear();
            out.set_message_id(id);
            ActionReply* reply = out.mutable_action_reply();
            reply->set_message_id(id);
            reply->set_success(true);
            for (std::uint32_t i = 0; i < 2; ++i)
            {
                AuctionListingEvent* listing = reply->add_listings();
                listing->set_player_guid(42);
                listing->set_request_id(id);
                listing->set_auction_guid(900 + i);
                listing->set_item_entry(2000 + i);
                listing->set_count(1);
                listing->set_buyout(1000 + 500 * i);
                listing->set_inventory_type(7);
                listing->set_quality(2);
                listing->set_required_level(10);
                listing->set_item_level(12 + i);
                listing->set_upgrade_score(30 + i);
            }
            EXPECT_IN(coreFails, WriteEnvelope(socket, out));
        }
        // 2: the buyout, refused.
        got = readAction(in);
        EXPECT_IN(coreFails, got);
        if (got)
        {
            AuctionHouseAction const& ah = in.action_request().auction_house();
            EXPECT_IN(coreFails, ah.op() == AuctionHouseAction::kBuyout && ah.auction_guid() == 900 && ah.buyout() == 1000 && ah.player_guid() == 42);
            std::uint64_t const id = in.action_request().message_id();
            out.Clear();
            out.set_message_id(id);
            ActionReply* reply = out.mutable_action_reply();
            reply->set_message_id(id);
            reply->set_success(false);
            reply->set_reason("not enough money");
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
    manifest.module_name = "auctionbuy-module";
    manifest.module_version = "0.0.1";
    manifest.tier1_hooks = {"WorldScript.OnStartup"};
    manifest.heartbeat_interval = 20ms;
    ModuleClient client(manifest);
    bool gotListings = false;
    bool gotRefusal = false;
    client.OnNotify("WorldScript.OnStartup", [&](EventNotify const&)
    {
        ModuleClient::AuctionBrowseFilter filter;
        filter.inventoryType = 7;
        filter.minLevel = 5;
        filter.maxLevel = 15;
        filter.maxResults = 8;
        filter.maxBuyout = 4000;
        filter.onlyUpgrades = true;
        client.AuctionBrowseByFilterForBot(42, 9002, filter, [&](ActionReply const& r)
        {
            gotListings = r.success() && r.listings_size() == 2 && r.listings(0).auction_guid() == 900 && r.listings(0).upgrade_score() == 30 &&
                          r.listings(1).buyout() == 1500 && r.listings(1).inventory_type() == 7 && r.listings(1).quality() == 2 &&
                          r.listings(1).required_level() == 10 && r.listings(1).item_level() == 13;
            client.AuctionHouseForBot(42, ModuleClient::AuctionHouseOp::kBuyout, 9002, 0, 900, 0, 0, 0, 1000, [&](ActionReply const& b)
            {
                gotRefusal = !b.success() && b.reason() == "not enough money";
            });
        });
    });
    EXPECT_IN(clientFails, client.Connect(path));
    EXPECT_IN(clientFails, client.Handshake(2000ms));
    client.Run();
    core.join();

    EXPECT_IN(clientFails, gotListings);
    EXPECT_IN(clientFails, gotRefusal);

    Failures all = clientFails;
    all.insert(all.end(), coreFails.begin(), coreFails.end());
    return all;
}

int main()
{
    int failedScenarios = 0;
    Report("accept, events, heartbeat, core Shutdown", RunAcceptScenario("accept", true), failedScenarios);
    Report("reject on protocol mismatch", RunRejectScenario(), failedScenarios);
    Report("module-side Shutdown() ends Run()", RunAcceptScenario("shutdown", false), failedScenarios);
    Report("loot notify answered with sell and message actions", RunLootScenario(), failedScenarios);
    Report("control: spawn request, revoke, release", RunControlScenario(), failedScenarios);
    Report("timer fires on the Run() thread while the core is silent", RunTimerScenario(), failedScenarios);
    Report("quest item loot answered with a share action", RunShareScenario(), failedScenarios);
    Report("vendor/AH purchases carry the bot identity and browse events correlate", RunEconomyActorScenario(), failedScenarios);
    Report("bot speech and bot trade: whisper, list bags, set item, trade event", RunBotTradeScenario(), failedScenarios);
    Report("bot talents: role only, role and spec, refusal", RunTalentsScenario(), failedScenarios);
    Report("auction browse by filter: listings in the reply, a refused buyout", RunAuctionBuyScenario(), failedScenarios);
    return failedScenarios == 0 ? 0 : 1;
}
