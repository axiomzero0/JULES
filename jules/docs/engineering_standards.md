# JULES Compiler Engineering Standards

**Status:** Mandatory. Non-negotiable. Enforced by CI.
**Scope:** All C++26 code in `compiler/`, `runtime/`, `tools/`, `tests/`
**Enforcement:** Automated where possible. Human review where not. Violations block merge.

---

## Part I: Language Subset Restrictions

JULES is built in C++26, but **not all of C++26 is permitted.** The following restrictions are enforced by compiler flags, clang-tidy, and CI static analysis.

### Standard 1 — Forbidden Language Features

The following are **banned project-wide.** No exceptions. No waivers.

| Feature | Reason | Enforcement |
| :--- | :--- | :--- |
| C++ exceptions (`throw`, `try`, `catch`) | Unpredictable codegen, hidden control flow, binary size | `-fno-exceptions` compile flag. CI grep for `throw`/`try`/`catch` |
| RTTI (`dynamic_cast`, `typeid`) | Virtual dispatch overhead, hidden allocations | `-fno-rtti` compile flag. Clang-tidy check |
| `std::shared_ptr`, `std::weak_ptr`, `std::unique_ptr` (in hot paths) | Atomic overhead, heap allocation, indirect ownership | Clang-tidy custom check. Arena ownership model instead |
| `std::function` | Heap allocation, type erasure overhead, indirect call | Clang-tidy check. Use function pointers or templates |
| `std::string` in IR/passes | Allocation, locale, encoding overhead | `SymbolId` only. Clang-tidy check |
| `std::unordered_map`, `std::map`, `std::set` | Node-based allocation, pointer chasing, poor cache | Custom flat hash map required. Clang-tidy check |
| `std::vector<bool>` | Bit-packing breaks reference semantics | Clang-tidy check. Use `BitVector` |
| Global constructors/destructors | Initialization order fiasco, hidden side effects | `-Wglobal-constructors` warning as error |
| `reinterpret_cast` outside backend | Type punning UB, optimizer confusion | Clang-tidy check. `std::bit_cast` where needed |
| C-style casts `(int)x` | Silent, invisible, dangerous | Clang-tidy check. Use `static_cast`, `bit_cast` |
| `goto` | Unstructured control flow | Clang-tidy check |
| `setjmp`/`longjmp` | C exceptions in disguise | Clang-tidy check |
| `volatile` (except FFI/MMIO) | Broken semantics in C++, not atomic, not ordered | Clang-tidy check. Explicit atomics required |
| `register` keyword | Meaningless since C++17 | Clang-tidy check |
| `union` (except type-punning with `std::bit_cast`) | Active member tracking is manual, error-prone | Clang-tidy check. Use `std::variant` or tagged structs |
| Implicit conversions (single-arg constructors without `explicit`) | Silent type confusion | All single-arg constructors MUST be `explicit` |
| Operator overloading outside IR node types | Surprise semantics | Clang-tidy check. Only IR arithmetic nodes may overload |

**Consequence:** Code containing any forbidden feature does not compile. CI fails. PR cannot merge. No discussion.

---

### Standard 2 — Mandatory Compiler Flags

Every translation unit in the compiler and runtime MUST be compiled with:

```cmake
# Warnings as errors - all of them
-Wall -Wextra -Wpedantic -Werror

# Specific brutal warnings
-Wconversion          # Implicit narrowing
-Wsign-conversion     # Signed/unsigned mismatch
-Wshadow              # Variable shadowing
-Wnon-virtual-dtor    # Polymorphic base without virtual dtor
-Wold-style-cast      # C-style casts
-Woverloaded-virtual  # Hidden virtual functions
-Wformat=2            # Printf-style format checking
-Wnull-dereference    # Potential null deref
-Wdouble-promotion    # float -> double implicit
-Wundefined-func-template
-Wconditional-uninitialized
-Wunreachable-code-aggressive
-Wunused              # All unused warnings

# Hardening
-fstack-protector-strong
-fsanitize=undefined  # In debug/CI
-fno-exceptions
-fno-rtti
-fstrict-aliasing
-fno-math-errno       # We handle FP errors explicitly
```

**Debug builds additionally:**
```cmake
-fsanitize=address,undefined
-DJULES_DEBUG=1
-DJULES_VERIFY_IR=1      # Graph verifier after every pass
-DJULES_TRACK_ALLOCS=1   # Arena allocation tracking
```

**Release builds additionally:**
```cmake
-O3
-march=native            # For development. CI uses explicit target
-flto=thin
-fno-plt                 # Direct calls, no PLT indirection
-fvisibility=hidden
```

