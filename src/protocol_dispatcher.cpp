#include "qiven/host/protocol_dispatcher.hpp"

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace qiven::host
{
namespace
{
struct LeaseGenerationResult final
{
    bool ok {};
    std::uint32_t native_error {};
};

bool id_nonzero(const LeaseId& value) noexcept
{
    for (const std::uint8_t byte : value.bytes)
    {
        if (byte != 0)
            return true;
    }
    return false;
}

LeaseGenerationResult generate_lease(LeaseId& output) noexcept
{
#if !defined(_WIN32)
    (void)output;
    return { false, 0 };
#else
    for (unsigned attempt = 0; attempt != 4; ++attempt)
    {
        LeaseId candidate {};
        const NTSTATUS status = BCryptGenRandom(nullptr, candidate.bytes.data(), static_cast<ULONG>(candidate.bytes.size()),
                                                BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0)
            return { false, static_cast<std::uint32_t>(status) };
        if (id_nonzero(candidate))
        {
            output = candidate;
            return { true, 0 };
        }
    }
    return { false, ERROR_INVALID_DATA };
#endif
}

ProtocolResultCode protocol_result(const AuthorityBrokerResult& status) noexcept
{
    switch (status.authority_error)
    {
    case AuthorityError::none:
        break;
    case AuthorityError::not_ready:
        return ProtocolResultCode::not_ready;
    case AuthorityError::already_leased:
    case AuthorityError::concurrent_execution:
    case AuthorityError::stale_execution:
        return ProtocolResultCode::concurrent_execution;
    case AuthorityError::stale_lease:
        return ProtocolResultCode::stale_lease;
    case AuthorityError::stale_fence:
        return ProtocolResultCode::stale_fence;
    case AuthorityError::invalid_sequence:
        return ProtocolResultCode::invalid_sequence;
    case AuthorityError::replay:
        return ProtocolResultCode::replay;
    case AuthorityError::replay_conflict:
        return ProtocolResultCode::replay_conflict;
    case AuthorityError::sequence_exhausted:
        return ProtocolResultCode::sequence_exhausted;
    case AuthorityError::operation_in_flight:
        return ProtocolResultCode::operation_in_flight;
    case AuthorityError::reconciling:
        return ProtocolResultCode::reconciling;
    case AuthorityError::quarantined:
        return ProtocolResultCode::quarantined;
    case AuthorityError::persistence_failure:
        return ProtocolResultCode::persistence_failure;
    case AuthorityError::recovery_not_authorized:
    case AuthorityError::recovery_context_stale:
    case AuthorityError::generation_exhausted:
    case AuthorityError::fence_exhausted:
        return ProtocolResultCode::internal_failure;
    }

    switch (status.error)
    {
    case AuthorityBrokerError::none:
        return ProtocolResultCode::ok;
    case AuthorityBrokerError::not_started:
        return ProtocolResultCode::not_ready;
    case AuthorityBrokerError::persistence_fault:
    case AuthorityBrokerError::inconsistent_persistence:
    case AuthorityBrokerError::fence_store_failure:
    case AuthorityBrokerError::journal_failure:
    case AuthorityBrokerError::identity_digest_failure:
        return ProtocolResultCode::persistence_failure;
    case AuthorityBrokerError::already_started:
    case AuthorityBrokerError::platform_unsupported:
        return ProtocolResultCode::internal_failure;
    }
    return ProtocolResultCode::internal_failure;
}

ProtocolAuthorityPhase protocol_phase(AuthorityPhase phase) noexcept
{
    switch (phase)
    {
    case AuthorityPhase::ready:
        return ProtocolAuthorityPhase::ready;
    case AuthorityPhase::leased:
        return ProtocolAuthorityPhase::leased;
    case AuthorityPhase::reconciling:
        return ProtocolAuthorityPhase::reconciling;
    case AuthorityPhase::quarantined:
        return ProtocolAuthorityPhase::quarantined;
    }
    return ProtocolAuthorityPhase::quarantined;
}

std::uint64_t request_digest(std::span<const std::byte> payload) noexcept
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t digest = offset;
    for (const std::byte byte : payload)
    {
        digest ^= std::to_integer<std::uint8_t>(byte);
        digest *= prime;
    }
    return digest;
}

ProtocolDispatchResult malformed(ProtocolCodecError error) noexcept
{
    ProtocolDispatchResult result {};
    result.error = ProtocolDispatchError::malformed_request;
    result.codec_error = error;
    return result;
}

ProtocolDispatchResult encode_failure(ProtocolCodecError error) noexcept
{
    ProtocolDispatchResult result {};
    result.error = ProtocolDispatchError::response_encode_failure;
    result.codec_error = error;
    return result;
}

template <class Message>
ProtocolDispatchResult encode_response(const Message& message, ProtocolFrame& output) noexcept
{
    ProtocolFrame candidate {};
    const ProtocolCodecResult encoded = encode_protocol_message(message, candidate);
    if (!encoded.ok())
        return encode_failure(encoded.error);
    output = candidate;
    return {};
}

bool same_lease(const Lease& left, const Lease& right) noexcept
{
    return left.execution == right.execution && left.lease == right.lease && left.epoch == right.epoch;
}

OperationOutcome execute_no_op() noexcept
{
    return OperationOutcome::success;
}
} // namespace

