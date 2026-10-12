#include "forever_module/module_client.h"

#include "forever_module/protocol_version.h"
#include "forever_module/tier2_session.h"

#include <array>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace forever_module {

namespace {

// Blocking Unix-domain socket I/O. POSIX uses AF_UNIX directly; Windows uses AF_UNIX through Winsock,
// which needs Windows 10 1803 or later.
#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

// AF_UNIX and sockaddr_un are not in every Windows SDK header set.
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
struct SockAddrUn
{
    unsigned short sun_family;
    char sun_path[108];
};
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
using SockAddrUn = sockaddr_un;
#endif

bool StartSockets()
{
#ifdef _WIN32
    static bool started = false;
    static int result = 0;
    if (!started)
    {
        WSADATA data{};
        result = WSAStartup(MAKEWORD(2, 2), &data);
        started = true;
    }
    return result == 0;
#else
    return true;
#endif
}

void CloseSocket(SocketHandle fd)
{
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

SocketHandle OpenUnixSocket(std::string const& path)
{
    SocketHandle fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == kInvalidSocket)
        return kInvalidSocket;

    SockAddrUn addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        CloseSocket(fd);
        return kInvalidSocket;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        CloseSocket(fd);
        return kInvalidSocket;
    }
    return fd;
}

// Reads exactly `length` bytes. False on error, timeout (SO_RCVTIMEO) or peer close.
bool ReadAll(SocketHandle fd, void* buffer, std::size_t length)
{
    auto* out = static_cast<char*>(buffer);
    while (length > 0)
    {
#ifdef _WIN32
        int const n = ::recv(fd, out, static_cast<int>(length), 0);
#else
        ssize_t const n = ::recv(fd, out, length, 0);
        if (n < 0 && errno == EINTR)
            continue;
#endif
        if (n <= 0)
            return false;
        out += n;
        length -= static_cast<std::size_t>(n);
    }
    return true;
}

