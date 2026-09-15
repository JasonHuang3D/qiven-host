#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace qiven::host
{
template <class Tag>
struct OpaqueId final
{
    std::array<std::uint8_t, 16> bytes {};
    friend constexpr bool operator==(const OpaqueId&, const OpaqueId&) = default;
};
struct ExecutionTag;
struct LeaseTag;
struct OperationTag;
struct SessionTag;
using ExecutionId     = OpaqueId<ExecutionTag>;
using LeaseId         = OpaqueId<LeaseTag>;
using OperationId     = OpaqueId<OperationTag>;
using BrokerSessionId = OpaqueId<SessionTag>;

struct FencingEpoch final
{
    std::uint64_t value {};
    friend constexpr bool operator==(FencingEpoch, FencingEpoch) = default;
};
struct RequestSequence final
{
    std::uint64_t value {};
    friend constexpr bool operator==(RequestSequence, RequestSequence) = default;
};
struct PersistentGeneration final
{
    std::uint64_t value {};
    friend constexpr bool operator==(PersistentGeneration, PersistentGeneration) = default;
};

enum class AuthorityPhase : std::uint8_t
{
    ready,
    leased,
    reconciling,
    quarantined
};
enum class AuthorityError : std::uint8_t
{
    none,
    not_ready,
    already_leased,
    concurrent_execution,
    stale_execution,
    stale_lease,
    stale_fence,
    invalid_sequence,
    replay,
    replay_conflict,
    sequence_exhausted,
    operation_in_flight,
    reconciling,
    quarantined,
    recovery_not_authorized,
    recovery_context_stale,
    generation_exhausted,
    fence_exhausted,
    persistence_failure
};

struct Lease final
{
    ExecutionId execution;
    LeaseId lease;
    FencingEpoch epoch;
};
struct Request final
{
    Lease authority;
    RequestSequence sequence;
    OperationId operation;
    std::uint64_t digest {};
};
struct TransitionResult final
{
    AuthorityError error {};
    bool replayed {};
    bool ok() const noexcept
    {
        return error == AuthorityError::none;
    }
};

class TestOwnerPresenceVerifier;
class TestAuthorityAccess;
class VerifiedRecoveryAuthorization final
{
public:
    VerifiedRecoveryAuthorization(const VerifiedRecoveryAuthorization&)            = delete;
    VerifiedRecoveryAuthorization& operator=(const VerifiedRecoveryAuthorization&) = delete;
    VerifiedRecoveryAuthorization(VerifiedRecoveryAuthorization&&) noexcept        = default;

private:
    VerifiedRecoveryAuthorization(PersistentGeneration generation, FencingEpoch epoch, AuthorityPhase phase) noexcept
    :
    generation_(generation), epoch_(epoch), phase_(phase)
    {
    }
    PersistentGeneration generation_;
    FencingEpoch epoch_;
    AuthorityPhase phase_;
    friend class TestOwnerPresenceVerifier;
    friend class AuthorityState;
};
class AuthorityState final
{
public:
    AuthorityState(PersistentGeneration generation, FencingEpoch epoch) noexcept :
    generation_(generation), epoch_(epoch)
    {
    }
    AuthorityPhase phase() const noexcept
    {
        return phase_;
    }
    FencingEpoch epoch() const noexcept
    {
        return epoch_;
    }
    PersistentGeneration generation() const noexcept
    {
        return generation_;
    }
    RequestSequence next_sequence() const noexcept
    {
        return next_;
    }
    TransitionResult acquire(BrokerSessionId session, ExecutionId execution, LeaseId lease) noexcept;
    TransitionResult accept(BrokerSessionId session, const Request& request) noexcept;
    TransitionResult complete(const Request& request) noexcept;
    TransitionResult release(BrokerSessionId session, const Lease& lease) noexcept;
    TransitionResult disconnect(BrokerSessionId session, const Lease& lease) noexcept;
    TransitionResult observe_competitor(ExecutionId execution) noexcept;
    TransitionResult abandon_prior_authority_and_advance_fence(VerifiedRecoveryAuthorization authorization) noexcept;

private:
    bool owns(const Lease&) const noexcept;
    void clear() noexcept;
    AuthorityPhase phase_ { AuthorityPhase::ready };
    PersistentGeneration generation_ {};
    FencingEpoch epoch_ {};
    RequestSequence next_ { 1 };
    std::optional<ExecutionId> execution_;
    std::optional<LeaseId> lease_;
    std::optional<BrokerSessionId> session_;
    std::optional<Request> in_flight_;
    std::optional<Request> completed_;
    friend class TestAuthorityAccess;
};
} // namespace qiven::host
