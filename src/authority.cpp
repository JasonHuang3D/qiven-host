#include "qiven/host/authority.hpp"

#include <cassert>
#include <limits>

namespace qiven::host
{
bool AuthorityState::owns(const Lease& value) const noexcept
{
    return execution_ && lease_ && *execution_ == value.execution && *lease_ == value.lease && epoch_ == value.epoch;
}
void AuthorityState::clear() noexcept
{
    execution_.reset();
    lease_.reset();
    session_.reset();
    in_flight_.reset();
    completed_.reset();
    next_ = { 1 };
}
TransitionResult AuthorityState::acquire(BrokerSessionId session, ExecutionId execution, LeaseId lease) noexcept
{
    if (phase_ == AuthorityPhase::reconciling)
        return { AuthorityError::reconciling };
    if (phase_ == AuthorityPhase::quarantined)
        return { AuthorityError::quarantined };
    if (phase_ == AuthorityPhase::leased)
    {
        if (execution_ && *execution_ != execution)
            phase_ = AuthorityPhase::quarantined;
        return { AuthorityError::concurrent_execution };
    }
    execution_ = execution;
    lease_     = lease;
    session_   = session;
    next_      = { 1 };
    phase_     = AuthorityPhase::leased;
    return {};
}
TransitionResult AuthorityState::accept(BrokerSessionId session, const Request& r) noexcept
{
    if (phase_ == AuthorityPhase::quarantined)
        return { AuthorityError::quarantined };
    if (phase_ == AuthorityPhase::reconciling)
        return { AuthorityError::reconciling };
    if (phase_ != AuthorityPhase::leased)
        return { AuthorityError::not_ready };
    if (!session_ || *session_ != session)
    {
        phase_ = AuthorityPhase::quarantined;
        return { AuthorityError::concurrent_execution };
    }
    if (!execution_ || *execution_ != r.authority.execution)
    {
        phase_ = AuthorityPhase::quarantined;
        return { AuthorityError::concurrent_execution };
    }
    if (!lease_ || *lease_ != r.authority.lease)
        return { AuthorityError::stale_lease };
    if (epoch_ != r.authority.epoch)
        return { AuthorityError::stale_fence };
    if (in_flight_)
        return { AuthorityError::operation_in_flight };
    if (r.sequence.value < next_.value)
    {
        if (completed_ && completed_->sequence == r.sequence)
        {
            if (completed_->operation == r.operation && completed_->digest == r.digest)
                return { AuthorityError::none, true };
            return { AuthorityError::replay_conflict };
        }
        return { AuthorityError::replay };
    }
    if (r.sequence.value > next_.value)
        return { AuthorityError::invalid_sequence };
    in_flight_ = r;
    return {};
}
TransitionResult AuthorityState::complete(BrokerSessionId session, const Request& r) noexcept
{
    if (!in_flight_)
        return { AuthorityError::replay_conflict };
    if (!session_ || *session_ != session)
    {
        phase_ = AuthorityPhase::quarantined;
        return { AuthorityError::concurrent_execution };
    }
    if (!execution_ || *execution_ != r.authority.execution)
    {
        phase_ = AuthorityPhase::quarantined;
        return { AuthorityError::concurrent_execution };
    }
    if (!lease_ || *lease_ != r.authority.lease)
        return { AuthorityError::stale_lease };
    if (epoch_ != r.authority.epoch)
        return { AuthorityError::stale_fence };
    if (in_flight_->sequence != r.sequence || in_flight_->operation != r.operation || in_flight_->digest != r.digest)
        return { AuthorityError::replay_conflict };
    if (next_.value == std::numeric_limits<std::uint64_t>::max())
    {
        phase_ = AuthorityPhase::quarantined;
        return { AuthorityError::sequence_exhausted };
    }
    completed_ = in_flight_;
    in_flight_.reset();
    ++next_.value;
    return {};
}
TransitionResult AuthorityState::release(BrokerSessionId session, const Lease& value) noexcept
{
    if (phase_ != AuthorityPhase::leased)
        return { phase_ == AuthorityPhase::quarantined ? AuthorityError::quarantined : AuthorityError::not_ready };
    if (!session_ || *session_ != session)
    {
        phase_ = AuthorityPhase::quarantined;
        return { AuthorityError::concurrent_execution };
    }
    if (!owns(value))
        return { value.epoch == epoch_ ? AuthorityError::stale_lease : AuthorityError::stale_fence };
    if (in_flight_)
        return { AuthorityError::operation_in_flight };
    clear();
    phase_ = AuthorityPhase::ready;
    return {};
}
TransitionResult AuthorityState::disconnect(BrokerSessionId session, const Lease& value) noexcept
{
    if (phase_ != AuthorityPhase::leased)
        return { AuthorityError::not_ready };
    if (!session_ || *session_ != session)
    {
        phase_ = AuthorityPhase::quarantined;
        return { AuthorityError::concurrent_execution };
    }
    if (!owns(value))
        return { value.epoch == epoch_ ? AuthorityError::stale_lease : AuthorityError::stale_fence };
    phase_ = AuthorityPhase::reconciling;
    return {};
}
TransitionResult AuthorityState::observe_competitor(ExecutionId execution) noexcept
{
    if (phase_ == AuthorityPhase::leased && execution_ && *execution_ != execution)
    {
        phase_ = AuthorityPhase::quarantined;
        return { AuthorityError::concurrent_execution };
    }
    return { phase_ == AuthorityPhase::quarantined ? AuthorityError::quarantined : AuthorityError::not_ready };
}
TransitionResult AuthorityState::abandon_prior_authority_and_advance_fence(VerifiedRecoveryAuthorization authorization) noexcept
{
    if (phase_ != AuthorityPhase::reconciling && phase_ != AuthorityPhase::quarantined)
        return { AuthorityError::not_ready };
    if (authorization.generation_ != generation_ || authorization.epoch_ != epoch_ || authorization.phase_ != phase_)
        return { AuthorityError::recovery_context_stale };
    if (generation_.value == std::numeric_limits<std::uint64_t>::max())
        return { AuthorityError::generation_exhausted };
    if (epoch_.value == std::numeric_limits<std::uint64_t>::max())
        return { AuthorityError::fence_exhausted };
    clear();
    ++generation_.value;
    ++epoch_.value;
    phase_ = AuthorityPhase::ready;
    assert(!execution_ && !lease_ && !session_ && !in_flight_);
    return {};
}
} // namespace qiven::host
