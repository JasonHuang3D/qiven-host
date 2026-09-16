#include "qiven/host/authority_broker.hpp"

#include <limits>
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
#if defined(_WIN32)
template <class Id>
bool digest_id(const Id& id, JournalDigest& digest, std::uint32_t& native_error) noexcept
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0)
    {
        native_error = static_cast<std::uint32_t>(status);
        return false;
    }

    status = BCryptHash(algorithm, nullptr, 0,
                        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(id.bytes.data())),
                        static_cast<ULONG>(id.bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
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
#else
template <class Id>
bool digest_id(const Id&, JournalDigest&, std::uint32_t&) noexcept
{
    return false;
}
#endif

JournalOutcome journal_outcome(OperationOutcome outcome) noexcept
{
    switch (outcome)
    {
    case OperationOutcome::success:
        return JournalOutcome::success;
    case OperationOutcome::failed:
        return JournalOutcome::failed;
    case OperationOutcome::uncertain:
        return JournalOutcome::uncertain;
    }
    return JournalOutcome::uncertain;
}
} // namespace

AuthorityBroker::AuthorityBroker(std::filesystem::path root) :
    fence_store_(root / "authority"), journal_(root / "journal")
{
}

AuthorityBrokerResult AuthorityBroker::ready_locked() const noexcept
{
    if (persistence_fault_)
        return { fault_error_ == AuthorityBrokerError::none ? AuthorityBrokerError::persistence_fault : fault_error_,
                 AuthorityError::persistence_failure, {}, {}, fault_native_error_ };
    if (!started_)
        return { AuthorityBrokerError::not_started };
    return {};
}

AuthorityBrokerResult AuthorityBroker::fail_closed(AuthorityBrokerError error, std::uint32_t native_error) noexcept
{
    persistence_fault_ = true;
    fault_error_ = error;
    fault_native_error_ = native_error;
    return { error, AuthorityError::persistence_failure, {}, {}, native_error };
}

DurableAuthority AuthorityBroker::durable_state_locked() const noexcept
{
    return { state_.generation(), state_.epoch(), state_.phase() };
}

AuthorityBrokerResult AuthorityBroker::persist_state_locked() noexcept
{
    const FenceStoreResult persisted = fence_store_.store(durable_state_locked());
    if (!persisted.ok())
    {
        persistence_fault_ = true;
        fault_error_ = AuthorityBrokerError::fence_store_failure;
        fault_native_error_ = persisted.native_error;
        return { AuthorityBrokerError::fence_store_failure, AuthorityError::persistence_failure, persisted.error, {},
                 persisted.native_error };
    }
    return {};
}

AuthorityBrokerResult AuthorityBroker::append_locked(const JournalEntry& entry) noexcept
{
    const JournalResult appended = journal_.append(entry);
    if (!appended.ok())
    {
        persistence_fault_ = true;
        fault_error_ = AuthorityBrokerError::journal_failure;
        fault_native_error_ = appended.native_error;
        return { AuthorityBrokerError::journal_failure, AuthorityError::persistence_failure, {}, appended.error,
                 appended.native_error };
    }
    return {};
}