**Consequence:** Code that triggers any warning does not compile. Warnings are not suppressed. Warnings are fixed.

---

### Standard 3 — Permitted C++26 Features (Explicit Allowlist)

Only these modern features are sanctioned:

| Feature | Usage |
| :--- | :--- |
| `std::expected<T, E>` | All fallible operations. No exceptions. |
| `std::optional<T>` | Maybe-values where `nullptr` is ambiguous |
| `std::span<T>` | Non-owning contiguous views |
| `std::string_view` | Read-only string parameters only. Never stored. |
| `std::array<T, N>` | Fixed-size arrays on stack |
| `std::pmr::*` | Arena-aware containers |
| `constexpr` everything | Compile-time evaluation preferred |
| `consteval` | Guaranteed compile-time functions |
| `constinit` | Static initialization without constructors |
| Concepts | Constrain templates. No SFINAE. |
| `requires` clauses | Explicit template constraints |
| `[[nodiscard]]` | On all `expected`, `Result`, factory functions |
| `[[likely]]` / `[[unlikely]]` | On profile-driven branches |
| `[[assume(expr)]]` | Internal invariants after verification |
| `std::bit_cast` | Type punning. Never `reinterpret_cast` |
| Structured bindings | `auto [key, value] = ...` |
| `if constexpr` | Compile-time branching in templates |
| Designated initializers | `Node{ .kind = NodeKind::Add, .lhs = a }` |
| `std::source_location` | Diagnostic locations |
| Coroutines | **FORBIDDEN** in hot paths. Allowed in driver/tools only |
| Ranges | **FORBIDDEN** in hot paths. Allowed in driver/tools only |
| Modules | Allowed. Header units for stdlib |

---

## Part II: Naming & File Conventions

### Standard 4 — Naming Rules (Machine-Enforced)

All naming is checked by `clang-tidy` custom rules and a dedicated `naming_lint` CI job. Violations fail CI.

| Element | Convention | Example |
| :--- | :--- | :--- |
| Types (class, struct, enum, alias) | `PascalCase` | `SeaOfNodesGraph`, `NodeId`, `CompileMode` |
| Functions/methods | `snake_case` | `build_graph()`, `run_pass()`, `emit_code()` |
| Variables (local, member) | `snake_case` | `node_count`, `graph_arena` |
| Member variables | `snake_case_` (trailing underscore) | `graph_arena_`, `pass_count_` |
| Constants (`constexpr`) | `kPascalCase` | `kMaxInlineDepth`, `kGVNHashTableSize` |
| Enum values | `PascalCase` | `NodeKind::BinaryAdd`, `CompileMode::AOT` |
| Namespaces | `snake_case` | `jules::ir`, `jules::passes::vectorization` |
| Files (headers) | `snake_case.h` | `sea_of_nodes.h`, `gvn_pass.h` |
| Files (source) | `snake_case.cpp` | `sea_of_nodes.cpp`, `gvn_pass.cpp` |
| Test files | `test_snake_case.cpp` | `test_gvn_redundant_load.cpp` |
| Macros (if unavoidable) | `JULES_SCREAMING_CASE` | `JULES_UNREACHABLE()` |
| Template parameters | `PascalCase` | `template <typename NodeType>` |
| Abbreviations | Treated as words | `run_gvn_pass()`, not `runGVNPass()` |

**Consequence:** Naming violations are caught by pre-commit hook. Commit rejected. No manual override.

---

### Standard 5 — File Size Limits

| Metric | Limit | Enforcement |
| :--- | :--- | :--- |
| Header file line count | ≤ 800 lines | CI script. Split required. |
| Source file line count | ≤ 1200 lines | CI script. Split required. |
| Single function body | ≤ 120 lines | Clang-tidy `readability-function-size` |
| Function parameter count | ≤ 6 parameters | Clang-tidy. Use struct for more. |
| Nesting depth | ≤ 4 levels | Clang-tidy `readability-braces-around-statements` + custom |
| Cyclomatic complexity | ≤ 15 | Clang-tidy `readability-function-cognitive-complexity` |
| File include depth | ≤ 3 levels | Custom include-what-you-use check |

**Consequence:** Files exceeding limits fail CI. Must be refactored before merge. "It's one logical unit" is not an excuse. Split it.

---

### Standard 6 — Include Rules

```cpp
// 1. Corresponding header (for .cpp files)
#include "jules/passes/gvn_pass.h"

// 2. C system headers (rare, only in runtime)
#include <cstdint>

// 3. C++ standard library
#include <expected>
#include <span>

// 4. Project headers (alphabetical)
#include "jules/ir/node.h"
#include "jules/ir/graph.h"

// 5. Test-only headers (in test files)
#include "jules/test/ir_builder.h"
```

