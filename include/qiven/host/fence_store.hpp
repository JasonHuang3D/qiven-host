#pragma once

#include "qiven/host/authority.hpp"

#include <cstdint>
#include <filesystem>

namespace qiven::host
{
struct DurableAuthority final
{
    PersistentGeneration generation;
    FencingEpoch epoch;
    AuthorityPhase phase;

    friend constexpr bool operator==(const DurableAuthority&, const DurableAuthority&) = default;
};

enum class FenceStoreError : std::uint8_t
{
    none,
    not_initialized,
    already_initialized,
    corrupt,
    io_failure,
    non_monotonic,
    revision_exhausted,
    invalid_value
};

struct FenceStoreResult final
{
    FenceStoreError error {};
    std::uint32_t native_error {};

    bool ok() const noexcept
    {
        return error == FenceStoreError::none;
    }
};

struct FenceStoreLoadResult final
{
    FenceStoreResult status;
    DurableAuthority value {};

    bool ok() const noexcept
    {
        return status.ok();
    }
};

class FenceStore final
{
public:
    explicit FenceStore(std::filesystem::path root);

    FenceStoreResult initialize(DurableAuthority value) noexcept;
    FenceStoreLoadResult load() const noexcept;
    FenceStoreResult store(DurableAuthority value) noexcept;

private:
    std::filesystem::path root_;
};
} // namespace qiven::host
