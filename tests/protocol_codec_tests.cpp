#include "qiven/host/protocol_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

using namespace qiven::host;

namespace
{
template <class T>
T id(std::uint8_t value)
{
    T result {};
    result.bytes[0] = value;
    return result;
}

void require(bool value)
{
    if (!value)
        std::abort();
}

template <std::size_t N>
void set_text(std::array<char, N>& target, std::uint8_t& size, std::string_view value)
{
    require(value.size() <= N);
    size = static_cast<std::uint8_t>(value.size());
    for (std::size_t i = 0; i != value.size(); ++i)
        target[i] = value[i];
}

ProtocolFrameView inspect(const ProtocolFrame& frame)
{
    ProtocolFrameView view {};
    require(inspect_protocol_frame(frame.view(), view).ok());
    return view;
}

void set_u16(ProtocolFrame& frame, std::size_t offset, std::uint16_t value)
{
    frame.bytes[offset] = static_cast<std::byte>(value & 0xffu);
    frame.bytes[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xffu);
}

void set_u32(ProtocolFrame& frame, std::size_t offset, std::uint32_t value)
{
    for (unsigned shift = 0; shift != 32; shift += 8)
        frame.bytes[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffu);
}

void zero_range(ProtocolFrame& frame, std::size_t offset, std::size_t count)
{
    for (std::size_t i = 0; i != count; ++i)
        frame.bytes[offset + i] = std::byte { 0 };
}
} // namespace

