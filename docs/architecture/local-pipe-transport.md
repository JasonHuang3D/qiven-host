# Owner-Only Local Named-Pipe Transport

This Batch 000 layer proves the Windows local transport and connection-session boundary independently of protocol dispatch or protected operations. It consumes the already-proven bounded protocol frame type but does not interpret or execute protocol messages.

## Endpoint and security

The server endpoint is `\\.\pipe\QivenHost.Broker.<current-owner-SID>`. The SID is resolved from the current process token; inability to establish that identity fails creation rather than falling back to a broader endpoint.

Server instances are created with an explicit protected DACL containing one allow ACE for the current owner SID with generic-all access. The implementation never relies on the Windows default named-pipe security descriptor. `PIPE_REJECT_REMOTE_CLIENTS` is set on every instance, so network-originated clients are rejected before application protocol handling. Handles are non-inheritable. The client opens with `SECURITY_IDENTIFICATION` quality of service rather than granting the server an unconstrained impersonation context.

The first server instance uses `FILE_FLAG_FIRST_PIPE_INSTANCE`. A second server attempting to claim the same owner endpoint fails rather than silently creating a parallel endpoint. This is transport endpoint defense-in-depth; `BrokerInstanceGuard` remains the canonical process-singleton layer.

## Bounded connection pool and I/O liveness

V1 pre-creates exactly four server pipe instances, all with the same `nMaxInstances=4`, message mode, `FILE_FLAG_OVERLAPPED`, and protocol-sized input/output buffer requests. Four is a deliberate hard Batch 000 resource bound, not a claim that four is the permanent production tuning value.

The four instances are independently connectable. `ConnectNamedPipe` may be pending on all four at once, allowing multiple local clients to establish transport sessions concurrently. This is required for the authority architecture: a second execution contender must be able to reach later broker dispatch and cause deterministic quarantine. The transport must not manufacture apparent safety by accepting only one connection and queuing all other callers outside `AuthorityState`.

A bounded slot count alone is not sufficient. The rejected synchronous prototype proved that blocking `ConnectNamedPipe`, `ReadFile`, or `WriteFile` can let a local peer occupy a slot or caller thread indefinitely. Server accept/read/write and client read/write therefore use overlapped I/O plus an explicit deadline. The production default is currently 5 seconds and tests may supply a narrower deadline. A timeout cancels the exact outstanding I/O with `CancelIoEx`; because `OVERLAPPED` storage is stack-owned, cancellation completion is drained before that storage leaves scope. This completion drain is kernel-lifetime safety after the peer-facing deadline, not permission to resume an unbounded peer wait.

When all four instances are occupied, a new local client receives an explicit busy result; the client helper does not wait or queue for later admission.

The Windows local-pipe CTest also has a 20-second test-level timeout. That fuse is acceptance-harness defense in depth: it ensures a newly introduced deadlock becomes an explicit failed test instead of an indefinitely running CI job. It does not replace transport deadlines or make a blocking design acceptable.

## Host-owned BrokerSessionId

A successful server-side accept creates a fresh nonzero 128-bit `BrokerSessionId` using the Windows system-preferred CSPRNG. Assignment is serialized only around in-memory session state; the pending pipe accept itself is not globally serialized. The candidate is checked against every connected, retiring, or poisoned slot and regenerated on collision. The session identity is retained only on the server-side connection slot and is never transmitted in protocol frames.

This implements `MEM-20260916T042800Z-B71E3C` and the later competing-session correction: transport connection identity is established by Host, not asserted by a client, and equality of client-controlled `ExecutionId` does not make two Host sessions equivalent. Future dispatch will inject this exact session into `AuthorityBroker::acquire`, operation admission/completion, release, and disconnect calls.

## Fail-closed session retirement

A normal pipe slot lifecycle is `Listening -> Accepting -> Connected -> Retiring -> Listening`. Once a Host-owned session has been accepted, transport loss is not allowed to erase that identity immediately.

Broken pipe, empty message, oversized message, timeout, read/write failure, or explicit server disconnect first attempts to disconnect the Windows pipe instance while preserving its `BrokerSessionId`. Only a successful disconnect, or the explicit OS state `ERROR_PIPE_NOT_CONNECTED`, may establish `Retiring`. A retiring slot rejects a new accept with `retirement_required`; it cannot be silently reused for another caller.

