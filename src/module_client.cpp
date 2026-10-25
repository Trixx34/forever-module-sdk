#include "forever_module/module_client.h"

#include "forever_module/protocol_version.h"
#include "forever_module/tier2_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sys/prctl.h> // prctl(2) / PR_SET_PDEATHSIG: Linux only
#include <unistd.h>    // getppid(2)
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <poll.h>     // poll(2): the timed readiness wait WaitReadable performs
#include <sys/time.h> // timeval, the payload of the SO_RCVTIMEO socket option
#endif

#include <boost/asio.hpp>

namespace ba = boost::asio;
namespace local = boost::asio::local;

namespace forever_module {

namespace {

bool StartSockets()
{
#ifdef _WIN32
    // A function-local static is initialized once, thread-safely (C++11), so two threads cannot race into WSAStartup.
    static int const result = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    return result == 0;
#else
    return true;
#endif
}

// Boost removed socket_base::receive_timeout (and send_timeout) in 1.87. This is the same option it
// described -- SOL_SOCKET/SO_RCVTIMEO carrying a timeval, which Asio hands straight to setsockopt(2)
// through its socket-option concept -- spelled out here because boost::asio::detail is not part of the
// SDK's stable surface. Only stream sockets honour it, which is the only kind ModuleClient uses.
// level/name/data/size are each templated on the protocol, as that concept requires.
class ReceiveTimeoutOption
{
public:
    explicit ReceiveTimeoutOption(std::chrono::milliseconds timeout)
    {
        auto const millis = timeout.count();
        value_.tv_sec = static_cast<decltype(value_.tv_sec)>(millis / 1000);
        value_.tv_usec = static_cast<decltype(value_.tv_usec)>((millis % 1000) * 1000);
    }

    template <typename Protocol>
    int level(const Protocol&) const
    {
        return SOL_SOCKET;
    }

    template <typename Protocol>
    int name(const Protocol&) const
    {
        return SO_RCVTIMEO;
    }

    template <typename Protocol>
    const timeval* data(const Protocol&) const
    {
        return &value_;
    }

    template <typename Protocol>
    std::size_t size(const Protocol&) const
    {
        return sizeof(value_);
    }

private:
    timeval value_{};
};

// The quest operation sits in the top byte of BotAction::spell_id, so a quest id must fit in the low 24 bits.
constexpr std::uint32_t kQuestIdLimit = 0x01000000u;

// The error_code overload of set_option, so a failed option is reported to the caller rather than thrown.
bool SetReceiveTimeout(local::stream_protocol::socket& socket, std::chrono::milliseconds timeout)
{
    boost::system::error_code ec;
    socket.set_option(ReceiveTimeoutOption(timeout), ec);
    return !ec;
}

} // namespace

struct ModuleClient::Transport
{
    ba::io_context ioContext;
    local::stream_protocol::socket socket;
    std::mutex writeMutex; // serializes writes from the heartbeat thread and the Run() thread

