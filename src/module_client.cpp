#include "forever_module/module_client.h"

#include "forever_module/protocol_version.h"

#ifdef _WIN32
#error "forever-module-sdk: the Windows named-pipe transport is not implemented yet (Tier 1 client is POSIX-only for now)"
#endif

#include <array>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <thread>

#include <boost/asio.hpp>

#include <sys/socket.h>
#include <sys/time.h>

namespace forever_module {

struct ModuleClient::Transport
{
    boost::asio::io_context io;
    boost::asio::local::stream_protocol::socket socket{io};
    std::mutex writeMutex; // serializes writes from the heartbeat thread and the Run() thread
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
    // The core may close its end while we are writing (e.g. during a crash or shutdown). Turn the
    // resulting SIGPIPE into an EPIPE error we can handle. This is process-wide, which is acceptable
    // because the module runs as its own process.
    std::signal(SIGPIPE, SIG_IGN);

    transport_ = std::make_unique<Transport>();

    boost::system::error_code ec;
    transport_->socket.connect(boost::asio::local::stream_protocol::endpoint(endpoint), ec);
    if (ec)
    {
        transport_.reset();
        return false;
    }
    return true;
}

bool ModuleClient::Handshake(std::chrono::milliseconds timeout)
{
    if (!transport_ || !transport_->socket.is_open())
    {
        rejectionReason_ = "not connected";
        return false;
    }

    // Bound the blocking reads below by the handshake timeout. Cleared again before returning.
    auto const setRecvTimeout = [this](std::chrono::milliseconds t)
    {
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(t.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((t.count() % 1000) * 1000);
        ::setsockopt(transport_->socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    };
    setRecvTimeout(timeout);

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
        setRecvTimeout(std::chrono::milliseconds(0));
        return false;
    }

    Envelope in;
    if (!ReadEnvelope(in) || in.payload_case() != Envelope::kHandshakeResponse)
    {
        rejectionReason_ = "no HandshakeResponse within timeout";
        setRecvTimeout(std::chrono::milliseconds(0));
        return false;
    }

    HandshakeResponse const& response = in.handshake_response();
    if (!response.accepted())
    {
        rejectionReason_ = response.reason();
        setRecvTimeout(std::chrono::milliseconds(0));
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
        setRecvTimeout(std::chrono::milliseconds(0));
        return false;
    }

    config_.clear();
    for (auto const& [key, value] : in.module_config().values())
        config_[key] = value;
    haveConfig_ = true;

    setRecvTimeout(std::chrono::milliseconds(0));
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

void ModuleClient::OnControlRevoked(std::function<void(std::uint64_t, std::string const&)> callback)
{
    revokedCallback_ = std::move(callback);
}

ControlResult ModuleClient::RequestControl(std::uint64_t /*botGuid*/, SpawnParams const& /*params*/)
{
    // Bot control is a later slice (doc 04 §6). Reported explicitly rather than silently ignored.
    ControlResult result;
    result.reason = "bot control is not implemented in this SDK version";
    return result;
}

void ModuleClient::ReleaseControl(std::uint64_t /*botGuid*/)
{
    // No-op until bot control is implemented (see RequestControl).
}

Tier2Session* ModuleClient::AttachTier2()
{
    // Tier 2 is a later slice.
    return nullptr;
}

void ModuleClient::Run()
{
    if (!transport_ || !transport_->socket.is_open())
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
    if (transport_ && transport_->socket.is_open())
        ::shutdown(transport_->socket.native_handle(), SHUT_RDWR);
}

bool ModuleClient::ReadEnvelope(Envelope& envelope)
{
    boost::system::error_code ec;

    std::array<std::uint8_t, 4> header{};
    boost::asio::read(transport_->socket, boost::asio::buffer(header), ec);
    if (ec)
        return false;

    std::uint32_t const length = DecodeLength(header);
    if (length == 0 || length > kMaxFrameBytes)
        return false;

    std::string body(length, '\0');
    boost::asio::read(transport_->socket, boost::asio::buffer(body), ec);
    if (ec)
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

    std::array<boost::asio::const_buffer, 2> buffers{
        boost::asio::buffer(header),
        boost::asio::buffer(body),
    };

    std::lock_guard<std::mutex> lock(transport_->writeMutex);
    boost::system::error_code ec;
    boost::asio::write(transport_->socket, buffers, ec);
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
        case Envelope::kModuleConfig:
        {
            config_.clear();
            for (auto const& [key, value] : envelope.module_config().values())
                config_[key] = value;
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
