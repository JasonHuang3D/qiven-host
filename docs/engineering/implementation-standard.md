# Implementation Standard

This document defines implementation quality expected from `jason-worker`. Read it with the root `AGENTS.md`, the current CTO feature specification, and applicable repository architecture.

## 1. Engineering objective

Optimize for correctness and durable simplicity, not maximum code production. Important properties should be visible: ownership, lifetime, failure behavior, runtime cost, allocation, synchronization, platform boundaries, and invariants.

Do not hide meaningful cost or failure behind convenience APIs.

## 2. Scope discipline

The CTO feature specification defines the permitted change surface. Before editing, identify required behavior, allowed public contract changes, expected modules/files, explicit out-of-scope work, stop conditions, and validation profile.

Distinguish work required for correctness/integration from unrelated improvement opportunities. If an unrelated defect blocks the feature, report it; do not silently broaden scope.

## 3. Minimal sufficient abstraction

Prefer the smallest abstraction that completely expresses the required semantics. Do not build generalized infrastructure for hypothetical future needs.

Registries, plugin systems, generic factories, reflection, serialization frameworks, custom containers, portability wrappers, traits frameworks, or allocator hierarchies require concrete justification.

## 4. API quality

Public APIs should be small, explicit, difficult to misuse, clear about ownership/lifetime/failure, predictable in cost, and portable unless intentionally platform-specific.

Avoid convenience overloads without concrete need, Boolean parameters that obscure meaning, and public implementation details. If public naming or API shape materially changes the contract and the feature specification did not decide it, request CTO review.

## 5. Ownership and lifetime

For every pointer, reference, span, handle, allocator/backend reference, and resource owner, know whether it is owning, borrowing, nullable, empty-but-valid, lifetime-bound, or required to outlive another object.

Move-only ownership types require deliberate handling of moved-from state, destination cleanup, provenance, self-move when relevant, destruction after move, and valid empty states.

Raw storage ownership and C++ object lifetime are separate responsibilities. Do not silently construct/destroy typed objects inside a raw-storage abstraction unless the contract explicitly owns object lifetime.

## 6. Arithmetic, ranges, and conversion safety

Code handling byte sizes, offsets, counts, alignments, ranges, indices, or address arithmetic must reason about overflow, underflow, narrowing, sign changes, invalid alignment, and pointer-width versus fixed-width values.

Use established checked primitives when they fit. Do not cast solely to silence a warning. If arithmetic failure semantics are not defined, do not invent them locally.

## 7. Error and contract handling

Do not conflate caller programming errors, invariant violations, recoverable runtime failures, and environmental failures. Assertions are not substitutes for recoverable error channels.

Do not swallow failures, convert them into silent defaults, invent ambiguous sentinels, or introduce exceptions into a non-throwing contract. If failure category/channel affects the public contract and is unclear, escalate.

Use `noexcept` only when the full implementation can uphold it as a real semantic guarantee.

## 8. Standard library and generic code

Use the standard library when it provides the required semantics without unacceptable hidden cost or dependency impact. Do not reimplement standard functionality for style, but do not choose a high-level abstraction that violates allocation, ownership, exception, RTTI, or runtime-cost constraints.

Templates, concepts, type erasure, inheritance, and metaprogramming must earn their complexity. A single feature should not create a framework for hypothetical future types.

## 9. Platform boundaries

Portable code should remain platform-neutral. Platform-specific branches should be narrow and explicit. Prefer the least invasive native dependency and keep native headers out of public APIs unless exposure is an intentional contract.

Do not assume Windows behavior proves Linux/macOS behavior, or vice versa. Verify documented semantics where platform equivalence matters.

## 10. Public headers and includes

Public headers must be self-contained according to repository policy. Include what they directly require; do not rely on accidental transitive includes. Avoid heavy implementation dependencies and platform leakage.

When a new public header is added, update the repository's header-check/build registration when required by existing convention.

## 11. Naming, comments, and formatting

Follow established naming unless the feature specification deliberately changes it. Names should communicate semantics, not implementation history.

Comments are for non-obvious intent, invariants, platform quirks, ownership/lifetime constraints, or important trade-offs. Do not narrate syntax or write tutorial essays in source files.

Repository `.clang-format` is authoritative. Use `tools\format.cmd` and `tools\format-check.cmd`, then inspect the actual diff. Never modify formatting policy as a side effect of an unrelated feature.

## 12. Build configuration

CMake is the native build-system source of truth. Shared build mechanism and compiler policy come from the Devkit-managed `cmake/qiven/` snapshot; repositories own their semantic target/source/dependency graph. Do not duplicate shared warning flags, language policy, source-dependency resolution, or common test policy in repository CMake. Improve Devkit when shared mechanism is missing, then sync the managed snapshot. Do not hand-maintain divergent IDE configuration when CMake should express the setting.

## 13. Performance and concurrency

When performance materially affects design, identify the cost being controlled. Prefer predictable complexity, avoid hidden allocation, virtual dispatch, and synchronization unless required, and keep hot-path work explicit.

Thread-safety is part of the contract. Do not add locks "just in case". If correctness requires a new concurrency contract, stop for CTO review.

## 14. Diff quality

Before commit, inspect the full diff and remove temporary diagnostics, debug output, commented experiments, unrelated formatting, generated build products, local paths, accidental renames, and speculative TODO implementations.

A feature diff should tell one coherent story.

## 15. Done means understood

Do not consider a feature complete merely because it compiles. Before handoff, be able to explain the invariant, ownership/lifetime behavior, failure behavior, meaningful edge cases, what was tested, what local validation could not prove, and why the implementation stays inside architecture and scope.
