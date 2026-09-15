#include "qiven/host/fence_store.hpp"

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
#endif

namespace qiven::host
{
namespace
{
constexpr std::uint64_t record_magic = 0x3145434E45465651ULL; // "QVFENCE1" little-endian
constexpr std::uint32_t format_version = 1;
constexpr std::size_t record_size = 56;
constexpr std::size_t checksum_offset = 48;

using Record = std::array<std::uint8_t, record_size>;

enum class SlotState : std::uint8_t
{
    missing,
    valid,
    invalid,
    io_failure
};

struct SlotRead final
{
    SlotState state { SlotState::missing };
    std::uint64_t revision {};
    DurableAuthority value {};
    std::uint32_t native_error {};
};

struct RootRead final
{
    bool exists {};
    FenceStoreResult status {};
};

struct Selection final
{
    FenceStoreResult status;
    std::uint64_t revision {};
    DurableAuthority value {};

    bool ok() const noexcept
    {
        return status.ok();
    }
};

void put_u32(Record& record, std::size_t offset, std::uint32_t value) noexcept
{
    for (unsigned i = 0; i != 4; ++i)
        record[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void put_u64(Record& record, std::size_t offset, std::uint64_t value) noexcept
{
    for (unsigned i = 0; i != 8; ++i)
        record[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint32_t get_u32(const Record& record, std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i)
        value |= static_cast<std::uint32_t>(record[offset + i]) << (i * 8);
    return value;
}

std::uint64_t get_u64(const Record& record, std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i)
        value |= static_cast<std::uint64_t>(record[offset + i]) << (i * 8);
    return value;
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

bool valid_phase(AuthorityPhase phase) noexcept
{
    switch (phase)
    {
    case AuthorityPhase::ready:
    case AuthorityPhase::leased:
    case AuthorityPhase::reconciling:
    case AuthorityPhase::quarantined:
        return true;
    }
    return false;
}

Record encode(std::uint64_t revision, DurableAuthority value) noexcept
{
    Record record {};
    put_u64(record, 0, record_magic);
    put_u32(record, 8, format_version);
    put_u32(record, 12, static_cast<std::uint32_t>(record_size));
    put_u64(record, 16, revision);
    put_u64(record, 24, value.generation.value);
    put_u64(record, 32, value.epoch.value);
    put_u32(record, 40, static_cast<std::uint32_t>(value.phase));
    put_u32(record, 44, 0);
    put_u32(record, checksum_offset, crc32(record.data(), checksum_offset));
    put_u32(record, 52, 0);
    return record;
}

bool decode(const Record& record, std::uint64_t& revision, DurableAuthority& value) noexcept
{
    if (get_u64(record, 0) != record_magic || get_u32(record, 8) != format_version ||
        get_u32(record, 12) != record_size || get_u32(record, 44) != 0 || get_u32(record, 52) != 0)
        return false;
    if (get_u32(record, checksum_offset) != crc32(record.data(), checksum_offset))
        return false;

    const auto raw_phase = get_u32(record, 40);
    if (raw_phase > static_cast<std::uint32_t>(AuthorityPhase::quarantined))
        return false;

    revision = get_u64(record, 16);
    if (revision == 0)
        return false;
    value = { { get_u64(record, 24) }, { get_u64(record, 32) }, static_cast<AuthorityPhase>(raw_phase) };
    return valid_phase(value.phase);
}

RootRead read_root(const std::filesystem::path& root) noexcept
{
    std::error_code error;
    const bool exists = std::filesystem::exists(root, error);
    if (error)
        return { false, { FenceStoreError::io_failure, static_cast<std::uint32_t>(error.value()) } };
    if (!exists)
        return {};

    const bool directory = std::filesystem::is_directory(root, error);
    if (error || !directory)
        return { true, { FenceStoreError::io_failure, static_cast<std::uint32_t>(error.value()) } };
    return { true, {} };
}

#if defined(_WIN32)
std::uint32_t close_handle(HANDLE handle) noexcept
{
    if (CloseHandle(handle) != 0)
        return 0;
    return GetLastError();
}

SlotRead read_slot(const std::filesystem::path& path) noexcept
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        return { SlotState::io_failure, 0, {}, error };
    }

    LARGE_INTEGER file_size {};
    if (GetFileSizeEx(handle, &file_size) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { SlotState::io_failure, 0, {}, error };
    }
    if (file_size.QuadPart != static_cast<LONGLONG>(record_size))
    {
        close_handle(handle);
        return { SlotState::invalid, 0, {}, 0 };
    }

    Record record {};
    constexpr DWORD expected_size = static_cast<DWORD>(record_size);
    DWORD bytes_read = 0;
    if (ReadFile(handle, record.data(), expected_size, &bytes_read, nullptr) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { SlotState::io_failure, 0, {}, error };
    }
    const auto close_error = close_handle(handle);
    if (close_error != 0)
        return { SlotState::io_failure, 0, {}, close_error };
    if (bytes_read != expected_size)
        return { SlotState::invalid, 0, {}, 0 };

    std::uint64_t revision = 0;
    DurableAuthority value {};
    if (!decode(record, revision, value))
        return { SlotState::invalid, 0, {}, 0 };
    return { SlotState::valid, revision, value, 0 };
}

FenceStoreResult write_slot(const std::filesystem::path& path, std::uint64_t revision, DurableAuthority value) noexcept
{
    const Record record = encode(revision, value);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return { FenceStoreError::io_failure, GetLastError() };

    constexpr DWORD expected_size = static_cast<DWORD>(record_size);
    DWORD written = 0;
    if (WriteFile(handle, record.data(), expected_size, &written, nullptr) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { FenceStoreError::io_failure, error };
    }
    if (written != expected_size)
    {
        close_handle(handle);
        return { FenceStoreError::io_failure, 0 };
    }
    if (FlushFileBuffers(handle) == 0)
    {
        const auto error = GetLastError();
        close_handle(handle);
        return { FenceStoreError::io_failure, error };
    }
    const auto close_error = close_handle(handle);
    if (close_error != 0)
        return { FenceStoreError::io_failure, close_error };

    const SlotRead verification = read_slot(path);
    if (verification.state == SlotState::io_failure)
        return { FenceStoreError::io_failure, verification.native_error };
    if (verification.state != SlotState::valid || verification.revision != revision || !(verification.value == value))
        return { FenceStoreError::corrupt, 0 };
    return {};
}
#else
SlotRead read_slot(const std::filesystem::path&) noexcept
{
    return { SlotState::io_failure, 0, {}, 0 };
}

FenceStoreResult write_slot(const std::filesystem::path&, std::uint64_t, DurableAuthority) noexcept
{
    return { FenceStoreError::io_failure, 0 };
}
#endif

Selection select_latest(const SlotRead& a, const SlotRead& b, bool root_exists) noexcept
{
    if (a.state == SlotState::io_failure)
        return { { FenceStoreError::io_failure, a.native_error }, 0, {} };
    if (b.state == SlotState::io_failure)
        return { { FenceStoreError::io_failure, b.native_error }, 0, {} };

    const bool a_valid = a.state == SlotState::valid;
    const bool b_valid = b.state == SlotState::valid;
    if (!a_valid && !b_valid)
    {
        const bool any_present = a.state != SlotState::missing || b.state != SlotState::missing;
        return { { any_present || root_exists ? FenceStoreError::corrupt : FenceStoreError::not_initialized, 0 }, 0, {} };
    }
    if (a_valid && !b_valid)
        return { {}, a.revision, a.value };
    if (!a_valid && b_valid)
        return { {}, b.revision, b.value };

    if (a.revision > b.revision)
        return { {}, a.revision, a.value };
    if (b.revision > a.revision)
        return { {}, b.revision, b.value };
    if (a.value == b.value)
        return { {}, a.revision, a.value };
    return { { FenceStoreError::corrupt, 0 }, 0, {} };
}
} // namespace

FenceStore::FenceStore(std::filesystem::path root) : root_(std::move(root))
{
}

FenceStoreLoadResult FenceStore::load() const noexcept
{
    try
    {
        const RootRead root = read_root(root_);
        if (!root.status.ok())
            return { root.status, {} };
        const Selection selected = select_latest(read_slot(root_ / "fence.a"), read_slot(root_ / "fence.b"), root.exists);
        return { selected.status, selected.value };
    }
    catch (...)
    {
        return { { FenceStoreError::io_failure, 0 }, {} };
    }
}

FenceStoreResult FenceStore::initialize(DurableAuthority value) noexcept
{
    if (!valid_phase(value.phase))
        return { FenceStoreError::invalid_value, 0 };

    try
    {
        const auto existing = load();
        if (existing.ok())
            return { FenceStoreError::already_initialized, 0 };
        if (existing.status.error != FenceStoreError::not_initialized)
            return existing.status;

        std::error_code error;
        std::filesystem::create_directories(root_, error);
        if (error)
            return { FenceStoreError::io_failure, static_cast<std::uint32_t>(error.value()) };

        return write_slot(root_ / "fence.a", 1, value);
    }
    catch (...)
    {
        return { FenceStoreError::io_failure, 0 };
    }
}

FenceStoreResult FenceStore::store(DurableAuthority value) noexcept
{
    if (!valid_phase(value.phase))
        return { FenceStoreError::invalid_value, 0 };

    try
    {
        const RootRead root = read_root(root_);
        if (!root.status.ok())
            return root.status;
        const SlotRead a = read_slot(root_ / "fence.a");
        const SlotRead b = read_slot(root_ / "fence.b");
        const Selection current = select_latest(a, b, root.exists);
        if (!current.ok())
            return current.status;

        if (value.generation.value < current.value.generation.value || value.epoch.value < current.value.epoch.value ||
            (value.epoch.value > current.value.epoch.value && value.generation.value <= current.value.generation.value))
            return { FenceStoreError::non_monotonic, 0 };
        if (current.revision == std::numeric_limits<std::uint64_t>::max())
            return { FenceStoreError::revision_exhausted, 0 };

        const std::uint64_t next_revision = current.revision + 1;
        const bool write_a = a.state != SlotState::valid ||
                             (b.state == SlotState::valid && a.revision <= b.revision);
        return write_slot(root_ / (write_a ? "fence.a" : "fence.b"), next_revision, value);
    }
    catch (...)
    {
        return { FenceStoreError::io_failure, 0 };
    }
}
} // namespace qiven::host
