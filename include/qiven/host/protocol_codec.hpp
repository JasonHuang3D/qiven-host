#pragma once

#include "qiven/host/authority.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qiven::host
{
inline constexpr std::uint16_t protocol_version = 1;
inline constexpr std::size_t protocol_header_size = 16;
inline constexpr std::size_t protocol_max_frame_size = 256;
inline constexpr std::size_t protocol_max_payload_size = protocol_max_frame_size - protocol_header_size;
inline constexpr std::size_t protocol_max_scope_bytes = 32;
inline constexpr std::size_t protocol_max_intent_bytes = 64;

enum class ProtocolMessageKind : std::uint16_t
{
    acquire_request = 1,
    acquire_response = 2,
    execute_test_operation_request = 3,
    execute_test_operation_response = 4,
    release_request = 5,
    release_response = 6,
    query_status_request = 7,
    query_status_response = 8
};

enum class ProtocolResultCode : std::uint16_t
{
    ok = 0,
    not_ready = 1,
    concurrent_execution = 2,
    stale_lease = 3,
    stale_fence = 4,
    invalid_sequence = 5,
    replay = 6,
    replay_conflict = 7,
    sequence_exhausted = 8,
    operation_in_flight = 9,
    reconciling = 10,
    quarantined = 11,
    persistence_failure = 12,
    internal_failure = 13
};

enum class ProtocolAuthorityPhase : std::uint8_t
{
    ready = 1,
    leased = 2,
    reconciling = 3,
    quarantined = 4
};

enum class TestOperationCode : std::uint16_t
{
    no_op = 1
};

enum class ProtocolCodecError : std::uint8_t
{
    none,
    frame_too_small,
    frame_too_large,
    bad_magic,
    unsupported_version,
    unknown_message_kind,
    nonzero_reserved,
    payload_size_mismatch,
    wrong_message_kind,
    invalid_field,
    internal_overflow
};

struct ProtocolCodecResult final
{
    ProtocolCodecError error {};

    bool ok() const noexcept
    {
        return error == ProtocolCodecError::none;
    }
};

struct ProtocolFrame final
{
    std::array<std::byte, protocol_max_frame_size> bytes {};
    std::uint16_t size {};

    std::span<const std::byte> view() const noexcept
    {
        return { bytes.data(), size };
    }
};

struct ProtocolFrameView final
{
    ProtocolMessageKind kind { ProtocolMessageKind::query_status_request };
    std::span<const std::byte> payload {};
};

struct AcquireRequestMessage final
{
    ExecutionId execution {};
    std::uint8_t scope_size {};
    std::uint8_t intent_size {};
    std::array<char, protocol_max_scope_bytes> scope {};
    std::array<char, protocol_max_intent_bytes> intent {};
};

struct AcquireResponseMessage final
{
    ProtocolResultCode result { ProtocolResultCode::ok };
    ExecutionId execution {};
    LeaseId lease {};
    PersistentGeneration generation {};
    FencingEpoch epoch {};
};

struct ExecuteTestOperationRequestMessage final
{
    ExecutionId execution {};
    LeaseId lease {};
    OperationId operation {};
    FencingEpoch epoch {};
    RequestSequence sequence {};
    TestOperationCode operation_code { TestOperationCode::no_op };
};

struct ExecuteTestOperationResponseMessage final
{
    ProtocolResultCode result { ProtocolResultCode::ok };
    bool replayed {};
};

struct ReleaseRequestMessage final
{
    ExecutionId execution {};
    LeaseId lease {};
    FencingEpoch epoch {};
};

struct ReleaseResponseMessage final
{
    ProtocolResultCode result { ProtocolResultCode::ok };
};

struct QueryStatusRequestMessage final
{
};

struct QueryStatusResponseMessage final
{
    ProtocolResultCode result { ProtocolResultCode::ok };
    bool started {};
    bool persistence_fault {};
    ProtocolAuthorityPhase phase { ProtocolAuthorityPhase::ready };
    PersistentGeneration generation {};
    FencingEpoch epoch {};
    RequestSequence next_sequence {};
};

ProtocolCodecResult inspect_protocol_frame(std::span<const std::byte> bytes, ProtocolFrameView& output) noexcept;

ProtocolCodecResult encode_protocol_message(const AcquireRequestMessage& message, ProtocolFrame& output) noexcept;
ProtocolCodecResult encode_protocol_message(const AcquireResponseMessage& message, ProtocolFrame& output) noexcept;
ProtocolCodecResult encode_protocol_message(const ExecuteTestOperationRequestMessage& message, ProtocolFrame& output) noexcept;
ProtocolCodecResult encode_protocol_message(const ExecuteTestOperationResponseMessage& message, ProtocolFrame& output) noexcept;
ProtocolCodecResult encode_protocol_message(const ReleaseRequestMessage& message, ProtocolFrame& output) noexcept;
ProtocolCodecResult encode_protocol_message(const ReleaseResponseMessage& message, ProtocolFrame& output) noexcept;
ProtocolCodecResult encode_protocol_message(const QueryStatusRequestMessage& message, ProtocolFrame& output) noexcept;
ProtocolCodecResult encode_protocol_message(const QueryStatusResponseMessage& message, ProtocolFrame& output) noexcept;

ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, AcquireRequestMessage& output) noexcept;
ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, AcquireResponseMessage& output) noexcept;
ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, ExecuteTestOperationRequestMessage& output) noexcept;
ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, ExecuteTestOperationResponseMessage& output) noexcept;
ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, ReleaseRequestMessage& output) noexcept;
ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, ReleaseResponseMessage& output) noexcept;
ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, QueryStatusRequestMessage& output) noexcept;
ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, QueryStatusResponseMessage& output) noexcept;
} // namespace qiven::host