-   `#pragma once` in all headers. No include guards.
-   Forward declarations preferred over includes where possible.
-   No circular includes. CI dependency graph check enforces DAG.
-   No `using namespace` in headers. Ever. In source files, only inside function scope.

---

## Part III: Memory & Ownership

### Standard 7 — Arena Allocation is Mandatory

All compiler-phase allocations MUST go through the arena system.

```cpp
// Every compilation unit has an arena
class CompilationUnit {
    std::pmr::monotonic_buffer_resource arena_;
    // All IR nodes, pass data, temporaries allocate here
};

// Allocation pattern
auto* node = arena_.new<Node>();        // Constructed in arena
auto  vec  = std::pmr::vector<NodeId>(&arena_);  // Arena-backed vector
```

**Rules:**
-   No `new`/`delete` outside arena in compiler hot paths.
-   No `malloc`/`free` outside arena in compiler hot paths.
-   Arena is bulk-freed at end of compilation phase.
-   Individual deallocation is **forbidden** within a phase.
-   Arena size is pre-estimated. Exceeding estimate logs telemetry but does not fail.

**Consequence:** Heap allocation in hot path detected by ASan custom allocator hook. CI fails. PR blocked.

---

### Standard 8 — Ownership Model

| Pattern | Usage |
| :--- | :--- |
| Arena-owned raw pointer | IR nodes, pass temporaries. `Node*` valid within phase. |
| `NodeId` (uint32_t) | Cross-phase references. Stable. Serializable. |
| `std::unique_ptr` | Cold-path ownership only (driver, tools). Never in passes. |
| `std::span<T>` | Non-owning view into arena data. |
| `std::pmr::vector<T>` | Arena-backed growable collection. |
| Raw pointer (non-owning) | Observer pointers. Must document lifetime. |

**Forbidden:**
-   `std::shared_ptr` anywhere in compiler.
-   `std::weak_ptr` anywhere in compiler.
-   Raw `new`/`delete` in passes.
-   `std::make_unique` in hot paths.

---

### Standard 9 — No Leaks, No Dangles

-   ASan + LSan run on every CI test. Zero leaks permitted.
-   Arena bulk-free means leaks are impossible *within* a phase, but cross-phase references via `NodeId` must be validated.
-   Dangling pointer detection: debug builds use `NodeId` validation (index < graph.size()). Release builds use `[[assume]]`.
-   Every pointer parameter must document lifetime: `// Valid for duration of current pass` or `// Valid for duration of compilation`.

---

## Part IV: Error Handling

### Standard 10 — `std::expected` is the Only Error Mechanism

```cpp
// Every fallible function returns expected
[[nodiscard]]
std::expected<Graph, Diagnostic> build_graph(const Module& module);

[[nodiscard]]
std::expected<void, Diagnostic> run_pass(Graph& graph, const PassConfig& config);

// Monadic chaining
auto result = build_graph(module)
    .and_then([](Graph& g) { return verify_graph(g); })
    .and_then([](Graph& g) { return optimize(g); })
    .and_then([](Graph& g) { return emit_code(g); });

// Error propagation macro (the ONLY sanctioned macro)
#define JULES_TRY(expr)                        \
    ({                                          \
        auto _result = (expr);                  \
        if (!_result.has_value()) {             \
            return std::unexpected(_result.error()); \
        }                                       \
        std::move(*_result);                    \
    })
```

**Rules:**
-   Every function that can fail MUST return `std::expected<T, Diagnostic>`.
-   Every `std::expected` MUST be marked `[[nodiscard]]`.
-   Ignoring an `expected` is a compile error.
-   `Diagnostic` MUST contain: source location, error code, message, expected vs actual, suggested fix.
-   No `assert()` for recoverable errors. `assert` is for internal invariants only.
-   No `abort()` for recoverable errors. `abort` is for internal invariant violations only.

---

### Standard 11 — Diagnostic Quality

Every `Diagnostic` emitted by the compiler MUST satisfy:

```cpp
struct Diagnostic {
    SourceLocation location;      // File, line, column, span
    DiagCode code;                // JULES-E0042, not "error"
    Severity severity;            // Error, Warning, Note, Help
    std::string_view message;     // Human-readable summary
    std::string_view expected;    // What was expected
    std::string_view found;       // What was found
    std::string_view suggestion;  // How to fix it
    std::vector<SourceLocation> related;  // Related locations
    std::string_view explanation; // Why this is an error
};
```

