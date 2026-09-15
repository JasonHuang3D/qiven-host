# BrokerInstanceGuard

`BrokerInstanceGuard` prevents two Host broker processes for the same Windows owner from simultaneously existing as accepted broker instances. It is a process-lifetime singleton primitive, not the per-request authority mutex and not a replacement for lease/fencing.

The Windows implementation creates a named mutex object in the global named-object namespace. The object name includes the current process-token user SID, so distinct Windows owners do not collide. The first process keeps the returned kernel-object handle open for the entire guard lifetime. A later process that observes `ERROR_ALREADY_EXISTS` deterministically returns `already_running` and closes its handle without disturbing the accepted broker.

The guard deliberately uses named-object **existence**, not mutex thread ownership. It therefore does not depend on one broker thread remaining the mutex owner and does not call `ReleaseMutex`; closing the last accepted guard handle ends the singleton lifetime. Process termination also releases the kernel handle naturally.

The default Windows token security descriptor remains the access boundary for the named kernel object. Name/SID scoping prevents accidental cross-owner collision but is not claimed as a defense against arbitrary malicious code already running with unrestricted control under the same user token. A foreign or malformed pre-existing named object causes fail-closed acquisition failure rather than a second accepted broker.

Batch 000 acceptance requires a real Windows multiprocess race in which all contenders attempt acquisition while the winner remains alive and exactly one process obtains the guard. This singleton only constrains broker-process multiplicity; execution authority still requires the broker's in-process state mutex, lease, persistent fencing epoch, sequence/idempotency checks, quarantine, and recovery semantics.