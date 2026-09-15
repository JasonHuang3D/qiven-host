#include "qiven/host/bounded_journal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

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
constexpr std::uint64_t segment_magic = 0x31304745534A5651ULL; // "QVJSEG01" little-endian
constexpr std::uint64_t record_magic = 0x31304345524A5651ULL;  // "QVJREC01" little-endian
constexpr std::uint32_t format_version = 1;
constexpr std::size_t segment_header_size = 80;
constexpr std::size_t record_size = 272;
constexpr std::size_t segment_crc_offset = 72;
constexpr std::size_t record_hash_offset = 224;
constexpr std::size_t record_crc_offset = 256;
constexpr std::uint64_t segment_maximum_size =
    segment_header_size + static_cast<std::uint64_t>(BoundedJournal::records_per_segment) * record_size;
static_assert(BoundedJournal::maximum_retained_bytes == segment_maximum_size * 2);

using SegmentHeaderBytes = std::array<std::uint8_t, segment_header_size>;
using RecordBytes = std::array<std::uint8_t, record_size>;

struct SegmentHeader final
{
    std::uint64_t generation {};
    std::uint64_t first_index {};
    JournalDigest anchor {};

    friend constexpr bool operator==(const SegmentHeader&, const SegmentHeader&) = default;
};

enum class SegmentState : std::uint8_t
{
    missing,
    valid,
    invalid,
    io_failure,
    crypto_failure
};

struct SegmentScan final
{
    SegmentState state { SegmentState::missing };
    SegmentHeader header {};
    std::uint32_t count {};
    JournalHead head {};
    std::uint32_t native_error {};
};

struct RootRead final
{
    bool exists {};
    JournalResult status {};
};

struct JournalScan final
{
    JournalResult status {};
    JournalHead head {};
    bool active_a { true };
    SegmentHeader active_header {};
    std::uint32_t active_count {};

    bool ok() const noexcept
    {
        return status.ok();
    }
};

template <class Bytes>
void put_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) noexcept
{
    for (unsigned i = 0; i != 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

template <class Bytes>
void put_u64(Bytes& bytes, std::size_t offset, std::uint64_t value) noexcept
{
    for (unsigned i = 0; i != 8; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

template <class Bytes>
std::uint32_t get_u32(const Bytes& bytes, std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i)
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8);
    return value;
}

template <class Bytes>
std::uint64_t get_u64(const Bytes& bytes, std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
    return value;
}

template <class Bytes>
void put_digest(Bytes& bytes, std::size_t offset, const JournalDigest& digest) noexcept
{
    for (std::size_t i = 0; i != digest.size(); ++i)
        bytes[offset + i] = digest[i];
}

template <class Bytes>
JournalDigest get_digest(const Bytes& bytes, std::size_t offset) noexcept
{
    JournalDigest digest {};
    for (std::size_t i = 0; i != digest.size(); ++i)
        digest[i] = bytes[offset + i];
    return digest;
}

bool zero_digest(const JournalDigest& digest) noexcept
{
    for (const auto byte : digest)
    {
        if (byte != 0)
            return false;
    }
    return true;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i != size; ++i)
    {
        crc ^= data[i];
        for (unsigned bit = 0; bit != 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) != 0u ? 0xedb88320u : 0u);
    }
    return ~crc;
}

bool valid_event(JournalEvent event) noexcept
{
    return static_cast<std::uint32_t>(event) <= static_cast<std::uint32_t>(JournalEvent::protocol_rejected);
}

bool valid_outcome(JournalOutcome outcome) noexcept
{
    return static_cast<std::uint32_t>(outcome) <= static_cast<std::uint32_t>(JournalOutcome::failed);
}

RootRead read_root(const std::filesystem::path& root) noexcept
{
    std::error_code error;
    const bool exists = std::filesystem::exists(root, error);
    if (error)
        return { false, { JournalError::io_failure, static_cast<std::uint32_t>(error.value()), {} } };
    if (!exists)
        return {};

    const bool directory = std::filesystem::is_directory(root, error);
    if (error || !directory)
        return { true, { JournalError::io_failure, static_cast<std::uint32_t>(error.value()), {} } };
    return { true, {} };
}

