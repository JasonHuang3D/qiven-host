#include "qiven/host/authority.hpp"

#include <cstdlib>
#include <limits>
#include <utility>

using namespace qiven::host;
namespace qiven::host
{
class TestAuthorityAccess final
{
public:
    static void set_next(AuthorityState& state, std::uint64_t value)
    {
        state.next_ = { value };
    }
};
class TestOwnerPresenceVerifier final
{
public:
    static VerifiedRecoveryAuthorization verify(PersistentGeneration generation, FencingEpoch epoch, AuthorityPhase phase) noexcept
    {
        return VerifiedRecoveryAuthorization(generation, epoch, phase);
    }
};
} // namespace qiven::host
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
} // namespace
int main()
{
    AuthorityState state { { 1 }, { 7 } };
    const BrokerSessionId sa = id<BrokerSessionId>(8), sb = id<BrokerSessionId>(9);
    const ExecutionId a = id<ExecutionId>(1), b = id<ExecutionId>(2);
    const Lease lease { a, id<LeaseId>(3), { 7 } };
    require(state.phase() == AuthorityPhase::ready);
    require(state.acquire(sa, a, lease.lease).ok());
    require(state.phase() == AuthorityPhase::leased);
    const Request request { lease, { 1 }, id<OperationId>(4), 9 };
    require(state.accept(sa, request).ok());
    require(state.complete(sa, request).ok());
    require(state.next_sequence().value == 2);
    require(state.observe_competitor(b).error == AuthorityError::concurrent_execution);
    require(state.phase() == AuthorityPhase::quarantined);
    auto recovery = TestOwnerPresenceVerifier::verify({ 1 }, { 7 }, AuthorityPhase::quarantined);
    require(state.abandon_prior_authority_and_advance_fence(std::move(recovery)).ok());
    require(state.phase() == AuthorityPhase::ready);
    require(state.acquire(sa, a, lease.lease).ok());
    require(state.disconnect(sa, { a, lease.lease, { 8 } }).ok());
    require(state.phase() == AuthorityPhase::reconciling);
    AuthorityState cloned { { 1 }, { 7 } };
    require(cloned.acquire(sa, a, lease.lease).ok());
    require(cloned.accept(sb, request).error == AuthorityError::concurrent_execution);
    require(cloned.phase() == AuthorityPhase::quarantined);
    require(cloned.complete(sa, request).error == AuthorityError::replay_conflict);

    AuthorityState acquire_identity { { 5 }, { 10 } };
    const LeaseId first_lease = id<LeaseId>(40);
    require(acquire_identity.acquire(sa, a, first_lease).ok());
    require(acquire_identity.acquire(sa, a, id<LeaseId>(41)).error == AuthorityError::concurrent_execution);
    require(acquire_identity.phase() == AuthorityPhase::leased);
    require(acquire_identity.acquire(sb, a, id<LeaseId>(42)).error == AuthorityError::concurrent_execution);
    require(acquire_identity.phase() == AuthorityPhase::quarantined);

    AuthorityState validation { { 4 }, { 11 } };
    const Lease current { a, id<LeaseId>(5), { 11 } };
    require(validation.acquire(sa, a, current.lease).ok());
    Request wrong { current, { 2 }, id<OperationId>(6), 1 };
    require(validation.accept(sa, wrong).error == AuthorityError::invalid_sequence);
    wrong.sequence        = { 1 };
    wrong.authority.lease = id<LeaseId>(7);
    require(validation.accept(sa, wrong).error == AuthorityError::stale_lease);
    wrong.authority.lease = current.lease;
    wrong.authority.epoch = { 10 };
    require(validation.accept(sa, wrong).error == AuthorityError::stale_fence);
    wrong.authority.epoch = { 11 };
    require(validation.accept(sa, wrong).ok());
    require(validation.complete(sb, wrong).error == AuthorityError::concurrent_execution);
    require(validation.phase() == AuthorityPhase::quarantined);
    require(validation.complete(sa, wrong).ok());
    require(validation.accept(sa, wrong).error == AuthorityError::quarantined);

    AuthorityState replay { { 4 }, { 12 } };
    const Lease replay_lease { a, id<LeaseId>(8), { 12 } };
    require(replay.acquire(sa, a, replay_lease.lease).ok());
    Request replay_request { replay_lease, { 1 }, id<OperationId>(7), 1 };
    require(replay.accept(sa, replay_request).ok());
    require(replay.complete(sa, replay_request).ok());
    require(replay.accept(sa, replay_request).replayed);
    replay_request.operation = id<OperationId>(99);
    require(replay.accept(sa, replay_request).error == AuthorityError::replay_conflict);

    AuthorityState in_flight { { 2 }, { 20 } };
    const Lease active { a, id<LeaseId>(10), { 20 } };
    const Request accepted { active, { 1 }, id<OperationId>(11), 22 };
    require(in_flight.acquire(sa, a, active.lease).ok());
    require(in_flight.accept(sa, accepted).ok());
    require(in_flight.accept(sb, accepted).error == AuthorityError::concurrent_execution);
    require(in_flight.phase() == AuthorityPhase::quarantined);
    require(in_flight.complete(sa, accepted).ok());
    require(in_flight.accept(sa, { active, { 2 }, id<OperationId>(12), 23 }).error == AuthorityError::quarantined);

    AuthorityState atomic { { 3 }, { 30 } };
    require(atomic.accept(sa, accepted).error == AuthorityError::not_ready);
    require(atomic.phase() == AuthorityPhase::ready && atomic.next_sequence().value == 1);
    require(atomic.acquire(sa, a, id<LeaseId>(13)).ok());
    const Lease owned { a, id<LeaseId>(13), { 30 } };
    require(atomic.release(sa, { a, id<LeaseId>(14), { 30 } }).error == AuthorityError::stale_lease);
    require(atomic.phase() == AuthorityPhase::leased && atomic.next_sequence().value == 1);
    require(atomic.release(sa, owned).ok());
    require(atomic.phase() == AuthorityPhase::ready);

    AuthorityState precedence { { 1 }, { 40 } };
    require(precedence.acquire(sa, a, id<LeaseId>(15)).ok());
    Request multi { { b, id<LeaseId>(16), { 39 } }, { 99 }, id<OperationId>(17), 2 };
    require(precedence.accept(sb, multi).error == AuthorityError::concurrent_execution);
    require(precedence.phase() == AuthorityPhase::quarantined);

    AuthorityState sequence_overflow { { 1 }, { 50 } };
    const Lease max_lease { a, id<LeaseId>(18), { 50 } };
    require(sequence_overflow.acquire(sa, a, max_lease.lease).ok());
    TestAuthorityAccess::set_next(sequence_overflow, std::numeric_limits<std::uint64_t>::max());
    const Request max_request { max_lease, { std::numeric_limits<std::uint64_t>::max() }, id<OperationId>(19), 3 };
    require(sequence_overflow.accept(sa, max_request).ok());
    require(sequence_overflow.complete(sa, max_request).error == AuthorityError::sequence_exhausted);
    require(sequence_overflow.next_sequence().value == std::numeric_limits<std::uint64_t>::max());

    AuthorityState generation_overflow { { std::numeric_limits<std::uint64_t>::max() }, { 60 } };
    require(generation_overflow.acquire(sa, a, id<LeaseId>(20)).ok());
    require(generation_overflow.disconnect(sa, { a, id<LeaseId>(20), { 60 } }).ok());
    auto max_generation = TestOwnerPresenceVerifier::verify({ std::numeric_limits<std::uint64_t>::max() }, { 60 }, AuthorityPhase::reconciling);
    require(generation_overflow.abandon_prior_authority_and_advance_fence(std::move(max_generation)).error == AuthorityError::generation_exhausted);
    require(generation_overflow.phase() == AuthorityPhase::reconciling);

    AuthorityState fence_overflow { { 9 }, { std::numeric_limits<std::uint64_t>::max() } };
    require(fence_overflow.acquire(sa, a, id<LeaseId>(21)).ok());
    require(fence_overflow.disconnect(sa, { a, id<LeaseId>(21), { std::numeric_limits<std::uint64_t>::max() } }).ok());
    auto max_fence = TestOwnerPresenceVerifier::verify({ 9 }, { std::numeric_limits<std::uint64_t>::max() }, AuthorityPhase::reconciling);
    require(fence_overflow.abandon_prior_authority_and_advance_fence(std::move(max_fence)).error == AuthorityError::fence_exhausted);
    require(fence_overflow.phase() == AuthorityPhase::reconciling);
    return 0;
}
