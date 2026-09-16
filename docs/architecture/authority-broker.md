# AuthorityBroker

`AuthorityBroker` is the Batch 000 orchestration boundary that composes `AuthorityState`, `FenceStore`, and `BoundedJournal` under one primary in-process mutex. It does not make the two durable stores an atomic filesystem transaction. Instead, every ordering is chosen so that partial durable success remains fail-closed, and any required persistence failure latches the running broker closed until restart or later authorized recovery.

Fresh bootstrap initializes durable authority as generation 1 / fence 1 / `Ready`, then creates the journal. If only one durable substrate is initialized on a later start, Host reports inconsistent persistence rather than silently manufacturing the missing half. Corrupt or unreadable durable state also fails closed.

Restart never resurrects an ephemeral lease. Persisted `Ready` restores as `Ready`; persisted `Leased` is immediately converted and durably rewritten to `Reconciling`; persisted `Reconciling` remains `Reconciling`; persisted `Quarantined` remains `Quarantined`. A `Leased` restart transition is journaled as uncertain because the prior process may have admitted or executed work whose terminal result is not known.

Authority acquisition persists `Leased` before it can return a usable lease. Operation admission is durably journaled before the caller can treat an operation as admitted. Terminal operation evidence is journaled before a later release can return the host to `Ready`. A normal release uses conservative ordering: first journal an uncertain authority transition while the durable fence still says `Leased`, then persist `Ready`, then journal successful completion of the transition. Therefore failure before the fence update restarts as `Reconciling`, while failure after the durable `Ready` update still leaves prior transition evidence and causes the current broker lifetime to fail closed.

Transitions to `Reconciling` or `Quarantined` persist the fail-closed authority state before journal evidence is appended. A journal failure after that point cannot reopen authority. Rejected protocol/authority attempts are journaled; inability to create required evidence is itself a fail-closed broker fault.

`AuthorityState::complete` authenticates the original broker session, execution, lease, fence, request sequence, operation identity, and digest before consuming an in-flight request. The original owner may still complete an already-admitted request to its safe boundary after a competitor has forced `Quarantined`; the competing caller cannot steal completion authority.

The authority mutex is held only while validating and durably recording authority transitions. Future protected test operations execute outside this mutex between `begin_operation` and `finish_operation`, allowing a competing request to reach the authority state and trigger quarantine while work is in flight. This checkpoint does not implement the protected operation itself.

This layer is Windows-owner-local Batch 000 infrastructure. It intentionally does not implement named-pipe protocol/IPC, Runtime or DCR production adapters, external filesystem/Git/process side effects, normal-protocol recovery, or Windows Hello/WebAuthn recovery. Those boundaries remain in their accepted later checkpoints; production no-bypass proof remains Batch 001.