    Transport() : socket(ioContext) {}
    ~Transport() = default;
};

struct ModuleClient::HeartbeatState
{
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
};

namespace {

// Frame header is a 4-byte little-endian payload length.
void EncodeLength(std::uint32_t length, std::array<std::uint8_t, 4>& out)
{
    out[0] = static_cast<std::uint8_t>(length & 0xFF);
    out[1] = static_cast<std::uint8_t>((length >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((length >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((length >> 24) & 0xFF);
}

std::uint32_t DecodeLength(std::array<std::uint8_t, 4> const& in)
{
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
}

} // namespace

ModuleClient::ModuleClient(ModuleManifest manifest)
    : manifest_(std::move(manifest))
{
}

ModuleClient::~ModuleClient()
{
    StopHeartbeat();
}

bool ModuleClient::Connect(std::string const& endpoint)
{
#ifdef __linux__
    // Die with the core: if the worldserver crashes or exits, the kernel sends us SIGTERM instead of
    // leaving this process orphaned (doc/module-system/04 §2). If the core is already gone, we have
    // been re-parented to init (pid 1) and the signal would never come, so refuse to run.
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() == 1)
        return false;
#endif

    // The core may close its end while we are writing (e.g. during a crash or shutdown). Turn the
    // resulting SIGPIPE into an EPIPE error we can handle. This is process-wide, which is acceptable
    // because the module runs as its own process. Windows has no SIGPIPE.
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    if (!StartSockets())
        return false;

    transport_ = std::make_unique<Transport>();
    boost::system::error_code ec;
    transport_->socket.connect(local::stream_protocol::endpoint(endpoint), ec);
    if (ec)
    {
        transport_.reset();
        return false;
    }
    return true;
}

bool ModuleClient::Handshake(std::chrono::milliseconds timeout)
{
    if (!transport_)
    {
        rejectionReason_ = "not connected";
        return false;
    }

    // Bound the blocking reads below by the handshake timeout via the socket's receive timeout.
    if (!SetReceiveTimeout(transport_->socket, timeout))
    {
        rejectionReason_ = "cannot set the handshake timeout on the socket";
        return false;
    }

    HandshakeRequest request;
    request.set_module_name(manifest_.module_name);
    request.set_module_version(manifest_.module_version);
    request.set_protocol_version(kProtocolVersion);
    request.set_sdk_package_version(kSdkPackageVersion);
    for (std::string const& hook : manifest_.tier1_hooks)
        request.add_tier1_hooks(hook);
    request.set_tier2_participates(manifest_.tier2_participates);
    request.set_requested_max_bots(manifest_.requested_max_bots);
    request.set_requested_max_nearby_units(manifest_.requested_max_nearby_units);
    request.set_requested_max_actions(manifest_.requested_max_actions);
    request.set_requested_tick_interval(manifest_.requested_tick_interval);

    Envelope out;
    *out.mutable_handshake_request() = request;
    // A failed clear leaves the handshake timeout in place, which only bounds the reads that follow.
    auto clearTimeout = [this] {
        SetReceiveTimeout(transport_->socket, std::chrono::hours(1));
    };

    if (!WriteEnvelope(out))
    {
        rejectionReason_ = "failed to send HandshakeRequest";
        clearTimeout();
        return false;
    }

    Envelope in;
    if (!ReadEnvelope(in) || in.payload_case() != Envelope::kHandshakeResponse)
    {
        rejectionReason_ = "no HandshakeResponse within timeout";
        clearTimeout();
        return false;
    }

    HandshakeResponse const& response = in.handshake_response();
    if (!response.accepted())
    {
        rejectionReason_ = response.reason();
        clearTimeout();
        return false;
    }

    moduleId_ = response.module_id();
    tier2RegionName_ = response.tier2_region_name();
    bounds_.max_bots = response.negotiated_max_bots();
    bounds_.max_nearby_units = response.negotiated_max_nearby_units();
    bounds_.max_actions = response.negotiated_max_actions();
    bounds_.tick_interval = response.negotiated_tick_interval();

    // The core grants at most what was asked (ModuleMgr: min(requested, config)). A grant above the request, or a
    // region with no bots or no actions, means the core and this SDK disagree; refuse rather than run on bad bounds.
    if (!tier2RegionName_.empty() &&
        (bounds_.max_bots == 0 || bounds_.max_actions == 0 || bounds_.max_bots > manifest_.requested_max_bots ||
         bounds_.max_nearby_units > manifest_.requested_max_nearby_units ||
         bounds_.max_actions > manifest_.requested_max_actions))
    {
        rejectionReason_ = "core granted Tier 2 bounds outside the requested limits";
        clearTimeout();
        return false;
    }

    // Core sends the config immediately after accepting (doc 03 §Config delivery).
    if (!ReadEnvelope(in) || in.payload_case() != Envelope::kModuleConfig)
    {
        rejectionReason_ = "expected ModuleConfig after HandshakeResponse";
        clearTimeout();
        return false;
    }

    config_.clear();
    for (ConfigEntry const& entry : in.module_config().values())
        config_[entry.key()] = entry.value();
    haveConfig_ = true;

    // Clear the handshake receive timeout so Run() can block indefinitely on the socket. A zero
    // timeout can mean "poll immediately" on some platforms, so use a long duration instead.
    if (!SetReceiveTimeout(transport_->socket, std::chrono::hours(1)))
    {
        rejectionReason_ = "cannot clear the handshake timeout on the socket";
        return false;
    }
    rejectionReason_.clear();
    return true;
}

std::uint64_t ModuleClient::module_id() const
{
    return moduleId_;
}

std::string const& ModuleClient::tier2_region_name() const
{
    return tier2RegionName_;
}

NegotiatedBounds const& ModuleClient::negotiated_bounds() const
{
    return bounds_;
}

std::string const& ModuleClient::rejection_reason() const
{
    return rejectionReason_;
}

void ModuleClient::OnConfig(std::function<void(std::unordered_map<std::string, std::string> const&)> callback)
{
    configCallback_ = std::move(callback);
    if (haveConfig_ && configCallback_)
        configCallback_(config_);
}

void ModuleClient::OnNotify(std::string const& hookName, std::function<void(EventNotify const&)> callback)
{
    notifyCallbacks_[hookName] = std::move(callback);
}

void ModuleClient::OnRequest(std::string const& hookName, std::function<EventReply(EventRequest const&)> callback)
{
    requestCallbacks_[hookName] = std::move(callback);
}

std::uint64_t ModuleClient::RequestAction(ActionRequest request, ActionCallback onReply)
{
    std::uint64_t const id = nextActionId_++;
    request.set_message_id(id);
    if (onReply)
        actionCallbacks_[id] = std::move(onReply);

    Envelope out;
    out.set_message_id(id);
    *out.mutable_action_request() = request;
    if (!WriteEnvelope(out))
    {
        actionCallbacks_.erase(id); // connection is gone; Run() reports the loss
        std::cerr << "forever_module: action " << id << " was not sent; its reply will not arrive\n";
    }
    return id;
}

std::uint64_t ModuleClient::SellItem(std::uint64_t playerGuid, std::uint64_t itemGuid, std::uint32_t itemEntry,
                                     std::uint32_t count, ActionCallback onReply)
{
    ActionRequest request;
    SellItemAction* sell = request.mutable_sell_item();
    sell->set_player_guid(playerGuid);
    sell->set_item_guid(itemGuid);
    sell->set_item_entry(itemEntry);
    sell->set_count(count);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::ShareQuestItem(std::uint64_t playerGuid, std::uint32_t itemEntry, std::uint32_t count,
                                           std::uint32_t rangeYards, ActionCallback onReply)
{
    ActionRequest request;
    ShareQuestItemAction* share = request.mutable_share_quest_item();
    share->set_player_guid(playerGuid);
    share->set_item_entry(itemEntry);
    share->set_count(count);
    share->set_range_yards(rangeYards);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::Log(std::uint32_t level, std::string const& text, ActionCallback onReply)
{
    ActionRequest request;
    LogAction* log = request.mutable_log();
    log->set_level(level);
    log->set_text(text);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::BuyItem(std::uint64_t vendorGuid, std::uint32_t itemEntry,
                                    std::uint32_t count, ActionCallback onReply)
{
    ActionRequest request;
    BuyItemAction* buy = request.mutable_buy_item();
    buy->set_vendor_guid(vendorGuid);
    buy->set_item_entry(itemEntry);
    buy->set_count(count);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::BuyItemForBot(std::uint64_t botGuid, std::uint64_t vendorGuid,
                                          std::uint32_t itemEntry, std::uint32_t count,
                                          ActionCallback onReply)
{
    ActionRequest request;
    BuyItemAction* buy = request.mutable_buy_item();
    buy->set_vendor_guid(vendorGuid);
    buy->set_item_entry(itemEntry);
    buy->set_count(count);
    buy->set_player_guid(botGuid);
    return RequestAction(std::move(request), std::move(onReply));
}

void ModuleClient::Vendor(std::uint64_t botGuid, std::uint64_t vendorGuid, std::uint32_t spellId,
                          ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kVendor;
    action.spell_id = spellId; // bitmask: 1 sell, 2 repair, 4 equip
    action.target_guid = vendorGuid;
    PublishBotAction(action);
}

std::uint64_t ModuleClient::AuctionHouse(AuctionHouseOp op, std::uint64_t auctioneerGuid,
                                         std::uint64_t itemGuid, std::uint64_t auctionGuid,
                                         std::uint32_t itemEntry, std::uint32_t count,
                                         std::uint32_t bid, std::uint32_t buyout,
                                         ActionCallback onReply)
{
    ActionRequest request;
    AuctionHouseAction* ah = request.mutable_auction_house();
    ah->set_op(static_cast<AuctionHouseAction_AHOp>(static_cast<std::uint32_t>(op)));
    ah->set_auctioneer_guid(auctioneerGuid);
    ah->set_item_guid(itemGuid);
    ah->set_auction_guid(auctionGuid);
    ah->set_item_entry(itemEntry);
    ah->set_count(count);
    ah->set_bid(bid);
    ah->set_buyout(buyout);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::AuctionHouseForBot(std::uint64_t botGuid, AuctionHouseOp op,
                                              std::uint64_t auctioneerGuid, std::uint64_t itemGuid,
                                              std::uint64_t auctionGuid, std::uint32_t itemEntry,
                                              std::uint32_t count, std::uint32_t bid,
                                              std::uint32_t buyout, ActionCallback onReply)
{
    ActionRequest request;
    AuctionHouseAction* ah = request.mutable_auction_house();
    ah->set_op(static_cast<AuctionHouseAction_AHOp>(static_cast<std::uint32_t>(op)));
    ah->set_auctioneer_guid(auctioneerGuid);
    ah->set_item_guid(itemGuid);
    ah->set_auction_guid(auctionGuid);
    ah->set_item_entry(itemEntry);
    ah->set_count(count);
    ah->set_bid(bid);
    ah->set_buyout(buyout);
    ah->set_player_guid(botGuid);
    return RequestAction(std::move(request), std::move(onReply));
}

// Queues a single bot action for this pass. Attaches Tier 2 on first use, so the convenience methods work without a
// separate AttachTier2() call. FlushBotActions() sends the queue once the callbacks for the pass have run.
bool ModuleClient::PublishBotAction(BotAction const& action)
{
    if (!tier2_ && !AttachTier2())
        return false;
    if (pendingActions_.size() >= bounds_.max_actions)
    {
        tier2Error_ = "more bot actions queued than the core granted (" + std::to_string(bounds_.max_actions) + ")";
        return false;
    }
    pendingActions_.push_back(action);
    return true;
}

// Sends the queued actions in one publish. Waits while no snapshot has been read, since the actions answer one.
// A publish that fails is dropped and logged, because the bounds were checked when each action was queued.
void ModuleClient::FlushBotActions()
{
    if (pendingActions_.empty() || !tier2_ || !tier2_->HasSnapshot())
        return;
    std::string error;
    if (!tier2_->PublishActions(pendingActions_, error))
        std::cerr << "forever_module: dropped " << pendingActions_.size() << " bot actions: " << error << "\n";
    pendingActions_.clear();
}

std::uint64_t ModuleClient::SendPlayerMessage(std::uint64_t playerGuid, std::string const& text, ActionCallback onReply)
{
    ActionRequest request;
    SendPlayerMessageAction* message = request.mutable_send_message();
    message->set_player_guid(playerGuid);
    message->set_text(text);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::BotSpeak(std::uint64_t botGuid, SpeakKind kind, std::uint64_t playerGuid, std::string const& text,
                                     ActionCallback onReply)
{
    ActionRequest request;
    BotSpeakAction* speak = request.mutable_bot_speak();
    speak->set_bot_guid(botGuid);
    speak->set_kind(static_cast<std::uint32_t>(kind));
    speak->set_player_guid(playerGuid);
    speak->set_text(text);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::SpendBotTalents(std::uint64_t botGuid, TalentRole role, ActionCallback onReply)
{
    ActionRequest request;
    SpendBotTalentsAction* talents = request.mutable_spend_bot_talents();
    talents->set_bot_guid(botGuid);
    talents->set_role(static_cast<std::uint32_t>(role));
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::BotTrade(std::uint64_t botGuid, BotTradeOp op, std::uint32_t slot, std::uint64_t itemGuid,
                                     std::uint64_t copper, ActionCallback onReply)
{
    ActionRequest request;
    BotTradeAction* trade = request.mutable_bot_trade();
    trade->set_op(static_cast<BotTradeAction_TradeOp>(static_cast<std::uint32_t>(op)));
    trade->set_bot_guid(botGuid);
    trade->set_slot(slot);
    trade->set_item_guid(itemGuid);
    trade->set_copper(copper);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::StoreGet(std::string const& key, ActionCallback onReply)
{
    ActionRequest request;
    request.mutable_store_get()->set_key(key);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::StoreSet(std::string const& key, std::string const& value, ActionCallback onReply)
{
    ActionRequest request;
    StoreSetAction* set = request.mutable_store_set();
    set->set_key(key);
    set->set_value(value);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::StoreDelete(std::string const& key, ActionCallback onReply)
{
    ActionRequest request;
    request.mutable_store_delete()->set_key(key);
    return RequestAction(std::move(request), std::move(onReply));
}

std::uint64_t ModuleClient::StoreList(std::string const& prefix, ActionCallback onReply)
{
    ActionRequest request;
    request.mutable_store_list()->set_prefix(prefix);
    return RequestAction(std::move(request), std::move(onReply));
}

// --- Tier 2 bot-action convenience methods ---

void ModuleClient::CastSpell(std::uint64_t botGuid, std::uint32_t spellId, std::uint64_t targetGuid,
                             ActionCallback onReply)
{
    (void)onReply; // Tier 2 actions have no Tier 1 reply; result appears in the next snapshot
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kCastSpell;
    action.spell_id = spellId;
    action.target_guid = targetGuid;
    PublishBotAction(action);
}

void ModuleClient::MoveTo(std::uint64_t botGuid, float x, float y, float z, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kMoveTo;
    action.dest_x = x;
    action.dest_y = y;
    action.dest_z = z;
    PublishBotAction(action);
}

void ModuleClient::AttackTarget(std::uint64_t botGuid, std::uint64_t targetGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kAttackTarget;
    action.target_guid = targetGuid;
    PublishBotAction(action);
}

void ModuleClient::Follow(std::uint64_t botGuid, std::uint64_t targetGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kFollow;
    action.target_guid = targetGuid;
    PublishBotAction(action);
}

void ModuleClient::Stop(std::uint64_t botGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kStop;
    PublishBotAction(action);
}

void ModuleClient::Rest(std::uint64_t botGuid, bool sitDown, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kRest;
    action.spell_id = sitDown ? 1u : 0u;
    PublishBotAction(action);
}

void ModuleClient::Train(std::uint64_t botGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kTrain;
    PublishBotAction(action);
}

void ModuleClient::Loot(std::uint64_t botGuid, std::uint64_t corpseGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kLoot;
    action.target_guid = corpseGuid;
    PublishBotAction(action);
}

void ModuleClient::GroupInvite(std::uint64_t botGuid, std::uint64_t targetGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kGroup;
    action.spell_id = 1u; // invite
    action.target_guid = targetGuid;
    PublishBotAction(action);
}

void ModuleClient::GroupAccept(std::uint64_t botGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kGroup;
    action.spell_id = 2u; // accept
    PublishBotAction(action);
}

void ModuleClient::GroupLeave(std::uint64_t botGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kGroup;
    action.spell_id = 3u; // leave
    PublishBotAction(action);
}

void ModuleClient::QuestAccept(std::uint64_t botGuid, std::uint32_t questId, ActionCallback onReply)
{
    (void)onReply;
    if (questId >= kQuestIdLimit)
    {
        tier2Error_ = "quest id " + std::to_string(questId) + " does not fit in 24 bits";
        return;
    }
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kQuest;
    action.spell_id = (1u << 24) | questId; // operation 1 = accept
    PublishBotAction(action);
}

void ModuleClient::QuestTurnIn(std::uint64_t botGuid, std::uint32_t questId, ActionCallback onReply)
{
    (void)onReply;
    if (questId >= kQuestIdLimit)
    {
        tier2Error_ = "quest id " + std::to_string(questId) + " does not fit in 24 bits";
        return;
    }
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kQuest;
    action.spell_id = (2u << 24) | questId; // operation 2 = hand in
    PublishBotAction(action);
}

void ModuleClient::QuestAbandon(std::uint64_t botGuid, std::uint32_t questId, ActionCallback onReply)
{
    (void)onReply;
    if (questId >= kQuestIdLimit)
    {
        tier2Error_ = "quest id " + std::to_string(questId) + " does not fit in 24 bits";
        return;
    }
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kQuest;
    action.spell_id = (3u << 24) | questId; // operation 3 = abandon
    PublishBotAction(action);
}

void ModuleClient::Teleport(std::uint64_t botGuid, std::uint32_t mapId, float x, float y, float z,
                            ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kTeleport;
    action.spell_id = mapId;
    action.dest_x = x;
    action.dest_y = y;
    action.dest_z = z;
    PublishBotAction(action);
}

void ModuleClient::Grant(std::uint64_t botGuid, std::uint32_t itemEntry, std::uint32_t count,
                         ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kGrant;
    action.spell_id = itemEntry;
    action.dest_x = static_cast<float>(count);
    PublishBotAction(action);
}

void ModuleClient::AutoGear(std::uint64_t botGuid, bool tank, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kAutoGear;
    action.spell_id = tank ? 1u : 0u;
    PublishBotAction(action);
}

void ModuleClient::ReleaseSpirit(std::uint64_t botGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kReleaseSpirit;
    PublishBotAction(action);
}

void ModuleClient::Resurrect(std::uint64_t botGuid, std::uint64_t resurrecterGuid, bool accept,
                             ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kResurrect;
    action.target_guid = resurrecterGuid;
    action.spell_id = accept ? 0u : 1u;
    PublishBotAction(action);
}

void ModuleClient::ReclaimCorpse(std::uint64_t botGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kReclaimCorpse;
    PublishBotAction(action);
}

void ModuleClient::Summon(std::uint64_t botGuid, std::uint64_t targetGuid, float spreadIndex,
                          ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kSummon;
    action.target_guid = targetGuid;
    action.dest_x = spreadIndex;
    PublishBotAction(action);
}

void ModuleClient::Revive(std::uint64_t botGuid, std::uint64_t resurrecterGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kRevive;
    action.target_guid = resurrecterGuid;
    PublishBotAction(action);
}

void ModuleClient::Mount(std::uint64_t botGuid, bool mountUp, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kMount;
    action.spell_id = mountUp ? 1u : 0u;
    PublishBotAction(action);
}

void ModuleClient::Craft(std::uint64_t botGuid, std::uint32_t spellId, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kCraft;
    action.spell_id = spellId; // profession spell / recipe id
    PublishBotAction(action);
}

void ModuleClient::Gather(std::uint64_t botGuid, std::uint64_t targetGuid, ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kGather;
    action.target_guid = targetGuid; // node/object unit reference
    PublishBotAction(action);
}

void ModuleClient::PetCommand(std::uint64_t botGuid, std::uint32_t cmd, std::uint64_t targetGuid,
                              ActionCallback onReply)
{
    (void)onReply;
    BotAction action;
    action.bot_guid = botGuid;
    action.type = ActionType::kPetCommand;
    action.spell_id = cmd; // 1 attack target_guid, 2 follow, 3 stay; 4 = stance in dest_x
    if (targetGuid != 0)
        action.target_guid = targetGuid;
    PublishBotAction(action);
}

void ModuleClient::OnControlRevoked(std::function<void(std::uint64_t, std::string const&)> callback)
{
    revokedCallback_ = std::move(callback);
}

std::uint64_t ModuleClient::RequestControl(std::uint64_t botGuid, SpawnParams const& params, ControlCallback onReply)
{
    std::uint64_t const id = nextActionId_++;
    Envelope out;
    out.set_message_id(id);
    ControlRequest* request = out.mutable_control_request();
    request->set_bot_guid(botGuid);
    if (botGuid == 0)
        *request->mutable_spawn_params() = params;

    if (onReply)
        controlCallbacks_[id] = std::move(onReply);
    if (!WriteEnvelope(out))
    {
        controlCallbacks_.erase(id); // connection is gone; Run() reports the loss
        std::cerr << "forever_module: control request " << id << " was not sent; its reply will not arrive\n";
    }
    return id;
}

void ModuleClient::ReleaseControl(std::uint64_t botGuid)
{
    Envelope out;
    out.mutable_release_control()->set_bot_guid(botGuid);
    WriteEnvelope(out);
}

Tier2Session* ModuleClient::AttachTier2()
{
    if (tier2RegionName_.empty())
    {
        // Not granted, or the module did not ask for Tier 2. Recorded so a caller can see why it got nothing.
        tier2Error_ = "no Tier 2 region was granted at handshake";
        return nullptr;
    }
    if (!tier2_)
    {
        tier2Error_.clear();
        tier2_ = Tier2Session::Attach(tier2RegionName_, tier2Error_);
    }
    return tier2_.get();
}

std::string const& ModuleClient::tier2_error() const
{
    return tier2Error_;
}

namespace {

// Waits until the socket has data or the timeout passes. Returns false only on timeout; a failed wait reports true, so
// the read after it sees the failure.
bool WaitReadable(local::stream_protocol::socket& socket, std::chrono::milliseconds timeout)
{
    // Boost removed wait(wait_type, duration, error_code); poll(2) on the native descriptor is what it
    // did internally. UntilNextTimer caps the wait at an hour, so the timeout always fits in an int.
    pollfd descriptor{};
    descriptor.fd = static_cast<int>(socket.native_handle());
    descriptor.events = POLLIN;

    // poll returns 0 only when nothing became readable before the timeout. Any other outcome -- readable,
    // hung up, or a failed poll -- means proceed to read, which reports the real failure.
    return ::poll(&descriptor, 1, static_cast<int>(timeout.count())) != 0;
}

} // namespace

void ModuleClient::AddTimer(std::chrono::milliseconds period, TimerCallback callback)
{
    // A zero period would make Run() spin without waiting, so it is raised to the smallest period that still waits.
    // Next starts one period from now; Run() sets it again from its own start for timers added before Run().
    period = std::max(period, std::chrono::milliseconds(1));
    timers_.push_back(Timer{ period, std::chrono::steady_clock::now() + period, std::move(callback) });
}

// The time until the earliest timer, or a long wait when there are none.
std::chrono::milliseconds ModuleClient::UntilNextTimer(std::chrono::steady_clock::time_point now) const
{
    std::chrono::milliseconds wait = std::chrono::hours(1);
    for (Timer const& timer : timers_)
    {
        auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(timer.Next - now);
        if (left < wait)
            wait = left;
    }
    return wait < std::chrono::milliseconds(0) ? std::chrono::milliseconds(0) : wait;
}

void ModuleClient::FireDueTimers(std::chrono::steady_clock::time_point now)
{
    // Indexed, not range-for: a callback may call AddTimer(), which can reallocate timers_ and invalidate a reference.
    for (std::size_t i = 0; i < timers_.size(); ++i)
    {
        if (now < timers_[i].Next)
            continue;
        timers_[i].Next = now + timers_[i].Period;
        // Copied, so a callback that replaces its own timer's callback does not destroy the function while it runs.
        TimerCallback callback = timers_[i].Callback;
        callback();
    }
}

void ModuleClient::Run()
{
    if (!transport_)
        return;

    StartHeartbeat();

    // Stops the heartbeat on every way out of Run(), including an exception from a callback.
    struct HeartbeatGuard
    {
        ModuleClient* client;
        ~HeartbeatGuard() { client->StopHeartbeat(); }
    } heartbeatGuard{ this };

    auto const start = std::chrono::steady_clock::now();
    for (Timer& timer : timers_)
        timer.Next = start + timer.Period;

    Envelope envelope;
    while (!stopping_)
    {
        // With no timers the read simply blocks. With timers, wait for data up to the next timer, then fire the timers.
        auto const now = std::chrono::steady_clock::now();
        if (!timers_.empty() && !WaitReadable(transport_->socket, UntilNextTimer(now)))
        {
            FireDueTimers(std::chrono::steady_clock::now());
            FlushBotActions();
            continue;
        }

        if (!ReadEnvelope(envelope))
        {
            if (!stopping_)
                std::cerr << "forever_module: lost the connection to core\n";
            break;
        }
        bool const keepRunning = Dispatch(envelope);
        FireDueTimers(std::chrono::steady_clock::now());
        FlushBotActions();
        if (!keepRunning)
            break;
    }

    FlushBotActions();
}

void ModuleClient::Shutdown()
{
    stopping_ = true;

    // Unblocks a read in Run() from another thread: shutdown(2) makes the pending recv return EOF
    // and makes wait_read return readable. The socket itself is closed when Transport is destroyed.
    if (transport_)
    {
        boost::system::error_code ec;
        transport_->socket.shutdown(local::stream_protocol::socket::shutdown_both, ec);
    }
}

bool ModuleClient::ReadEnvelope(Envelope& envelope)
{
    std::array<std::uint8_t, 4> header{};
    boost::system::error_code ec;
    ba::read(transport_->socket, ba::buffer(header), ec);
    if (ec)
        return false;

    std::uint32_t const length = DecodeLength(header);
    if (length == 0 || length > kMaxFrameBytes)
    {
        std::cerr << "forever_module: core sent a frame of " << length << " bytes; the connection is dropped\n";
        return false;
    }

    std::string body(length, '\0');
    ba::read(transport_->socket, ba::buffer(body), ec);
    if (ec)
        return false;

    if (!envelope.ParseFromString(body))
    {
        std::cerr << "forever_module: core sent a frame that does not parse; the connection is dropped\n";
        return false;
    }
    return true;
}

bool ModuleClient::WriteEnvelope(Envelope const& envelope)
{
    std::string body;
    if (!envelope.SerializeToString(&body))
    {
        std::cerr << "forever_module: an envelope does not serialize; it was not sent\n";
        return false;
    }
    if (body.size() > kMaxFrameBytes)
    {
        std::cerr << "forever_module: an envelope of " << body.size() << " bytes is over the frame limit; it was not sent\n";
        return false;
    }

    std::array<std::uint8_t, 4> header{};
    EncodeLength(static_cast<std::uint32_t>(body.size()), header);

    // Two buffers written in one call, so the header and body cannot interleave with another writer's frame.
    std::array<ba::const_buffer, 2> buffers{
        ba::buffer(header),
        ba::buffer(body),
    };

    std::lock_guard<std::mutex> lock(transport_->writeMutex);
    boost::system::error_code ec;
    ba::write(transport_->socket, buffers, ec);
    return !ec;
}

void ModuleClient::StartHeartbeat()
{
    if (heartbeat_ || manifest_.heartbeat_interval.count() <= 0)
        return;

    heartbeat_ = std::make_unique<HeartbeatState>();
    HeartbeatState* state = heartbeat_.get();

    state->thread = std::thread([this, state]
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        while (!state->stop)
        {
            if (state->cv.wait_for(lock, manifest_.heartbeat_interval, [state] { return state->stop; }))
                break;

            Envelope envelope;
            envelope.mutable_heartbeat()->set_sequence(++heartbeatSequence_);

            lock.unlock();
            bool const sent = WriteEnvelope(envelope);
            lock.lock();

            if (!sent)
                break;
        }
    });
}

void ModuleClient::StopHeartbeat()
{
    if (!heartbeat_)
        return;

    {
        std::lock_guard<std::mutex> lock(heartbeat_->mutex);
        heartbeat_->stop = true;
    }
    heartbeat_->cv.notify_all();

    if (heartbeat_->thread.joinable())
        heartbeat_->thread.join();

    heartbeat_.reset();
}

bool ModuleClient::Dispatch(Envelope const& envelope)
{
    switch (envelope.payload_case())
    {
        // Every callback is copied before it runs, so a callback that registers a replacement (OnNotify, OnConfig, ...)
        // does not destroy the function object it is executing in.
        case Envelope::kModuleConfig:
        {
            config_.clear();
            for (ConfigEntry const& entry : envelope.module_config().values())
                config_[entry.key()] = entry.value();
            haveConfig_ = true;
            if (auto const callback = configCallback_)
                callback(config_);
            return true;
        }

        case Envelope::kEventNotify:
        {
            EventNotify const& notify = envelope.event_notify();
            auto it = notifyCallbacks_.find(notify.hook_name());
            if (it != notifyCallbacks_.end() && it->second)
            {
                auto const callback = it->second;
                callback(notify);
            }
            return true;
        }

        case Envelope::kEventRequest:
        {
            EventRequest const& request = envelope.event_request();
            EventReply reply;
            auto it = requestCallbacks_.find(request.hook_name());
            if (it != requestCallbacks_.end() && it->second)
            {
                auto const callback = it->second;
                reply = callback(request);
            }

            // Always answer, even with an empty reply, so core doesn't wait out its timeout.
            reply.set_message_id(request.message_id());
            Envelope out;
            out.set_message_id(request.message_id());
            *out.mutable_event_reply() = reply;
            WriteEnvelope(out);
            return true;
        }

        case Envelope::kActionReply:
        {
            ActionReply const& reply = envelope.action_reply();
            auto it = actionCallbacks_.find(reply.message_id());
            if (it != actionCallbacks_.end())
            {
                ActionCallback callback = std::move(it->second);
                actionCallbacks_.erase(it);
                if (callback)
                    callback(reply);
            }
            return true;
        }

        case Envelope::kControlResponse:
        {
            auto it = controlCallbacks_.find(envelope.message_id());
            if (it != controlCallbacks_.end())
            {
                ControlCallback callback = std::move(it->second);
                controlCallbacks_.erase(it);
                if (callback)
                    callback(envelope.control_response());
            }
            return true;
        }

        case Envelope::kControlRevoked:
        {
            if (auto const callback = revokedCallback_)
                callback(envelope.control_revoked().bot_guid(), envelope.control_revoked().reason());
            return true;
        }

        case Envelope::kShutdown:
            return false;

        default:
            // Heartbeats from core and unknown payloads are ignored.
            return true;
    }
}

} // namespace forever_module
