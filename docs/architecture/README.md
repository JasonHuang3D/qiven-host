# Qiven Host Architecture

Architecture notes in this directory record Host-owned implementation contracts that are narrower than the cross-Qiven ADRs in qiven-context. They do not override canonical qiven-context decisions.

Current Batch 000 implementation contracts:

- `fence-store.md` — durable authority-state substrate and fail-closed fencing persistence.
- `bounded-journal.md` — bounded durable evidence, integrity chaining, rotation, and corruption semantics.
- `broker-instance-guard.md` — owner-scoped Windows broker singleton and multiprocess race semantics.
- `authority-broker.md` — orchestration, persistence ordering, restart conservatism, and persistence-fault latching.
- `protocol-codec.md` — bounded versioned normal-protocol wire representation and canonical decode rules.