ProtocolDispatchResult ProtocolDispatcher::dispatch(BrokerSessionId session, const ProtocolFrame& request,
                                                     ProtocolFrame& response) noexcept
{
    ProtocolFrameView frame {};
    const ProtocolCodecResult inspected = inspect_protocol_frame(request.view(), frame);
    if (!inspected.ok())
        return malformed(inspected.error);

    switch (frame.kind)
    {
    case ProtocolMessageKind::acquire_request:
    {
        AcquireRequestMessage message {};
        const ProtocolCodecResult decoded = decode_protocol_message(frame, message);
        if (!decoded.ok())
            return malformed(decoded.error);

        LeaseId lease_id {};
        const LeaseGenerationResult generated = generate_lease(lease_id);
        if (!generated.ok)
        {
            ProtocolDispatchResult result {};
            result.error = ProtocolDispatchError::lease_generation_failure;
            result.native_error = generated.native_error;
            return result;
        }

        const AuthorityBrokerAcquireResult acquired = broker_.acquire(session, message.execution, lease_id);
        AcquireResponseMessage reply {};
        reply.result = protocol_result(acquired.status);
        if (!acquired.ok())
            return encode_response(reply, response);

        const AuthorityBrokerStatus status = broker_.status();
        reply.execution = acquired.lease.execution;
        reply.lease = acquired.lease.lease;
        reply.generation = status.generation;
        reply.epoch = acquired.lease.epoch;

        ProtocolFrame candidate {};
        const ProtocolCodecResult encoded = encode_protocol_message(reply, candidate);
        if (!encoded.ok())
        {
            const AuthorityBrokerResult cleanup = broker_.disconnect(session, acquired.lease);
            if (!cleanup.ok())
            {
                ProtocolDispatchResult result {};
                result.error = ProtocolDispatchError::authority_cleanup_failure;
                result.broker_error = cleanup.error;
                result.authority_error = cleanup.authority_error;
                result.native_error = cleanup.native_error;
                return result;
            }
            return encode_failure(encoded.error);
        }

        {
            std::lock_guard lock(lease_mutex_);
            active_lease_ = ActiveLease { session, acquired.lease };
        }
        response = candidate;
        return {};
    }

    case ProtocolMessageKind::execute_test_operation_request:
    {
        ExecuteTestOperationRequestMessage message {};
        const ProtocolCodecResult decoded = decode_protocol_message(frame, message);
        if (!decoded.ok())
            return malformed(decoded.error);

        const Request operation {
            { message.execution, message.lease, message.epoch },
            message.sequence,
            message.operation,
            request_digest(frame.payload)
        };

        const AuthorityBrokerResult admitted = broker_.begin_operation(session, operation);
        ExecuteTestOperationResponseMessage reply {};
        reply.result = protocol_result(admitted);
        reply.replayed = admitted.ok() && admitted.replayed;

        if (admitted.ok() && !admitted.replayed)
        {
            const OperationOutcome outcome = execute_no_op();
            const AuthorityBrokerResult finished = broker_.finish_operation(session, operation, outcome);
            reply.result = protocol_result(finished);
            reply.replayed = false;
        }

        return encode_response(reply, response);
    }

    case ProtocolMessageKind::release_request:
    {
        ReleaseRequestMessage message {};
        const ProtocolCodecResult decoded = decode_protocol_message(frame, message);
        if (!decoded.ok())
            return malformed(decoded.error);

        const Lease lease { message.execution, message.lease, message.epoch };
        const AuthorityBrokerResult released = broker_.release(session, lease);
        if (released.ok())
        {
            std::lock_guard lock(lease_mutex_);
            if (active_lease_ && active_lease_->session == session && same_lease(active_lease_->lease, lease))
                active_lease_.reset();
        }

        ReleaseResponseMessage reply {};
        reply.result = protocol_result(released);
        return encode_response(reply, response);
    }

    case ProtocolMessageKind::query_status_request:
    {
        QueryStatusRequestMessage message {};
        const ProtocolCodecResult decoded = decode_protocol_message(frame, message);
        if (!decoded.ok())
            return malformed(decoded.error);

        const AuthorityBrokerStatus status = broker_.status();
        QueryStatusResponseMessage reply {};
        if (!status.started)
        {
            reply.result = ProtocolResultCode::not_ready;
            return encode_response(reply, response);
        }

        reply.result = ProtocolResultCode::ok;
        reply.started = true;
        reply.persistence_fault = status.persistence_fault;
        reply.phase = protocol_phase(status.phase);
        reply.generation = status.generation;
        reply.epoch = status.epoch;
        reply.next_sequence = status.next_sequence;
        return encode_response(reply, response);
    }

    case ProtocolMessageKind::acquire_response:
    case ProtocolMessageKind::execute_test_operation_response:
    case ProtocolMessageKind::release_response:
    case ProtocolMessageKind::query_status_response:
    {
        ProtocolDispatchResult result {};
        result.error = ProtocolDispatchError::unexpected_message_kind;
        return result;
    }
    }

    ProtocolDispatchResult result {};
    result.error = ProtocolDispatchError::unexpected_message_kind;
    return result;
}