The future dispatcher must read the preserved session identity, use it to finish authority-side disconnect/reconciliation handling, and only then call `retire(slot)`. Retirement clears the old session and returns the pipe instance to `Listening`. This ordering prevents a transport slot from admitting a replacement connection before the authority layer has accounted for the previous connection's uncertain lease/work. A replacement connection always receives a fresh Host-owned session identity.

If the transport cannot prove that the Windows pipe instance was disconnected cleanly, the slot enters `Poisoned` instead of `Retiring`. A poisoned slot preserves any published Host session identity and still exposes that identity through `session()` for authority cleanup, but rejects further I/O, accepts, and retirement. It remains unavailable until the server is replaced. This intentionally sacrifices bounded capacity rather than reusing an OS handle whose connection state is uncertain.

A connection that fails before a Host session is published is returned to `Listening` only after the same clean-disconnect proof. If that proof fails, the unpublished slot is likewise poisoned rather than reused.

This checkpoint does not itself call `AuthorityBroker::disconnect`; it proves the lifecycle contract needed for the next dispatch layer to do so truthfully.

## Bounded message I/O

Each named-pipe message is read through overlapped I/O into a fixed `ProtocolFrame` buffer. A complete message larger than the 256-byte protocol cap causes `ERROR_MORE_DATA`; after clean transport retirement the server returns `message_too_large` rather than publishing or dispatching a prefix. Empty messages, deadline expiry, and broken/disconnected pipes likewise preserve the transport session for upper-layer authority cleanup. Successful reads publish the candidate frame only after one complete bounded pipe message has been received.

Writes reject zero-length or over-cap `ProtocolFrame` values before invoking Windows I/O. Timed-out or failed writes preserve the server-side session for authority cleanup and never permit immediate slot reuse under uncertainty; an unexpected pipe-disconnect failure poisons the slot.

The transport does not call `inspect_protocol_frame`; codec validation remains the next layer. This separation ensures malformed application messages cannot be partially dispatched merely because the underlying pipe delivered them successfully.

## Acceptance evidence expected from this checkpoint

Windows tests must demonstrate a protected owner-only DACL, duplicate-server rejection, bounded accept timeout with no client, four simultaneously accepted local connections, four distinct nonzero Host-owned session identities, explicit saturation at the fifth connection, message exchange through all normal connections, oversized-message rejection without partial publication, preserved session identity while the failed slot is retiring, refusal to reaccept before explicit retirement, a fresh session identity after retirement/reuse, preserved session identity after peer disconnect until retirement, a silent connected client timing out rather than blocking Host indefinitely, and injected invalid-handle failure proving an uncertain disconnect poisons the slot, preserves the published session for cleanup, and cannot be retired or reaccepted. Linux and macOS continue to compile the platform-neutral API through the explicit unsupported implementation path.

## Concurrency contract

Connection slots are independent and may be accepted concurrently. The transport state mutex protects slot/session metadata, not arbitrary protocol processing. The intended dispatcher owns at most one active read/write flow per connected slot; Batch 000 does not promise concurrent overlapping I/O calls on the same slot. Explicit disconnect may race a pending read: cancellation and state transition are serialized so later transport use observes a retiring or poisoned result rather than reopening the slot.

## Historical rejected transport heads

`d556587900fb5323bcb7316f519ba90a09e17979` introduced the first transport candidate but its Windows test did not compile because a local frame-inspection helper was missing. `e0ce63742034f20c71d24e9ee2d4078e220553bb` repaired only that test helper; Windows then compiled but Debug `ctest` remained stuck in the synchronous-I/O design and exact run `35058068291` was cancelled. Neither head is accepted transport evidence.

## Out of scope

This checkpoint does not implement AuthorityBroker dispatch, lease generation, replay-digest derivation, protected `NoOp`, authority-side disconnect/reconciliation wiring, Runtime/DCR adapters, arbitrary filesystem/Git/process operations, recovery, or WebAuthn. Normal IPC still has no `Reconcile` surface. Mutating DCR remains disabled.
