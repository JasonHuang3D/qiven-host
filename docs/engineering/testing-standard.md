# Testing Standard

Tests exist to validate semantics and protect architectural contracts. The goal is not maximum test count; it is strong coverage of plausible ways a feature could be subtly wrong.

## 1. Tests are part of implementation

Design tests while reasoning about the contract, not after production code as a checkbox. For each feature identify primary success behavior, boundaries, failure states, ownership/lifetime transitions, arithmetic hazards, platform-sensitive assumptions, and invariants a plausible incorrect implementation could violate.

## 2. Test the contract, not implementation trivia

Prefer tests that survive reasonable internal refactors. Do not overfit to private helper structure, incidental call order, internal representation, or behavior not promised by the contract.

Inspect backend/resource interactions only when provenance, cleanup, or another interaction is itself part of the contract.

## 3. Risk families

When applicable, cover:

- successful acquisition/use/destruction;
- allocation or acquisition failure;
- zero/empty success distinct from failure;
- move construction/assignment and moved-from state;
- destination cleanup and backend/allocator provenance;
- minimum/maximum values and exact boundary success;
- one-past-boundary failure, overflow, underflow, narrowing, sign changes;
- alignment boundaries and invalid alignment according to contract;
- exact/truncated/empty byte ranges and state after failure;
- deterministic regression cases for defects already found.

Do not assume one happy-path test proves ownership or failure correctness.

## 4. Determinism and isolation

Tests must be deterministic, isolated, readable, and capable of failing for the defect they protect against. Do not depend on test order, mutable developer-machine state, network availability, unpinned external data, or another test's leftovers.

Filesystem/tooling tests should use disposable fixtures and verify cleanup where that behavior matters.

## 5. Contracts and undefined behavior

Do not write tests that deliberately depend on undefined behavior merely to prove an assertion exists. When behavior differs between assert-enabled and assert-disabled configurations, make that distinction explicit and consistent with repository policy.

Do not weaken a contract to make it easier to test.

## 6. Public-header checks

New or materially changed public headers must remain self-contained according to repository policy. Update header-check/build registration when required. A header compiling only because another translation unit included prerequisites first is insufficient.

## 7. Local validation profiles

Two Work-mode local validation profiles exist.

### FULL

`FULL` is the default. It includes repository-required Debug and Release build/test coverage plus formatting, diff inspection, and `git diff --check`.

For the standard generated C++ library workflow, configure/generate as needed with:

```cmd
tools\gen-vs2022-x64.cmd
```

Then run the repository's Debug and Release build/test presets.

### FOCUSED

`FOCUSED` is a CTO-authorized optimization, not a worker-selected shortcut. It may be used only when the current feature specification provides exact build targets, tests, selectors, or commands. Focused validation must exercise the authorized scope in both Debug and Release configurations unless the specification explicitly says otherwise.

If the focused scope is missing, ambiguous, or becomes insufficient because implementation broadens the affected surface, use `FULL` or stop for CTO review.

Architectural barriers and cross-cutting infrastructure changes normally require `FULL`.

### Batch-final full validation

When any feature in a batch uses `FOCUSED`, the final stack tip normally receives one complete repository-level FULL Debug/Release validation before normal handoff. This preserves repository confidence while avoiding repeated full-suite execution after every small stacked feature.

Cross-platform CI remains an independent later gate.

## 8. Formatting validation

Formatting scripts operate on files visible through `git ls-files`. When a feature creates new C/C++ files, expose only those new files without staging their contents:

```cmd
git add -N -- <each new C/C++ file created by this feature>
tools\format.cmd
tools\format-check.cmd
```

Do not use a broad `git add -N .` merely to make the formatter discover new files. Inspect the diff afterward because a formatter can legitimately change more text than expected.

## 9. Documentation-only changes

Documentation-only work does not automatically require full Debug/Release compilation unless the feature specification requires it, build/configuration commands changed, documentation is programmatically validated, or another applicable contract requires it.

Still inspect diffs and verify paths/commands against the repository.

## 10. Failed validation

When required local validation fails:

1. diagnose the failure;
2. fix it if the fix is inside current scope;
3. rerun the failed validation;
4. rerun any validation invalidated by the fix.

A failure already present on the clean authorized base is `BASELINE_VALIDATION_BLOCKED`, not a feature implementation failure. If resolving a failure requires architecture or unrelated scope expansion, stop and escalate.

## 11. Never weaken the detector

Do not obtain a pass by disabling/commenting tests, reducing assertions, suppressing sanitizers, lowering warning levels, changing compiler flags, excluding failing targets, adding unsafe casts merely to silence diagnostics, or reducing test scope without authorization.

If a detector is actually wrong, report evidence and fix the detector under explicit scope.

## 12. Regression fixes

A bug fix should normally add a regression test that would fail for the defective behavior and pass for the correction. If a deterministic regression is impractical, record why.

## 13. Cross-platform limits

Local validation proves only the local toolchain/configuration actually exercised. It does not silently prove GCC/Clang/AppleClang compatibility, Linux/macOS APIs, architecture variants, sanitizer cleanliness, exception/RTTI configurations, or another unexecuted matrix dimension.

Do not claim unexecuted environments are verified.

## 14. Human-facing test runner output

Test entry points such as `test.cmd`, `test.sh`, or equivalent repository-level runners are human-facing engineering tools. Their output quality is part of maintainability and operational reliability.

Prefer a compact structured layout with stable status tags such as `[ RUN]`, `[WAIT]`, `[ OK ]`, and `[FAIL]`. Use color when the terminal supports it, while retaining a deterministic plain-text path for CI, redirected output, and explicit no-color operation.

A runner should normally:

- identify the repository/ref or exact HEAD when practical;
- acknowledge suite start immediately;
- show truthful suite/milestone progress during meaningful waits;
- provide a concise final summary;
- buffer noisy expected-failure subprocess output and print detailed logs only when the enclosing suite actually fails, unless verbose output was explicitly requested;
- make unexpected failures visually obvious and include the evidence needed to diagnose them;
- avoid fake percentages, fabricated ETA, decorative animation, or output that obscures the actual process state.

For longer-running runners, emit a truthful heartbeat when a reasonable silent interval would otherwise make the process appear hung. Progress output must report real state, not invented progress.

Where supported, provide explicit controls analogous to `QIVEN_TEST_VERBOSE=1` and `QIVEN_TEST_NO_COLOR=1` rather than forcing either maximum verbosity or ANSI output on every environment.

## 15. Future incremental validation

The repository may later add labels, affected-target analysis, caching, sharding, or other incremental mechanisms. Until such mechanisms are explicit and validated, `FOCUSED` is the only worker-level route for reducing per-feature validation scope. Test-latency optimization must preserve confidence rather than merely reduce elapsed time.
