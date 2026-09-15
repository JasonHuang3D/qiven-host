# BoundedJournal

`BoundedJournal` is bounded durable evidence for Host authority and protocol events. It is not the authority source of truth: `FenceStore` and the broker's validated in-memory state own authority admission, while the journal records enough evidence to diagnose and reconcile uncertain execution.

The journal retains exactly two fixed-capacity segments. Each segment has an explicit fixed-width header and up to 64 fixed-width records. The maximum retained file bytes are therefore deterministic and hard-bounded at 34,976 bytes. Rotation overwrites only the inactive older segment after the active segment is full.

Every record carries a monotonic journal index, current authority generation/fence/request sequence, bounded digests for execution/lease/session/operation identity, event/outcome categories, the previous record digest, and a SHA-256 digest of the current record prefix. The chain detects accidental or adversarial modification only as integrity evidence; it is unkeyed and is **not authentication**. CRC32 separately detects serialization/storage corruption.

Windows durability uses native file I/O with write-through intent, `FlushFileBuffers`, close, and read-back/full-scan verification. A partially written or malformed header/record fails closed as `corrupt`; the implementation does not silently truncate to a previous valid prefix. An already-created journal root without a valid initial segment is also corrupt rather than a fresh initialization opportunity.

Rotation creates the replacement segment header first with a new internal segment generation, the next monotonic record index, and the previous journal-head digest as its anchor. A crash after that header is durable but before its first record is valid and preserves the previous head through the anchor. Once a second generation exists, deleting or corrupting either retained segment is fail-closed because continuity can no longer be proven.

Wall-clock time is stored only for diagnostics. Authority ordering comes from fencing, request sequence, journal index, and broker state; wall-clock values never determine ownership or validity.

`BoundedJournal` intentionally does not own a mutex or interprocess singleton. Host broker admission serializes mutation before calling it. Batch 000 tests exercise corruption, truncation, restart/reopen, bounded rotation, missing retained history, interrupted bootstrap, and deterministic byte bounds. Production Runtime/DCR side-effect reconciliation remains Batch 001 work.