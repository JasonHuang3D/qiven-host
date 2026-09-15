# CTO Feature Specification Contract

This document defines how `jason-brother` should assign implementation-ready work to `jason-worker`. The purpose is not to micromanage code; it is to communicate architectural intent and semantic boundaries strongly enough that the worker can exercise senior local engineering judgment without guessing public contracts or expanding scope.

## 1. Required batch header

A Work request should normally establish:

```text
WORK BATCH
Base branch:
Base commit: <optional when branch state is authoritative>
Maximum features:
Usage policy:
Batch-final validation:
Remote policy: local only; no push/merge/PR

AUTHORIZED FEATURE QUEUE
1. <feature A>
2. <feature B>
3. <feature C>

STOP AFTER <last authorized feature>.
```

Feature dependency order must be explicit. Do not make the worker infer whether later work starts from `main` or from an earlier local feature branch.

## 2. Required feature fields

For each feature, define the relevant subset of:

```text
Feature:
Branch:
Base:
Intent:
Required semantics:
Public API:
Ownership/lifetime:
Failure semantics:
Performance/cost constraints:
Platform constraints:
Expected integration:
Required tests:
Validation profile:
Focused validation:
Out of scope:
Stop conditions:
Documentation:
Architectural barrier:
Notes:
```

Use `N/A` when a contract-sensitive field is deliberately irrelevant instead of leaving it accidentally ambiguous.

## 3. Intent and required semantics

Intent should explain why the feature exists in a short paragraph. Required semantics should state externally meaningful behavior and invariants, not prescribe line-by-line implementation.

A strong specification identifies things such as:

- success and valid empty states;
- failure behavior;
- ownership/lifetime and cleanup responsibility;
- moved-from semantics when ownership moves;
- allocator/backend provenance where relevant;
- whether hidden allocation, synchronization, virtual dispatch, or exceptions are allowed;
- portability and platform exposure boundaries;
- observable compatibility requirements.

Ordinary private implementation choices that preserve these semantics remain the worker's responsibility.

## 4. Public API

When public naming or API shape is architecturally important, specify it or constrain the allowed shape. If public contract design is deliberately unresolved, mark the feature as requiring CTO review before implementation rather than encouraging the worker to invent it.

Do not ask the worker to choose between `assert`, optional/result/error code, exception, termination, or silent fallback when that choice changes public failure semantics.

## 5. Ownership and lifetime

For ownership-related work, define what is owned and borrowed, nullable/empty states, what must outlive what, cleanup responsibility, move behavior, backend/allocator provenance, and raw-storage versus typed-object lifetime when applicable.

If these points materially affect the contract and are not decided, the feature is not ready for autonomous implementation.

## 6. Failure semantics

Classify meaningful failure channels. Distinguish caller programming errors, invariant violations, recoverable runtime failure, and environmental failure. State whether failure leaves output/state unchanged when that matters.

Do not leave the worker to invent a failure category because the happy path is obvious.

## 7. Performance and cost

State only concrete constraints that matter, for example:

```text
- no hidden allocation;
- O(1) move;
- no synchronization;
- no virtual dispatch;
- bounded temporary storage;
```

Avoid vague demands such as "high performance" without naming the controlled cost.

## 8. Platform constraints

State special cross-platform requirements when a feature touches native facilities. Public contracts should remain platform-neutral unless native exposure is intentional and approved.

A platform-sensitive feature is a likely architectural barrier when equivalent semantics or ownership cannot be guaranteed locally.

## 9. Expected integration

List required repository integration when useful, such as public-header registration, implementation-source registration, header self-checks, dedicated tests, generated metadata, or architecture documentation.

A correct implementation file that is not integrated into the repository is not a complete feature.

## 10. Required tests

Specify semantic risks rather than only test filenames. Name success, boundary, failure, ownership/move, overflow/alignment, state-preservation, regression, and portability-sensitive cases when they are part of the contract.

The worker may add another directly relevant test when it exposes a meaningful risk, but should not inflate test count mechanically.

## 11. Validation profile

Every source feature should explicitly state:

```text
Validation profile:
FULL
```

or:

```text
Validation profile:
FOCUSED

Focused validation:
<exact build targets, tests, selectors, or commands>
```

`FULL` is the default when omitted. `FOCUSED` is appropriate only when the affected surface is known well enough to define exact validation. A batch containing focused feature validation should normally require batch-final `FULL` validation.

## 12. Out of scope

Explicitly name adjacent work that must remain unchanged. This is a primary defense against opportunistic redesign, cleanup, dependency additions, format churn, and speculative abstractions.

A feature scope is a boundary, not a minimum.

## 13. Stop conditions

List conditions that require escalation, such as:

- an upstream/public contract must change;
- a new failure abstraction appears necessary;
- platform behavior cannot be made equivalent;
- ownership/lifetime cannot be expressed under the approved contract;
- a third-party dependency appears necessary;
- compiler/build/CI architecture must change;
- an earlier frozen stack layer proves semantically wrong.

Repository-wide stop conditions from `AGENTS.md` always apply even if not repeated.

## 14. Documentation

Say whether architecture or engineering documentation should change. Do not force documentation churn for trivial private implementation details, but update durable architecture when a public architectural contract changes.

## 15. Architectural barrier

Use:

```text
Architectural barrier:
YES
```

when dependent features should wait for Chat/GitHub/CI review before stacking continues.

Use `NO` only when the CTO has consciously decided local stacking is safe. If omitted for an obviously cross-cutting feature, the worker should conservatively stop after completing it.

## 16. Specification quality check

Before assigning a batch, verify:

- feature dependencies and bases are explicit;
- public semantics are decided;
- ownership/lifetime and failure behavior are decided where relevant;
- performance/platform constraints name real boundaries;
- out-of-scope work is explicit;
- high-risk test cases are named;
- validation profile is explicit enough to execute;
- focused validation has exact scope;
- batch-final validation policy is clear;
- architectural barriers are identified;
- the queue is finite and reviewable;
- a senior engineer can tell when to stop without needing trivial implementation instructions.

If missing information affects architecture or externally observable behavior, improve the specification or stop for CTO review before spending implementation budget.
