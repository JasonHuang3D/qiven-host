# qiven-host Engineering Protocol

This directory contains the shared repository-level engineering protocol used by `jason-worker` and by humans reviewing Work-mode implementation.

Repository architecture remains repository-owned. These documents define **how** implementation work is performed safely and consistently; they do not replace domain or architectural contracts.

## Documents

- `build-system-standard.md` — Devkit-owned native CMake mechanism, repository-owned build graph, and dependency rules.
- `implementation-standard.md` — implementation quality, scope discipline, C++ design, dependency, portability, and review rules.
- `testing-standard.md` — semantic test design, validation profiles, and local evidence requirements.
- `worker-protocol.md` — Work-mode branch, batch, commit, validation, stopping, blocker, and handoff procedure.
- `feature-spec.md` — contract used by `jason-brother` to assign implementation-ready work to `jason-worker`.

## Roles

### jason-brother

CTO, architect, reviewer, feature planner, GitHub/CI reviewer, and merge/release gate. The CTO decides what should be built, the architectural contract, the feature order, and whether a result is accepted.

### jason-worker

Work-mode implementation engineer. The worker converts approved feature specifications into high-quality local code, tests, and coherent commits. It does not push or merge by default and does not independently expand architecture.

## Normal flow

```text
Chat / jason-brother
    define ordered feature queue
            |
            v
Work / jason-worker
    feature A -> local validation -> local commit
        |
        v
    feature B -> local validation -> local commit
        |
        v
    STOP + structured handoff
            |
            v
Chat / jason-brother + user
    remote review -> CI as required -> merge/release gate
```

The Work batch is intentionally local. This separates high-throughput implementation from cross-platform validation and release authority.

## Instruction precedence

Use the precedence defined by the root `AGENTS.md`. A feature specification may specialize ordinary implementation details for one feature, but it may not silently override repository-wide architecture or safety rules. Deliberate exceptions must be explicit.

## Protocol evolution

These documents are expected to evolve when evidence shows that the existing protocol failed to prevent a recurring class of mistake.

When a process failure occurs, ask:

1. Was the feature specification ambiguous?
2. Was a shared engineering rule missing?
3. Was the rule present but too vague to be operational?
4. Was it contradicted by another instruction?
5. Should the rule become an automated check instead of prose?

Fix the underlying protocol or detector when appropriate rather than relying on the same warning being remembered manually in future sessions.
