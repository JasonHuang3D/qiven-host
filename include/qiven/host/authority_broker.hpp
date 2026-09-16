#pragma once

#include "qiven/host/authority.hpp"
#include "qiven/host/bounded_journal.hpp"
#include "qiven/host/fence_store.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>

namespace qiven::host
{
enum class AuthorityBrokerError : std::uint8_t
{
    none,
    not_started,
    already_started,
    platform_unsupported,
    persistence_fault,
    inconsistent_persistence,
    fence_store_failure,
    journal_failure,
    identity_digest_failure
};

enum class OperationOutcome : std::uint8_t
{
    success,
    failed,
    uncertain
};

struct AuthorityBrokerResult final
{
    AuthorityBrokerError error {};
    AuthorityError authority_error {};
    FenceStoreError fence_store_error {};
    JournalError journal_error {};
    std::uint32_t native_error {};
    bool replayed {};

    bool ok() const noexcept
    {
        return error == AuthorityBrokerError::none && authority_error == AuthorityError::none;
    }
};

struct AuthorityBrokerAcquireResult final
{
    AuthorityBrokerResult status;
    Lease lease {};

    bool ok() const noexcept
    {
        return status.ok();
    }
};

struct AuthorityBrokerStatus final
{
    bool started {};
    bool persistence_fault {};
    AuthorityBrokerError fault_error {};
    AuthorityPhase phase { AuthorityPhase::ready };
    PersistentGeneration generation {};
    FencingEpoch epoch {};
    RequestSequence next_sequence { 1 };
};

class AuthorityBroker final
{
public:
    explicit AuthorityBroker(std::filesystem::path root);

    AuthorityBroker(const AuthorityBroker&) = delete;
    AuthorityBroker& operator=(const AuthorityBroker&) = delete;
    AuthorityBroker(AuthorityBroker&&) = delete;
    AuthorityBroker& operator=(AuthorityBroker&&) = delete;

    AuthorityBrokerResult start() noexcept;
    AuthorityBrokerStatus status() const noexcept;

    AuthorityBrokerAcquireResult acquire(BrokerSessionId session, ExecutionId execution, LeaseId lease) noexcept;
    AuthorityBrokerResult begin_operation(BrokerSessionId session, const Request& request) noexcept;
    AuthorityBrokerResult finish_operation(BrokerSessionId session, const Request& request, OperationOutcome outcome) noexcept;
    AuthorityBrokerResult release(BrokerSessionId session, const Lease& lease) noexcept;
    AuthorityBrokerResult disconnect(BrokerSessionId session, const Lease& lease) noexcept;
    AuthorityBrokerResult observe_competitor(ExecutionId execution) noexcept;

private:
    AuthorityBrokerResult ready_locked() const noexcept;
    AuthorityBrokerResult fail_closed(AuthorityBrokerError error, std::uint32_t native_error = 0) noexcept;
    AuthorityBrokerResult persist_state_locked() noexcept;
    AuthorityBrokerResult append_locked(const JournalEntry& entry) noexcept;
    DurableAuthority durable_state_locked() const noexcept;

    mutable std::mutex mutex_;
    FenceStore fence_store_;
    BoundedJournal journal_;
    AuthorityState state_ { { 0 }, { 0 } };
    bool started_ {};
    bool persistence_fault_ {};
    AuthorityBrokerError fault_error_ {};
    std::uint32_t fault_native_error_ {};
};
} // namespace qiven::host