SegmentHeaderBytes encode_header(const SegmentHeader& header) noexcept
{
    SegmentHeaderBytes bytes {};
    put_u64(bytes, 0, segment_magic);
    put_u32(bytes, 8, format_version);
    put_u32(bytes, 12, static_cast<std::uint32_t>(segment_header_size));
    put_u64(bytes, 16, header.generation);
    put_u64(bytes, 24, header.first_index);
    put_digest(bytes, 32, header.anchor);
    put_u32(bytes, 64, BoundedJournal::records_per_segment);
    put_u32(bytes, 68, 0);
    put_u32(bytes, segment_crc_offset, crc32(bytes.data(), segment_crc_offset));
    put_u32(bytes, 76, 0);
    return bytes;
}

bool decode_header(const SegmentHeaderBytes& bytes, SegmentHeader& header) noexcept
{
    if (get_u64(bytes, 0) != segment_magic || get_u32(bytes, 8) != format_version ||
        get_u32(bytes, 12) != segment_header_size || get_u32(bytes, 64) != BoundedJournal::records_per_segment ||
        get_u32(bytes, 68) != 0 || get_u32(bytes, 76) != 0 ||
        get_u32(bytes, segment_crc_offset) != crc32(bytes.data(), segment_crc_offset))
        return false;

    header.generation = get_u64(bytes, 16);
    header.first_index = get_u64(bytes, 24);
    header.anchor = get_digest(bytes, 32);
    return header.generation != 0 && header.first_index != 0;
}

#if defined(_WIN32)
bool sha256(const std::uint8_t* data, std::size_t size, JournalDigest& digest, std::uint32_t& native_error) noexcept
{
    if (size > std::numeric_limits<ULONG>::max())
    {
        native_error = ERROR_ARITHMETIC_OVERFLOW;
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0)
    {
        native_error = static_cast<std::uint32_t>(status);
        return false;
    }

    status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data)),
                        static_cast<ULONG>(size), digest.data(), static_cast<ULONG>(digest.size()));
    const NTSTATUS close_status = BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
    {
        native_error = static_cast<std::uint32_t>(status);
        return false;
    }
    if (close_status < 0)
    {
        native_error = static_cast<std::uint32_t>(close_status);
        return false;
    }
    return true;
}

