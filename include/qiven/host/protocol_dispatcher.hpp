#pragma once

#include "qiven/host/authority_broker.hpp"
#include "qiven/host/local_pipe_transport.hpp"
#include "qiven/host/protocol_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace qiven::host
{
enum class ProtocolDispatchError : std::uint8_t
{
    none,
    malformed_request,
    unexpected_message_kind,
    lease_generation_failure,
    response_encode_failure,
    authority_cleanup_failure,
    transport_failure
};

struct ProtocolDispatchResult final
{
    ProtocolDispatchError error {};
    ProtocolCodecError codec_error {};
    AuthorityBrokerError broker_error {};
    AuthorityError authority_error {};
    LocalPipeError transport_error {};
    std::uint32_t native_error {};

    bool ok() const noexcept
    {
        return error == ProtocolDispatchError::none;
    }
};

class ProtocolDispatcher final
{
public:
    explicit ProtocolDispatcher(AuthorityBroker& broker) noexcept : broker_(broker)
    {
    }

    ProtocolDispatcher(const ProtocolDispatcher&) = delete;
    ProtocolDispatcher& operator=(const ProtocolDispatcher&) = delete;
    ProtocolDispatcher(ProtocolDispatcher&&) = delete;
    ProtocolDispatcher& operator=(ProtocolDispatcher&&) = delete;

    ProtocolDispatchResult dispatch(BrokerSessionId session, const ProtocolFrame& request,
                                    ProtocolFrame& response) noexcept;

    // Establishes the transport Retiring boundary, performs any required authority-side
    // disconnect/reconciliation for the Host-owned session, and only then permits slot reuse.
    ProtocolDispatchResult close_connection(OwnerPipeServer& server, std::size_t slot) noexcept;

private:
    struct ActiveLease final
    {
        BrokerSessionId session {};
        Lease lease {};
    };

    AuthorityBroker& broker_;
    std::mutex lease_mutex_;
    std::optional<ActiveLease> active_lease_;
};
} // namespace qiven::host
