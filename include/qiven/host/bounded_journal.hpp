#pragma once

#include "qiven/host/authority.hpp"

#include <array>
#include <cstdint>
#include <filesystem>

namespace qiven::host
{
using JournalDigest = std::array<std::uint8_t, 32>;

enum class JournalEvent : std::uint8_t
{
    authority_transition,
    operation_admitted,
    operation_completed,
    recovery,
    protocol_rejected
};

enum class JournalOutcome : std::uint8_t
{
    none,
    success,
    rejected,
    uncertain,
    failed
};

struct JournalEntry final
{
    PersistentGeneration generation;
    FencingEpoch epoch;
    RequestSequence sequence;
    JournalDigest execution_digest {};
    JournalDigest lease_digest {};
    JournalDigest session_digest {};
    JournalDigest operation_digest {};
    JournalEvent event {};
    JournalOutcome outcome {};
};

enum class JournalError : std::uint8_t
{
    none,
    not_initialized,
    already_initialized,
    corrupt,
    io_failure,
    crypto_failure,
    index_exhausted,
    segment_generation_exhausted,
    invalid_value
};

struct JournalHead final
{
    std::uint64_t index {};
    JournalDigest digest {};

    friend constexpr bool operator==(const JournalHead&, const JournalHead&) = default;
};

struct JournalResult final
{
    JournalError error {};
    std::uint32_t native_error {};
    JournalHead head {};

    bool ok() const noexcept
    {
        return error == JournalError::none;
    }
};

class BoundedJournal final
{
public:
    static constexpr std::uint32_t records_per_segment = 64;
    static constexpr std::uint64_t maximum_retained_bytes = 34'976;

    explicit BoundedJournal(std::filesystem::path root);

    JournalResult initialize() noexcept;
    JournalResult head() const noexcept;
    JournalResult append(const JournalEntry& entry) noexcept;

private:
    std::filesystem::path root_;
};
} // namespace qiven::host