std::uint64_t diagnostic_time() noexcept
{
    FILETIME time {};
    GetSystemTimePreciseAsFileTime(&time);
    ULARGE_INTEGER value {};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

std::uint32_t close_handle(HANDLE handle) noexcept
{
    if (CloseHandle(handle) != 0)
        return 0;
    return GetLastError();
}

bool encode_record(std::uint64_t index, const JournalDigest& previous, const JournalEntry& entry, RecordBytes& bytes,
                   JournalDigest& hash, std::uint32_t& native_error) noexcept
{
    bytes = {};
    put_u64(bytes, 0, record_magic);
    put_u32(bytes, 8, format_version);
    put_u32(bytes, 12, static_cast<std::uint32_t>(record_size));
    put_u64(bytes, 16, index);
    put_u64(bytes, 24, diagnostic_time());
    put_u64(bytes, 32, entry.generation.value);
    put_u64(bytes, 40, entry.epoch.value);
    put_u64(bytes, 48, entry.sequence.value);
    put_u32(bytes, 56, static_cast<std::uint32_t>(entry.event));
    put_u32(bytes, 60, static_cast<std::uint32_t>(entry.outcome));
    put_digest(bytes, 64, entry.execution_digest);
    put_digest(bytes, 96, entry.lease_digest);
    put_digest(bytes, 128, entry.session_digest);
    put_digest(bytes, 160, entry.operation_digest);
    put_digest(bytes, 192, previous);
    if (!sha256(bytes.data(), record_hash_offset, hash, native_error))
        return false;
    put_digest(bytes, record_hash_offset, hash);
    put_u32(bytes, record_crc_offset, crc32(bytes.data(), record_crc_offset));
    put_u32(bytes, 260, 0);
    put_u32(bytes, 264, 0);
    put_u32(bytes, 268, 0);
    return true;
}

bool decode_record(const RecordBytes& bytes, std::uint64_t expected_index, const JournalDigest& expected_previous,
                   JournalHead& head, std::uint32_t& native_error, bool& crypto_failure) noexcept
{
    if (get_u64(bytes, 0) != record_magic || get_u32(bytes, 8) != format_version || get_u32(bytes, 12) != record_size ||
        get_u32(bytes, 260) != 0 || get_u32(bytes, 264) != 0 || get_u32(bytes, 268) != 0 ||
        get_u32(bytes, record_crc_offset) != crc32(bytes.data(), record_crc_offset))
        return false;
    if (get_u64(bytes, 16) != expected_index || get_digest(bytes, 192) != expected_previous)
        return false;

    const auto raw_event = get_u32(bytes, 56);
    const auto raw_outcome = get_u32(bytes, 60);
    if (raw_event > static_cast<std::uint32_t>(JournalEvent::protocol_rejected) ||
        raw_outcome > static_cast<std::uint32_t>(JournalOutcome::failed))
        return false;

    JournalDigest calculated {};
    if (!sha256(bytes.data(), record_hash_offset, calculated, native_error))
    {
        crypto_failure = true;
        return false;
    }
    const JournalDigest stored = get_digest(bytes, record_hash_offset);
    if (stored != calculated)
        return false;

    head = { expected_index, stored };
    return true;
}

SegmentScan scan_segment(const std::filesystem::path& path) noexcept
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        return { SegmentState::io_failure, {}, 0, {}, error };
    }

    LARGE_INTEGER file_size {};
    if (GetFileSizeEx(handle, &file_size) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { SegmentState::io_failure, {}, 0, {}, error };
    }
    if (file_size.QuadPart < static_cast<LONGLONG>(segment_header_size) ||
        file_size.QuadPart > static_cast<LONGLONG>(segment_maximum_size) ||
        (file_size.QuadPart - static_cast<LONGLONG>(segment_header_size)) % static_cast<LONGLONG>(record_size) != 0)
    {
        close_handle(handle);
        return { SegmentState::invalid };
    }

    SegmentHeaderBytes header_bytes {};
    constexpr DWORD expected_header_size = static_cast<DWORD>(segment_header_size);
    DWORD bytes_read = 0;
    if (ReadFile(handle, header_bytes.data(), expected_header_size, &bytes_read, nullptr) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { SegmentState::io_failure, {}, 0, {}, error };
    }
    if (bytes_read != expected_header_size)
    {
        close_handle(handle);
        return { SegmentState::invalid };
    }

    SegmentHeader header {};
    if (!decode_header(header_bytes, header))
    {
        close_handle(handle);
        return { SegmentState::invalid };
    }

    const auto count = static_cast<std::uint32_t>((file_size.QuadPart - static_cast<LONGLONG>(segment_header_size)) /
                                                   static_cast<LONGLONG>(record_size));
    JournalHead head { header.first_index - 1, header.anchor };
    JournalDigest previous = header.anchor;
    constexpr DWORD expected_record_size = static_cast<DWORD>(record_size);
    for (std::uint32_t i = 0; i != count; ++i)
    {
        if (header.first_index > std::numeric_limits<std::uint64_t>::max() - i)
        {
            close_handle(handle);
            return { SegmentState::invalid };
        }
        const std::uint64_t expected_index = header.first_index + i;
        RecordBytes record {};
        bytes_read = 0;
        if (ReadFile(handle, record.data(), expected_record_size, &bytes_read, nullptr) == 0)
        {
            const auto error = GetLastError();
            close_handle(handle);
            return { SegmentState::io_failure, {}, 0, {}, error };
        }
        if (bytes_read != expected_record_size)
        {
            close_handle(handle);
            return { SegmentState::invalid };
        }
        std::uint32_t native_error = 0;
        bool crypto_failure = false;
        if (!decode_record(record, expected_index, previous, head, native_error, crypto_failure))
        {
            close_handle(handle);
            return { crypto_failure ? SegmentState::crypto_failure : SegmentState::invalid, {}, 0, {}, native_error };
        }
        previous = head.digest;
    }

    const auto close_error = close_handle(handle);
    if (close_error != 0)
        return { SegmentState::io_failure, {}, 0, {}, close_error };
    return { SegmentState::valid, header, count, head, 0 };
}

JournalResult write_header(const std::filesystem::path& path, const SegmentHeader& header) noexcept
{
    const SegmentHeaderBytes bytes = encode_header(header);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return { JournalError::io_failure, GetLastError(), {} };

    constexpr DWORD expected_size = static_cast<DWORD>(segment_header_size);
    DWORD written = 0;
    if (WriteFile(handle, bytes.data(), expected_size, &written, nullptr) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { JournalError::io_failure, error, {} };
    }
    if (written != expected_size)
    {
        close_handle(handle);
        return { JournalError::io_failure, 0, {} };
    }
    if (FlushFileBuffers(handle) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { JournalError::io_failure, error, {} };
    }
    const auto close_error = close_handle(handle);
    if (close_error != 0)
        return { JournalError::io_failure, close_error, {} };

    const SegmentScan verification = scan_segment(path);
    if (verification.state == SegmentState::io_failure)
        return { JournalError::io_failure, verification.native_error, {} };
    if (verification.state == SegmentState::crypto_failure)
        return { JournalError::crypto_failure, verification.native_error, {} };
    if (verification.state != SegmentState::valid || verification.count != 0 || !(verification.header == header))
        return { JournalError::corrupt, 0, {} };
    return { JournalError::none, 0, verification.head };
}