**The following diagnostic output is a fireable offense:**
```
error: something went wrong
error: invalid type
error: compilation failed
```

**Required diagnostic output:**
```
error[JULES-E0312]: cannot add `Vec3` and `String`

  12 | let result = position + name;
     |              ^^^^^^^^^^^^^^

  expected: type implementing `Add<Vec3>`
  found:    `String`

  `Vec3` provides:
      Add<Vec3> → Vec3
      Add<f32>  → Vec3

  `String` provides:
      Add<String> → String

  help: did you mean to convert? 
        let result = position + name.parse::<Vec3>()?;
```

---

## Part V: Testing Standards

### Standard 12 — Coverage Requirements

| Metric | Requirement | Enforcement |
| :--- | :--- | :--- |
| Line coverage (compiler) | ≥ 95% | CI coverage report. PR blocked below. |
| Branch coverage (compiler) | ≥ 90% | CI coverage report. PR blocked below. |
| Line coverage (runtime) | ≥ 98% | CI coverage report. PR blocked below. |
| Mutation testing kill rate | ≥ 85% | Weekly CI job. Release gate. |
| Every pass | ≥ 10 golden IR tests | CI checks test count per pass |
| Every bug fix | ≥ 5 regression tests | CI checks PR label + test count |
| Every diagnostic | ≥ 3 test cases (trigger, variant, boundary) | CI diagnostic test suite |
| Every FFI boundary | ≥ 5 ABI tests | CI FFI test suite |
| Every deopt point | ≥ 1 forced-deopt test | CI deopt test suite |

**Consequence:** Coverage below threshold blocks merge. No exceptions. "It's hard to test" is not an excuse. Write the test.

---

### Standard 13 — Test Quality Rules

Every test MUST:

1.  **Be self-contained.** No external files, no network, no environment assumptions.
2.  **Be deterministic.** Same input → same output. No `rand()` without seed. No time-dependent logic.
3.  **Be named descriptively.** `test_gvn_eliminated_redundant_load_across_phi` not `test_gvn_3`.
4.  **Test one behavior.** If a test has two `CHECK` assertions for unrelated behaviors, split it.
5.  **Include negative cases.** "This should NOT be optimized" tests are as important as "this SHOULD be optimized" tests.
6.  **Run in both AOT and JIT modes.** Every IR test runs twice.
7.  **Verify deopt state.** Every speculative optimization test must verify fallback state reconstruction.

**Forbidden test patterns:**
```cpp
// FORBIDDEN: sleeping
std::this_thread::sleep_for(100ms);

// FORBIDDEN: ordering assumptions
EXPECT_EQ(results[0], expected);  // If order isn't guaranteed

// FORBIDDEN: exact float comparison
EXPECT_EQ(computed, 3.14159);

// FORBIDDEN: environment dependence
EXPECT_EQ(getenv("HOME"), "/home/user");
```

---

### Standard 14 — Differential Testing

Every CI run MUST execute:

1.  **AOT vs JIT equivalence:** Same source → same output. Run 1000 test programs.
2.  **Optimized vs unoptimized:** `-O0` vs `-O3` semantic equivalence (not performance).
3.  **Cross-target:** x86-64 vs AArch64 output equivalence for portable code.
4.  **With/without each pass:** Disabling any single pass must not change semantics (only performance).
5.  **Deopt vs non-deopt:** Forced deopt at every guard → same result as no speculation.

**Any divergence is a P0 bug.** Blocks release. No exceptions.

---

### Standard 15 — Fuzzing Requirements

| Target | Frequency | Duration |
| :--- | :--- | :--- |
| IR graph mutations | Every PR (10 min) | 10K iterations |
| Source file mutations | Nightly | 1 hour |
| Profile data mutations | Nightly | 1 hour |
| AOT artifact corruption | Weekly | 4 hours |
| Deopt state reconstruction | Weekly | 4 hours |
| FFI boundary abuse | Weekly | 2 hours |
| Concurrent compilation | Nightly | 30 min |

**Rules:**
-   All fuzz targets must be reproducible from seed.
-   Crash → P0. Wrong result → P0. Hang → P1.
-   Untriaged fuzz failures block release.
-   Fuzz corpus is versioned in repo.

---

## Part VI: Performance Standards

### Standard 16 — Compile Time Budgets

