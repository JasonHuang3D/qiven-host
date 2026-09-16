#include "qiven/host/protocol_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace qiven::host
{
namespace
{
constexpr std::array<std::uint8_t, 4> magic { 'Q', 'V', 'H', '0' };
constexpr std::size_t acquire_request_payload_size = 116;
constexpr std::size_t acquire_response_payload_size = 52;
constexpr std::size_t execute_request_payload_size = 72;
constexpr std::size_t execute_response_payload_size = 8;
constexpr std::size_t release_request_payload_size = 40;
constexpr std::size_t release_response_payload_size = 4;
constexpr std::size_t query_request_payload_size = 0;
constexpr std::size_t query_response_payload_size = 32;

static_assert(acquire_request_payload_size <= protocol_max_payload_size);
static_assert(acquire_response_payload_size <= protocol_max_payload_size);
static_assert(execute_request_payload_size <= protocol_max_payload_size);
static_assert(execute_response_payload_size <= protocol_max_payload_size);
static_assert(release_request_payload_size <= protocol_max_payload_size);
static_assert(release_response_payload_size <= protocol_max_payload_size);
static_assert(query_response_payload_size <= protocol_max_payload_size);

class Writer final
{
public:
    explicit Writer(std::span<std::byte> output) noexcept : output_(output)
    {
    }

    bool u8(std::uint8_t value) noexcept
    {
        if (!has(1))
            return false;
        output_[offset_++] = static_cast<std::byte>(value);
        return true;
    }

    bool u16(std::uint16_t value) noexcept
    {
        return u8(static_cast<std::uint8_t>(value & 0xffu)) &&
               u8(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    }

    bool u32(std::uint32_t value) noexcept
    {
        for (unsigned shift = 0; shift != 32; shift += 8)
        {
            if (!u8(static_cast<std::uint8_t>((value >> shift) & 0xffu)))
                return false;
        }
        return true;
    }

    bool u64(std::uint64_t value) noexcept
    {
        for (unsigned shift = 0; shift != 64; shift += 8)
        {
            if (!u8(static_cast<std::uint8_t>((value >> shift) & 0xffu)))
                return false;
        }
        return true;
    }

    bool raw(const std::uint8_t* bytes, std::size_t size) noexcept
    {
        if (!has(size))
            return false;
        for (std::size_t i = 0; i != size; ++i)
            output_[offset_ + i] = static_cast<std::byte>(bytes[i]);
        offset_ += size;
        return true;
    }

    bool text(const char* bytes, std::size_t size, std::size_t capacity) noexcept
    {
        if (size > capacity || !has(capacity))
            return false;
        for (std::size_t i = 0; i != size; ++i)
            output_[offset_ + i] = static_cast<std::byte>(static_cast<unsigned char>(bytes[i]));
        for (std::size_t i = size; i != capacity; ++i)
            output_[offset_ + i] = std::byte { 0 };
        offset_ += capacity;
        return true;
    }

    bool zeroes(std::size_t count) noexcept
    {
        if (!has(count))
            return false;
        for (std::size_t i = 0; i != count; ++i)
            output_[offset_ + i] = std::byte { 0 };
        offset_ += count;
        return true;
    }

    std::size_t size() const noexcept
    {
        return offset_;
    }

private:
    bool has(std::size_t count) const noexcept
    {
        return offset_ <= output_.size() && count <= output_.size() - offset_;
    }

    std::span<std::byte> output_;
    std::size_t offset_ {};
};

class Reader final
{
public:
    explicit Reader(std::span<const std::byte> input) noexcept : input_(input)
    {
    }

    bool u8(std::uint8_t& value) noexcept
    {
        if (!has(1))
            return false;
        value = std::to_integer<std::uint8_t>(input_[offset_++]);
        return true;
    }

    bool u16(std::uint16_t& value) noexcept
    {
        std::uint8_t b0 = 0;
        std::uint8_t b1 = 0;
        if (!u8(b0) || !u8(b1))
            return false;
        value = static_cast<std::uint16_t>(b0) | (static_cast<std::uint16_t>(b1) << 8u);
        return true;
    }

    bool u32(std::uint32_t& value) noexcept
    {
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
        {
            std::uint8_t byte = 0;
            if (!u8(byte))
                return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }

    bool u64(std::uint64_t& value) noexcept
    {
        value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8)
        {
            std::uint8_t byte = 0;
            if (!u8(byte))
                return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }

    bool raw(std::uint8_t* bytes, std::size_t size) noexcept
    {
        if (!has(size))
            return false;
        for (std::size_t i = 0; i != size; ++i)
            bytes[i] = std::to_integer<std::uint8_t>(input_[offset_ + i]);
        offset_ += size;
        return true;
    }

    bool text(char* bytes, std::size_t capacity) noexcept
    {
        if (!has(capacity))
            return false;
        for (std::size_t i = 0; i != capacity; ++i)
            bytes[i] = static_cast<char>(std::to_integer<unsigned char>(input_[offset_ + i]));
        offset_ += capacity;
        return true;
    }

    bool zeroes(std::size_t count) noexcept
    {
        if (!has(count))
            return false;
        for (std::size_t i = 0; i != count; ++i)
        {
            if (input_[offset_ + i] != std::byte { 0 })
                return false;
        }
        offset_ += count;
        return true;
    }

    bool done() const noexcept
    {
        return offset_ == input_.size();
    }

private:
    bool has(std::size_t count) const noexcept
    {
        return offset_ <= input_.size() && count <= input_.size() - offset_;
    }

    std::span<const std::byte> input_;
    std::size_t offset_ {};
};

template <class Id>
bool id_nonzero(const Id& id) noexcept
{
    return std::any_of(id.bytes.begin(), id.bytes.end(), [](std::uint8_t value) { return value != 0; });
}

template <class Id>
bool id_zero(const Id& id) noexcept
{
    return !id_nonzero(id);
}

template <class Id>
bool write_id(Writer& writer, const Id& id) noexcept
{
    return writer.raw(id.bytes.data(), id.bytes.size());
}

template <class Id>
bool read_id(Reader& reader, Id& id) noexcept
{
    return reader.raw(id.bytes.data(), id.bytes.size());
}

template <std::size_t Capacity>
bool valid_text(const std::array<char, Capacity>& text, std::uint8_t size) noexcept
{
    if (size == 0 || size > Capacity)
        return false;
    for (std::size_t i = 0; i != size; ++i)
    {
        const unsigned char value = static_cast<unsigned char>(text[i]);
        if (value < 0x20u || value > 0x7eu)
            return false;
    }
    return true;
}

template <std::size_t Capacity>
bool valid_decoded_text(const std::array<char, Capacity>& text, std::uint8_t size) noexcept
{
    if (!valid_text(text, size))
        return false;
    for (std::size_t i = size; i != Capacity; ++i)
    {
        if (text[i] != '\0')
            return false;
    }
    return true;
}

bool valid_result(ProtocolResultCode code) noexcept
{
    switch (code)
    {
    case ProtocolResultCode::ok:
    case ProtocolResultCode::not_ready:
    case ProtocolResultCode::concurrent_execution:
    case ProtocolResultCode::stale_lease:
    case ProtocolResultCode::stale_fence:
    case ProtocolResultCode::invalid_sequence:
    case ProtocolResultCode::replay:
    case ProtocolResultCode::replay_conflict:
    case ProtocolResultCode::sequence_exhausted:
    case ProtocolResultCode::operation_in_flight:
    case ProtocolResultCode::reconciling:
    case ProtocolResultCode::quarantined:
    case ProtocolResultCode::persistence_failure:
    case ProtocolResultCode::internal_failure:
        return true;
    }
    return false;
}

bool valid_phase(ProtocolAuthorityPhase phase) noexcept
{
    switch (phase)
    {
    case ProtocolAuthorityPhase::ready:
    case ProtocolAuthorityPhase::leased:
    case ProtocolAuthorityPhase::reconciling:
    case ProtocolAuthorityPhase::quarantined:
        return true;
    }
    return false;
}

bool known_kind(ProtocolMessageKind kind) noexcept
{
    switch (kind)
    {
    case ProtocolMessageKind::acquire_request:
    case ProtocolMessageKind::acquire_response:
    case ProtocolMessageKind::execute_test_operation_request:
    case ProtocolMessageKind::execute_test_operation_response:
    case ProtocolMessageKind::release_request:
    case ProtocolMessageKind::release_response:
    case ProtocolMessageKind::query_status_request:
    case ProtocolMessageKind::query_status_response:
        return true;
    }
    return false;
}

std::size_t expected_payload_size(ProtocolMessageKind kind) noexcept
{
    switch (kind)
    {
    case ProtocolMessageKind::acquire_request:
        return acquire_request_payload_size;
    case ProtocolMessageKind::acquire_response:
        return acquire_response_payload_size;
    case ProtocolMessageKind::execute_test_operation_request:
        return execute_request_payload_size;
    case ProtocolMessageKind::execute_test_operation_response:
        return execute_response_payload_size;
    case ProtocolMessageKind::release_request:
        return release_request_payload_size;
    case ProtocolMessageKind::release_response:
        return release_response_payload_size;
    case ProtocolMessageKind::query_status_request:
        return query_request_payload_size;
    case ProtocolMessageKind::query_status_response:
        return query_response_payload_size;
    }
    return protocol_max_payload_size + 1;
}

bool write_header(Writer& writer, ProtocolMessageKind kind, std::size_t payload_size) noexcept
{
    if (payload_size > std::numeric_limits<std::uint32_t>::max())
        return false;
    return writer.raw(magic.data(), magic.size()) && writer.u16(protocol_version) &&
           writer.u16(static_cast<std::uint16_t>(kind)) && writer.u32(static_cast<std::uint32_t>(payload_size)) &&
           writer.u32(0);
}

ProtocolCodecResult begin_encode(ProtocolMessageKind kind, std::size_t payload_size, ProtocolFrame& candidate,
                                 Writer& writer) noexcept
{
    if (payload_size > protocol_max_payload_size || protocol_header_size + payload_size > candidate.bytes.size())
        return { ProtocolCodecError::internal_overflow };
    if (!write_header(writer, kind, payload_size))
        return { ProtocolCodecError::internal_overflow };
    return {};
}

ProtocolCodecResult finish_encode(std::size_t expected_size, ProtocolFrame& candidate, Writer& writer,
                                  ProtocolFrame& output) noexcept
{
    if (writer.size() != protocol_header_size + expected_size || writer.size() > std::numeric_limits<std::uint16_t>::max())
        return { ProtocolCodecError::internal_overflow };
    candidate.size = static_cast<std::uint16_t>(writer.size());
    output = candidate;
    return {};
}

ProtocolCodecResult require_frame(const ProtocolFrameView& frame, ProtocolMessageKind kind,
                                  std::size_t payload_size) noexcept
{
    if (frame.kind != kind)
        return { ProtocolCodecError::wrong_message_kind };
    if (frame.payload.size() != payload_size)
        return { ProtocolCodecError::payload_size_mismatch };
    return {};
}
} // namespace

ProtocolCodecResult inspect_protocol_frame(std::span<const std::byte> bytes, ProtocolFrameView& output) noexcept
{
    if (bytes.size() < protocol_header_size)
        return { ProtocolCodecError::frame_too_small };
    if (bytes.size() > protocol_max_frame_size)
        return { ProtocolCodecError::frame_too_large };

    Reader reader(bytes.first(protocol_header_size));
    std::array<std::uint8_t, 4> read_magic {};
    std::uint16_t version = 0;
    std::uint16_t raw_kind = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t reserved = 0;
    if (!reader.raw(read_magic.data(), read_magic.size()) || !reader.u16(version) || !reader.u16(raw_kind) ||
        !reader.u32(payload_size) || !reader.u32(reserved) || !reader.done())
        return { ProtocolCodecError::frame_too_small };
    if (read_magic != magic)
        return { ProtocolCodecError::bad_magic };
    if (version != protocol_version)
        return { ProtocolCodecError::unsupported_version };
    const auto kind = static_cast<ProtocolMessageKind>(raw_kind);
    if (!known_kind(kind))
        return { ProtocolCodecError::unknown_message_kind };
    if (reserved != 0)
        return { ProtocolCodecError::nonzero_reserved };
    if (payload_size > protocol_max_payload_size || payload_size != expected_payload_size(kind) ||
        bytes.size() != protocol_header_size + payload_size)
        return { ProtocolCodecError::payload_size_mismatch };

    ProtocolFrameView candidate { kind, bytes.subspan(protocol_header_size, payload_size) };
    output = candidate;
    return {};
}

ProtocolCodecResult encode_protocol_message(const AcquireRequestMessage& message, ProtocolFrame& output) noexcept
{
    if (!id_nonzero(message.execution) || !valid_text(message.scope, message.scope_size) ||
        !valid_text(message.intent, message.intent_size))
        return { ProtocolCodecError::invalid_field };

    ProtocolFrame candidate {};
    Writer writer(candidate.bytes);
    if (!begin_encode(ProtocolMessageKind::acquire_request, acquire_request_payload_size, candidate, writer).ok() ||
        !write_id(writer, message.execution) || !writer.u8(message.scope_size) || !writer.u8(message.intent_size) ||
        !writer.u16(0) || !writer.text(message.scope.data(), message.scope_size, message.scope.size()) ||
        !writer.text(message.intent.data(), message.intent_size, message.intent.size()))
        return { ProtocolCodecError::internal_overflow };
    return finish_encode(acquire_request_payload_size, candidate, writer, output);
}

ProtocolCodecResult encode_protocol_message(const AcquireResponseMessage& message, ProtocolFrame& output) noexcept
{
    if (!valid_result(message.result))
        return { ProtocolCodecError::invalid_field };
    const bool success = message.result == ProtocolResultCode::ok;
    if ((success && (!id_nonzero(message.execution) || !id_nonzero(message.lease) || message.generation.value == 0 ||
                     message.epoch.value == 0)) ||
        (!success && (!id_zero(message.execution) || !id_zero(message.lease) || message.generation.value != 0 ||
                      message.epoch.value != 0)))
        return { ProtocolCodecError::invalid_field };

    ProtocolFrame candidate {};
    Writer writer(candidate.bytes);
    if (!begin_encode(ProtocolMessageKind::acquire_response, acquire_response_payload_size, candidate, writer).ok() ||
        !writer.u16(static_cast<std::uint16_t>(message.result)) || !writer.u16(0) ||
        !write_id(writer, message.execution) || !write_id(writer, message.lease) || !writer.u64(message.generation.value) ||
        !writer.u64(message.epoch.value))
        return { ProtocolCodecError::internal_overflow };
    return finish_encode(acquire_response_payload_size, candidate, writer, output);
}

ProtocolCodecResult encode_protocol_message(const ExecuteTestOperationRequestMessage& message,
                                             ProtocolFrame& output) noexcept
{
    if (!id_nonzero(message.execution) || !id_nonzero(message.lease) || !id_nonzero(message.operation) ||
        message.epoch.value == 0 || message.sequence.value == 0 || message.operation_code != TestOperationCode::no_op)
        return { ProtocolCodecError::invalid_field };

    ProtocolFrame candidate {};
    Writer writer(candidate.bytes);
    if (!begin_encode(ProtocolMessageKind::execute_test_operation_request, execute_request_payload_size, candidate,
                      writer)
             .ok() ||
        !write_id(writer, message.execution) || !write_id(writer, message.lease) || !write_id(writer, message.operation) ||
        !writer.u64(message.epoch.value) || !writer.u64(message.sequence.value) ||
        !writer.u16(static_cast<std::uint16_t>(message.operation_code)) || !writer.zeroes(6))
        return { ProtocolCodecError::internal_overflow };
    return finish_encode(execute_request_payload_size, candidate, writer, output);
}

ProtocolCodecResult encode_protocol_message(const ExecuteTestOperationResponseMessage& message,
                                             ProtocolFrame& output) noexcept
{
    if (!valid_result(message.result) || (message.result != ProtocolResultCode::ok && message.replayed))
        return { ProtocolCodecError::invalid_field };

    ProtocolFrame candidate {};
    Writer writer(candidate.bytes);
    if (!begin_encode(ProtocolMessageKind::execute_test_operation_response, execute_response_payload_size, candidate,
                      writer)
             .ok() ||
        !writer.u16(static_cast<std::uint16_t>(message.result)) || !writer.u8(message.replayed ? 1u : 0u) ||
        !writer.zeroes(5))
        return { ProtocolCodecError::internal_overflow };
    return finish_encode(execute_response_payload_size, candidate, writer, output);
}

ProtocolCodecResult encode_protocol_message(const ReleaseRequestMessage& message, ProtocolFrame& output) noexcept
{
    if (!id_nonzero(message.execution) || !id_nonzero(message.lease) || message.epoch.value == 0)
        return { ProtocolCodecError::invalid_field };

    ProtocolFrame candidate {};
    Writer writer(candidate.bytes);
    if (!begin_encode(ProtocolMessageKind::release_request, release_request_payload_size, candidate, writer).ok() ||
        !write_id(writer, message.execution) || !write_id(writer, message.lease) || !writer.u64(message.epoch.value))
        return { ProtocolCodecError::internal_overflow };
    return finish_encode(release_request_payload_size, candidate, writer, output);
}

ProtocolCodecResult encode_protocol_message(const ReleaseResponseMessage& message, ProtocolFrame& output) noexcept
{
    if (!valid_result(message.result))
        return { ProtocolCodecError::invalid_field };

    ProtocolFrame candidate {};
    Writer writer(candidate.bytes);
    if (!begin_encode(ProtocolMessageKind::release_response, release_response_payload_size, candidate, writer).ok() ||
        !writer.u16(static_cast<std::uint16_t>(message.result)) || !writer.u16(0))
        return { ProtocolCodecError::internal_overflow };
    return finish_encode(release_response_payload_size, candidate, writer, output);
}

ProtocolCodecResult encode_protocol_message(const QueryStatusRequestMessage&, ProtocolFrame& output) noexcept
{
    ProtocolFrame candidate {};
    Writer writer(candidate.bytes);
    if (!begin_encode(ProtocolMessageKind::query_status_request, query_request_payload_size, candidate, writer).ok())
        return { ProtocolCodecError::internal_overflow };
    return finish_encode(query_request_payload_size, candidate, writer, output);
}

ProtocolCodecResult encode_protocol_message(const QueryStatusResponseMessage& message, ProtocolFrame& output) noexcept
{
    if (!valid_result(message.result))
        return { ProtocolCodecError::invalid_field };
    const bool success = message.result == ProtocolResultCode::ok;
    if ((success && (!message.started || !valid_phase(message.phase) || message.generation.value == 0 ||
                     message.epoch.value == 0 || message.next_sequence.value == 0)) ||
        (!success && (message.started || message.persistence_fault || message.generation.value != 0 ||
                      message.epoch.value != 0 || message.next_sequence.value != 0)))
        return { ProtocolCodecError::invalid_field };

    ProtocolFrame candidate {};
    Writer writer(candidate.bytes);
    if (!begin_encode(ProtocolMessageKind::query_status_response, query_response_payload_size, candidate, writer).ok() ||
        !writer.u16(static_cast<std::uint16_t>(message.result)) || !writer.u8(message.started ? 1u : 0u) ||
        !writer.u8(message.persistence_fault ? 1u : 0u) ||
        !writer.u8(success ? static_cast<std::uint8_t>(message.phase) : 0u) || !writer.zeroes(3) ||
        !writer.u64(message.generation.value) || !writer.u64(message.epoch.value) ||
        !writer.u64(message.next_sequence.value))
        return { ProtocolCodecError::internal_overflow };
    return finish_encode(query_response_payload_size, candidate, writer, output);
}

ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, AcquireRequestMessage& output) noexcept
{
    const auto required = require_frame(frame, ProtocolMessageKind::acquire_request, acquire_request_payload_size);
    if (!required.ok())
        return required;

    AcquireRequestMessage candidate {};
    Reader reader(frame.payload);
    std::uint16_t reserved = 0;
    if (!read_id(reader, candidate.execution) || !reader.u8(candidate.scope_size) || !reader.u8(candidate.intent_size) ||
        !reader.u16(reserved) || !reader.text(candidate.scope.data(), candidate.scope.size()) ||
        !reader.text(candidate.intent.data(), candidate.intent.size()) || !reader.done())
        return { ProtocolCodecError::payload_size_mismatch };
    if (reserved != 0)
        return { ProtocolCodecError::nonzero_reserved };
    if (!id_nonzero(candidate.execution) || !valid_decoded_text(candidate.scope, candidate.scope_size) ||
        !valid_decoded_text(candidate.intent, candidate.intent_size))
        return { ProtocolCodecError::invalid_field };
    output = candidate;
    return {};
}

ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, AcquireResponseMessage& output) noexcept
{
    const auto required = require_frame(frame, ProtocolMessageKind::acquire_response, acquire_response_payload_size);
    if (!required.ok())
        return required;

    AcquireResponseMessage candidate {};
    Reader reader(frame.payload);
    std::uint16_t raw_result = 0;
    std::uint16_t reserved = 0;
    if (!reader.u16(raw_result) || !reader.u16(reserved) || !read_id(reader, candidate.execution) ||
        !read_id(reader, candidate.lease) || !reader.u64(candidate.generation.value) || !reader.u64(candidate.epoch.value) ||
        !reader.done())
        return { ProtocolCodecError::payload_size_mismatch };
    if (reserved != 0)
        return { ProtocolCodecError::nonzero_reserved };
    candidate.result = static_cast<ProtocolResultCode>(raw_result);
    if (!valid_result(candidate.result))
        return { ProtocolCodecError::invalid_field };
    const bool success = candidate.result == ProtocolResultCode::ok;
    if ((success && (!id_nonzero(candidate.execution) || !id_nonzero(candidate.lease) || candidate.generation.value == 0 ||
                     candidate.epoch.value == 0)) ||
        (!success && (!id_zero(candidate.execution) || !id_zero(candidate.lease) || candidate.generation.value != 0 ||
                      candidate.epoch.value != 0)))
        return { ProtocolCodecError::invalid_field };
    output = candidate;
    return {};
}

ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame,
                                             ExecuteTestOperationRequestMessage& output) noexcept
{
    const auto required = require_frame(frame, ProtocolMessageKind::execute_test_operation_request,
                                        execute_request_payload_size);
    if (!required.ok())
        return required;

    ExecuteTestOperationRequestMessage candidate {};
    Reader reader(frame.payload);
    std::uint16_t raw_operation = 0;
    if (!read_id(reader, candidate.execution) || !read_id(reader, candidate.lease) ||
        !read_id(reader, candidate.operation) || !reader.u64(candidate.epoch.value) ||
        !reader.u64(candidate.sequence.value) || !reader.u16(raw_operation))
        return { ProtocolCodecError::payload_size_mismatch };
    if (!reader.zeroes(6))
        return { ProtocolCodecError::nonzero_reserved };
    if (!reader.done())
        return { ProtocolCodecError::payload_size_mismatch };
    candidate.operation_code = static_cast<TestOperationCode>(raw_operation);
    if (!id_nonzero(candidate.execution) || !id_nonzero(candidate.lease) || !id_nonzero(candidate.operation) ||
        candidate.epoch.value == 0 || candidate.sequence.value == 0 || candidate.operation_code != TestOperationCode::no_op)
        return { ProtocolCodecError::invalid_field };
    output = candidate;
    return {};
}

ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame,
                                             ExecuteTestOperationResponseMessage& output) noexcept
{
    const auto required = require_frame(frame, ProtocolMessageKind::execute_test_operation_response,
                                        execute_response_payload_size);
    if (!required.ok())
        return required;

    ExecuteTestOperationResponseMessage candidate {};
    Reader reader(frame.payload);
    std::uint16_t raw_result = 0;
    std::uint8_t replayed = 0;
    if (!reader.u16(raw_result) || !reader.u8(replayed))
        return { ProtocolCodecError::payload_size_mismatch };
    if (replayed > 1)
        return { ProtocolCodecError::invalid_field };
    if (!reader.zeroes(5))
        return { ProtocolCodecError::nonzero_reserved };
    if (!reader.done())
        return { ProtocolCodecError::payload_size_mismatch };
    candidate.result = static_cast<ProtocolResultCode>(raw_result);
    candidate.replayed = replayed != 0;
    if (!valid_result(candidate.result) || (candidate.result != ProtocolResultCode::ok && candidate.replayed))
        return { ProtocolCodecError::invalid_field };
    output = candidate;
    return {};
}

ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, ReleaseRequestMessage& output) noexcept
{
    const auto required = require_frame(frame, ProtocolMessageKind::release_request, release_request_payload_size);
    if (!required.ok())
        return required;

    ReleaseRequestMessage candidate {};
    Reader reader(frame.payload);
    if (!read_id(reader, candidate.execution) || !read_id(reader, candidate.lease) || !reader.u64(candidate.epoch.value) ||
        !reader.done())
        return { ProtocolCodecError::payload_size_mismatch };
    if (!id_nonzero(candidate.execution) || !id_nonzero(candidate.lease) || candidate.epoch.value == 0)
        return { ProtocolCodecError::invalid_field };
    output = candidate;
    return {};
}

ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, ReleaseResponseMessage& output) noexcept
{
    const auto required = require_frame(frame, ProtocolMessageKind::release_response, release_response_payload_size);
    if (!required.ok())
        return required;

    ReleaseResponseMessage candidate {};
    Reader reader(frame.payload);
    std::uint16_t raw_result = 0;
    std::uint16_t reserved = 0;
    if (!reader.u16(raw_result) || !reader.u16(reserved) || !reader.done())
        return { ProtocolCodecError::payload_size_mismatch };
    if (reserved != 0)
        return { ProtocolCodecError::nonzero_reserved };
    candidate.result = static_cast<ProtocolResultCode>(raw_result);
    if (!valid_result(candidate.result))
        return { ProtocolCodecError::invalid_field };
    output = candidate;
    return {};
}

ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, QueryStatusRequestMessage& output) noexcept
{
    const auto required = require_frame(frame, ProtocolMessageKind::query_status_request, query_request_payload_size);
    if (!required.ok())
        return required;
    output = {};
    return {};
}