JournalResult append_record(const std::filesystem::path& path, const SegmentHeader& header, std::uint32_t expected_count,
                            std::uint64_t index, const JournalDigest& previous, const JournalEntry& entry) noexcept
{
    RecordBytes bytes {};
    JournalDigest hash {};
    std::uint32_t native_error = 0;
    if (!encode_record(index, previous, entry, bytes, hash, native_error))
        return { JournalError::crypto_failure, native_error, {} };

    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return { JournalError::io_failure, GetLastError(), {} };

    LARGE_INTEGER size {};
    if (GetFileSizeEx(handle, &size) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { JournalError::io_failure, error, {} };
    }
    const auto expected_file_size = static_cast<LONGLONG>(segment_header_size) +
                                    static_cast<LONGLONG>(expected_count) * static_cast<LONGLONG>(record_size);
    if (size.QuadPart != expected_file_size)
    {
        close_handle(handle);
        return { JournalError::corrupt, 0, {} };
    }

    LARGE_INTEGER end {};
    end.QuadPart = 0;
    if (SetFilePointerEx(handle, end, nullptr, FILE_END) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { JournalError::io_failure, error, {} };
    }

    constexpr DWORD expected_size = static_cast<DWORD>(record_size);
    DWORD written = 0;
    if (WriteFile(handle, bytes.data(), expected_size, &written, nullptr) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { JournalError::io_failure, error, {} };
    }
    if (written != expected_size)
    {
        close_handle(handle);
        return { JournalError::io_failure, 0, {} };
    }
    if (FlushFileBuffers(handle) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { JournalError::io_failure, error, {} };
    }
    const auto close_error = close_handle(handle);
    if (close_error != 0)
        return { JournalError::io_failure, close_error, {} };

    const SegmentScan verification = scan_segment(path);
    if (verification.state == SegmentState::io_failure)
        return { JournalError::io_failure, verification.native_error, {} };
    if (verification.state == SegmentState::crypto_failure)
        return { JournalError::crypto_failure, verification.native_error, {} };
    if (verification.state != SegmentState::valid || !(verification.header == header) ||
        verification.count != expected_count + 1 || verification.head.index != index || verification.head.digest != hash)
        return { JournalError::corrupt, 0, {} };
    return { JournalError::none, 0, verification.head };
}
#else
SegmentScan scan_segment(const std::filesystem::path&) noexcept
{
    return { SegmentState::io_failure };
}

JournalResult write_header(const std::filesystem::path&, const SegmentHeader&) noexcept
{
    return { JournalError::io_failure, 0, {} };
}

JournalResult append_record(const std::filesystem::path&, const SegmentHeader&, std::uint32_t, std::uint64_t,
                            const JournalDigest&, const JournalEntry&) noexcept
{
    return { JournalError::io_failure, 0, {} };
}
#endif

JournalResult segment_error(const SegmentScan& scan) noexcept
{
    if (scan.state == SegmentState::io_failure)
        return { JournalError::io_failure, scan.native_error, {} };
    if (scan.state == SegmentState::crypto_failure)
        return { JournalError::crypto_failure, scan.native_error, {} };
    return { JournalError::corrupt, 0, {} };
}