| Operation | Budget | Measurement |
| :--- | :--- | :--- |
| Lexing + Parsing | ≤ 50 MB/s throughput | CI benchmark |
| Semantic analysis | ≤ 30 MB/s throughput | CI benchmark |
| Full AOT compile (small project) | ≤ 1s for 10K LOC | CI benchmark |
| Single pass execution | ≤ 5% of total compile time | Per-pass timing |
| JIT compilation (single function) | ≤ 10ms p50, ≤ 50ms p99 | Runtime telemetry |
| JIT compilation (hot loop) | ≤ 100ms p99 | Runtime telemetry |
| Deopt latency | ≤ 5μs p99 | Runtime telemetry |
| Graph verifier (debug) | ≤ 2x compile time overhead | CI benchmark |

**Consequence:** Exceeding budget by >10% blocks merge. Exceeding by >5% requires waiver. Performance regression >5% on any benchmark requires root-cause analysis + tech lead approval.

---

### Standard 17 — Runtime Performance Gates

Every PR that touches code generation MUST run the full benchmark suite:

| Benchmark Category | Regression Limit |
| :--- | :--- |
| Integer compute (SPEC-like) | ≤ 1% geomean regression |
| FP compute | ≤ 1% geomean regression |
| Memory-intensive | ≤ 2% regression |
| Branch-heavy | ≤ 1% regression |
| SIMD workloads | ≤ 2% regression |
| Code size | ≤ 1% increase |
| Startup time (JIT) | ≤ 5% regression |
| Deopt rate | ≤ 10% increase |
| Guard overhead | ≤ 2% increase |

**Consequence:** Regression beyond limit blocks merge. Requires explicit waiver with root-cause, tracking issue, and expiry date.

---

### Standard 18 — Hot Path Performance Rules

Code in the hot path (pass execution, guard checks, IC dispatch, deopt entry) MUST:

1.  **Not allocate.** Zero heap allocations. Arena only.
2.  **Not take locks.** No mutexes, no atomics (except IC patching with documented justification).
3.  **Not call virtual functions.** Devirtualized or CRTP only.
4.  **Not throw.** No exceptions. `-fno-exceptions` enforced.
5.  **Not use `std::function`.** Function pointers or templates.
6.  **Not use `std::string`.** `SymbolId` or `std::string_view`.
7.  **Not use `std::unordered_map`.** Flat hash map only.
8.  **Minimize branch misprediction.** `[[likely]]`/`[[unlikely]]` on profile-driven branches.
9.  **Be cache-friendly.** SoA layout for bulk processing. No pointer chasing.
10. **Be branchless where possible.** CMOV, predication, lookup tables preferred over branches.

**Verification:** `perf stat` in CI measures branch-miss rate, cache-miss rate, IPC for hot path benchmarks. Regression >5% on any metric blocks merge.

---

## Part VII: Code Review Standards

### Standard 19 — Review Requirements

Every PR MUST receive:

1.  **Two approvals** from senior compiler engineers.
2.  **One approval** from the owner of every subsystem touched.
3.  **CI green** on all checks (tests, coverage, benchmarks, sanitizers, fuzzing).
4.  **No unresolved review comments.** Every comment must be addressed or explicitly dismissed with justification.
5.  **Slop checklist completed** (see Standard 22).

**Review turnaround:** Reviewers MUST respond within 24 hours. Failure to review is a performance issue tracked in team metrics.

---

### Standard 20 — Review Checklist (Mandatory Per PR)

Every reviewer MUST verify and check off:

**Correctness:**
- [ ] All new code paths have tests
- [ ] All bug fixes have 5 regression tests
- [ ] Differential testing passes (AOT vs JIT)
- [ ] Deopt paths tested for speculative changes
- [ ] No semantic divergence from JULES spec

**Performance:**
- [ ] No allocations in hot paths
- [ ] No locks in hot paths
- [ ] No virtual dispatch in hot paths
- [ ] Benchmark results attached if perf-sensitive
- [ ] No O(n²) where O(n) or O(n log n) is feasible

**Safety:**
- [ ] No raw pointer arithmetic without bounds proof
- [ ] No `reinterpret_cast` outside backend
- [ ] No undefined behavior (ASan/UBSan clean)
- [ ] No data races (TSan clean for concurrent code)
- [ ] W^X maintained for JIT code

**Style:**
- [ ] Naming conventions followed
- [ ] No magic numbers (all constants named)
- [ ] No dead code
- [ ] No commented-out code
- [ ] No TODO without tracking issue

**Documentation:**
- [ ] All public APIs documented
- [ ] All non-obvious algorithms documented
- [ ] All invariants documented
- [ ] ADR attached if architectural decision

**Speculation (if applicable):**
- [ ] Every guard has FrameState
- [ ] Every speculative node has metadata
- [ ] Every assumption has invalidation path
- [ ] Fallback path tested
- [ ] Deopt materialization tested

---

### Standard 21 — Review Rejection Criteria