ProtocolCodecResult decode_protocol_message(const ProtocolFrameView& frame, QueryStatusResponseMessage& output) noexcept
{
    const auto required = require_frame(frame, ProtocolMessageKind::query_status_response, query_response_payload_size);
    if (!required.ok())
        return required;

    QueryStatusResponseMessage candidate {};
    Reader reader(frame.payload);
    std::uint16_t raw_result = 0;
    std::uint8_t started = 0;
    std::uint8_t persistence_fault = 0;
    std::uint8_t raw_phase = 0;
    if (!reader.u16(raw_result) || !reader.u8(started) || !reader.u8(persistence_fault) || !reader.u8(raw_phase))
        return { ProtocolCodecError::payload_size_mismatch };
    if (started > 1 || persistence_fault > 1)
        return { ProtocolCodecError::invalid_field };
    if (!reader.zeroes(3))
        return { ProtocolCodecError::nonzero_reserved };
    if (!reader.u64(candidate.generation.value) || !reader.u64(candidate.epoch.value) ||
        !reader.u64(candidate.next_sequence.value) || !reader.done())
        return { ProtocolCodecError::payload_size_mismatch };
    candidate.result = static_cast<ProtocolResultCode>(raw_result);
    candidate.started = started != 0;
    candidate.persistence_fault = persistence_fault != 0;
    candidate.phase = raw_phase == 0 ? ProtocolAuthorityPhase::ready : static_cast<ProtocolAuthorityPhase>(raw_phase);
    if (!valid_result(candidate.result))
        return { ProtocolCodecError::invalid_field };
    const bool success = candidate.result == ProtocolResultCode::ok;
    if ((success && (!candidate.started || raw_phase == 0 || !valid_phase(candidate.phase) ||
                     candidate.generation.value == 0 || candidate.epoch.value == 0 || candidate.next_sequence.value == 0)) ||
        (!success && (candidate.started || candidate.persistence_fault || raw_phase != 0 || candidate.generation.value != 0 ||
                      candidate.epoch.value != 0 || candidate.next_sequence.value != 0)))
        return { ProtocolCodecError::invalid_field };
    output = candidate;
    return {};
}
} // namespace qiven::host