int main()
{
    static_assert(protocol_max_frame_size == 256);
    static_assert(protocol_header_size == 16);
    static_assert(protocol_max_payload_size == 240);

    AcquireRequestMessage acquire {};
    acquire.execution = id<ExecutionId>(2);
    set_text(acquire.scope, acquire.scope_size, "qiven-host");
    set_text(acquire.intent, acquire.intent_size, "batch-000-protocol-codec");

    ProtocolFrame acquire_frame {};
    require(encode_protocol_message(acquire, acquire_frame).ok());
    require(acquire_frame.size == protocol_header_size + 116);
    auto acquire_view = inspect(acquire_frame);
    require(acquire_view.kind == ProtocolMessageKind::acquire_request);
    AcquireRequestMessage decoded_acquire {};
    require(decode_protocol_message(acquire_view, decoded_acquire).ok());
    require(decoded_acquire.execution == acquire.execution);
    require(decoded_acquire.scope_size == acquire.scope_size);
    require(decoded_acquire.intent_size == acquire.intent_size);

    ProtocolFrame unchanged {};
    unchanged.size = 7;
    AcquireRequestMessage invalid_acquire = acquire;
    invalid_acquire.scope_size = 0;
    require(encode_protocol_message(invalid_acquire, unchanged).error == ProtocolCodecError::invalid_field);
    require(unchanged.size == 7);

    {
        ProtocolFrame bad = acquire_frame;
        bad.bytes[0] = std::byte { 0 };
        ProtocolFrameView sentinel { ProtocolMessageKind::release_request, {} };
        require(inspect_protocol_frame(bad.view(), sentinel).error == ProtocolCodecError::bad_magic);
        require(sentinel.kind == ProtocolMessageKind::release_request);
    }
    {
        ProtocolFrame bad = acquire_frame;
        set_u16(bad, 4, 2);
        ProtocolFrameView view {};
        require(inspect_protocol_frame(bad.view(), view).error == ProtocolCodecError::unsupported_version);
    }
    {
        ProtocolFrame bad = acquire_frame;
        set_u16(bad, 6, 0xffffu);
        ProtocolFrameView view {};
        require(inspect_protocol_frame(bad.view(), view).error == ProtocolCodecError::unknown_message_kind);
    }
    {
        ProtocolFrame bad = acquire_frame;
        set_u32(bad, 12, 1);
        ProtocolFrameView view {};
        require(inspect_protocol_frame(bad.view(), view).error == ProtocolCodecError::nonzero_reserved);
    }
    {
        ProtocolFrame bad = acquire_frame;
        set_u32(bad, 8, 115);
        ProtocolFrameView view {};
        require(inspect_protocol_frame(bad.view(), view).error == ProtocolCodecError::payload_size_mismatch);
    }
    {
        ProtocolFrameView view {};
        require(inspect_protocol_frame(acquire_frame.view().first(protocol_header_size - 1), view).error ==
                ProtocolCodecError::frame_too_small);
        std::array<std::byte, protocol_max_frame_size + 1> oversized {};
        require(inspect_protocol_frame(oversized, view).error == ProtocolCodecError::frame_too_large);
    }
    {
        std::array<std::byte, protocol_max_frame_size> storage {};
        for (std::size_t i = 0; i != acquire_frame.size; ++i)
            storage[i] = acquire_frame.bytes[i];
        ProtocolFrameView view {};
        require(inspect_protocol_frame(std::span<const std::byte>(storage.data(), acquire_frame.size + 1), view).error ==
                ProtocolCodecError::payload_size_mismatch);
    }
    {
        ProtocolFrame bad = acquire_frame;
        zero_range(bad, protocol_header_size, 16);
        AcquireRequestMessage sentinel {};
        sentinel.execution = id<ExecutionId>(99);
        auto view = inspect(bad);
        require(decode_protocol_message(view, sentinel).error == ProtocolCodecError::invalid_field);
        require(sentinel.execution == id<ExecutionId>(99));
    }
    {
        ProtocolFrame bad = acquire_frame;
        bad.bytes[protocol_header_size + 20 + acquire.scope_size] = static_cast<std::byte>('X');
        AcquireRequestMessage decoded {};
        require(decode_protocol_message(inspect(bad), decoded).error == ProtocolCodecError::invalid_field);
    }
    {
        ProtocolFrame bad = acquire_frame;
        bad.bytes[protocol_header_size + 18] = std::byte { 1 };
        AcquireRequestMessage decoded {};
        require(decode_protocol_message(inspect(bad), decoded).error == ProtocolCodecError::nonzero_reserved);
    }

    AcquireResponseMessage acquired {};
    acquired.result = ProtocolResultCode::ok;
    acquired.execution = acquire.execution;
    acquired.lease = id<LeaseId>(3);
    acquired.generation = { 4 };
    acquired.epoch = { 5 };
    ProtocolFrame acquired_frame {};
    require(encode_protocol_message(acquired, acquired_frame).ok());
    AcquireResponseMessage decoded_acquired {};
    require(decode_protocol_message(inspect(acquired_frame), decoded_acquired).ok());
    require(decoded_acquired.execution == acquired.execution && decoded_acquired.lease == acquired.lease);
    require(decoded_acquired.generation.value == 4 && decoded_acquired.epoch.value == 5);

    AcquireResponseMessage rejected {};
    rejected.result = ProtocolResultCode::quarantined;
    require(encode_protocol_message(rejected, acquired_frame).ok());
    rejected.lease = id<LeaseId>(9);
    require(encode_protocol_message(rejected, acquired_frame).error == ProtocolCodecError::invalid_field);

    ExecuteTestOperationRequestMessage execute {};
    execute.execution = acquire.execution;
    execute.lease = acquired.lease;
    execute.operation = id<OperationId>(4);
    execute.epoch = acquired.epoch;
    execute.sequence = { 1 };
    execute.operation_code = TestOperationCode::no_op;
    ProtocolFrame execute_frame {};
    require(encode_protocol_message(execute, execute_frame).ok());
    require(execute_frame.size == protocol_header_size + 72);
    ExecuteTestOperationRequestMessage decoded_execute {};
    require(decode_protocol_message(inspect(execute_frame), decoded_execute).ok());
    require(decoded_execute.execution == execute.execution && decoded_execute.lease == execute.lease);
    require(decoded_execute.operation == execute.operation && decoded_execute.sequence.value == 1);
    {
        ProtocolFrame bad = execute_frame;
        zero_range(bad, protocol_header_size + 56, 8);
        ExecuteTestOperationRequestMessage decoded {};
        require(decode_protocol_message(inspect(bad), decoded).error == ProtocolCodecError::invalid_field);
    }
    {
        ProtocolFrame bad = execute_frame;
        set_u16(bad, protocol_header_size + 64, 2);
        ExecuteTestOperationRequestMessage decoded {};
        require(decode_protocol_message(inspect(bad), decoded).error == ProtocolCodecError::invalid_field);
    }
    {
        ProtocolFrame bad = execute_frame;
        bad.bytes[protocol_header_size + 66] = std::byte { 1 };
        ExecuteTestOperationRequestMessage decoded {};
        require(decode_protocol_message(inspect(bad), decoded).error == ProtocolCodecError::nonzero_reserved);
    }

    ExecuteTestOperationResponseMessage execute_response { ProtocolResultCode::ok, true };
    ProtocolFrame execute_response_frame {};
    require(encode_protocol_message(execute_response, execute_response_frame).ok());
    ExecuteTestOperationResponseMessage decoded_execute_response {};
    require(decode_protocol_message(inspect(execute_response_frame), decoded_execute_response).ok());
    require(decoded_execute_response.replayed);
    execute_response.result = ProtocolResultCode::replay_conflict;
    require(encode_protocol_message(execute_response, execute_response_frame).error == ProtocolCodecError::invalid_field);

    ReleaseRequestMessage release { acquire.execution, acquired.lease, acquired.epoch };
    ProtocolFrame release_frame {};
    require(encode_protocol_message(release, release_frame).ok());
    require(release_frame.size == protocol_header_size + 40);
    ReleaseRequestMessage decoded_release {};
    require(decode_protocol_message(inspect(release_frame), decoded_release).ok());
    require(decoded_release.execution == release.execution && decoded_release.lease == release.lease);
    require(decoded_release.epoch.value == release.epoch.value);

    ReleaseResponseMessage release_response { ProtocolResultCode::ok };
    ProtocolFrame release_response_frame {};
    require(encode_protocol_message(release_response, release_response_frame).ok());
    ReleaseResponseMessage decoded_release_response {};
    require(decode_protocol_message(inspect(release_response_frame), decoded_release_response).ok());

    QueryStatusRequestMessage query {};
    ProtocolFrame query_frame {};
    require(encode_protocol_message(query, query_frame).ok());
    require(query_frame.size == protocol_header_size);
    QueryStatusRequestMessage decoded_query {};
    require(decode_protocol_message(inspect(query_frame), decoded_query).ok());
    AcquireRequestMessage wrong_type {};
    require(decode_protocol_message(inspect(query_frame), wrong_type).error == ProtocolCodecError::wrong_message_kind);

    QueryStatusResponseMessage status {};
    status.result = ProtocolResultCode::ok;
    status.started = true;
    status.persistence_fault = true;
    status.phase = ProtocolAuthorityPhase::quarantined;
    status.generation = { 7 };
    status.epoch = { 9 };
    status.next_sequence = { 3 };
    ProtocolFrame status_frame {};
    require(encode_protocol_message(status, status_frame).ok());
    QueryStatusResponseMessage decoded_status {};
    require(decode_protocol_message(inspect(status_frame), decoded_status).ok());
    require(decoded_status.started && decoded_status.persistence_fault);
    require(decoded_status.phase == ProtocolAuthorityPhase::quarantined);
    require(decoded_status.generation.value == 7 && decoded_status.epoch.value == 9 && decoded_status.next_sequence.value == 3);

    QueryStatusResponseMessage failed_status {};
    failed_status.result = ProtocolResultCode::internal_failure;
    require(encode_protocol_message(failed_status, status_frame).ok());
    failed_status.started = true;
    require(encode_protocol_message(failed_status, status_frame).error == ProtocolCodecError::invalid_field);

    return 0;
}
