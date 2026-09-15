# Qiven Native Build-System Standard

This repository consumes the Qiven native build-system snapshot managed by qiven-devkit.
The snapshot lives under `cmake/qiven/` and is repository-local after generation or sync.
Repositories never call back into a live Devkit checkout during configure, build, or test.

## Ownership

Devkit owns shared CMake mechanism and compiler policy.
The repository owns its target graph, source lists, public dependency edges, feature options,
and architecture-specific build decisions.

Do not copy shared warning flags, C++ language policy, dependency-discovery algorithms,
or common test-target policy into repository CMake files. Improve Devkit and sync the snapshot.

Do not turn the shared modules into a second build language. Root and subdirectory
`CMakeLists.txt` files should remain idiomatic CMake using `add_library`, `add_executable`,
`target_sources`, `target_link_libraries`, and `add_subdirectory` where appropriate.

## Build-system API compatibility

Every managed snapshot declares a Qiven build-system API version. A single CMake configure graph may compose multiple repository snapshots only when they declare the same API. The first compatible snapshot loaded owns the shared function implementation for that configure graph; later compatible snapshots reuse it. An incompatible API is a hard configure error.

Template/repository release versions and build-system API versions are intentionally separate. Any incompatible change to shared CMake function names, arguments, failure semantics, or policy must advance the build-system API rather than silently changing behavior under the same compatibility contract.

## Required root pattern

After `project(...)`, add the managed module directory and load `QivenBuild`:

```cmake
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qiven")
include(QivenBuild)
qiven_project_defaults()
```

Apply `qiven_target_defaults()` to every first-party compiled target. Select `SCOPE PUBLIC`
when consumers require the target's C++20 compile feature, `PRIVATE` for executables or
implementation-only requirements, and `INTERFACE` for interface libraries.

Repository code may add stricter target-specific flags. It must not weaken the shared baseline merely to make a compiler green. Platform-specific definitions belong with the target or backend that actually requires them, not in global flags. Exception, RTTI, sanitizer, SIMD, LTO/IPO, CRT-linkage, and similar ABI/performance policy must not be hidden in generic defaults until an accepted Qiven architecture decision assigns that policy to the shared build-system layer.

## Source dependencies

Qiven native repositories compose approved source dependencies locally; configure must not
silently download another Qiven repository from the network.

Use `qiven_resolve_source_dependency()` for the shared resolution mechanism. The consuming
repository still declares the semantic dependency, required target, root cache variable,
default sibling checkout, verification paths, and child test option.

CI must checkout each Qiven dependency at an immutable approved commit SHA and pass or arrange
its source root deterministically. A moving dependency branch is not a release or CI baseline.

If a lower dependency lacks a required semantic capability, improve that lower owner first.
Do not clone the capability into the consumer just to avoid a dependency edge.

## Tests

Test executables are ordinary CMake targets. Apply `qiven_test_target_defaults()` and register
tests with `qiven_register_test()` when its semantics fit. Keep fixture-specific wiring in the
repository; shared test policy stays in Devkit.