AuthorityBrokerResult AuthorityBroker::start() noexcept
{
    std::lock_guard lock(mutex_);
    if (started_)
        return { AuthorityBrokerError::already_started };
    if (persistence_fault_)
        return ready_locked();

#if !defined(_WIN32)
    return { AuthorityBrokerError::platform_unsupported };
#else
    const FenceStoreLoadResult fence = fence_store_.load();
    const JournalResult journal = journal_.head();
    const bool fence_missing = fence.status.error == FenceStoreError::not_initialized;
    const bool journal_missing = journal.error == JournalError::not_initialized;

    if (fence_missing != journal_missing)
        return fail_closed(AuthorityBrokerError::inconsistent_persistence);

    if (fence_missing)
    {
        state_ = AuthorityState({ 1 }, { 1 });
        const FenceStoreResult initialized = fence_store_.initialize(durable_state_locked());
        if (!initialized.ok())
        {
            persistence_fault_ = true;
            fault_error_ = AuthorityBrokerError::fence_store_failure;
            fault_native_error_ = initialized.native_error;
            return { AuthorityBrokerError::fence_store_failure, AuthorityError::persistence_failure, initialized.error, {},
                     initialized.native_error };
        }
        const JournalResult journal_initialized = journal_.initialize();
        if (!journal_initialized.ok())
        {
            persistence_fault_ = true;
            fault_error_ = AuthorityBrokerError::journal_failure;
            fault_native_error_ = journal_initialized.native_error;
            return { AuthorityBrokerError::journal_failure, AuthorityError::persistence_failure, {},
                     journal_initialized.error, journal_initialized.native_error };
        }
        started_ = true;
        return {};
    }

    if (!fence.ok())
    {
        persistence_fault_ = true;
        fault_error_ = AuthorityBrokerError::fence_store_failure;
        fault_native_error_ = fence.status.native_error;
        return { AuthorityBrokerError::fence_store_failure, AuthorityError::persistence_failure, fence.status.error, {},
                 fence.status.native_error };
    }
    if (!journal.ok())
    {
        persistence_fault_ = true;
        fault_error_ = AuthorityBrokerError::journal_failure;
        fault_native_error_ = journal.native_error;
        return { AuthorityBrokerError::journal_failure, AuthorityError::persistence_failure, {}, journal.error,
                 journal.native_error };
    }

    state_ = AuthorityState(fence.value.generation, fence.value.epoch);
    switch (fence.value.phase)
    {
    case AuthorityPhase::ready:
        state_.phase_ = AuthorityPhase::ready;
        break;
    case AuthorityPhase::leased:
        state_.phase_ = AuthorityPhase::reconciling;
        break;
    case AuthorityPhase::reconciling:
        state_.phase_ = AuthorityPhase::reconciling;
        break;
    case AuthorityPhase::quarantined:
        state_.phase_ = AuthorityPhase::quarantined;
        break;
    }

    if (fence.value.phase == AuthorityPhase::leased)
    {
        const AuthorityBrokerResult persisted = persist_state_locked();
        if (!persisted.ok())
            return persisted;
        JournalEntry entry {};
        entry.generation = state_.generation();
        entry.epoch = state_.epoch();
        entry.event = JournalEvent::authority_transition;
        entry.outcome = JournalOutcome::uncertain;
        const AuthorityBrokerResult appended = append_locked(entry);
        if (!appended.ok())
            return appended;
    }

    started_ = true;
    return {};
#endif
}

AuthorityBrokerStatus AuthorityBroker::status() const noexcept
{
    std::lock_guard lock(mutex_);
    return { started_, persistence_fault_, fault_error_, state_.phase(), state_.generation(), state_.epoch(),
             state_.next_sequence() };
}

AuthorityBrokerAcquireResult AuthorityBroker::acquire(BrokerSessionId session, ExecutionId execution, LeaseId lease) noexcept
{
    std::lock_guard lock(mutex_);
    const AuthorityBrokerResult ready = ready_locked();
    if (!ready.ok())
        return { ready, {} };

    JournalEntry entry {};
    std::uint32_t native_error = 0;
    if (!digest_id(execution, entry.execution_digest, native_error) ||
        !digest_id(lease, entry.lease_digest, native_error) ||
        !digest_id(session, entry.session_digest, native_error))
        return { fail_closed(AuthorityBrokerError::identity_digest_failure, native_error), {} };

    const AuthorityPhase before = state_.phase();
    const TransitionResult transition = state_.acquire(session, execution, lease);
    entry.generation = state_.generation();
    entry.epoch = state_.epoch();
    entry.event = transition.ok() ? JournalEvent::authority_transition : JournalEvent::protocol_rejected;
    entry.outcome = transition.ok() ? JournalOutcome::success : JournalOutcome::rejected;

    if (state_.phase() != before)
    {
        const AuthorityBrokerResult persisted = persist_state_locked();
        if (!persisted.ok())
            return { persisted, {} };
    }

    const AuthorityBrokerResult appended = append_locked(entry);
    if (!appended.ok())
        return { appended, {} };
    if (!transition.ok())
        return { { {}, transition.error }, {} };

    return { {}, { execution, lease, state_.epoch() } };
}