A PR MUST be rejected (not just requested changes) if:

1.  It introduces any forbidden language feature (Standard 1).
2.  It reduces test coverage below threshold.
3.  It introduces a performance regression >5% without waiver.
4.  It contains a silent fallback without telemetry.
5.  It contains a magic number in optimization logic.
6.  It modifies generated code without updating golden tests.
7.  It touches the verifier without updating verifier tests.
8.  It adds a pass without 10 golden tests.
9.  It contains `HACK`, `TODO` without issue, or `FIXME` without deadline.
10. It has been open >7 days without response (reviewer accountability).

---

### Standard 22 — Slop Detection Checklist

In addition to Standard 20, every reviewer runs this checklist. **Any single failure blocks merge.**

- [ ] No unnamed numeric constants in logic
- [ ] No duplicated code blocks (>5 lines structural similarity)
- [ ] No silent fallbacks or unsafe default returns
- [ ] No `std::unordered_map` or heap-backed `std::vector` in hot paths
- [ ] All invariants documented and validated
- [ ] No premature abstractions without ≥2 consumers
- [ ] No untracked workarounds or `HACK` comments
- [ ] No target-specific logic outside `backend/`
- [ ] All new code paths have test coverage
- [ ] Hot-path changes justified with profiling/benchmarks
- [ ] Every new guard has a `FrameState` attachment
- [ ] Every speculative node carries complete metadata
- [ ] Every deopt point is reachable and has complete metadata
- [ ] No `getenv()` or mutex-locking in dispatch loops
- [ ] No atomic RMW in per-instruction hot paths without justification
- [ ] W^X maintained
- [ ] Code publication is atomic with release/acquire semantics
- [ ] Pending panics not dropped across FFI transitions
- [ ] No GC artifacts (stack maps, barriers, TLAB references)
- [ ] No exceptions, no RTTI, no `std::function` in hot paths

---

## Part VIII: CI Pipeline Requirements

### Standard 23 — CI Stages (All Mandatory, All Blocking)

Every PR triggers this pipeline. No stage may be skipped.

```
Stage 1: Format & Lint (< 30 seconds)
  ├── clang-format check
  ├── naming convention check
  ├── include order check
  └── file size limit check

Stage 2: Build (< 5 minutes)
  ├── Debug build (ASan + UBSan)
  ├── Release build (LTO)
  ├── All warnings as errors
  └── Static analysis (clang-tidy, cppcheck)

Stage 3: Unit Tests (< 10 minutes)
  ├── All unit tests (debug)
  ├── All unit tests (release)
  ├── Coverage report generation
  └── Coverage threshold check

Stage 4: Integration Tests (< 20 minutes)
  ├── Golden IR tests (all 89 passes × AOT + JIT)
  ├── Differential testing (AOT vs JIT)
  ├── Deopt stress tests
  ├── FFI/ABI tests
  └── Diagnostic output tests

Stage 5: Performance (< 30 minutes)
  ├── Compile-time benchmarks
  ├── Runtime benchmarks (full suite)
  ├── Code size comparison
  ├── Hot-path perf stat (IPC, cache, branch)
  └── Regression detection + waiver check

Stage 6: Fuzzing (< 15 minutes)
  ├── IR mutation fuzzing
  ├── Profile corruption fuzzing
  ├── Artifact validation fuzzing
  └── Crash/hang detection

Stage 7: Concurrency (< 10 minutes)
  ├── TSan test suite
  ├── Concurrent compilation stress
  ├── Code installation race tests
  └── Deopt-during-patch tests

Stage 8: Security (< 10 minutes)
  ├── W^X verification
  ├── Executable memory accounting
  ├── Artifact loading validation
  └── JIT spraying PoC tests
```

**Total CI time target:** < 2 hours for full pipeline. Incremental (changed files only) target: < 15 minutes.

---

### Standard 24 — CI Failure Policy

| Failure Type | Severity | Action |
| :--- | :--- | :--- |
| Build failure | P0 | Block merge. Fix immediately. |
| Test failure | P0 | Block merge. Fix or revert. |
| Coverage below threshold | P1 | Block merge. Add tests. |
| Performance regression >5% | P1 | Block merge. Waiver required. |
| Performance regression >1% ≤5% | P2 | Warning. Waiver if justified. |
| Fuzz crash | P0 | Block merge. Block release. |
| Fuzz hang | P1 | Block merge. Fix within 24h. |
| Sanitizer error (ASan/UBSan/TSan) | P0 | Block merge. Fix immediately. |
| Warning (new) | P1 | Block merge. Fix warning. |
| Format violation | P2 | Block merge. Auto-fix available. |
| Documentation missing | P2 | Block merge. Add docs. |
| Benchmark infrastructure failure | P3 | Retry. If persistent, investigate. |