bool WriteAll(SocketHandle fd, void const* buffer, std::size_t length)
{
    auto const* in = static_cast<char const*>(buffer);
    while (length > 0)
    {
#ifdef _WIN32
        int const n = ::send(fd, in, static_cast<int>(length), 0);
#else
        ssize_t const n = ::send(fd, in, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
#endif
        if (n <= 0)
            return false;
        in += n;
        length -= static_cast<std::size_t>(n);
    }
    return true;
}

void SetReceiveTimeout(SocketHandle fd, std::chrono::milliseconds timeout)
{
#ifdef _WIN32
    DWORD const ms = static_cast<DWORD>(timeout.count());
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// Unblocks a read in another thread: both directions shut down, so a pending recv returns 0.
void ShutdownSocket(SocketHandle fd)
{
#ifdef _WIN32
    ::shutdown(fd, SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

} // namespace

struct ModuleClient::Transport
{
    SocketHandle socket = kInvalidSocket;
    std::mutex writeMutex; // serializes writes from the heartbeat thread and the Run() thread

    ~Transport()
    {
        if (socket != kInvalidSocket)
            CloseSocket(socket);
    }
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
    // because the module runs as its own process. Windows has no SIGPIPE; MSG_NOSIGNAL is POSIX only.
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    if (!StartSockets())
        return false;

    transport_ = std::make_unique<Transport>();
    transport_->socket = OpenUnixSocket(endpoint);
    if (transport_->socket == kInvalidSocket)
    {
        transport_.reset();
        return false;
    }
    return true;
}

bool ModuleClient::Handshake(std::chrono::milliseconds timeout)
{
    if (!transport_ || transport_->socket == kInvalidSocket)
    {
        rejectionReason_ = "not connected";
        return false;
    }

    // Bound the blocking reads below by the handshake timeout. Cleared again before returning.
    SetReceiveTimeout(transport_->socket, timeout);

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
    if (!WriteEnvelope(out))
    {
        rejectionReason_ = "failed to send HandshakeRequest";
        SetReceiveTimeout(transport_->socket, std::chrono::milliseconds(0));
        return false;
    }

    Envelope in;
    if (!ReadEnvelope(in) || in.payload_case() != Envelope::kHandshakeResponse)
    {
        rejectionReason_ = "no HandshakeResponse within timeout";
        SetReceiveTimeout(transport_->socket, std::chrono::milliseconds(0));
        return false;
    }

    HandshakeResponse const& response = in.handshake_response();
    if (!response.accepted())
    {
        rejectionReason_ = response.reason();
        SetReceiveTimeout(transport_->socket, std::chrono::milliseconds(0));
        return false;
    }

    moduleId_ = response.module_id();
    tier2RegionName_ = response.tier2_region_name();
    bounds_.max_bots = response.negotiated_max_bots();
    bounds_.max_nearby_units = response.negotiated_max_nearby_units();
    bounds_.max_actions = response.negotiated_max_actions();
    bounds_.tick_interval = response.negotiated_tick_interval();

    // Core sends the config immediately after accepting (doc 03 §Config delivery).
    if (!ReadEnvelope(in) || in.payload_case() != Envelope::kModuleConfig)
    {
        rejectionReason_ = "expected ModuleConfig after HandshakeResponse";
        SetReceiveTimeout(transport_->socket, std::chrono::milliseconds(0));
        return false;
    }

    config_.clear();
    for (ConfigEntry const& entry : in.module_config().values())
        config_[entry.key()] = entry.value();
    haveConfig_ = true;

    SetReceiveTimeout(transport_->socket, std::chrono::milliseconds(0));
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
        actionCallbacks_.erase(id); // connection is gone; Run() will report it
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

std::uint64_t ModuleClient::SendPlayerMessage(std::uint64_t playerGuid, std::string const& text, ActionCallback onReply)
{
    ActionRequest request;
    SendPlayerMessageAction* message = request.mutable_send_message();
    message->set_player_guid(playerGuid);
    message->set_text(text);
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
        controlCallbacks_.erase(id); // connection is gone; Run() will report it
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
        return nullptr; // not granted, or the module did not ask for Tier 2
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

void ModuleClient::Run()
{
    if (!transport_ || transport_->socket == kInvalidSocket)
        return;

    StartHeartbeat();

    Envelope envelope;
    while (!stopping_ && ReadEnvelope(envelope))
    {
        if (!Dispatch(envelope))
            break;
    }

    StopHeartbeat();
}

void ModuleClient::Shutdown()
{
    stopping_ = true;

    // Unblocks a read in Run() from another thread: shutdown(2) makes the pending read return EOF.
    if (transport_ && transport_->socket != kInvalidSocket)
        ShutdownSocket(transport_->socket);
}

bool ModuleClient::ReadEnvelope(Envelope& envelope)
{
    std::array<std::uint8_t, 4> header{};
    if (!ReadAll(transport_->socket, header.data(), header.size()))
        return false;

    std::uint32_t const length = DecodeLength(header);
    if (length == 0 || length > kMaxFrameBytes)
        return false;

    std::string body(length, '\0');
    if (!ReadAll(transport_->socket, body.data(), body.size()))
        return false;

    return envelope.ParseFromString(body);
}

bool ModuleClient::WriteEnvelope(Envelope const& envelope)
{
    std::string body;
    if (!envelope.SerializeToString(&body) || body.size() > kMaxFrameBytes)
        return false;

    std::array<std::uint8_t, 4> header{};
    EncodeLength(static_cast<std::uint32_t>(body.size()), header);

    // One buffer, so the header and body cannot interleave with another writer's frame.
    std::string frame(reinterpret_cast<char const*>(header.data()), header.size());
    frame += body;

    std::lock_guard<std::mutex> lock(transport_->writeMutex);
    return WriteAll(transport_->socket, frame.data(), frame.size());
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
        case Envelope::kModuleConfig:
        {
            config_.clear();
            for (ConfigEntry const& entry : envelope.module_config().values())
                config_[entry.key()] = entry.value();
            haveConfig_ = true;
            if (configCallback_)
                configCallback_(config_);
            return true;
        }

        case Envelope::kEventNotify:
        {
            EventNotify const& notify = envelope.event_notify();
            auto it = notifyCallbacks_.find(notify.hook_name());
            if (it != notifyCallbacks_.end() && it->second)
                it->second(notify);
            return true;
        }

        case Envelope::kEventRequest:
        {
            EventRequest const& request = envelope.event_request();
            EventReply reply;
            auto it = requestCallbacks_.find(request.hook_name());
            if (it != requestCallbacks_.end() && it->second)
                reply = it->second(request);

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
            if (revokedCallback_)
                revokedCallback_(envelope.control_revoked().bot_guid(), envelope.control_revoked().reason());
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
