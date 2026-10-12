#include "forever_module/module_client.h"

// SKELETON ONLY -- see header comment. No real boost::asio implementation yet.

namespace forever_module {

ModuleClient::ModuleClient(ModuleManifest manifest)
    : manifest_(std::move(manifest))
{
}

ModuleClient::~ModuleClient() = default;

bool ModuleClient::Connect(std::string const& /*endpoint*/)
{
    // TODO: open a boost::asio local stream socket (named pipe on Windows, Unix domain socket on
    // Linux) to the given endpoint.
    return false;
}

bool ModuleClient::Handshake(std::chrono::milliseconds /*timeout*/)
{
    // TODO: send HandshakeRequest (manifest_ + kProtocolVersion from protocol_version.h), wait for
    // HandshakeResponse with the given timeout, store assigned module_id and tier2_region_name.
    return false;
}

void ModuleClient::OnConfig(std::function<void(std::unordered_map<std::string, std::string> const&)> /*callback*/)
{
    // TODO: register callback, invoke it when a ModuleConfig envelope arrives.
}

void ModuleClient::OnNotify(std::string const& /*hookName*/, std::function<void(EventNotify const&)> /*callback*/)
{
    // TODO: register callback keyed by hookName, invoke on matching EventNotify envelopes.
}

void ModuleClient::OnRequest(std::string const& /*hookName*/, std::function<EventReply(EventRequest const&)> /*callback*/)
{
    // TODO: register callback keyed by hookName, invoke on matching EventRequest envelopes and
    // send back the returned EventReply with the correlating message_id.
}

void ModuleClient::OnControlRevoked(std::function<void(std::uint64_t, std::string const&)> /*callback*/)
{
    // TODO: register callback, invoke on ControlRevoked envelopes.
}

ControlResult ModuleClient::RequestControl(std::uint64_t /*botGuid*/, SpawnParams const& /*params*/)
{
    // TODO: send ControlRequest, block for ControlResponse, translate into ControlResult.
    return ControlResult{};
}

void ModuleClient::ReleaseControl(std::uint64_t /*botGuid*/)
{
    // TODO: send ReleaseControl envelope.
}

Tier2Session* ModuleClient::AttachTier2()
{
    // TODO: if manifest_.tier2_participates and handshake granted a tier2_region_name, attach to
    // the shared-memory region and return an owned Tier2Session*. Return nullptr otherwise.
    return nullptr;
}

void ModuleClient::Run()
{
    // TODO: blocking io_context.run() loop; dispatch heartbeats on an interval; invoke registered
    // callbacks as envelopes arrive; return on Shutdown() or connection loss.
}

void ModuleClient::Shutdown()
{
    // TODO: send a Shutdown envelope (or just close the connection) and stop Run()'s loop.
}

} // namespace forever_module