---

### Standard 25 — Nightly CI (Additional)

Every night, CI runs extended checks:

-   Full fuzzing (4 hours, all targets)
-   Mutation testing (full suite)
-   Cross-compilation (all supported targets)
-   Memory leak soak test (8 hours, ASan)
-   Compile-time regression tracking (30-day trend)
-   Code size trend tracking
-   Dependency vulnerability scan
-   Documentation link validation
-   Test flakiness detection (run all tests 10x)

**Results triaged by 10:00 next day.** Untriaged failures block releases.

---

## Part IX: Documentation Standards

### Standard 26 — Documentation is Not Optional

Every public entity MUST have documentation at the point of definition:

```cpp
/// Performs Global Value Numbering on the sea-of-nodes graph.
///
/// This pass assigns value numbers to all nodes based on their
/// opcode and operand value numbers. Nodes with identical value
/// numbers are merged, eliminating redundant computations.
///
/// ## Preconditions
/// - Graph must be in SSA form
/// - Dead nodes must be eliminated (Pass 1)
/// - Phi nodes must be simplified (Pass 7)
///
/// ## Postconditions
/// - All redundant pure computations eliminated
/// - Value numbers assigned to all nodes
/// - Memory SSA form preserved
///
/// ## Invalidated Analyses
/// - None (GVN only adds information)
///
/// ## Budget
/// - O(n) time where n = node count
/// - O(n) memory for hash table
///
/// ## Mode Behavior
/// - AOT: Full GVN with static constants
/// - JIT: GVN with profile-observed constants (guarded)
///
/// ## Tests
/// - test_gvn_redundant_load.cpp
/// - test_gvn_cross_block_cse.cpp
/// - test_gvn_memory_ssa_preserved.cpp
class GVNP final : public Pass { ... };
```

**Required documentation sections:**
1.  Purpose (what it does)
2.  Preconditions (what must be true before)
3.  Postconditions (what is true after)
4.  Invariants (what is always maintained)
5.  Edge cases (known tricky inputs)
6.  Cross-references (related passes, specs)
7.  Test references (where to find tests)

**Consequence:** Documentation lint runs in CI. Missing docs on public API = CI failure.

---

### Standard 27 — ADR Requirement

Any change that affects:
-   IR node semantics
-   Pass ordering
-   Deopt behavior
-   Memory model assumptions
-   ABI
-   Target backend interface

...MUST have an Architecture Decision Record (ADR) merged **before** the implementation PR.

ADR template:
```markdown
# ADR-042: Use SoA Layout for Node Opcode Array

## Status
Accepted

## Context
Pass 9 (GVN) iterates over all node opcodes to build hash buckets.
AoS layout causes cache misses due to interleaved unused fields.

## Options Considered
1. Keep AoS, accept cache misses
2. Extract opcodes to SoA array
3. Use SIMD gather for AoS access

## Decision
Option 2. Extract `opcode` field to contiguous SoA array.

## Consequences
- +30% GVN throughput on large graphs
- +1 cache line per node (memory overhead)
- Requires updating all passes that access opcode

## Performance Impact
Measured: 28% GVN speedup on 100K-node graphs

## Rollback Plan
Revert SoA extraction. No semantic change.
```

---

## Part X: Version Control Standards

### Standard 28 — Commit Requirements

Every commit MUST:

1.  **Have a descriptive message.** Format: `subsystem: imperative summary`
    -   Good: `passes/gvn: eliminate redundant loads across phi merges`
    -   Bad: `fix stuff`, `update`, `wip`
2.  **Be atomic.** One logical change per commit. No "fix typo" follow-ups.
3.  **Pass all CI.** No broken commits on main. Ever.
4.  **Reference issues.** `Fixes #1234` or `Relates to #5678`.
5.  **Not contain generated files.** Golden test outputs are checked in deliberately, not accidentally.

### Standard 29 — Branch Policy

-   `main` is always green. All CI passes. No exceptions.
-   Feature branches are short-lived (< 3 days). Long-lived branches require tech lead approval.
-   No force-push to `main`. No rebase of merged PRs.
-   Revert is preferred over fix-forward for broken merges.

### Standard 30 — Bisectability

`git bisect` MUST work at all times. Every commit must:
-   Compile
-   Pass all tests
-   Produce correct output

If a commit breaks bisectability, it is reverted. No discussion.

---

## Part XI: Dependency & Build Standards

### Standard 31 — Dependency Policy