AuthorityBrokerResult AuthorityBroker::begin_operation(BrokerSessionId session, const Request& request) noexcept
{
    std::lock_guard lock(mutex_);
    const AuthorityBrokerResult ready = ready_locked();
    if (!ready.ok())
        return ready;

    JournalEntry entry {};
    std::uint32_t native_error = 0;
    if (!digest_id(request.authority.execution, entry.execution_digest, native_error) ||
        !digest_id(request.authority.lease, entry.lease_digest, native_error) ||
        !digest_id(session, entry.session_digest, native_error) ||
        !digest_id(request.operation, entry.operation_digest, native_error))
        return fail_closed(AuthorityBrokerError::identity_digest_failure, native_error);

    const AuthorityPhase before = state_.phase();
    const TransitionResult transition = state_.accept(session, request);
    if (transition.replayed)
        return { {}, {}, {}, {}, 0, true };

    entry.generation = state_.generation();
    entry.epoch = state_.epoch();
    entry.sequence = request.sequence;
    entry.event = transition.ok() ? JournalEvent::operation_admitted : JournalEvent::protocol_rejected;
    entry.outcome = transition.ok() ? JournalOutcome::success : JournalOutcome::rejected;

    if (state_.phase() != before)
    {
        const AuthorityBrokerResult persisted = persist_state_locked();
        if (!persisted.ok())
            return persisted;
    }

    const AuthorityBrokerResult appended = append_locked(entry);
    if (!appended.ok())
        return appended;
    if (!transition.ok())
        return { {}, transition.error };
    return {};
}

AuthorityBrokerResult AuthorityBroker::finish_operation(BrokerSessionId session, const Request& request,
                                                         OperationOutcome outcome) noexcept
{
    std::lock_guard lock(mutex_);
    const AuthorityBrokerResult ready = ready_locked();
    if (!ready.ok())
        return ready;

    JournalEntry entry {};
    std::uint32_t native_error = 0;
    if (!digest_id(request.authority.execution, entry.execution_digest, native_error) ||
        !digest_id(request.authority.lease, entry.lease_digest, native_error) ||
        !digest_id(session, entry.session_digest, native_error) ||
        !digest_id(request.operation, entry.operation_digest, native_error))
        return fail_closed(AuthorityBrokerError::identity_digest_failure, native_error);

    const AuthorityPhase before = state_.phase();
    const TransitionResult transition = state_.complete(session, request);
    entry.generation = state_.generation();
    entry.epoch = state_.epoch();
    entry.sequence = request.sequence;
    entry.event = transition.ok() ? JournalEvent::operation_completed : JournalEvent::protocol_rejected;
    entry.outcome = transition.ok() ? journal_outcome(outcome) : JournalOutcome::rejected;

    if (state_.phase() != before)
    {
        const AuthorityBrokerResult persisted = persist_state_locked();
        if (!persisted.ok())
            return persisted;
    }

    const AuthorityBrokerResult appended = append_locked(entry);
    if (!appended.ok())
        return appended;
    if (!transition.ok())
        return { {}, transition.error };
    return {};
}