JournalScan scan_journal(const std::filesystem::path& root) noexcept
{
    const RootRead root_read = read_root(root);
    if (!root_read.status.ok())
        return { root_read.status };
    if (!root_read.exists)
        return { { JournalError::not_initialized, 0, {} } };

    const SegmentScan a = scan_segment(root / "journal.a");
    const SegmentScan b = scan_segment(root / "journal.b");

    if (a.state == SegmentState::io_failure || a.state == SegmentState::crypto_failure)
        return { segment_error(a) };
    if (b.state == SegmentState::io_failure || b.state == SegmentState::crypto_failure)
        return { segment_error(b) };
    if (a.state == SegmentState::invalid || b.state == SegmentState::invalid)
        return { { JournalError::corrupt, 0, {} } };

    const bool a_valid = a.state == SegmentState::valid;
    const bool b_valid = b.state == SegmentState::valid;
    if (!a_valid && !b_valid)
        return { { JournalError::corrupt, 0, {} } };

    if (a_valid != b_valid)
    {
        const SegmentScan& only = a_valid ? a : b;
        if (only.header.generation != 1 || only.header.first_index != 1 || !zero_digest(only.header.anchor))
            return { { JournalError::corrupt, 0, {} } };
        return { {}, only.head, a_valid, only.header, only.count };
    }

    const bool a_newer = a.header.generation > b.header.generation;
    const SegmentScan& newer = a_newer ? a : b;
    const SegmentScan& older = a_newer ? b : a;
    if (newer.header.generation == older.header.generation || older.header.generation == std::numeric_limits<std::uint64_t>::max() ||
        newer.header.generation != older.header.generation + 1 || older.count != BoundedJournal::records_per_segment ||
        older.head.index == std::numeric_limits<std::uint64_t>::max() || newer.header.first_index != older.head.index + 1 ||
        newer.header.anchor != older.head.digest)
        return { { JournalError::corrupt, 0, {} } };

    return { {}, newer.head, a_newer, newer.header, newer.count };
}
} // namespace

BoundedJournal::BoundedJournal(std::filesystem::path root) : root_(std::move(root))
{
}

JournalResult BoundedJournal::initialize() noexcept
{
    try
    {
        const JournalScan existing = scan_journal(root_);
        if (existing.ok())
            return { JournalError::already_initialized, 0, existing.head };
        if (existing.status.error != JournalError::not_initialized)
            return existing.status;

        std::error_code error;
        std::filesystem::create_directories(root_, error);
        if (error)
            return { JournalError::io_failure, static_cast<std::uint32_t>(error.value()), {} };

        const SegmentHeader initial { 1, 1, {} };
        const JournalResult written = write_header(root_ / "journal.a", initial);
        if (!written.ok())
            return written;

        const JournalScan verified = scan_journal(root_);
        if (!verified.ok())
            return verified.status;
        return { JournalError::none, 0, verified.head };
    }
    catch (...)
    {
        return { JournalError::io_failure, 0, {} };
    }
}

JournalResult BoundedJournal::head() const noexcept
{
    try
    {
        const JournalScan scan = scan_journal(root_);
        if (!scan.ok())
            return scan.status;
        return { JournalError::none, 0, scan.head };
    }
    catch (...)
    {
        return { JournalError::io_failure, 0, {} };
    }
}

JournalResult BoundedJournal::append(const JournalEntry& entry) noexcept
{
    if (!valid_event(entry.event) || !valid_outcome(entry.outcome))
        return { JournalError::invalid_value, 0, {} };

    try
    {
        JournalScan scan = scan_journal(root_);
        if (!scan.ok())
            return scan.status;
        if (scan.head.index == std::numeric_limits<std::uint64_t>::max())
            return { JournalError::index_exhausted, 0, scan.head };

        const std::uint64_t next_index = scan.head.index + 1;
        std::filesystem::path active = root_ / (scan.active_a ? "journal.a" : "journal.b");

        if (scan.active_count == records_per_segment)
        {
            if (scan.active_header.generation == std::numeric_limits<std::uint64_t>::max())
                return { JournalError::segment_generation_exhausted, 0, scan.head };

            scan.active_a = !scan.active_a;
            scan.active_header = { scan.active_header.generation + 1, next_index, scan.head.digest };
            scan.active_count = 0;
            active = root_ / (scan.active_a ? "journal.a" : "journal.b");
            const JournalResult rotated = write_header(active, scan.active_header);
            if (!rotated.ok())
                return rotated;
        }

        const JournalResult appended =
            append_record(active, scan.active_header, scan.active_count, next_index, scan.head.digest, entry);
        if (!appended.ok())
            return appended;

        const JournalScan verified = scan_journal(root_);
        if (!verified.ok())
            return verified.status;
        if (verified.head != appended.head)
            return { JournalError::corrupt, 0, {} };
        return { JournalError::none, 0, verified.head };
    }
    catch (...)
    {
        return { JournalError::io_failure, 0, {} };
    }
}
} // namespace qiven::host