| Category | Policy |
| :--- | :--- |
| C++ standard library | Permitted. Pinned to compiler version. |
| External C++ libraries | **FORBIDDEN** in compiler hot path. Allowed in tools/driver only with approval. |
| C libraries | Allowed for FFI testing only. Not linked into compiler. |
| Build system | CMake ≥ 3.28. No Makefiles. No Bazel. No custom build scripts. |
| Package manager | vcpkg or Conan for tools. No package manager for compiler core. |
| Vendored code | Must have license audit + security review. Tracked in `VENDOR.md`. |

**New dependency requires:**
1.  ADR explaining why existing code can't solve it
2.  License compatibility check
3.  Security review
4.  Size impact analysis
5.  Tech lead approval

---

### Standard 32 — Build Requirements

-   **Hermetic builds.** Same source + same toolchain = same binary. Bit-for-bit reproducible.
-   **No network access during build.** All dependencies vendored or cached.
-   **Incremental build < 2 seconds** for single-file change.
-   **Full rebuild < 10 minutes** on reference hardware (documented).
-   **Cross-compilation supported** for all targets from any host.
-   **Build warnings = errors.** No warning suppression.

---

## Part XII: Incident Response

### Standard 33 — Bug Severity and Response

| Severity | Definition | Response Time | Fix Deadline |
| :--- | :--- | :--- | :--- |
| P0 | Silent wrong codegen, security vuln, crash on valid input | Immediate | 24 hours |
| P1 | Wrong results with diagnostic, missing deopt metadata, memory unsafety | 4 hours | 3 days |
| P2 | Performance regression >5%, missing tests, doc debt | 24 hours | 1 week |
| P3 | Style, naming, minor optimization opportunity | 1 week | Backlog |

**P0 bugs:**
-   All other work stops.
-   Root-cause analysis required within 4 hours.
-   Fix + 5 regression tests required.
-   Post-mortem required within 48 hours.
-   Release blocked until fixed.

### Standard 34 — No Workarounds

Adding code to work around a bug in the compiler, runtime, or standard library is **forbidden.** The underlying defect MUST be fixed.

Temporary mitigations require:
1.  Tracking issue with severity
2.  Removal deadline ≤ 2 weeks
3.  Tech lead approval
4.  `MITIGATION:` comment with issue link
5.  CI check that mitigation is removed by deadline

**Consequence:** Workaround without tracking issue = PR rejection. Workaround past deadline = release blocker.

---

## Part XIII: Human Factors

### Standard 35 — No Heroics

-   No single person may be the only one who understands a subsystem.
-   Every subsystem has ≥ 2 designated owners.
-   Bus factor < 2 for any component is a P1 organizational bug.
-   Knowledge transfer is documented, not oral.

### Standard 36 — No Blame

-   Post-mortems are blameless.
-   Bugs are process failures, not personal failures.
-   "Who caused this" is the wrong question. "What process allowed this" is the right question.
-   Psychological safety is a prerequisite for brutal quality standards. People must feel safe to report bugs, admit mistakes, and ask for help.

### Standard 37 — Sustainable Pace

-   Brutal standards apply to code, not to people.
-   No expectation of 80-hour weeks.
-   CI failures at 5 PM are fixed tomorrow, not tonight. (P0 security exceptions apply.)
-   Burnout is a project risk. Monitored in team health surveys.

---

## Part XIV: Enforcement Hierarchy

| Level | Mechanism | Catches |
| :--- | :--- | :--- |
| 1 | Compiler flags (`-Werror`) | Language violations, warnings |
| 2 | Pre-commit hook (< 2s) | Format, naming, file size |
| 3 | CI Stage 1-2 | Build, static analysis, lint |
| 4 | CI Stage 3-4 | Tests, coverage, golden tests |
| 5 | CI Stage 5-8 | Performance, fuzzing, concurrency, security |
| 6 | Human review | Design, correctness, documentation |
| 7 | Nightly CI | Extended fuzzing, mutation, trends |
| 8 | Release gate | All of the above + compliance matrix |

**Nothing reaches `main` without passing all 8 levels.** There is no override. There is no admin bypass. There is no "merge anyway." The only exception is a P0 security fix, which requires two tech lead approvals and is followed by a full CI run within 1 hour.

---

*These standards are not aspirational. They are the minimum. Code that does not meet them does not ship. Code that does not meet them does not merge. Code that does not meet them does not exist in this repository.*

*Brutality is the point. Quality is non-negotiable. The compiler we are building will be trusted to generate code that runs in production systems. Every shortcut we take in building it is a shortcut that will manifest as a bug in someone's production system.*

*No slop. No exceptions. No mercy.*