AuthorityBrokerResult AuthorityBroker::release(BrokerSessionId session, const Lease& lease) noexcept
{
    std::lock_guard lock(mutex_);
    const AuthorityBrokerResult ready = ready_locked();
    if (!ready.ok())
        return ready;

    JournalEntry entry {};
    std::uint32_t native_error = 0;
    if (!digest_id(lease.execution, entry.execution_digest, native_error) ||
        !digest_id(lease.lease, entry.lease_digest, native_error) ||
        !digest_id(session, entry.session_digest, native_error))
        return fail_closed(AuthorityBrokerError::identity_digest_failure, native_error);

    const AuthorityPhase before = state_.phase();
    const RequestSequence prior_sequence = state_.next_sequence();
    const TransitionResult transition = state_.release(session, lease);
    entry.generation = state_.generation();
    entry.epoch = state_.epoch();
    entry.sequence = prior_sequence;

    if (!transition.ok())
    {
        if (state_.phase() != before)
        {
            const AuthorityBrokerResult persisted = persist_state_locked();
            if (!persisted.ok())
                return persisted;
        }
        entry.event = JournalEvent::protocol_rejected;
        entry.outcome = JournalOutcome::rejected;
        const AuthorityBrokerResult appended = append_locked(entry);
        if (!appended.ok())
            return appended;
        return { {}, transition.error };
    }

    entry.event = JournalEvent::authority_transition;
    entry.outcome = JournalOutcome::uncertain;
    AuthorityBrokerResult appended = append_locked(entry);
    if (!appended.ok())
        return appended;

    const AuthorityBrokerResult persisted = persist_state_locked();
    if (!persisted.ok())
        return persisted;

    entry.outcome = JournalOutcome::success;
    appended = append_locked(entry);
    if (!appended.ok())
        return appended;
    return {};
}

AuthorityBrokerResult AuthorityBroker::disconnect(BrokerSessionId session, const Lease& lease) noexcept
{
    std::lock_guard lock(mutex_);
    const AuthorityBrokerResult ready = ready_locked();
    if (!ready.ok())
        return ready;

    JournalEntry entry {};
    std::uint32_t native_error = 0;
    if (!digest_id(lease.execution, entry.execution_digest, native_error) ||
        !digest_id(lease.lease, entry.lease_digest, native_error) ||
        !digest_id(session, entry.session_digest, native_error))
        return fail_closed(AuthorityBrokerError::identity_digest_failure, native_error);

    const AuthorityPhase before = state_.phase();
    const TransitionResult transition = state_.disconnect(session, lease);
    entry.generation = state_.generation();
    entry.epoch = state_.epoch();
    entry.sequence = state_.next_sequence();
    entry.event = transition.ok() ? JournalEvent::authority_transition : JournalEvent::protocol_rejected;
    entry.outcome = transition.ok() ? JournalOutcome::uncertain : JournalOutcome::rejected;

    if (state_.phase() != before)
    {
        const AuthorityBrokerResult persisted = persist_state_locked();
        if (!persisted.ok())
            return persisted;
    }
    const AuthorityBrokerResult appended = append_locked(entry);
    if (!appended.ok())
        return appended;
    if (!transition.ok())
        return { {}, transition.error };
    return {};
}

AuthorityBrokerResult AuthorityBroker::observe_competitor(ExecutionId execution) noexcept
{
    std::lock_guard lock(mutex_);
    const AuthorityBrokerResult ready = ready_locked();
    if (!ready.ok())
        return ready;

    JournalEntry entry {};
    std::uint32_t native_error = 0;
    if (!digest_id(execution, entry.execution_digest, native_error))
        return fail_closed(AuthorityBrokerError::identity_digest_failure, native_error);

    const AuthorityPhase before = state_.phase();
    const TransitionResult transition = state_.observe_competitor(execution);
    entry.generation = state_.generation();
    entry.epoch = state_.epoch();
    entry.sequence = state_.next_sequence();
    entry.event = JournalEvent::protocol_rejected;
    entry.outcome = JournalOutcome::rejected;

    if (state_.phase() != before)
    {
        const AuthorityBrokerResult persisted = persist_state_locked();
        if (!persisted.ok())
            return persisted;
    }
    const AuthorityBrokerResult appended = append_locked(entry);
    if (!appended.ok())
        return appended;
    return { {}, transition.error };
}
} // namespace qiven::host
