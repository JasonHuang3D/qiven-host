# Host Protocol Dispatcher + Protected NoOp

This Batch 000 checkpoint connects the already-accepted bounded V1 codec and owner-only local named-pipe transport to `AuthorityBroker` without introducing arbitrary mutation capability.

## Boundary

`ProtocolDispatcher` consumes one complete `ProtocolFrame` from a concrete `OwnerPipeServer` slot. While holding its lifecycle gate, it reads the Host-owned `BrokerSessionId` that the transport assigned to that accepted connection. The session identity is never accepted as a dispatcher argument and is never decoded from the wire. The dispatcher supports only the normal V1 request kinds already frozen by the codec:

- `Acquire`;
- `ExecuteTestOperation` with the sole V1 operation code `NoOp`;
- `Release`;
- `QueryStatus`.

Response message kinds received from a client are rejected as unexpected request kinds. Structural or semantic codec failure does not partially dispatch authority work.

This checkpoint still does not implement Runtime/DCR adapters, filesystem/Git/process mutation, owner recovery, normal-protocol `Reconcile`, or any production mutation path.

## Dispatch/close lifecycle serialization

Transport retirement and authority dispatch are one lifecycle domain. Batch 000 therefore serializes all `ProtocolDispatcher::dispatch` and `ProtocolDispatcher::close_connection` calls through one dispatcher-owned lifecycle mutex. This conservative global serialization is intentional and matches ADR-0026's initial global single-writer/read-serialization model; it is not a permanent throughput claim.

The dispatcher resolves the current Host-owned session from `(server, slot)` only after acquiring that lifecycle gate. A caller cannot supply a stale/copied session identity directly. If close wins the lifecycle gate first, it disconnects and retires the transport slot before releasing the gate; a later stale frame then fails transport-session lookup and cannot acquire authority. If dispatch wins first, successful Acquire and the corresponding active-lease tracking become visible before close can run, so close performs authority-side disconnect before slot reuse.

The accepted transport still owns pipe I/O and permits only one active read/write flow per connected slot. The server loop must route accepted-frame dispatch and connection close through this dispatcher lifecycle surface rather than calling transport retirement directly around it.

## Host-issued lease

The client supplies `ExecutionId` plus bounded scope/intent metadata on `Acquire`; it never supplies a lease. The dispatcher generates a fresh nonzero 128-bit `LeaseId` using the Windows system-preferred CSPRNG and passes it, together with the transport-owned `BrokerSessionId`, to `AuthorityBroker::acquire`.

A successful acquire response returns the Host-issued lease and the current generation/fencing epoch. Failed acquire responses use only the stable `ProtocolResultCode` vocabulary and keep success-only fields zero. Native/broker enum values are never copied to the wire.

Lease generation remains local to this isolated dispatcher checkpoint. Refactoring the already-accepted transport CSPRNG helper into broader shared infrastructure is not required to prove dispatcher semantics and would unnecessarily modify the previously accepted transport implementation in this checkpoint.

The bounded scope/intent fields are carried by the frozen V1 Acquire wire contract but this dispatcher checkpoint does not yet add them to durable journal records. ADR-0026's bounded audit-metadata requirement remains a Batch 000 acceptance item and must be closed before Batch 000 as a whole is accepted; it is not silently considered satisfied by this checkpoint.

## Explicit result mapping

The dispatcher maps internal `AuthorityError` / `AuthorityBrokerError` values to the stable V1 protocol vocabulary. Persistence faults map to `persistence_failure`; normal authority rejections map to their corresponding protocol result; recovery-only/internal states do not gain new wire messages and map to `internal_failure` where no normal V1 result exists.

An authority rejection is not a dispatcher execution failure when a canonical protocol response was produced. Malformed frames, unexpected request kinds, lease-generation failure, impossible response-encoding failure, authority cleanup failure, and transport cleanup failure remain local dispatcher errors.

## Protected NoOp lifecycle

`ExecuteTestOperation(NoOp)` is the first protected end-to-end operation. The dispatcher:

1. verifies the current transport slot/session under the lifecycle gate;
2. validates and decodes the canonical V1 frame;
3. injects the exact Host-owned connection `BrokerSessionId` out-of-band;
4. derives the authority replay digest from the canonical decoded payload bytes rather than accepting a caller-selected digest field;
5. calls `AuthorityBroker::begin_operation`;
6. performs the protected `NoOp` only after admission succeeds;
7. calls `AuthorityBroker::finish_operation(..., success)`;
8. encodes the stable V1 response.

The payload digest uses deterministic FNV-1a over the already-validated canonical payload bytes. Cryptographic collision resistance is not being claimed or required here: `OperationId` and sequence are independently checked by `AuthorityState`; the digest binds the canonical request semantics used for exact replay/conflict classification and is not a trust/authentication primitive.

Exact completed-request replay returns `result=ok` with `replayed=true` and does not execute/finish the operation a second time. A conflicting request at the same completed sequence maps to `replay_conflict`; a future sequence gap maps to `invalid_sequence`.

## Query and release

`QueryStatus` remains observational and reports the broker-started flag, persistence-fault flag, authority phase, generation, fence, and next request sequence through the existing bounded response.

A successful `Release` clears the dispatcher's in-memory active lease tracking only after `AuthorityBroker::release` succeeds. Failed release leaves the tracked lease intact for later disconnect handling.

## Authority-aware connection close

Transport slot reuse is coordinated through `ProtocolDispatcher::close_connection` rather than by calling `OwnerPipeServer::retire` directly for a session that may own authority.

The close path, under the same lifecycle gate used by dispatch:

1. reads the preserved Host-owned session from the transport slot;
2. asks the transport to disconnect / enter `Retiring` (an already-retiring slot is accepted; a poisoned slot remains poisoned);
3. if that session owns the active lease, calls `AuthorityBroker::disconnect` before reuse;
4. requires the resulting authority to be durably fail-closed outside `Leased` (`Reconciling`, `Quarantined`, or already `Ready`) before forgetting the tracked lease;
5. only then calls transport `retire` when the transport itself proved clean disconnect.

If authority cleanup cannot prove a non-Leased, non-persistence-fault state, the slot is not retired. If transport is poisoned, authority cleanup still runs, but the poisoned slot remains unavailable rather than being washed back to Listening.

This ordering preserves the accepted transport invariant that connection identity survives transport loss long enough for authority-side reconciliation and prevents slot reuse from erasing an uncertain lease.

## Acceptance coverage

The Windows dispatcher acceptance test exercises real owner-pipe transport plus codec plus broker:

- Host-issued Acquire lease;
- status observation in `Leased` and `Ready`;
- protected NoOp admission/completion;
- exact replay without re-execution;
- replay conflict and sequence-gap rejection;
- successful Release;
- client loss with authority-aware close causing `Reconciling` before slot reuse;
- a frame read from a connection that is then closed/retired before dispatch, proving stale post-close dispatch is rejected and cannot obtain authority;
- two distinct Host-owned pipe sessions presenting the same client-controlled `ExecutionId`, with the second Acquire rejected and authority moved to `Quarantined`;
- cleanup of both transport sessions without weakening the quarantined authority state.

The dispatcher test has its own CTest timeout fuse; the underlying transport retains its accepted per-operation deadlines.