ProtocolDispatchResult ProtocolDispatcher::close_connection(OwnerPipeServer& server, std::size_t slot) noexcept
{
    BrokerSessionId session {};
    const LocalPipeResult session_status = server.session(slot, session);
    if (!session_status.ok())
    {
        ProtocolDispatchResult result {};
        result.error = ProtocolDispatchError::transport_failure;
        result.transport_error = session_status.error;
        result.native_error = session_status.native_error;
        return result;
    }

    const LocalPipeResult transport_disconnect = server.disconnect(slot);

    std::optional<Lease> lease;
    {
        std::lock_guard lock(lease_mutex_);
        if (active_lease_ && active_lease_->session == session)
            lease = active_lease_->lease;
    }

    if (lease)
    {
        const AuthorityBrokerResult disconnected = broker_.disconnect(session, *lease);
        bool authority_safe = disconnected.ok();
        if (!authority_safe)
        {
            const AuthorityBrokerStatus status = broker_.status();
            authority_safe = status.started && !status.persistence_fault && status.phase != AuthorityPhase::leased;
        }

        if (!authority_safe)
        {
            ProtocolDispatchResult result {};
            result.error = ProtocolDispatchError::authority_cleanup_failure;
            result.broker_error = disconnected.error;
            result.authority_error = disconnected.authority_error;
            result.native_error = disconnected.native_error;
            return result;
        }

        std::lock_guard lock(lease_mutex_);
        if (active_lease_ && active_lease_->session == session && same_lease(active_lease_->lease, *lease))
            active_lease_.reset();
    }

    if (!transport_disconnect.ok())
    {
        ProtocolDispatchResult result {};
        result.error = ProtocolDispatchError::transport_failure;
        result.transport_error = transport_disconnect.error;
        result.native_error = transport_disconnect.native_error;
        return result;
    }

    const LocalPipeResult retired = server.retire(slot);
    if (!retired.ok())
    {
        ProtocolDispatchResult result {};
        result.error = ProtocolDispatchError::transport_failure;
        result.transport_error = retired.error;
        result.native_error = retired.native_error;
        return result;
    }
    return {};
}
} // namespace qiven::host
