
# CEP&CC 0.1

## Cycle-Exact Performance & Clean Code

### Psychopathic Tier

Target dialect: **C++26**  
Version: **0.1**  
Status: **Draft standard**  
---

# 1. Purpose

CEP&CC 0.1 is a coding standard for C++26 codebases that require both of the following properties simultaneously:

1. **Cycle-exact performance discipline**  
   Performance is treated as a correctness requirement, not as a vague goal.

2. **Clean-code discipline**  
   The code must remain explicit, auditable, reviewable, failure-aware, and free of hidden assumptions.

This standard is intended for systems where performance surprises are unacceptable and code clarity is mandatory. Examples include:

- compilers,
- linkers,
- assemblers,
- disassemblers,
- profilers,
- debuggers,
- virtual machines,
- interpreters,
- JIT compilers,
- game engines,
- renderers,
- audio engines,
- video codecs,
- DSP systems,
- embedded firmware,
- deterministic simulators,
- real-time control systems,
- high-performance parsers,
- network data-plane systems,
- low-latency trading systems,
- operating-system-adjacent userland code.

The standard is deliberately strict. It assumes that:

- humans will forget rules,
- reviewers will miss details,
- compilers will change behavior,
- targets will evolve,
- benchmarks will regress,
- assumptions will become stale,
- comments will rot.

Therefore, CEP&CC favors mechanical enforcement, explicit documentation, measured evidence, and compile-time proof wherever possible.

---

# 2. What “CEP&CC” means

## 2.1 CEP: Cycle-Exact Performance

CEP means that the cost of code is explicit, bounded, and measured.

A function is not CEP-compliant merely because it is fast in one test. It is CEP-compliant only if its cost behavior is understood and documented.

A CEP-compliant hot function must be able to answer all of the following questions:

- What does it do?
- What is its worst-case behavior?
- What is its expected-case behavior?
- What branches does it contain?
- What are the branch probabilities?
- What memory does it read?
- What memory does it write?
- Are accesses aligned?
- Are accesses sequential or random?
- Are accesses cache-friendly?
- Does it cause cache pressure?
- Does it cause TLB pressure?
- Does it cause branch mispredictions?
- Does it allocate memory?
- Does it free memory?
- Does it lock anything?
- Does it perform atomic operations?
- Does it perform system calls?
- Does it perform I/O?
- Does it format text?
- Does it throw exceptions?
- Does it rely on RTTI?
- Does it rely on virtual dispatch?
- Does it rely on indirect function calls?
- Does it rely on compiler-generated temporaries?
- Does it rely on compiler-generated initialization?
- Does it rely on target-specific instructions?
- Does it rely on implementation-defined behavior?
- Does it rely on undefined behavior?
- Does it rely on floating-point behavior that is not explicitly controlled?
- Does it rely on input distribution?
- Does it rely on alignment?
- Does it rely on endianness?
- Does it rely on ABI details?
- Does it rely on OS behavior?
- Does it rely on allocator behavior?
- Does it rely on thread scheduling?
- Does it rely on locale?
- Does it rely on filesystem?
- Does it rely on environment variables?
- Does it rely on time?
- Does it rely on randomness?

If the answer to any of these questions is “I don’t know,” then the function is not CEP-compliant.

---

## 2.2 CC: Clean Code

Clean code in CEP&CC does not mean “short code,” “minimal code,” or “elegant code” in a subjective sense.

Clean code means that the code is explicit enough to be reviewed without guessing.

A clean function must make the following obvious:

- what it does,
- why it exists,
- what it assumes,
- what it owns,
- what it borrows,
- what it returns,
- what can fail,
- what cannot fail,
- what its cost class is,
- whether it is complete, partial, stubbed, or placeholder.

Clean code rejects cleverness that hides cost. It rejects abstractions that make control flow, allocation, or failure behavior invisible.

CEP&CC therefore treats the following as clean-code defects when they hide cost:

- clever template metaprogramming,
- macro-generated control flow,
- exception-based control flow,
- virtual dispatch in hot paths,
- type-erased callables in hot paths,
- implicit conversions,
- hidden constructors,
- hidden destructors,
- hidden allocations,
- hidden global initialization,
- hidden thread-local initialization,
- hidden locale behavior,
- hidden I/O,
- hidden formatting,
- hidden synchronization,
- hidden filesystem access,
- hidden random behavior,
- hidden nondeterministic iteration.

---

## 2.3 Psychopathic Tier

“Psychopathic tier” means the standard does not rely on good intentions.

It assumes that violations will occur unless prevented by:

- compiler warnings,
- static analysis,
- lint,
- comment parsers,
- CI checks,
- benchmark gates,
- unit tests,
- property tests,
- sanitizers,
- review checklists,
- signed exceptions,
- explicit waiver notes.

A rule that cannot be enforced is weak. CEP&CC rules are designed to be enforceable.

---

# 3. Normative terms

The following terms have normative meaning in this standard.

- **Must**: mandatory requirement.
- **Must not**: mandatory prohibition.
- **Should**: strong recommendation; deviation requires justification.
- **Should not**: strong discouragement; deviation requires justification.
- **May**: permitted optional behavior.
- **Banned**: forbidden in the specified context.
- **Restricted**: allowed only under explicit conditions.
- **Cold-only**: allowed only outside hot paths.
- **Hot code**: code where cycle cost, latency, jitter, throughput, or memory traffic matters.
- **Cold code**: code where correctness matters but exact cycle cost does not.
- **CEP-0**: cycle-exact hot code class.
- **CEP-1**: deterministic non-hot runtime code class.
- **CEP-2**: offline or non-deterministic tooling code class.
- **Complete**: implementation is finished, tested, documented, and measured if hot.
- **Partial**: implementation is intentionally incomplete for known cases.
- **Stub**: interface exists but behavior is intentionally minimal or absent.
- **Placeholder**: reserved for future implementation; not production behavior.

---

# 4. Core laws

These laws override all other preferences.

---

## Law 1: No hidden cost

Any mechanism that can introduce hidden cost must be explicitly approved and documented.

Hidden cost includes, but is not limited to:

- heap allocation,
- heap deallocation,
- virtual dispatch,
- indirect calls,
- type erasure,
- exception handling,
- RTTI,
- formatting,
- I/O,
- locale,
- filesystem access,
- syscalls,
- locks,
- atomic reference counting,
- thread creation,
- dynamic initialization,
- lazy initialization,
- guard variables,
- coroutine heap frames,
- dynamic library loading,
- nondeterministic hash iteration,
- unpredictable branch generation,
- hidden compiler builtins,
- hidden template instantiation cost,
- hidden code generation from reflection or macros.

If a feature can cause hidden cost, it must be either:

1. banned in the relevant code class,
2. restricted with explicit conditions,
3. measured and documented.

---

## Law 2: No silent assumptions

Every assumption must be explicit.

Assumptions include:

- alignment,
- endianness,
- integer width,
- pointer size,
- cache line size,
- page size,
- ABI,
- OS behavior,
- allocator behavior,
- floating-point rounding,
- floating-point contraction,
- errno behavior,
- thread interleaving,
- input validity,
- input size,
- lifetime,
- ownership,
- reentrancy,
- interrupt safety,
- signal safety,
- compiler optimization behavior.

If an assumption is not written down and enforced, it does not exist.

---

## Law 3: No comment-only invariants

Comments have literal zero runtime cost. That is required. But it also means they cannot enforce anything by themselves.

If a comment says:

```cpp
// Assumes input is sorted.
```

then the code must also have one of:

- a `static_assert` where possible,
- a debug assertion,
- a runtime check,
- a contract,
- a type constraint,
- a documented caller requirement with test coverage,
- a compile-time proof.

A comment alone is not enough.

---

## Law 4: No unmeasured hot code

Hot code must be measured.

Measurement must include at least:

- disassembly,
- benchmark results,
- target description,
- compiler version,
- compile flags,
- input description,
- date or artifact ID.

A performance claim without evidence is invalid.

---

## Law 5: No unreadable hot code

Hot code must be clean enough to be audited.

If a reviewer cannot understand the control flow, memory access pattern, or failure behavior, the code is not acceptable.

---

## Law 6: No unbounded failure

Failure behavior must be explicit.

Every function must declare whether it can fail and how.

Failure includes:

- invalid input,
- resource exhaustion,
- overflow,
- underflow,
- division by zero,
- NaN,
- infinity,
- alignment violation,
- lifetime violation,
- aliasing violation,
- allocation failure,
- syscall failure,
- hardware error,
- interruption,
- cancellation,
- timeout,
- deadlock,
- priority inversion,
- race condition,
- exception,
- assertion,
- contract violation.

If a function cannot fail, the reason must be documented or provable.

---

## Law 7: No hard-coded assumptions

Hard-coded assumptions are banned.

A hard-coded assumption is a literal or implicit belief about the environment that is not named, justified, and enforced.

Examples:

```cpp
constexpr int cache_line = 64;
```

```cpp
if (x < 4096) ...
```

```cpp
reinterpret_cast<std::uintptr_t>(p) & 15
```

```cpp
sizeof(long) == 8
```

```cpp
sizeof(int) == 4
```

```cpp
char is signed
```

```cpp
double is IEEE 754 binary64
```

These may be true on some targets, but they are not allowed as silent assumptions.

They must become named target configuration values, static assertions, debug assertions, or explicit runtime checks.

---

## Law 8: No stale documentation

A comment that no longer matches the code is a defect.

A performance claim that no longer matches the benchmark is a defect.

A status tag that no longer matches the implementation is a defect.

---

# 5. Conformance classes

CEP&CC defines several code classes. Different rules apply to different classes.

---

## 5.1 CEP-0: Cycle-exact hot code

CEP-0 is the strictest class.

CEP-0 code includes:

- inner loops,
- hot compiler passes,
- instruction scheduling,
- register allocation,
- parsing hot paths,
- opcode dispatch,
- render loops,
- audio callbacks,
- interrupt handlers,
- real-time control loops,
- lock-free data structure fast paths.

CEP-0 requirements:

- No dynamic allocation after initialization.
- No exceptions.
- No RTTI.
- No virtual dispatch unless explicitly waived.
- No `std::function` or `std::any`.
- No I/O.
- No formatting.
- No locale.
- No filesystem.
- No regex.
- No random number generation unless deterministic and budgeted.
- No syscalls.
- No locks.
- No blocking.
- No thread creation.
- No coroutine allocation.
- No hidden static initialization.
- No nondeterministic iteration order.
- No unbounded recursion.
- No unbounded loops.
- No uninitialized reads.
- No undefined behavior.
- No implementation-defined behavior unless documented and target-controlled.
- No platform-specific intrinsics unless isolated and documented.
- No inline assembly unless isolated and measured.
- No hidden copies.
- No hidden temporaries.
- No hidden constructors.
- No hidden destructors.
- No hidden exception unwind tables in hot code if exceptions are disabled.

CEP-0 functions must be marked as hot and must have:

- full CEP comment block,
- measured cost,
- evidence artifact,
- explicit failure list,
- explicit assumptions.

---

## 5.2 CEP-1: Deterministic runtime code

CEP-1 code is not cycle-exact, but it must still be deterministic and explicit.

Examples:

- startup code,
- shutdown code,
- configuration loading,
- compilation passes that are not hot,
- diagnostics,
- error reporting,
- file loading,
- module loading,
- test utilities,
- benchmark setup.

CEP-1 may use:

- controlled allocation,
- `std::expected`,
- exceptions if policy allows,
- logging,
- formatting,
- filesystem,
- threading,
- synchronization,

but only if the behavior is bounded and documented.

CEP-1 code still requires clean-code comments and explicit assumptions.

---

## 5.3 CEP-2: Offline or non-deterministic tooling

CEP-2 code includes tools where determinism and cycle-exactness are not required.

Examples:

- documentation generators,
- code generators run at build time,
- test dashboards,
- profiling utilities,
- experimental utilities.

CEP-2 still requires clean code, but performance rules are relaxed.

---

## 5.4 CC: Clean-code class

CC applies to all code, regardless of performance class.

Every file, module, type, function, and nontrivial block must satisfy clean-code rules.

---

# 6. Build and toolchain requirements

## 6.1 C++26 mode

The project must compile in C++26 mode.

Examples:

```text
-std=c++26
```

or compiler equivalent.

If the compiler does not fully support C++26, the project must gate unavailable features and document the minimum required compiler version.

---

## 6.2 Feature gating

Every optional C++26 feature must be gated by its official feature-test macro.

The project should maintain a central header or module:

```cpp
// cep/features.hpp
```

or:

```cpp
export module cep.features;
```

This module must define boolean constants such as:

```cpp
inline constexpr bool cep_has_modules = ...;
inline constexpr bool cep_has_consteval = ...;
inline constexpr bool cep_has_mdspan = ...;
inline constexpr bool cep_has_expected = ...;
```

Do not scatter feature checks across the codebase.

A feature that is not available must either:

1. be excluded cleanly, or
2. cause a compile-time error with a clear diagnostic.

Silent fallback is banned unless the fallback is explicitly documented.

---

## 6.3 Warnings

Warnings must be treated as errors.

Minimum recommended GCC/Clang flags:

```text
-Wall
-Wextra
-Wpedantic
-Wconversion
-Wsign-conversion
-Wshadow
-Wnon-virtual-dtor
-Wold-style-cast
-Woverloaded-virtual
-Wformat=2
-Werror
```

Minimum recommended MSVC flags:

```text
/W4
/WX
/permissive-
/utf-8
/Zc:__cplusplus
```

Additional warnings should be enabled where available.

---

## 6.4 Forbidden compiler options

The following options are banned in CEP-0 unless explicitly waived:

- `-ffast-math`
- `-funsafe-math-optimizations`
- equivalents that change floating-point semantics
- options that remove undefined behavior traps without documentation
- options that make builds nondeterministic
- options that embed local paths unintentionally
- options that depend on machine-specific state without target configuration

Profile-guided optimization may be used only if:

- the profile is checked in,
- the profile generation is reproducible,
- the benchmark suite is deterministic,
- performance regressions are gated.

---

## 6.5 Deterministic builds

Builds must be deterministic.

That means identical source, compiler, flags, and dependencies must produce identical binaries, except for intentionally embedded version metadata.

Builds must not depend on:

- locale,
- timezone,
- environment variables,
- random seed,
- file iteration order,
- hash seed,
- absolute source paths,
- user name,
- machine name,
- current date/time, unless versioning intentionally requires it.

---

## 6.6 Sanitizers

CI must run:

- AddressSanitizer,
- UndefinedBehaviorSanitizer,
- ThreadSanitizer where concurrency exists,
- MemorySanitizer where supported and relevant,
- debug assertions,
- contract checks if available.

CEP-0 code must be clean under all enabled sanitizers.

Sanitizers are not sufficient for cycle-exact verification, but they are necessary for correctness.

---

# 7. Source organization

## 7.1 Hot/cold separation

Hot code and cold code must be separated.

Recommended namespace layout:

```cpp
namespace cep::hot {}
namespace cep::cold {}
namespace cep::target {}
namespace cep::detail {}
```

Hot code should not depend on cold code except through explicit, documented, cold-entry interfaces.

Cold code may call hot code.

Hot code should not call cold code.

Examples of cold calls banned from hot code:

- logging,
- formatting,
- exception throwing,
- file I/O,
- allocation,
- mutex locking,
- dynamic loading,
- environment queries.

---

## 7.2 Target abstraction

Target-specific behavior must be isolated.

Recommended:

```cpp
namespace cep::target {
    inline constexpr std::size_t cache_line_bytes = CEP_TARGET_CACHE_LINE_BYTES;
    inline constexpr std::size_t page_bytes = CEP_TARGET_PAGE_BYTES;
    inline constexpr bool little_endian = CEP_TARGET_LITTLE_ENDIAN;
}
```

Target-specific code must not leak into generic hot code.

Bad:

```cpp
if (is_x86) {
    ...
}
```

Good:

```cpp
target::do_memory_barrier();
```

or:

```cpp
template <Target T>
void lower_instruction(const Instruction& insn);
```

---

## 7.3 Module boundaries

Modules should be used for all first-party code.

Module interfaces should export only stable API.

Implementation details should live in module partitions or implementation units.

Modules must not leak macros.

Modules must not depend on include order.

Modules must not require the consumer to define configuration macros before importing unless documented.

---

# 8. Detailed C++26 language feature policy

This section explains which language features to use, when to use them, why to use them, and when not to use them.

The general rule is:

> If a feature is not explicitly allowed here, it is restricted in CEP-0 until proven safe and measured.

---

## 8.1 Modules

### What they are

Modules replace textual header inclusion with compiled interface units.

### Use

Use modules for all first-party components.

Use modules to:

- define public interfaces,
- hide implementation details,
- reduce accidental macro leakage,
- improve build determinism,
- improve compile-time isolation.

### Why

Modules are cleaner than headers because they:

- avoid repeated textual inclusion,
- avoid include-order bugs,
- reduce macro pollution,
- make dependencies more explicit,
- support better tooling.

### When not to use

Do not use modules as a thin wrapper around chaotic headers.

Do not create module interfaces that re-export everything indiscriminately.

Do not use module interface units for generated code unless the generator is deterministic and reviewed.

### CEP comment requirements

Every module interface should have:

```cpp
// CEP:WHAT: Module interface for ...
// CEP:WHY: Provides ...
// CEP:STATUS: complete
// CEP:FAILURE: none
// CEP:ASSUMES: ...
// CEP:COST: compile-time only
// CEP:EVIDENCE: build graph / review
```

---

## 8.2 Header units

### What they are

Header units allow legacy headers to be imported instead of included.

### Use

Use header units to isolate third-party or legacy C headers.

### Why

They reduce macro leakage and can improve build consistency.

### When not to use

Do not use header units as the primary architecture if native modules are possible.

Do not import unstable generated headers into hot module interfaces.

Do not allow header units to hide missing dependencies.

---

## 8.3 Namespaces

### Use

All project code must be inside namespaces.

Use nested namespaces to express architecture:

```cpp
namespace cep::codegen {}
namespace cep::ir {}
namespace cep::target::arm64 {}
```

### Why

Namespaces prevent name collisions and communicate structure.

### When not to use

Do not use `using namespace` in module interfaces.

Do not use `using namespace std;` anywhere.

Do not use namespace aliases to hide important origin information in public APIs.

### Rules

- No `using namespace` in headers/module interfaces.
- `using namespace` may be allowed inside function bodies in implementation units, but only if it does not hurt clarity.
- `using enum` is allowed in limited scope when it improves clarity.

---

## 8.4 Preprocessor

### Policy

The preprocessor is banned for logic.

Allowed uses:

- include guards only where modules are not used,
- feature-test gating,
- target configuration macros,
- compiler workaround macros if documented,
- version macros.

Banned uses:

- macro-generated control flow,
- macro DSLs,
- macro-based reflection,
- macro-based serialization,
- macro-based loops,
- macro-based function generation,
- macro-based type generation, except where required by compiler bug workarounds.

### Why

Macros hide syntax, defeat type checking, defeat scoping, and make diagnostics worse.

### When macros are allowed

Macros are allowed only when:

- there is no language alternative,
- the macro is isolated,
- the macro has a `CEP_` prefix,
- the macro is documented,
- the macro is tested,
- the macro does not generate hidden control flow.

---

## 8.5 `auto`, `decltype`, and CTAD

### Use

Use `auto` when:

- the type is obvious from context,
- the type is long and does not matter semantically,
- avoiding truncation is important,
- generic programming requires it.

Use `decltype` and `decltype(auto)` when:

- exact type deduction is required,
- writing the type manually would be fragile.

Use class template argument deduction only when:

- the deduced type is obvious,
- there is no surprising ownership change,
- the deduction guide is explicit and reviewed.

### Why

These features reduce verbosity and prevent accidental narrowing.

### When not to use

Do not use `auto` when the exact type matters for:

- ABI,
- serialization,
- binary layout,
- overflow behavior,
- signedness,
- width,
- alignment,
- API clarity.

Do not use CTAD for owning resource types unless the deduction guide is explicit.

Bad:

```cpp
auto x = get_value();
```

Good when type matters:

```cpp
std::uint32_t x = get_value();
```

---

## 8.6 Structured bindings

### Use

Use structured bindings to decompose:

- pairs,
- tuples,
- structs,
- arrays,
- map iteration results.

### Why

They improve readability and reduce accidental misuse of tuple indices.

### When not to use

Do not use structured bindings when:

- they hide copies,
- they bind large objects by value,
- they create unused variables in hot code,
- they obscure lifetimes.

Use reference bindings when needed:

```cpp
auto&& [key, value] = *it;
```

---

## 8.7 Designated initializers

### Use

Use designated initializers for aggregate configuration structs.

Example:

```cpp
struct SchedulerConfig {
    bool enable_ra;
    bool enable_ls;
    std::size_t max_pressure;
};

constexpr SchedulerConfig cfg{
    .enable_ra = true,
    .enable_ls = false,
    .max_pressure = 64,
};
```

### Why

Designated initializers prevent positional mistakes and make configuration explicit.

### When not to use

Do not use them for:

- non-aggregates,
- types with hidden constructors,
- types where initialization order has side effects.

---

## 8.8 Aggregate initialization

### Use

Use aggregate initialization for plain data structures.

### Why

It is explicit, simple, and avoids constructor overhead.

### When not to use

Do not use aggregate initialization when:

- invariants must be enforced,
- construction can fail,
- resource ownership must be validated,
- initialization order is subtle.

If a type has invariants, give it a constructor or factory function.

---

## 8.9 `std::initializer_list`

### Use

Use `std::initializer_list` only in cold code or convenience APIs.

### Why

It is readable for small literal lists.

### When not to use

Do not use it in CEP-0 unless the generated code is measured.

Do not use it for large data.

Do not use it where allocation or temporary array creation is unacceptable.

---

## 8.10 `constexpr`

### Use

Use `constexpr` for:

- constants,
- lookup tables,
- compile-time computation,
- configuration values,
- math that can be done at compile time.

### Why

`constexpr` moves cost to compile time and can produce literal zero runtime cost.

### When not to use

Do not mark functions `constexpr` as decoration.

Do not use `constexpr` functions that perform dynamic allocation at compile time unless the compiler and library support it and the compile-time cost is controlled.

Do not rely on constant evaluation if the function can also run at runtime unless behavior is identical.

---

## 8.11 `consteval`

### Use

Use `consteval` for functions that must only run at compile time.

### Why

It guarantees no runtime residue.

### When not to use

Do not use `consteval` for functions that may need to run at runtime.

Do not use `consteval` if the function depends on runtime configuration.

---

## 8.12 `constinit`

### Use

Use `constinit` for global variables that are initialized at compile time but may be mutable.

### Why

It prevents dynamic initialization order bugs.

### When not to use

Do not use `constinit` for variables that can be `constexpr`.

Do not use `constinit` for variables with nontrivial destructors unless justified.

---

## 8.13 `if consteval`

### Use

Use `if consteval` to separate compile-time and runtime behavior.

### Why

It is clearer than older constant-evaluation checks.

### When not to use

Do not use it to create different observable semantics without documentation.

If compile-time and runtime behavior differ, the difference must be explained in comments.

---

## 8.14 Concepts

### Use

Use concepts to constrain templates.

Use concepts to express:

- numeric requirements,
- iterator requirements,
- callable requirements,
- layout requirements,
- target requirements,
- compile-time configuration requirements.

### Why

Concepts improve diagnostics and prevent invalid instantiations.

They are zero runtime cost.

### When not to use

Do not create overly complex concept chains that explode compile time.

Do not use concepts to encode runtime policy unless the policy is compile-time decidable.

Do not use concepts as a substitute for runtime validation.

---

## 8.15 Templates

### Use

Use templates for static polymorphism in hot code.

Templates are preferred over virtual dispatch in CEP-0.

### Why

Templates can inline and remove indirect dispatch.

### When not to use

Do not allow template instantiation explosion.

Do not use deeply recursive template metaprogramming in hot compile paths unless compile-time cost is measured.

Do not hide control flow in template specialization unless documented.

Template instantiations must be auditable.

---

## 8.16 Fold expressions

### Use

Use fold expressions for variadic operations where the operation is obvious.

### Why

They are concise and often generate good code.

### When not to use

Do not use fold expressions for complex control flow.

Do not use them when evaluation order matters and is not obvious.

---

## 8.17 Lambdas

### Use

Use lambdas for local function objects.

Use stateless lambdas in hot code when possible.

### Why

Lambdas inline well and avoid named function clutter.

### When not to use

Do not use lambdas with:

- complex captures,
- mutable hidden state,
- references with unclear lifetimes,
- large captures,
- runtime polymorphism,
- side effects in hot loops, unless explicitly documented.

Avoid:

```cpp
auto f = [x, y, z]() mutable { ... };
```

in CEP-0 unless the state machine is explicit.

---

## 8.18 Generic lambdas and template lambdas

### Use

Use generic lambdas for local generic algorithms.

### Why

They reduce boilerplate.

### When not to use

Do not use them if they obscure the exact type being processed.

Do not use them if they cause excessive instantiation.

---

## 8.19 Deducing this / explicit object parameters

### Use

Use deducing this when:

- recursive lambdas are needed,
- overload sets are simplified,
- explicit object parameter improves clarity.

### Why

It can reduce boilerplate and make overload control cleaner.

### When not to use

Do not use it in hot code without inspecting generated code.

Do not use it if it makes call semantics unclear.

---

## 8.20 Attributes

Attributes must be used carefully.

### `[[nodiscard]]`

Required for:

- error types,
- status types,
- factory functions,
- resource handles,
- validation functions.

Do not remove `[[nodiscard]]` to silence warnings.

### `[[fallthrough]]`

Required when switch cases intentionally fall through.

Do not rely on comments alone.

### `[[maybe_unused]]`

Allowed for intentionally unused variables in templates or debug-only code.

Do not use it to hide dead code.

### `[[deprecated]]`

Use for deprecated APIs with a message.

Do not allow deprecated APIs in CEP-0.

### `[[likely]]` and `[[unlikely]]`

Restricted.

Use only when profiling evidence exists.

Do not use them as aesthetic hints.

### `[[assume]]`

Restricted.

Use only when:

- the assumption is proven,
- debug assertion checks it,
- the assumption is documented,
- failure behavior is defined.

Never use `[[assume]]` as validation.

### `[[no_unique_address]]`

Conditional.

Use for empty allocator/tag members when ABI impact is understood.

Do not use in shared libraries without ABI review.

---

## 8.21 `static_assert`

### Use

Use `static_assert` for every compile-time assumption.

Examples:

- type width,
- alignment,
- endianness,
- layout,
- ABI compatibility,
- enum values,
- feature availability.

### Why

`static_assert` is literal zero runtime cost and makes assumptions visible.

### Rules

Every `static_assert` must have a meaningful message.

Bad:

```cpp
static_assert(sizeof(T) == 4);
```

Good:

```cpp
static_assert(sizeof(T) == 4, "T must be exactly 32 bits for binary format compatibility");
```

---

## 8.22 Contracts

If your C++26 toolchain provides contracts, use them.

### Use

Use contracts for:

- preconditions,
- postconditions,
- invariants.

### Why

Contracts make assumptions explicit and can be checked in debug or audit builds.

### When not to use

Do not use contracts for:

- side effects,
- logging in hot code,
- allocation,
- runtime error handling,
- replacing proper validation,
- changing observable behavior.

Contract checking must be configurable.

For CEP-0 release builds, contracts must either:

- be compiled out, or
- trap deterministically.

They must not allocate, format, or throw.

---

## 8.23 Static reflection

If your C++26 toolchain provides static reflection, treat it as a powerful but dangerous feature.

### Use

Use static reflection for:

- compile-time code generation,
- enum-to-string tables,
- serialization metadata,
- visitor generation,
- invariant checking,
- documentation generation,
- compile-time validation.

### Why

Static reflection can be zero runtime cost if it generates compile-time tables or inline functions.

### When not to use

Do not use static reflection if:

- generated code is not reviewed,
- generated code is not tested,
- generated code introduces hidden branches,
- generated code explodes compile time,
- generated code is difficult to debug,
- generated code depends on nondeterministic reflection ordering.

Generated code is not exempt from CEP&CC. It must satisfy the same rules as handwritten code.

---

## 8.24 Pattern matching

If your C++26 toolchain provides pattern matching, use it conditionally.

### Use

Use pattern matching for:

- closed state machines,
- tagged unions,
- opcode dispatch,
- IR node classification,
- exhaustive enum handling.

### Why

Pattern matching can improve clarity and generate jump tables.

### When not to use

Do not use pattern matching when:

- patterns allocate,
- patterns call opaque predicates,
- patterns hide side effects,
- patterns create temporary copies,
- pattern guards make branch prediction unclear,
- codegen is worse than explicit switch.

For CEP-0, inspect generated assembly.

---

## 8.25 Coroutines

### Policy

Coroutines are restricted.

### Use

Use coroutines only for:

- asynchronous orchestration,
- generator-like cold pipelines,
- state machines where clarity outweighs cost.

### Why

Coroutines can make asynchronous control flow easier to read.

### When not to use

Do not use coroutines in CEP-0 unless all of the following are true:

- no heap allocation occurs,
- promise type is explicit,
- allocator behavior is deterministic,
- resume points are deterministic,
- destruction is deterministic,
- generated state machine is measured,
- no hidden locking occurs.

Coroutines often hide:

- allocation,
- indirect resume,
- heap frames,
- destructor ordering,
- cancellation behavior.

Therefore they are banned by default in hot code.

---

## 8.26 Exceptions

### Policy

Exceptions are restricted.

### Use

Exceptions may be used in CEP-1 if:

- the project explicitly enables exceptions,
- exceptions are not used for control flow,
- destructors are noexcept,
- exception objects are small and non-allocating,
- hot boundaries are exception-free.

### Why

Exceptions can simplify cold error handling.

### When not to use

Exceptions are banned in CEP-0.

Do not use exceptions for:

- expected failures,
- parsing failures,
- validation failures,
- resource exhaustion,
- control flow,
- hot-path diagnostics,
- destructors,
- move constructors that must not fail,
- allocator failure in hot code.

Use `std::expected`, status codes, or result types instead.

---

## 8.27 RTTI

### Policy

RTTI is banned in hot code.

### Use

RTTI may be used in cold diagnostic or serialization code if required.

### Why

RTTI can simplify dynamic type queries in cold code.

### When not to use

Do not use RTTI in CEP-0.

Do not use `dynamic_cast` in hot paths.

Do not use `typeid` for dispatch in hot paths.

Use static dispatch, tagged enums, or concepts instead.

---

## 8.28 Virtual functions

### Policy

Virtual functions are banned in CEP-0 unless waived.

### Use

Use virtual functions for:

- plugin interfaces,
- cold abstraction boundaries,
- replaceable tools,
- diagnostic hooks.

### Why

Virtual functions provide runtime polymorphism.

### When not to use

Do not use virtual functions in:

- inner loops,
- dispatch loops,
- IR traversal hot paths,
- instruction selection,
- register allocation,
- renderer hot paths,
- audio callbacks.

Virtual dispatch hides indirect branch cost and prevents inlining.

Preferred alternatives:

- templates,
- concepts,
- CRTP,
- tagged unions,
- enums,
- function pointer tables,
- compile-time dispatch.

---

## 8.29 `new` and `delete`

### Policy

`new` and `delete` are banned in CEP-0.

### Use

Use `new` only in initialization phases or allocator implementations.

### Why

Dynamic allocation is expensive and nondeterministic.

### When not to use

Do not allocate after initialization in hot code.

Use:

- stack allocation,
- arena allocation,
- fixed buffers,
- `std::inplace_vector`,
- static storage.

---

## 8.30 Move semantics

### Use

Use move semantics for transferring ownership of resources.

### Why

Move semantics avoid unnecessary copies.

### Rules

- Move constructors should be `noexcept` when possible.
- Move assignment should leave objects in a valid but unspecified state.
- Moved-from objects must not be used except to destroy or reassign.
- Hot code should avoid move operations that hide deallocation.

---

## 8.31 Perfect forwarding

### Use

Use perfect forwarding in factory functions and emplacement APIs.

### Why

It avoids copies and preserves value category.

### When not to use

Do not use perfect forwarding when:

- the forwarded type is unclear,
- diagnostics become unreadable,
- hot code becomes dependent on template instantiation,
- forwarding hides allocation.

---

## 8.32 Operator overloading

### Use

Overload operators only when the operator meaning is conventional.

Examples:

- arithmetic types,
- iterators,
- mathematical vectors,
- strongly typed units.

### Why

Natural operator use can improve clarity.

### When not to use

Do not overload operators to create DSLs.

Do not overload operators that hide:

- allocation,
- I/O,
- locking,
- formatting,
- network calls,
- filesystem calls.

Do not overload `&&`, `||`, or `,`.

---

## 8.33 User-defined literals

### Use

User-defined literals may be used for strongly typed units:

- bytes,
- cycles,
- hertz,
- milliseconds,
- degrees.

### Why

They improve expressiveness and prevent unit mistakes.

### When not to use

Do not use them if they hide runtime work.

Do not use them for parsing complex strings.

Do not use them in hot code if they allocate.

---

## 8.34 Enumerations

### Use

Use `enum class` for all enumerations.

Do not use unscoped enums unless interfacing with C.

### Why

Scoped enums prevent implicit conversion and name pollution.

### Rules

- Give enums explicit underlying type if binary size matters.
- Use `std::to_underlying` for conversion.
- Do not cast raw integers to enums without validation.

---

## 8.35 `using enum`

### Use

Use `using enum` in local scopes where it improves readability.

### When not to use

Do not use `using enum` in module interfaces.

Do not use it in large scopes where it creates ambiguity.

---

## 8.36 Three-way comparison

### Use

Use `operator<=>` when:

- ordering is meaningful,
- the type is value-like,
- comparison is simple.

### Why

It reduces boilerplate.

### When not to use

Do not use three-way comparison if:

- comparison is expensive,
- comparison is nondeterministic,
- comparison depends on locale,
- comparison depends on uninitialized memory,
- comparison is not total.

For hot code, compare only fields that matter and document cost.

---

## 8.37 `noexcept`

### Use

Mark functions `noexcept` when they truly cannot throw.

Use `noexcept` for:

- move constructors,
- move assignment,
- destructors,
- swap,
- low-level accessors,
- hot functions when exceptions are disabled.

### Why

`noexcept` improves optimization and exception safety.

### Rules

Do not lie.

If a function can throw, do not mark it `noexcept`.

If a function is `noexcept` but calls potentially throwing code, that is a defect.

---

## 8.38 `volatile`

### Policy

`volatile` is restricted.

### Use

Use `volatile` only for memory-mapped I/O or hardware registers.

### When not to use

Do not use `volatile` for:

- concurrency,
- atomics,
- timing,
- preventing optimization of benchmarks,
- preventing compiler reordering in lock-free code.

Use `std::atomic` for concurrency.

---

## 8.39 Inline assembly

### Policy

Inline assembly is restricted and must be isolated.

### Use

Use inline assembly only when:

- a required instruction is not exposed by the compiler,
- exact instruction sequence is required,
- performance-critical sequence cannot be generated reliably.

### Why

Inline assembly gives exact control.

### When not to use

Do not use inline assembly in generic code.

Do not use it for trivial operations the compiler can do well.

Do not use it without documenting:

- clobbers,
- inputs,
- outputs,
- alignment,
- side effects,
- target constraints,
- cycle cost.

Inline assembly must live in target-specific modules.

---

## 8.40 Alignment

### Use

Use `alignas` to enforce alignment.

Use alignment for:

- SIMD,
- cache-line separation,
- DMA buffers,
- lock-free structures.

### Why

Alignment affects performance and correctness.

### Rules

Do not assume alignment.

Prove alignment with:

- `alignas`,
- allocator guarantees,
- static assertions,
- debug checks.

Do not use `reinterpret_cast` to infer alignment without checks.

---

## 8.41 `std::assume_aligned` equivalent

If using aligned pointer hints:

### Use

Use only when alignment is proven.

### Why

It can improve vectorization.

### When not to use

Do not use alignment hints without debug assertion or static proof.

An incorrect alignment hint is undefined behavior.

---

## 8.42 `thread_local`

### Policy

`thread_local` is restricted.

### Use

Use `thread_local` for:

- per-thread arenas,
- per-thread buffers,
- per-thread diagnostic state.

### Why

It can avoid false sharing and locks.

### When not to use

Do not use `thread_local` in CEP-0 unless:

- initialization cost is known,
- access cost is measured,
- destruction cost is known,
- TLS model is understood.

Do not use `thread_local` for hidden lazy initialization.

---

## 8.43 Atomics

### Use

Use atomics for concurrency.

Use explicit memory orders.

### Why

Atomics prevent data races.

### When not to use

Do not use atomics in hot code unless necessary.

Do not use default `std::memory_order_seq_cst` in hot code without justification.

Do not use atomics for:

- logging counters that can be batched,
- metrics that can be thread-local,
- reference counting in hot loops unless measured.

Every atomic operation must document:

- memory order,
- ordering rationale,
- failure points,
- ABA considerations,
- lifetime assumptions.

---

## 8.44 Source character set and string literals

### Use

Use UTF-8 source encoding.

Use `u8`, `u`, `U` string literals only when encoding matters.

### Why

Text encoding assumptions are a common portability defect.

### When not to use

Do not assume `char` is signed.

Do not assume `char` can hold arbitrary Unicode.

Do not use narrow strings for user-visible text without explicit encoding policy.

---

# 9. Detailed C++26 standard library policy

This section covers major standard library feature classes.

General rule:

> If a library feature is not explicitly allowed here, it is restricted in CEP-0 until proven safe and measured.

---

## 9.1 `std::array`

### Use

Use `std::array` for fixed-size arrays.

### Why

It provides bounds-aware access without heap allocation.

### When not to use

Do not use it for runtime-sized data.

Do not use it for large objects on small stacks.

---

## 9.2 `std::vector`

### Use

Use `std::vector` in CEP-1 for dynamic storage.

Use `reserve` when final size is known.

### Why

It is the default dynamic sequence container.

### When not to use

Do not use `std::vector` in CEP-0 unless:

- capacity is reserved before entering hot code,
- no reallocation occurs,
- no exceptions are used,
- element type is trivially relocatable or proven safe.

Do not use `std::vector<bool>` in performance-sensitive code.

---

## 9.3 `std::inplace_vector` or equivalent bounded vector

### Use

Use bounded inline vectors for small collections that must not allocate.

### Why

They avoid heap allocation while retaining vector-like API.

### When not to use

Do not use them when maximum capacity is unknown.

Do not use them for large objects.

Document behavior on overflow.

---

## 9.4 `std::span`

### Use

Use `std::span` for non-owning contiguous views.

### Why

It makes bounds explicit and avoids raw pointer/size pairs.

### When not to use

Do not use `std::span` for ownership.

Do not store spans to temporaries.

Do not use dynamic-extent spans in CEP-0 if static extent is possible.

---

## 9.5 `std::mdspan`

### Use

Use `std::mdspan` for multidimensional non-owning views.

### Why

It makes layout and stride explicit.

### When not to use

Do not use dynamic layout objects in hot code unless measured.

Do not allow hidden bounds checks in CEP-0 release builds.

---

## 9.6 `std::mdspan` subview facilities

If available, use subviews carefully.

### Use

Use subviews for slicing matrices or tensors without copying.

### Why

They avoid allocation.

### When not to use

Do not use subviews if they create hidden temporaries or complex stride calculations in hot code.

---

## 9.7 `std::string`

### Use

Use `std::string` for owning text in cold code.

### Why

It is convenient and safe compared to raw C strings.

### When not to use

Do not use `std::string` in CEP-0.

Do not use it for:

- hot parsing,
- hot formatting,
- hot logging,
- hot diagnostics.

Use `std::string_view` for read-only text.

---

## 9.8 `std::string_view`

### Use

Use `std::string_view` for non-owning read-only text.

### Why

It avoids allocation and copying.

### When not to use

Do not store string views to temporaries.

Do not assume null termination.

Do not use string views for text that may be mutated.

---

## 9.9 `std::optional`

### Use

Use `std::optional` for values that may be absent.

### Why

It makes absence explicit without using pointers.

### When not to use

Do not use `std::optional` when:

- absence is an error,
- absence requires allocation,
- the contained type is large and hot,
- the optional branch is unpredictable and costly.

Do not use `std::optional` as a substitute for proper initialization.

---

## 9.10 `std::optional` reference-like facilities

If C++26 provides optional references:

### Use

Use optional references for nullable reference semantics.

### Why

They can be clearer than raw pointers.

### When not to use

Do not use them if they hide nullability.

Do not use them if pointer codegen is better and the pointer is already non-owning.

---

## 9.11 `std::expected`

### Use

Use `std::expected` for recoverable errors.

### Why

It avoids exceptions and makes failure explicit.

### Rules

- Error type must be small.
- Error type must not allocate.
- Error type must not throw.
- Error construction must be cheap.
- Error paths must be documented.

### When not to use

Do not use `std::expected` for impossible failures.

Do not use it for control flow that is expected in the normal path unless performance is measured.

---

## 9.12 `std::variant`

### Use

Use `std::variant` for closed sum types.

### Why

It is type-safe compared to unions.

### When not to use

Do not use `std::variant` in CEP-0 unless:

- discriminant access is measured,
- visitation codegen is inspected,
- no allocation occurs,
- no exceptions occur,
- active index changes are controlled.

Do not use recursive variants in hot code.

---

## 9.13 `std::any`

### Policy

Banned in hot code.

### Use

Only allowed in cold reflective tooling if absolutely necessary.

### Why banned

It hides type erasure, allocation, and indirect access.

---

## 9.14 `std::function`

### Policy

Banned in hot code.

### Use

Allowed only in cold callback registration.

### Why banned

It hides:

- type erasure,
- allocation,
- indirect calls,
- possible virtual-like dispatch.

Use templates, concepts, or function pointers instead.

---

## 9.15 `std::bind`

### Policy

Banned.

Use lambdas instead.

---

## 9.16 `std::reference_wrapper`

### Use

Use for containers of references or temporary reference wrappers.

### When not to use

Do not use it to hide lifetime problems.

Do not use it in public APIs unless reference semantics are obvious.

---

## 9.17 `std::tuple` and `std::pair`

### Use

Use for simple heterogeneous aggregates.

### Why

They avoid boilerplate for small return types.

### When not to use

Do not use large tuples in hot code.

Do not use tuple indices when named struct fields would be clearer.

If a tuple has semantic meaning, create a struct.

---

## 9.18 `std::bitset`

### Use

Use for fixed-size bit flags.

### Why

It is explicit and compact.

### When not to use

Do not use it when dynamic size is needed.

Do not use it for performance-critical bitmask operations if generated code is worse than manual integers.

---

## 9.19 `std::vector<bool>`

### Policy

Banned in performance-sensitive code.

Use `std::vector<std::uint8_t>` or custom bitmask with explicit layout.

---

## 9.20 Associative containers

### `std::map` / `std::set`

Use only when ordered tree semantics are required.

Avoid in hot code due to pointer chasing.

### `std::unordered_map` / `std::unordered_set`

Restricted.

Use only when:

- hash is deterministic,
- bucket count is fixed,
- rehashing is disabled or budgeted,
- collision behavior is documented,
- iteration order is not relied upon.

Banned in CEP-0 unless fully measured.

### `std::flat_map` / `std::flat_set` if available

Use for read-mostly sorted associative data.

Avoid for high mutation.

Document insertion and deletion cost.

---

## 9.21 `std::list` and `std::forward_list`

### Policy

Banned in hot code.

### Use

Only for rare cases requiring stable iterators and node-based insertion in cold code.

### Why banned

Pointer chasing and allocation are poor for performance.

---

## 9.22 `std::deque`

### Use

Use for double-ended queues in cold or CEP-1 code.

### When not to use

Avoid in CEP-0 due to chunked layout and indirect access.

---

## 9.23 `std::hive` or equivalent stable container

If available:

### Use

Use when element stability is required and allocation is controlled.

### When not to use

Do not use in CEP-0 unless:

- chunk layout is measured,
- pointer chasing is acceptable,
- allocation is bounded.

---

## 9.24 Algorithms

### Use

Use standard algorithms when their behavior is clear.

Examples:

- `std::copy`,
- `std::fill`,
- `std::move`,
- `std::transform`,
- `std::find`,
- `std::sort` in cold code.

### Why

They are well-tested and often optimized.

### When not to use

Do not use algorithms with capturing lambdas if they prevent inlining.

Do not use parallel algorithms in CEP-0.

Do not use algorithms that hide allocation or comparison cost.

---

## 9.25 Ranges

### Policy

Restricted.

### Use

Use ranges in CEP-1 or CEP-2 for readable transformations.

### Why

Ranges can express pipelines cleanly.

### When not to use

Do not use ranges in CEP-0 unless:

- generated assembly is inspected,
- no hidden temporaries exist,
- no hidden allocations exist,
- no hidden branches exist,
- no indirect calls exist.

Prefer explicit loops in hot code when performance matters.

---

## 9.26 Views

### Use

Use views for non-owning transformations in cold code.

### When not to use

Do not use complex view adaptors in hot code.

Do not use filters with expensive predicates in hot code unless measured.

---

## 9.27 Execution policies

### Policy

Banned in CEP-0.

### Use

Use only for offline batch processing where nondeterminism and scheduling are acceptable.

### Why banned

Execution policies introduce scheduling, synchronization, and nondeterminism.

---

## 9.28 `std::execution` senders/receivers if available

### Policy

Restricted.

### Use

Use for structured asynchronous orchestration outside hot paths.

### Why restricted

Async frameworks hide scheduling, allocation, and cancellation behavior.

### When not to use

Do not use in CEP-0.

Do not use for deterministic frame loops or interrupt paths.

---

## 9.29 Threads

### Use

Use `std::thread` or `std::jthread` for control-plane concurrency.

### When not to use

Do not create threads in hot code.

Do not use threads for short work unless measured.

Use `std::jthread` for scoped lifetime and cancellation where appropriate.

---

## 9.30 Mutexes and locks

### Policy

Banned in CEP-0.

### Use

Use mutexes for coarse-grained protection in cold code.

### Why banned

Locks introduce priority inversion, cache contention, and nondeterministic latency.

### When allowed

Allowed in CEP-1 if:

- lock scope is documented,
- deadlock analysis exists,
- lock ordering is documented,
- contention is bounded.

---

## 9.31 Condition variables

### Use

Use for thread coordination in cold or control-plane code.

### When not to use

Do not use in real-time hot paths.

Document spurious wakeup handling.

---

## 9.32 Latches, barriers, semaphores

### Use

Use for synchronization patterns where their semantics fit.

### When not to use

Do not use in CEP-0.

Document memory ordering and lifetime assumptions.

---

## 9.33 `std::atomic`

Already covered in language section, but library rules:

- Use explicit memory orders.
- Use `std::atomic_ref` only when necessary.
- Avoid atomic reference counting in hot code.
- Avoid atomic arithmetic for metrics in hot loops; use thread-local accumulation.

---

## 9.34 Hazard pointers and RCU if available

### Policy

Restricted.

### Use

Use only for specialized lock-free reclamation.

### When not to use

Do not use without rigorous proof and measurement.

Do not use in CEP-0 unless reclamation cost is budgeted.

---

## 9.35 `std::chrono`

### Use

Use for timing, clocks, and durations in cold or measurement code.

### Why

Portable time representation.

### When not to use

Do not call clock functions inside CEP-0 unless the clock source is budgeted and deterministic.

Do not use `std::chrono` for per-frame hot timestamps without measuring overhead.

---

## 9.36 `std::format`

### Policy

Banned in hot code.

### Use

Use for cold diagnostics and logs.

### Why banned

Formatting is expensive and often allocates.

---

## 9.37 `std::print` / `std::println`

### Policy

Banned in hot code.

### Use

Use for cold output.

### Why banned

I/O is expensive and nondeterministic.

---

## 9.38 Iostreams

### Policy

Banned in hot code.

### Use

Use only for legacy or cold diagnostics.

### Why banned

Iostreams can be heavy, locale-sensitive, and synchronized.

Prefer explicit binary I/O or cold logging interfaces.

---

## 9.39 `std::filesystem`

### Policy

Banned in hot code.

### Use

Use for file operations in cold code.

### Why banned

Filesystem operations involve syscalls, allocations, and nondeterminism.

---

## 9.40 `std::regex`

### Policy

Banned in hot code.

### Use

Use only for cold text processing if absolutely necessary.

### Why banned

Regex engines are expensive and often nondeterministic in performance.

Prefer explicit parsers.

---

## 9.41 Locale facilities

### Policy

Banned in hot code.

### Use

Use only when user-facing localization is required.

### Why banned

Locale behavior is expensive, stateful, and nondeterministic for performance.

---

## 9.42 Random number generation

### Policy

Restricted.

### Use

Use deterministic engines with explicit seeds for testing or simulation.

### When not to use

Do not use random number generation in CEP-0 unless:

- deterministic,
- bounded,
- measured,
- required by algorithm.

Do not use nondeterministic random devices in hot code.

---

## 9.43 Numeric facilities

Use:

- `std::bit_cast`,
- `std::byteswap`,
- `std::countl_zero`,
- `std::countr_zero`,
- `std::popcount`,
- `std::rotl`,
- `std::rotr`,
- `std::bit_ceil`,
- `std::bit_floor`,
- `std::bit_width`,
- `std::midpoint`,
- `std::numbers`.

### Why

These are explicit, portable, and often map to hardware.

### When not to use

Do not assume hardware support.

Gate target-specific intrinsics and provide fallbacks.

---

## 9.44 Math functions

### Use

Use math functions only when needed.

### Why restricted

Math functions may:

- allocate,
- set errno,
- have target-specific behavior,
- have unpredictable latency.

### Rules

- Document required precision.
- Document NaN/Inf behavior.
- Document errno behavior.
- Use `std::fma` only when intended.
- Avoid transcendental functions in CEP-0 unless budgeted.

---

## 9.45 Floating-point types

### Use

Use fixed-width floating-point types if available:

- `std::float32_t`
- `std::float64_t`
- `std::bfloat16_t`

### Why

They make precision explicit.

### When not to use

Do not use `long double` unless target support is documented.

Do not assume `float` or `double` sizes in binary formats.

---

## 9.46 `std::complex`

### Use

Use for complex arithmetic in cold or measured numeric code.

### When not to use

Do not use in CEP-0 unless codegen is measured.

---

## 9.47 `std::valarray`

### Policy

Should not be used.

Use explicit arrays, spans, or numeric libraries instead.

---

## 9.48 `std::linalg` if available

### Policy

Restricted.

### Use

Use only if backend kernels are audited.

### Why restricted

Linear algebra libraries can hide allocation, threading, and blocking behavior.

### When not to use

Do not use in CEP-0 without inspecting generated kernels.

---

## 9.49 Type traits

### Use

Use type traits for compile-time decisions.

### Why

Zero runtime cost.

### Rules

Use `std::is_trivially_copyable`, `std::is_nothrow_move_constructible`, etc., to enforce assumptions.

Do not use type traits to silently select dangerous behavior without comments.

---

## 9.50 `std::source_location`

### Use

Use for diagnostics and assertions.

### Why

Better than macros.

### When not to use

Do not store source locations in hot structures.

Do not pass them through CEP-0 inner loops.

---

## 9.51 `std::stacktrace`

### Policy

Cold-only.

### Use

Use for crash reporting.

### Why banned in hot code

Stacktrace capture is expensive.

---

## 9.52 Error handling facilities

Use:

- `std::error_code`,
- `std::error_category`,
- `std::system_error` in cold code,
- `std::expected` for recoverable errors.

Avoid exceptions in hot code.

---

## 9.53 Memory facilities

### Use

- `std::allocator` only in generic containers where unavoidable.
- `std::pmr` only with explicit arenas.
- `std::addressof` when overloading `&` is possible.
- `std::construct_at` / `std::destroy_at` in allocator-aware code.

### When not to use

Do not use polymorphic allocators in CEP-0 unless the resource is fixed and allocation is budgeted.

---

## 9.54 Smart pointers

### `std::unique_ptr`

Use for unique ownership.

Allowed in CEP-1.

Restricted in CEP-0 because deletion may be hidden.

### `std::shared_ptr`

Banned in hot code.

Atomic reference counting is expensive.

### `std::weak_ptr`

Banned in hot code.

Use only in cold ownership graphs.

### Intrusive reference counting

Allowed only if measured and explicitly implemented.

---

## 9.55 `std::function` and callable wrappers

Already covered, but important:

- Banned in CEP-0.
- Use templates/concepts in hot code.
- Use function pointers only when indirect call is budgeted.

---

## 9.56 `std::invoke`

### Use

Use for generic invocation in cold code.

### When not to use

Do not use if it obscures call target in hot code.

---

## 9.57 `std::mem_fn`

Banned.

Use lambdas.

---

## 9.58 `std::not_fn`

Use sparingly.

Do not use if it harms clarity.

---

## 9.59 `std::bind_front` / `std::bind_back`

Conditional.

Use only if clearer than lambda and no hidden allocation.

---

## 9.60 `std::integer_sequence`

Use for compile-time integer sequences.

Why: zero runtime cost.

Do not use to generate huge instantiation chains without compile-time budget.

---

## 9.61 `std::spanstream` or equivalent if available

### Use

Use for buffered text over fixed spans in cold or measured code.

### When not to use

Do not use for hot formatting.

---

## 9.62 `std::charconv`

### Use

Use for fast integer and floating-point conversion when needed.

### Why

It is generally faster than iostreams.

### When not to use

Do not use in CEP-0 unless measured.

Do not assume all formats are supported.

---

# 10. Comment standard

This is one of the most important parts of CEP&CC.

Comments must be:

- zero runtime cost,
- machine-parseable,
- maintained,
- explicit,
- failure-aware,
- assumption-aware,
- cost-aware,
- status-aware.

---

## 10.1 Literal zero-cost requirement

Comments must have literal zero runtime cost.

That means:

- comments are removed by the compiler,
- comments do not create runtime strings,
- comments do not create debug strings unless explicitly intended,
- comments do not affect ABI,
- comments do not affect code generation,
- comments do not create reflection metadata at runtime,
- comments do not create log messages by themselves.

If a comment is parsed by a tool, the generated artifact is not a comment. The generated artifact must satisfy all CEP&CC rules.

---

## 10.2 Required comment fields

Every nontrivial file, module, type, function, hot loop, unsafe block, stub, placeholder, partial implementation, and target-specific block must include these fields:

```cpp
// CEP:WHAT:
// CEP:WHY:
// CEP:STATUS:
// CEP:FAILURE:
// CEP:ASSUMES:
// CEP:COST:
// CEP:EVIDENCE:
```

Optional fields:

```cpp
// CEP:OWNER:
// CEP:TICKET:
// CEP:TARGET:
// CEP:SECURITY:
// CEP:PORTABILITY:
// CEP:REVIEW:
```

---

## 10.3 `CEP:WHAT`

This field describes what the entity is.

It must be a clear noun phrase or short paragraph.

Good:

```cpp
// CEP:WHAT: Decodes a 32-bit instruction into an opcode descriptor.
```

Bad:

```cpp
// decode
```

The WHAT field must not merely repeat the name.

Bad:

```cpp
// CEP:WHAT: decode_instruction function
```

Good:

```cpp
// CEP:WHAT: Decodes a 32-bit RISC instruction into opcode, operand, and privilege metadata.
```

---

## 10.4 `CEP:WHY`

This field explains why the entity exists and why this approach was chosen.

Good:

```cpp
// CEP:WHY: Table lookup is faster than nested switches for the 7-bit opcode space and keeps dispatch data-driven.
```

Bad:

```cpp
// CEP:WHY: fast
```

The WHY field must include rejected alternatives when the choice is non-obvious.

Example:

```cpp
// CEP:WHY: A sorted vector is used instead of unordered_map because the table is read-mostly and cache locality dominates lookup cost.
```

---

## 10.5 `CEP:STATUS`

This field must be one of:

```text
complete
partial
stub
placeholder
```

### Complete

```cpp
// CEP:STATUS: complete
```

Means:

- implemented,
- tested,
- documented,
- reviewed,
- measured if hot.

### Partial

```cpp
// CEP:STATUS: partial
```

Means:

- implemented for a known subset,
- missing cases are listed,
- owner/ticket exists.

Example:

```cpp
// CEP:STATUS: partial
// CEP:TODO(alice): CEP-314: Handle vector predicated instructions.
```

### Stub

```cpp
// CEP:STATUS: stub
```

Means:

- interface exists,
- body is intentionally minimal,
- behavior is not production-complete.

Stubs must fail loudly in debug.

Example:

```cpp
// CEP:STATUS: stub
// CEP:FAILURE: Debug assertion fires if called.
```

### Placeholder

```cpp
// CEP:STATUS: placeholder
```

Means:

- reserved for future implementation,
- should not be used in production paths,
- may not have meaningful behavior.

Placeholders must not be callable in release builds without explicit error handling.

---

## 10.6 `CEP:FAILURE`

This field lists failure points.

If there are no failure points, write:

```cpp
// CEP:FAILURE: none
```

Do not write vague phrases like:

```cpp
// should not fail
```

Good:

```cpp
// CEP:FAILURE: Returns parse_error::bad_opcode if opcode is outside the valid range. No allocation. No throw.
```

Good for hot loop:

```cpp
// CEP:FAILURE: none; input size is bounded and arithmetic uses wrapping u32 by design.
```

Failure points to consider:

- invalid input,
- empty input,
- oversized input,
- overflow,
- underflow,
- NaN,
- infinity,
- division by zero,
- misalignment,
- aliasing,
- dangling reference,
- allocation failure,
- syscall failure,
- timeout,
- interruption,
- race condition,
- deadlock,
- priority inversion,
- unsupported target,
- compiler divergence,
- hardware fault,
- contract violation.

---

## 10.7 `CEP:ASSUMES`

This field lists assumptions.

If there are no assumptions, write:

```cpp
// CEP:ASSUMES: none
```

Do not allow hard-coded assumptions.

Bad:

```cpp
// CEP:ASSUMES: x86-64, little endian
```

Good:

```cpp
// CEP:ASSUMES: target endianness is little; enforced by static_assert in cep::target.
```

Good:

```cpp
// CEP:ASSUMES: data is 4-byte aligned; checked by debug assertion.
```

Every assumption must be enforced by one of:

- `static_assert`,
- debug assertion,
- runtime check,
- contract,
- type constraint,
- target configuration,
- documented caller contract with tests.

---

## 10.8 `CEP:COST`

This field describes performance cost.

For cold code:

```cpp
// CEP:COST: cold; not performance-critical.
```

For compile-time code:

```cpp
// CEP:COST: compile-time only; no runtime instructions.
```

For hot code:

```cpp
// CEP:COST: 3 cycles/element on target cortex-m7, -O3, measured 2026-09-01.
```

Or:

```cpp
// CEP:COST: 12 cycles expected, 18 cycles worst-case on target arm64-a78, artifact bench-193.
```

If cost is not measured:

```cpp
// CEP:COST: not measured; not valid for CEP-0.
```

A CEP-0 function cannot be `complete` without measured cost.

---

## 10.9 `CEP:EVIDENCE`

This field points to proof.

Evidence may be:

- benchmark ID,
- unit test ID,
- fuzz test ID,
- disassembly artifact,
- compiler explorer link hash,
- review record,
- ticket ID.

Examples:

```cpp
// CEP:EVIDENCE: bench CEP-0019, asm artifact a41c9e2.
```

```cpp
// CEP:EVIDENCE: unit test cep::decode::test_opcode_table.
```

```cpp
// CEP:EVIDENCE: review 2026-09-12 by alice.
```

If there is no evidence, the code cannot be marked complete for hot paths.

---

## 10.10 `CEP:OWNER` and `CEP:TICKET`

Required for:

- partial,
- stub,
- placeholder,
- known defects,
- waivers.

Example:

```cpp
// CEP:OWNER: alice
// CEP:TICKET: CEP-512
```

Do not allow anonymous TODOs.

Bad:

```cpp
// TODO: fix later
```

Good:

```cpp
// CEP:TODO(alice): CEP-512: Handle predicated vector encodings.
```

---

## 10.11 `CEP:TARGET`

Use when code is target-specific.

Example:

```cpp
// CEP:TARGET: arm64
```

or:

```cpp
// CEP:TARGET: riscv64
```

This field must exist for:

- inline assembly,
- intrinsics,
- target-specific alignment,
- target-specific cache assumptions,
- target-specific instruction latency.

---

## 10.12 `CEP:SECURITY`

Use when security-sensitive behavior exists.

Examples:

- parsing untrusted input,
- handling secrets,
- cryptographic operations,
- privilege transitions,
- memory safety boundaries.

Example:

```cpp
// CEP:SECURITY: Input may be untrusted; all bounds are checked before read.
```

---

## 10.13 `CEP:PORTABILITY`

Use when behavior depends on portability concerns.

Examples:

- endianness,
- ABI,
- floating-point format,
- character encoding,
- file path semantics.

---

## 10.14 Comment anti-patterns

The following are defects:

### Repeating code

Bad:

```cpp
// increment i
++i;
```

### Vague performance claims

Bad:

```cpp
// fast path
```

Required:

```cpp
// CEP:COST: ...
// CEP:EVIDENCE: ...
```

### Comment-only assumptions

Bad:

```cpp
// assumes aligned
```

Required:

```cpp
// CEP:ASSUMES: aligned to 16 bytes; enforced by CEP_ASSERT.
```

### Commented-out code

Banned.

Use version control.

### Stale comments

Defect.

### Jokes or personal notes

Banned in normative code comments.

### Secrets

Banned.

### TODO without owner/ticket

Banned.

---

# 11. Hard-coded assumptions ban

This section expands the hard-coded assumption rule.

---

## 11.1 What counts as a hard-coded assumption?

Any literal or implicit belief that is not named and enforced.

Examples:

```cpp
if (size > 4096)
```

```cpp
alignas(64)
```

if 64 is not a named target constant.

```cpp
x & 63
```

if 63 assumes 64-byte cache line.

```cpp
sizeof(int) == 4
```

```cpp
sizeof(void*) == 8
```

```cpp
std::endian::native == std::endian::little
```

without static assertion.

---

## 11.2 Required handling

Every assumption must be handled in one of these ways.

### 1. Named constant

```cpp
namespace cep::target {
    inline constexpr std::size_t page_bytes = CEP_TARGET_PAGE_BYTES;
}
```

### 2. Static assertion

```cpp
static_assert(sizeof(std::uint32_t) == 4, "u32 must be exactly 4 bytes");
```

### 3. Debug assertion

```cpp
CEP_ASSERT(is_aligned(ptr, 16));
```

### 4. Runtime check

```cpp
if (!is_aligned(ptr, 16)) return error::misaligned;
```

### 5. Contract

If C++26 contracts are available:

```cpp
pre(is_aligned(ptr, 16));
```

### 6. Type constraint

Use types that make invalid states unrepresentable.

Example:

```cpp
template <std::size_t Alignment>
class AlignedPtr;
```

---

## 11.3 Magic numbers

Magic numbers are banned except for trivial arithmetic identities where meaning is obvious.

Even then, prefer named constants.

Bad:

```cpp
if (opcode == 0x7F)
```

Good:

```cpp
inline constexpr Opcode kVectorOpcode = static_cast<Opcode>(0x7F);
```

Better:

```cpp
enum class Opcode : std::uint8_t {
    vector = 0x7F,
};
```

---

# 12. Failure-point documentation

Every function must document failure behavior.

---

## 12.1 Failure classes

The comment must identify relevant classes:

- input validation failure,
- resource exhaustion,
- arithmetic overflow,
- arithmetic underflow,
- NaN,
- infinity,
- misalignment,
- aliasing violation,
- lifetime violation,
- allocation failure,
- syscall failure,
- concurrency race,
- deadlock,
- timeout,
- cancellation,
- interruption,
- unsupported feature,
- target mismatch,
- compiler divergence,
- hardware fault.

---

## 12.2 Failure behavior

For each failure, state what happens:

- returns error,
- traps,
- asserts,
- throws (if allowed),
- logs cold,
- aborts,
- retries,
- ignores safely.

Do not leave failure behavior implicit.

---

# 13. Cycle-exact measurement requirements

## 13.1 Measurement artifacts

For every CEP-0 function, store:

- source revision,
- compiler version,
- compiler flags,
- target CPU,
- target frequency state,
- input dataset,
- benchmark harness version,
- measured cycles,
- measured instructions,
- disassembly excerpt or hash.

---

## 13.2 Benchmark environment

Benchmarks should:

- pin threads,
- disable turbo where possible,
- use fixed frequency,
- isolate CPUs,
- disable background work,
- use huge pages if relevant,
- lock memory if relevant,
- warm caches if measuring steady state,
- flush caches if measuring cold behavior.

The benchmark configuration must be documented.

---

## 13.3 Cost categories

Cost comments should distinguish:

- best case,
- expected case,
- worst case,
- cache miss case,
- branch mispredict case,
- fault case.

Example:

```cpp
// CEP:COST: expected 14 cycles, worst 34 cycles with L1 miss, artifact bench-201.
```

---

## 13.4 Regression gates

CI must fail if:

- CEP-0 benchmark regresses beyond allowed threshold,
- disassembly changes unexpectedly,
- branch count changes unexpectedly,
- allocation appears in hot code,
- exceptions appear in hot code,
- virtual calls appear in hot code.

---

# 14. Clean-code requirements

## 14.1 Naming

Use one consistent style.

Recommended:

```cpp
namespace cep::codegen {}

class InstructionScheduler {};

struct RegisterMask {};

constexpr std::size_t kMaxOpcodeTableEntries = 512;

auto schedule_instruction() -> ScheduleResult;

std::int32_t frame_index;
```

Rules:

- types: `PascalCase`,
- functions/variables: `snake_case`,
- constants: `kPascalCase`,
- macros: `CEP_UPPER_SNAKE_CASE`,
- namespaces: `snake_case`.

No single-letter names except loop indices, template parameters, and mathematical conventions.

---

## 14.2 Ownership

Ownership must be explicit.

| Type | Meaning |
|---|---|
| `T` | value ownership |
| `T*` | non-owning, nullable |
| `T&` | non-owning, non-null |
| `std::unique_ptr<T>` | exclusive ownership |
| `std::shared_ptr<T>` | shared ownership, banned in hot code |
| `std::span<T>` | non-owning contiguous mutable view |
| `std::span<const T>` | non-owning contiguous read-only view |
| `std::string_view` | non-owning read-only text view |

Raw pointers must not own resources.

---

## 14.3 Const correctness

Default to `const`.

Use `constexpr` for compile-time constants.

Use `constinit` for global variables that are not `constexpr`.

Use `mutable` only with a comment explaining why mutation is logically const.

---

## 14.4 Explicitness

Required:

- `explicit` constructors unless copy/move,
- explicit integer widths,
- explicit casts,
- `noexcept` where guaranteed,
- `[[nodiscard]]` on status/error/factory functions.

Banned:

- C-style casts,
- implicit narrowing,
- implicit signed/unsigned comparisons,
- implicit conversion constructors,
- implicit exception paths in hot code.

Use:

```cpp
static_cast<T>
```

or named conversion functions.

---

## 14.5 Functions

Every function must answer:

1. What does it do?
2. What does it assume?
3. What can fail?
4. What is its cost?
5. Is it complete?

Recommended limits:

- one function, one responsibility,
- avoid functions over ~80 lines unless hot-loop structure requires it,
- avoid deep nesting,
- prefer early returns for error handling,
- no output parameters where a return is clearer,
- no in/out parameters unless required by performance and documented.

---

## 14.6 Headers and modules

Module interfaces must not:

- export macros,
- include unnecessary implementation details,
- leak private implementation types,
- depend on include order.

Use partitions for large components.

Use implementation units for non-public code.

---

# 15. Compiler-specific additions

If the codebase is itself a compiler, these additional rules apply.

---

## 15.1 Deterministic diagnostics

Diagnostics must not depend on:

- pointer addresses,
- hash iteration order,
- locale,
- time,
- environment,
- unstable file ordering.

Diagnostic output must be stable across runs.

---

## 15.2 Pass ordering

Compiler passes must be explicit and versioned.

No pass may silently depend on another pass’s output unless documented.

Pass pipelines must be testable in isolation.

---

## 15.3 IR stability

Intermediate representations must:

- be printable,
- be hashable deterministically,
- have stable IDs,
- avoid hidden target assumptions.

IR must not rely on pointer values for semantic identity.

---

## 15.4 Target hooks

All target-specific behavior must go through explicit target hooks.

No generic backend code may contain:

```cpp
if (is_x86) ...
```

Use target policy objects or concept-constrained target interfaces.

---

## 15.5 Compiler memory

Compiler allocations must use arena/pool allocators per compilation phase.

Do not use global `new` in passes.

---

# 16. Enforcement and CI

CI must enforce:

- C++26 mode,
- warnings as errors,
- sanitizer cleanliness,
- comment schema presence,
- no TODO without owner/ticket,
- no stub in CEP-0 without waiver,
- no placeholder in production hot path,
- benchmark regression gates,
- disassembly golden checks,
- no magic constants lint,
- no banned features in hot code,
- no hard-coded assumptions lint.

---

# 17. Review checklist

A change is compliant only if all are true.

## Build

- [ ] Compiles as C++26.
- [ ] All optional features are feature-test gated.
- [ ] Warnings as errors.
- [ ] Sanitizers clean.
- [ ] No undefined behavior.

## Performance

- [ ] No hidden allocation in hot code.
- [ ] No exceptions in hot code.
- [ ] No RTTI in hot code.
- [ ] No virtual dispatch in hot code.
- [ ] No I/O in hot code.
- [ ] No formatting in hot code.
- [ ] No locks in hot code.
- [ ] All hot branches documented.
- [ ] All hot loops have measured cost.

## Clean code

- [ ] Ownership is explicit.
- [ ] Lifetimes are explicit.
- [ ] Error handling is explicit.
- [ ] No magic constants.
- [ ] No commented-out code.
- [ ] No stale comments.
- [ ] All public APIs are `[[nodiscard]]` where appropriate.

## Comments

- [ ] `CEP:WHAT` present.
- [ ] `CEP:WHY` present.
- [ ] `CEP:STATUS` present.
- [ ] `CEP:FAILURE` present.
- [ ] `CEP:ASSUMES` present.
- [ ] `CEP:COST` present for performance-relevant code.
- [ ] `CEP:EVIDENCE` present for measured claims.

## Assumptions

- [ ] No hard-coded endianness.
- [ ] No hard-coded alignment.
- [ ] No hard-coded cache line size.
- [ ] No hard-coded page size.
- [ ] No hard-coded ABI assumptions.
- [ ] No hard-coded floating-point behavior.
- [ ] All assumptions are enforced or cited.

---

# 18. Example: compliant CEP&CC function

```cpp
// CEP:WHAT: Computes a 64-bit additive checksum over a contiguous u32 buffer.
// CEP:WHY: Control-plane validation needs a cheap, allocation-free checksum.
// CEP:STATUS: complete
// CEP:FAILURE: Returns kChecksumEmpty for empty input. No allocation. No UB.
// CEP:ASSUMES: data.data() is 4-byte aligned; enforced by debug assert.
// CEP:COST: 1 cycle/element on target cortex-m7, -O3, measured 2026-09-01.
// CEP:EVIDENCE: bench CEP-0019, asm artifact a41c9e2.
[[nodiscard]]
constexpr auto compute_checksum(std::span<const std::uint32_t> data) noexcept
    -> std::uint64_t
{
    // CEP:WHAT: Empty-input sentinel.
    // CEP:WHY: Lets caller distinguish empty buffer from zero checksum.
    // CEP:STATUS: complete
    // CEP:FAILURE: none
    // CEP:ASSUMES: none
    // CEP:COST: constant
    // CEP:EVIDENCE: unit test cep::checksum::empty
    if (data.empty()) {
        return kChecksumEmpty;
    }

    CEP_ASSERT(
        (reinterpret_cast<std::uintptr_t>(data.data()) % alignof(std::uint32_t)) == 0
    );

    std::uint64_t sum = 0;

    // CEP:WHAT: Main accumulation loop.
    // CEP:WHY: 64-bit accumulator prevents overflow for any bounded input size.
    // CEP:STATUS: complete
    // CEP:FAILURE: none
    // CEP:ASSUMES: data.size() <= kMaxChecksumElements; checked by caller.
    // CEP:COST: 1 cycle/element measured.
    // CEP:EVIDENCE: bench CEP-0019
    for (std::size_t i = 0; i != data.size(); ++i) {
        sum += data[i];
    }

    return sum;
}
```

---

# 19. Example: stub

```cpp
// CEP:WHAT: Lowers target-specific vector intrinsics.
// CEP:WHY: Required for vector codegen, not yet implemented.
// CEP:STATUS: stub
// CEP:FAILURE: Debug assertion fires if called in debug. Release returns unsupported error.
// CEP:ASSUMES: only called from cold lowering phase.
// CEP:COST: not applicable; stub.
// CEP:EVIDENCE: ticket CEP-512.
[[nodiscard]]
auto lower_vector_intrinsics([[maybe_unused]] const VectorOp& op) noexcept
    -> std::expected<LoweredOp, LowerError>
{
    CEP_ASSERT(false && "vector lowering stub");
    return std::unexpected(LowerError::unsupported);
}
```

---

# 20. Example: placeholder

```cpp
// CEP:WHAT: Reserved hook for future register pressure feedback.
// CEP:WHY: Scheduler API must remain stable while allocator integration is designed.
// CEP:STATUS: placeholder
// CEP:FAILURE: static_assert if instantiated in production build.
// CEP:ASSUMES: no production call sites.
// CEP:COST: zero; not emitted.
// CEP:EVIDENCE: design doc CEP-ARCH-009.
```

---

Added. The following are new normative chapters to append to **CEP&CC 0.1**.

These additions introduce:

1. **Security requirements**.
2. **Literal optimal code definition**.
3. **Multi-language companion policy** for:
   - Rust,
   - C,
   - Zig,
   - Python/Lua tooling.

These chapters are written as direct additions to the existing standard.

---

# 22. Security

Security is a first-class correctness requirement in CEP&CC.

A function is not compliant if it is fast but exploitable.

Security rules apply to all code classes, with stricter rules for CEP-0 and FFI boundaries.

---

## 22.1 Security objective

The security objective is:

> No input, environment state, toolchain artifact, or FFI boundary may cause undefined behavior, memory unsafety, privilege escalation, secret leakage, denial of service, or silent corruption beyond the documented failure policy.

Security defects include:

- buffer overflow,
- out-of-bounds read,
- use-after-free,
- double-free,
- uninitialized read,
- uninitialized write,
- integer overflow,
- signed overflow,
- unchecked truncation,
- stack overflow,
- uncontrolled recursion,
- format string injection,
- path traversal,
- command injection,
- deserialization attack,
- TOCTOU race,
- side-channel leakage,
- secret leakage in logs,
- secret leakage in error messages,
- untrusted allocator control,
- untrusted code generation,
- supply-chain compromise,
- compiler/toolchain tampering.

---

## 22.2 Threat model requirement

Every component must have a threat model.

At minimum, the component must answer:

- What input is trusted?
- What input is untrusted?
- What boundaries exist?
- What privileges does the component have?
- What secrets does the component touch?
- What resources can be exhausted?
- What failure mode is acceptable under attack?
- What failure mode is forbidden under attack?

If no threat model exists, the component must treat all external input as untrusted.

---

## 22.3 Trust boundaries

Trust boundaries must be explicit.

Examples of trust boundaries:

- user input,
- network input,
- file input,
- IPC input,
- environment variables,
- command-line arguments,
- configuration files,
- plugin APIs,
- FFI calls,
- kernel interfaces,
- hypervisor interfaces,
- hardware registers,
- generated code,
- toolchain output,
- third-party libraries.

Data crossing a trust boundary must be validated before use.

Validation must include:

- size bounds,
- type bounds,
- alignment,
- encoding,
- lifetime,
- ownership,
- permissions,
- resource limits,
- semantic invariants.

A trust boundary must not be crossed by raw pointers, raw lengths, or unchecked enums without validation.

---

## 22.4 Memory safety

Memory safety is mandatory.

CEP&CC memory-safety rules:

- No out-of-bounds access.
- No use-after-free.
- No double-free.
- No uninitialized reads.
- No uninitialized writes.
- No dangling references.
- No dangling spans.
- No dangling string views.
- No hidden lifetime extension.
- No unsafe pointer arithmetic without bounds proof.
- No aliasing violations.
- No type punning through invalid casts.
- No stack overflow from recursion.
- No unbounded alloca-like behavior.
- No variable-length arrays.

In C++, use:

- `std::span` for contiguous views,
- `std::string_view` only with lifetime proof,
- `std::expected` for recoverable errors,
- RAII for resource ownership,
- static assertions for layout assumptions,
- sanitizers in CI.

In unsafe languages, unsafe blocks must be isolated and documented.

---

## 22.5 Input validation

All external input is untrusted until validated.

Input validation must be explicit.

Validation must answer:

- What is the minimum size?
- What is the maximum size?
- What is the required alignment?
- What is the required encoding?
- What is the required lifetime?
- What resource limits apply?
- What happens if validation fails?
- Does failure leak information?

Bad:

```cpp
parse_packet(data);
```

Good:

```cpp
auto packet = validate_packet(data);
if (!packet) return packet.error();
process_packet(*packet);
```

Validation functions must be documented with:

```cpp
// CEP:SECURITY: Validates untrusted network input.
// CEP:FAILURE: Returns error on malformed bounds. No allocation. No throw.
```

---

## 22.6 Integer safety

Integer misuse is a security defect.

Required rules:

- Use fixed-width integer types.
- Do not allow silent narrowing.
- Do not allow silent signed/unsigned mixing.
- Do not allow unchecked overflow.
- Do not allow unchecked underflow.
- Do not allow unchecked multiplication used for allocation sizing.
- Do not allow unchecked array index calculation.
- Do not allow unchecked pointer arithmetic.

Use:

- `std::uint8_t`,
- `std::uint16_t`,
- `std::uint32_t`,
- `std::uint64_t`,
- `std::int8_t`,
- `std::int16_t`,
- `std::int32_t`,
- `std::int64_t`,
- `std::size_t` for sizes,
- `std::ptrdiff_t` for differences.

Use safe comparison helpers:

- `std::cmp_less`,
- `std::cmp_equal`,
- `std::cmp_greater`,
- `std::in_range`.

If wrapping is intentional, it must be documented:

```cpp
// CEP:ASSUMES: u32 wrapping is intentional and part of checksum semantics.
```

If overflow is impossible, prove it:

```cpp
static_assert(kMaxEntries <= SIZE_MAX / sizeof(Entry));
```

---

## 22.7 Unsafe code

Unsafe code is restricted.

Unsafe code includes:

C++:

- raw pointer arithmetic,
- `reinterpret_cast`,
- union type punning where not permitted,
- inline assembly,
- `volatile` hardware access,
- manual lifetime management,
- placement new,
- custom allocator code.

Rust:

- `unsafe` blocks,
- raw pointers,
- FFI declarations,
- manual layout assumptions,
- `MaybeUninit`,
- inline assembly.

C:

- all pointer arithmetic,
- casts,
- unions,
- volatile,
- inline assembly.

Zig:

- pointer casts,
- `@ptrCast`,
- `@bitCast`,
- `@intFromPtr`,
- volatile loads/stores,
- inline assembly.

Unsafe code must have:

```cpp
// CEP:SECURITY: unsafe block
// CEP:WHAT:
// CEP:WHY:
// CEP:FAILURE:
// CEP:ASSUMES:
// CEP:COST:
// CEP:EVIDENCE:
```

Unsafe code must be isolated in small modules.

Unsafe code must not leak unsafe invariants into safe APIs.

---

## 22.8 FFI security

FFI is a security boundary.

FFI rules:

- Every FFI type must have explicit layout.
- Every FFI pointer must be validated where possible.
- Every FFI length must be validated.
- Every FFI enum must be validated before conversion.
- Every FFI callback must be documented.
- Every FFI error contract must be documented.
- Every FFI allocation ownership must be documented.
- Every FFI deallocation owner must be documented.
- Every FFI string encoding must be documented.
- Every FFI struct must be `repr(C)` or equivalent.
- Every FFI function must specify calling convention.
- Every FFI function must be `noexcept`/`extern "C"` or equivalent unless intentionally propagating exceptions, which is banned in CEP-0.

Do not assume safety guarantees cross language boundaries.

Rust references, C pointers, Zig slices, and C++ spans all lose validity guarantees at FFI boundaries unless explicitly checked.

---

## 22.9 Secrets and side channels

Secrets require special treatment.

Secrets include:

- cryptographic keys,
- passwords,
- tokens,
- session identifiers,
- private user data,
- hardware secrets,
- signing keys,
- decrypt keys,
- authentication material.

Rules:

- Secrets must not be logged.
- Secrets must not be formatted into error messages.
- Secrets must not appear in crash dumps unless redacted.
- Secrets must not be copied unnecessarily.
- Secrets must be zeroized according to policy if required.
- Secret-dependent branches are banned in cryptographic hot paths unless explicitly allowed.
- Secret-dependent memory indexing is banned in constant-time paths.
- Timing side channels must be documented.
- Cache side channels must be considered.
- Speculative execution side channels must be considered for security-critical targets.

Security-sensitive code must state:

```cpp
// CEP:SECURITY: constant-time path; no secret-dependent branches.
```

or:

```cpp
// CEP:SECURITY: not constant-time; must not process secret material.
```

---

## 22.10 Denial of service

CEP&CC security treats resource exhaustion as a security defect.

Components must define limits for:

- input size,
- recursion depth,
- allocation count,
- allocation size,
- file descriptors,
- threads,
- stack usage,
- CPU time,
- memory usage,
- open handles,
- queue depth,
- timeout behavior.

Hot paths must not allow untrusted input to cause unbounded work.

Banned:

- unbounded recursion,
- unbounded loop over untrusted length,
- unbounded allocation,
- unbounded string construction,
- unbounded container growth,
- unbounded regex matching,
- unbounded parsing depth,
- unbounded deserialization nesting.

If limits are enforced, the limit value must not be hard-coded without justification.

Bad:

```cpp
if (size > 4096) return error::too_large;
```

Good:

```cpp
if (size > cep::limit::max_packet_bytes) return error::too_large;
```

---

## 22.11 Supply-chain security

Dependencies must be controlled.

Required:

- pinned dependency versions,
- dependency lockfiles,
- dependency hashes,
- vendored third-party code where practical,
- SBOM generation,
- review of new dependencies,
- no network fetch during deterministic release builds unless explicitly approved.

Banned:

- unpinned dependencies,
- mutable dependency tags,
- build scripts that fetch arbitrary remote code in release builds,
- telemetry in build tools,
- undocumented post-install scripts,
- dependency code that violates CEP&CC security rules.

Generated code from dependencies is subject to the same review as handwritten code.

---

## 22.12 Toolchain security

The compiler toolchain is part of the trusted computing base.

Required:

- pinned compiler version,
- compiler hash or signature,
- reproducible builds,
- deterministic flags,
- no hidden environment dependence,
- no telemetry in release builds,
- audited linker behavior,
- audited standard library version.

If a compiler plugin is used, it is security-critical.

Compiler plugins must be:

- reviewed,
- versioned,
- deterministic,
- sandboxed where possible,
- documented.

---

## 22.13 Logging and diagnostics security

Logs are a security surface.

Rules:

- Logs must not contain secrets.
- Logs must not contain unvalidated user-controlled format strings.
- Logs must not contain unbounded user input.
- Logs must be rate-limited where appropriate.
- Error messages must not leak sensitive internal state.
- Stack traces must be disabled or redacted in production where required.
- Debug assertions must not expose secrets.

In CEP-0, logging is banned unless:

- logging is cold,
- logging is lock-free where required,
- logging cannot block the hot path,
- logging cannot allocate in the hot path.

---

## 22.14 Security comment fields

Security-sensitive code must include additional comment fields.

Required where relevant:

```cpp
// CEP:SECURITY:
// CEP:TRUST:
// CEP:THREAT:
// CEP:UNSAFE:
```

### `CEP:SECURITY`

Describes security role.

Examples:

```cpp
// CEP:SECURITY: Parses untrusted binary input.
```

```cpp
// CEP:SECURITY: constant-time comparison of authentication tags.
```

### `CEP:TRUST`

Describes input trust level.

Allowed values:

```text
trusted
validated
untrusted
```

Example:

```cpp
// CEP:TRUST: untrusted until validate_packet returns success.
```

### `CEP:THREAT`

Describes threats considered.

Example:

```cpp
// CEP:THREAT: malformed length, integer overflow, out-of-bounds read.
```

### `CEP:UNSAFE`

Describes unsafe operations.

Example:

```cpp
// CEP:UNSAFE: pointer arithmetic bounded by checked length.
```

If no unsafe behavior exists:

```cpp
// CEP:UNSAFE: none
```

---

## 22.15 Security checklist

A change is security-compliant only if:

- [ ] Threat model is documented.
- [ ] Trust boundaries are explicit.
- [ ] All untrusted input is validated.
- [ ] Integer operations are safe or explicitly documented.
- [ ] Memory safety is enforced.
- [ ] Unsafe code is isolated and documented.
- [ ] FFI boundaries validate all inputs.
- [ ] No secrets appear in logs or errors.
- [ ] Side-channel policy is documented.
- [ ] Resource limits are explicit.
- [ ] Dependencies are pinned and reviewed.
- [ ] Toolchain is pinned and reproducible.
- [ ] Sanitizers pass.
- [ ] Fuzz tests pass where input is untrusted.

---

# 23. Optimal code

This chapter defines what CEP&CC means by “optimal code.”

The term “optimal” must not be used as a vague compliment.

In CEP&CC, optimality is a normative claim with evidence.

---

## 23.1 Literal optimal code definition

CEP&CC defines three optimality classes.

### OPT-0: Locally optimal

A function is locally optimal if, under the documented target and cost model, no known implementation satisfying the same observable behavior has lower measured cost.

This is the minimum required claim for CEP-0 hot code.

### OPT-1: Lower-bound optimal

A function is lower-bound optimal if the implementer provides a lower-bound argument showing that the function cannot be faster under the documented cost model.

Examples of lower-bound arguments:

- must read N bytes,
- must write M bytes,
- must execute at least K dependent operations,
- must perform at least one branch due to input-dependent control flow,
- must call a hardware instruction with known latency,
- must touch at least P cache lines.

### OPT-2: Provably optimal

A function is provably optimal if a formal or exhaustive proof shows optimality under a precise cost model.

This is rare and only required where justified.

---

## 23.2 Practical meaning of “literally optimal”

For CEP&CC, “literally optimal” means:

> The implementation performs no instruction, memory access, branch, allocation, synchronization, or hidden work that is not required by the observable specification, target cost model, and chosen failure policy.

This means optimal code must not contain:

- redundant loads,
- redundant stores,
- redundant branches,
- redundant copies,
- hidden temporaries,
- hidden allocations,
- hidden synchronization,
- hidden initialization,
- hidden exception machinery,
- hidden type erasure,
- hidden virtual dispatch,
- hidden formatting,
- hidden logging,
- hidden locale behavior,
- hidden I/O,
- hidden allocator calls,
- hidden bounds checks in release unless required by security policy,
- hidden compiler runtime calls,
- hidden dynamic initialization,
- hidden destructor work,
- hidden cleanup paths that are semantically unnecessary.

---

## 23.3 Optimality is target-relative

There is no universal optimal code.

Optimality is always relative to:

- target ISA,
- target microarchitecture,
- compiler version,
- compile flags,
- memory hierarchy,
- branch predictor state,
- cache state,
- input distribution,
- failure policy,
- security policy,
- determinism policy.

Therefore every optimality claim must specify the target.

Bad:

```cpp
// optimal
```

Good:

```cpp
// CEP:OPTIMAL: target-optimal on arm64-a78 for aligned 64-byte cache-line inputs.
```

---

## 23.4 Optimality evidence

An optimality claim requires evidence.

Evidence may include:

- disassembly,
- instruction count,
- dependency-chain analysis,
- measured cycles,
- measured uops,
- measured cache misses,
- measured branch mispredictions,
- lower-bound argument,
- exhaustive search,
- compiler output comparison,
- handwritten assembly comparison,
- benchmark suite artifact.

Required comment fields:

```cpp
// CEP:OPTIMAL:
// CEP:OPTPROOF:
```

Examples:

```cpp
// CEP:OPTIMAL: target-optimal
// CEP:OPTPROOF: bench CEP-201; minimum required loads = 8; measured loads = 8.
```

```cpp
// CEP:OPTIMAL: lower-bound optimal
// CEP:OPTPROOF: function must read 32 bytes and write 16 bytes; measured memory ops match lower bound.
```

---

## 23.5 Optimality requirements for CEP-0

CEP-0 hot code must be at least OPT-0.

To claim OPT-0, the following must be true:

1. The function is measured.
2. The target is documented.
3. The input classes are documented.
4. The cost model is documented.
5. Alternatives were considered.
6. Disassembly was reviewed.
7. No hidden work exists.
8. No cheaper known implementation exists.
9. The claim is reviewed.
10. The evidence is stored.

A CEP-0 function cannot be marked:

```cpp
// CEP:STATUS: complete
```

unless it also has either:

```cpp
// CEP:OPTIMAL: target-optimal
```

or:

```cpp
// CEP:OPTIMAL: not-optimal
```

with an optimization ticket if performance matters.

If the function is not optimal but acceptable, it must say:

```cpp
// CEP:OPTIMAL: not-optimal
// CEP:OPTNOTE: acceptable due to maintainability; see CEP-771.
```

Do not lie about optimality.

---

## 23.6 Code-level optimality criteria

Optimal code should satisfy the following where applicable.

### 23.6.1 Minimal work

The function must not do unnecessary work.

Banned examples:

- recomputing invariant values inside loops,
- copying values that could be moved or referenced,
- formatting strings that are not used,
- checking conditions that are already proven,
- initializing memory that will be immediately overwritten,
- calling destructors for trivial objects unnecessarily,
- performing virtual dispatch where static dispatch is possible.

### 23.6.2 Minimal memory traffic

Optimal code minimizes memory traffic.

Required considerations:

- read each required input once,
- write each required output once,
- avoid false sharing,
- avoid cache-line splitting,
- avoid misaligned access,
- avoid unnecessary cache invalidation,
- avoid pointer chasing where contiguous layout is possible,
- prefer structure-of-arrays where vectorization matters.

### 23.6.3 Minimal branching

Optimal code avoids unnecessary branches.

Branch reduction requires:

- profile evidence,
- branchless alternatives considered,
- predication considered,
- lookup tables considered,
- jump tables considered,
- switch lowering inspected.

Do not remove branches if they are required for:

- security checks,
- overflow checks,
- bounds checks,
- failure handling,
- correctness.

Security checks are not “unnecessary work” unless proven redundant.

### 23.6.4 Minimal indirection

Optimal code avoids unnecessary indirection.

Banned in hot optimal code unless required:

- virtual functions,
- function pointers,
- `std::function`,
- type-erased iterators,
- dynamic dispatch,
- pointer-to-pointer chains,
- polymorphic allocators,
- runtime polymorphic containers.

### 23.6.5 Minimal allocation

Optimal hot code does not allocate.

Allocation is hidden work and usually nondeterministic.

Allowed allocation only if:

- done before entering hot path,
- arena-based,
- bounded,
- measured,
- documented.

### 23.6.6 Minimal compile-time residue

Optimal code should not leave unnecessary compile-time residue.

Prefer:

- `constexpr`,
- `consteval`,
- compile-time tables,
- template specialization only where necessary,
- static reflection only if generated code is audited.

Compile-time work is not free. It must be budgeted.

---

## 23.7 Optimality and security

Security checks may prevent some forms of micro-optimization.

CEP&CC rule:

> Security correctness beats micro-optimality.

A function that is faster but exploitable is not optimal.

If a security check costs cycles, it must be documented as required cost.

Example:

```cpp
// CEP:OPTNOTE: bounds check retained; required for untrusted input security.
```

Do not remove bounds checks to claim optimality unless the input is trusted or proven safe.

---

## 23.8 Optimality and clean code

Clean code is required for optimality because hidden cost hides in unclear code.

A reviewer cannot certify optimal code if:

- ownership is unclear,
- lifetimes are unclear,
- assumptions are unclear,
- failure behavior is unclear,
- branch probabilities are unclear,
- memory layout is unclear,
- target dependencies are unclear.

Therefore clean-code violations are optimality blockers.

---

## 23.9 Optimality comment examples

Good:

```cpp
// CEP:OPTIMAL: target-optimal
// CEP:OPTPROOF: bench CEP-301; 16 loads, 16 stores, 0 branches; matches required memory lower bound.
```

Good:

```cpp
// CEP:OPTIMAL: not-optimal
// CEP:OPTNOTE: uses scalar loop; vectorization blocked by target errata; see CEP-402.
```

Bad:

```cpp
// optimal
```

Bad:

```cpp
// fastest possible
```

Bad:

```cpp
// should inline well
```

---

# 24. Multi-language companion policy

CEP&CC recognizes that real systems are rarely pure C++.

However, the standard remains C++26-primary unless otherwise stated.

Companion languages are allowed only when their role is explicit and their code is adapted to CEP&CC rules.

The companion languages defined here are:

1. Rust — safe systems alternative / FFI boundary.
2. C — legacy/hardware interface.
3. Zig — modern low-level alternative.
4. Python / Lua — CEP-2 tooling only.

---

## 24.1 General companion-language rules

All companion-language code must obey the same high-level CEP&CC laws:

- no hidden cost,
- no silent assumptions,
- no comment-only invariants,
- no unmeasured hot code,
- no unreadable hot code,
- no unbounded failure,
- no hard-coded assumptions,
- no stale documentation,
- explicit security policy,
- explicit optimality policy.

Companion-language code must use the same CEP comment schema:

```text
CEP:WHAT
CEP:WHY
CEP:STATUS
CEP:FAILURE
CEP:ASSUMES
CEP:COST
CEP:EVIDENCE
CEP:SECURITY
CEP:OPTIMAL
CEP:OPTPROOF
```

If the language comment syntax differs, use the closest line-comment form.

---

# 25. Rust companion policy

## 25.1 Role

Rust is the primary safe-systems companion language.

Its role is:

- safe systems alternative,
- FFI boundary language,
- memory-safe component language,
- tool for isolating unsafe logic behind safe APIs.

Rust is allowed where:

- memory safety is critical,
- FFI boundaries require safer ownership,
- components can be built deterministically,
- runtime cost is compatible with CEP requirements.

---

## 25.2 Why Rust is allowed

Rust is allowed because it provides:

- zero-cost abstractions comparable to C++,
- memory safety without garbage collection,
- strong type system,
- explicit lifetimes,
- explicit ownership,
- explicit mutability,
- `no_std` support,
- explicit unsafe boundaries.

However, Rust is not automatically safe in performance-critical code. Rust can still hide:

- allocation,
- panics,
- dynamic dispatch,
- drop glue,
- formatting,
- iterator overhead,
- atomics,
- synchronization,
- FFI undefined behavior.

Therefore CEP&CC adapts Rust rules as follows.

---

## 25.3 Rust CEP-0 rules

For Rust CEP-0 hot code:

### 25.3.1 Runtime

Banned:

- `std` runtime dependence in hot code,
- `Box`,
- `Vec`,
- `String`,
- `format!`,
- `println!`,
- `eprintln!`,
- `panic!`,
- `unwrap`,
- `expect`,
- `todo!`,
- `unimplemented!`,
- `unreachable!` unless proven and documented,
- `Rc`,
- `Arc`,
- `Mutex`,
- `RwLock`,
- channels,
- thread spawning,
- dynamic allocation,
- `async` runtime dependence,
- `Future` allocation in hot paths,
- trait objects via `dyn`,
- dynamic dispatch,
- formatting machinery,
- `std::io`,
- `std::fs`,
- `std::net`,
- `std::env`,
- `std::time` in hot paths unless budgeted.

Required:

- use `core::` over `std::` for hot code,
- use fixed-size slices `&[T]` and `&mut [T]`,
- use references with explicit lifetimes,
- use explicit integer types,
- use explicit error enums or result types that do not allocate,
- use `#[inline]` only where justified,
- use `#[repr(C)]` or explicit layout for FFI types,
- use `#[no_mangle]` only for FFI exports,
- use `extern "C"` or explicit ABI for FFI functions.

---

## 25.4 Rust panic policy

CEP-0 Rust functions must be panic-free or have proof that panic cannot occur.

Use one of:

- `#![no_std]`,
- `panic = "abort"` in release profile,
- static analysis,
- review,
- tests,
- contracts,
- `#[should_panic]` banned in CEP-0 tests unless testing failure behavior.

Banned in CEP-0:

```rust
.unwrap()
.expect("...")
panic!("...")
```

Allowed only if proof exists:

```rust
// CEP:ASSUMES: index is checked immediately above.
// CEP:SECURITY: bounds check retained.
```

If a panic is possible, the function is not CEP-0-complete.

---

## 25.5 Rust allocation policy

Allocation is banned in Rust CEP-0.

Banned:

- `Box::new`,
- `Vec::new`,
- `Vec::push`,
- `String::new`,
- `String::push_str`,
- `alloc::vec::Vec` in hot paths,
- any global allocator dependence.

Allowed:

- stack allocation,
- fixed arrays,
- slices,
- caller-provided buffers,
- arena allocation if the arena is initialized before hot path and deterministic.

---

## 25.6 Rust trait object policy

Trait objects are banned in CEP-0.

Banned:

```rust
&dyn Trait
Box<dyn Trait>
Arc<dyn Trait>
```

Reason:

Trait objects introduce dynamic dispatch, which is equivalent to hidden virtual dispatch.

Allowed alternatives:

- generics,
- static dispatch,
- enums,
- function pointer tables if measured,
- compile-time polymorphism.

---

## 25.7 Rust unsafe policy

Unsafe Rust is restricted.

Allowed only when:

- safe Rust cannot express the operation,
- the unsafe block is isolated,
- invariants are documented,
- FFI requires it,
- hardware access requires it.

Every unsafe block must have:

```rust
// CEP:UNSAFE: pointer arithmetic bounded by checked slice length.
// CEP:ASSUMES: ptr is aligned to 4 bytes; checked above.
// CEP:SECURITY: input is untrusted; bounds validated.
```

Unsafe code must not leak invariants.

A safe function wrapping unsafe code must uphold all safe guarantees.

---

## 25.8 Rust FFI policy

Rust FFI types must have explicit layout.

Required:

```rust
#[repr(C)]
```

or:

```rust
#[repr(transparent)]
```

where appropriate.

FFI functions must specify ABI:

```rust
pub extern "C" fn cep_decode(...)
```

FFI functions must not unwind across the boundary.

Use:

```rust
#[no_mangle]
pub extern "C" fn ...
```

only when required.

FFI inputs must be validated:

- pointers checked for null if nullable,
- lengths checked,
- alignment checked,
- enum values checked,
- lifetimes cannot be assumed from C/C++.

---

## 25.9 Rust comment example

```rust
// CEP:WHAT: Decodes a fixed-size 32-bit opcode table entry.
// CEP:WHY: Table lookup is faster than match for hot decoder path.
// CEP:STATUS: complete
// CEP:FAILURE: Returns DecodeError::BadOpcode for invalid opcode.
// CEP:ASSUMES: opcode < 128; checked by caller.
// CEP:COST: 3 cycles expected on arm64-a78, artifact bench-rs-19.
// CEP:EVIDENCE: bench CEP-RS-0019
// CEP:SECURITY: opcode may come from untrusted input.
// CEP:OPTIMAL: target-optimal
// CEP:OPTPROOF: one table load, one branch, no allocation.
#[no_mangle]
pub extern "C" fn cep_decode_opcode(opcode: u8) -> DecodeResult {
    if opcode >= 128 {
        return DecodeResult::err(DecodeError::BadOpcode);
    }

    // CEP:WHAT: Safe table access.
    // CEP:WHY: Bounds check ensures no out-of-bounds read.
    // CEP:SECURITY: bounds check retained for untrusted input.
    let entry = OPCODE_TABLE[opcode as usize];
    DecodeResult::ok(entry)
}
```

---

# 26. C companion policy

## 26.1 Role

C is allowed for:

- ABI stability,
- kernel interfaces,
- hypervisor interfaces,
- embedded bare-metal code,
- hardware registers,
- legacy firmware interfaces,
- stable C ABI boundaries.

C is unavoidable in OS-adjacent and hardware-adjacent code.

However, C is unsafe by default and must be restricted.

---

## 26.2 C standard subset

C code must follow a strict subset.

Acceptable baseline:

- MISRA-C:2023, or
- CERT-C, or
- a project-defined subset that is at least as strict.

If MISRA-C and CEP&CC conflict, the stricter rule applies unless waived.

---

## 26.3 C language restrictions

For CEP-0 C code:

Banned:

- dynamic allocation,
- `malloc`,
- `calloc`,
- `realloc`,
- `free`,
- `alloca`,
- variable-length arrays,
- recursion,
- varargs,
- floating point unless required,
- standard library calls unless intrinsic-like and target-approved,
- implicit integer promotion surprises,
- implicit narrowing,
- implicit signed/unsigned mixing,
- macro logic,
- undefined behavior,
- uninitialized reads,
- out-of-bounds access,
- non-reentrant library functions,
- locale-dependent functions,
- I/O functions,
- formatted printing,
- file operations,
- environment access.

Required:

- fixed-width types only,
- explicit casts where conversion is intentional,
- explicit bounds checks,
- explicit alignment checks,
- explicit volatile only for MMIO,
- `_Static_assert` for compile-time assumptions,
- `static inline` for small hot functions where appropriate,
- deterministic failure behavior.

---

## 26.4 Fixed-width types

C code must use fixed-width types exclusively for binary, hardware, ABI, and performance-critical code.

Required headers:

```c
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
```

Use:

```c
uint8_t
uint16_t
uint32_t
uint64_t
int8_t
int16_t
int32_t
int64_t
uintptr_t
size_t
ptrdiff_t
```

Banned as primary types in hot or ABI code:

```c
int
long
unsigned
unsigned long
short
char
```

unless required by a documented C ABI.

---

## 26.5 C macro policy

Macros are treated as defects unless justified.

Allowed macros:

- include guards,
- feature gates,
- target configuration,
- compiler workaround macros,
- `CEP_` prefixed constants.

Banned macros:

- macro-generated control flow,
- macro-generated loops,
- macro-generated types,
- macro-generated functions,
- macro DSLs,
- macro-based reflection,
- macro-based serialization.

If a macro is required, it must have:

```c
// CEP:WHAT:
// CEP:WHY:
// CEP:STATUS:
// CEP:FAILURE:
// CEP:ASSUMES:
// CEP:COST:
// CEP:EVIDENCE:
```

---

## 26.6 C standard library policy

CEP-0 C code must not call the standard library except for:

- compiler intrinsics,
- inline assembly wrappers,
- freestanding headers,
- target-specific runtime hooks explicitly approved.

Banned:

- `printf`,
- `fprintf`,
- `snprintf`, unless cold and bounded,
- `memcpy`? Conditional.

Important note: `memcpy`, `memset`, `memcmp` may be acceptable if target libc implementations are deterministic and measured. However, in strict CEP-0, even these must be approved because they may call optimized library routines with variable behavior.

If `memcpy` is used, document:

```c
// CEP:ASSUMES: memcpy is inline or deterministic for size <= 64 bytes.
```

---

## 26.7 C pointer rules

Pointers must be explicit.

Rules:

- pointer arithmetic only within known bounds,
- no NULL dereference,
- no dangling pointers,
- no aliasing violations,
- no casts that violate alignment,
- no casts that violate strict aliasing,
- no pointer-to-integer assumptions unless target-defined.

Use `uintptr_t` only for target-specific proven cases.

---

## 26.8 C volatile rules

`volatile` is only for hardware registers and MMIO.

Do not use `volatile` for:

- locks,
- atomics,
- timing,
- preventing optimization of benchmarks,
- thread communication.

---

## 26.9 C comment example

```c
// CEP:WHAT: Reads a 32-bit MMIO register.
// CEP:WHY: Hardware register must not be reordered or cached.
// CEP:STATUS: complete
// CEP:FAILURE: none if address is target-valid.
// CEP:ASSUMES: addr is 4-byte aligned and target-mapped.
// CEP:COST: one volatile load; target-specific latency.
// CEP:EVIDENCE: target manual section 12.4, bench C-011.
// CEP:SECURITY: MMIO input may be untrusted; caller validates value.
// CEP:OPTIMAL: target-optimal
// CEP:OPTPROOF: single required load.
static inline uint32_t cep_mmio_read_u32(volatile uint32_t const* addr)
{
    return *addr;
}
```

---

# 27. Zig companion policy

## 27.1 Role

Zig is allowed as a modern low-level alternative.

Its role is:

- explicit allocator control,
- comptime metaprogramming,
- C-ABI interoperability,
- embedded code,
- explicit error handling,
- replacement for some C/C++ template patterns.

Zig is allowed where its explicitness improves CEP&CC compliance.

---

## 27.2 Why Zig is allowed

Zig provides:

- explicit allocators,
- comptime evaluation,
- no hidden function calls in many contexts,
- explicit error unions,
- straightforward C ABI interop,
- packed/extern structs,
- compile-time generics without C++ template syntax.

However, Zig can still hide cost through:

- allocator choice,
- runtime safety checks,
- comptime explosion,
- `anytype` instantiation,
- error handling paths,
- standard library dependencies.

Therefore CEP&CC adapts Zig rules as follows.

---

## 27.3 Zig CEP-0 rules

For Zig CEP-0 hot code:

Banned:

- default allocator dependence,
- heap allocation in hot paths,
- `std.debug.print`,
- `std.io`,
- `std.fs`,
- `std.Thread`,
- `std.Mutex` in hot paths unless justified,
- `panic` in hot paths,
- `unreachable` unless proven and documented,
- runtime recursion,
- unbounded comptime expansion,
- hidden error handling allocation,
- dynamic dispatch in hot paths,
- opaque interface-style dynamic dispatch unless measured.

Required:

- explicit allocator parameter where allocation is possible,
- fixed buffer allocator or arena allocator for bounded allocation,
- explicit error unions for recoverable errors,
- explicit alignment checks,
- explicit packed/extern structs for ABI,
- explicit target configuration,
- deterministic comptime output.

---

## 27.4 Zig allocator policy

Zig’s explicit allocator model is a CEP&CC strength.

Use it strictly.

Banned in CEP-0:

- `std.heap.page_allocator`,
- `std.heap.c_allocator`,
- `std.heap.wasm_allocator`,
- any default allocator that may perform hidden OS allocation.

Allowed:

- stack allocation,
- fixed buffer allocator,
- caller-provided arena,
- static arena initialized before hot path.

Example:

```zig
// CEP:ASSUMES: allocator is a fixed buffer allocator initialized before hot path.
```

---

## 27.5 Zig `anytype` policy

`anytype` is restricted.

Allowed:

- compile-time-only contexts,
- generic utilities with explicit instantiation list,
- code where all instantiations are reviewed.

Banned:

- runtime-polymorphic use,
- unbounded generic instantiation,
- hidden dynamic behavior,
- hot code where the concrete type is not obvious.

`anytype` is analogous to C++ templates and generic lambdas. It must not hide cost.

---

## 27.6 Zig error policy

CEP-0 Zig functions must use explicit error unions for recoverable failure.

Required:

```zig
!ReturnType
```

or explicit error enum.

Banned as primary failure mechanism in CEP-0:

- `panic`,
- `unreachable`,
- `std.debug.assert` as release validation,
- crash-on-error unless the failure policy is fatal.

If `unreachable` is used, it must be proven:

```zig
// CEP:ASSUMES: state enum is exhaustive; invalid state prevented by type.
// CEP:FAILURE: unreachable in valid builds.
```

---

## 27.7 Zig comptime policy

Comptime is powerful and must be controlled.

Allowed:

- compile-time tables,
- compile-time validation,
- compile-time code generation,
- layout computation,
- constant folding.

Banned:

- comptime code that produces unreadable generated code,
- comptime code with nondeterministic output,
- comptime code that explodes compile time,
- comptime code that hides runtime branches,
- comptime code that depends on unstable environment.

Generated Zig code is subject to CEP&CC review.

---

## 27.8 Zig ABI and FFI

FFI types must use explicit layout.

Use:

```zig
extern struct
```

or:

```zig
packed struct
```

depending on ABI requirement.

FFI functions must specify calling convention:

```zig
extern "C"
```

or Zig equivalent.

FFI boundaries must validate:

- pointers,
- lengths,
- alignment,
- enum values,
- error codes.

---

## 27.9 Zig comment example

```zig
// CEP:WHAT: Computes bounded checksum over u32 slice.
// CEP:WHY: Validation requires allocation-free checksum.
// CEP:STATUS: complete
// CEP:FAILURE: Returns error.Empty for empty slice.
// CEP:ASSUMES: slice pointer is 4-byte aligned; Zig slice guarantees non-null.
// CEP:COST: 1 cycle/element on target, bench ZIG-010.
// CEP:EVIDENCE: bench CEP-ZIG-0010
// CEP:SECURITY: input may be untrusted; length bounded by caller.
// CEP:OPTIMAL: target-optimal
// CEP:OPTPROOF: one load and one add per element.
pub fn checksum(items: []const u32) !u64 {
    if (items.len == 0) return error.Empty;

    var sum: u64 = 0;
    for (items) |x| {
        sum += x;
    }
    return sum;
}
```

---

# 28. Python and Lua companion policy

## 28.1 Role

Python and Lua are allowed only for CEP-2 tooling.

Allowed roles:

- build scripts,
- code generators,
- test harnesses,
- benchmark orchestration,
- report generation,
- configuration validation,
- tooling glue.

They are not allowed for CEP-0 or CEP-1 runtime components unless a separate waiver is granted.

---

## 28.2 Why Python/Lua are allowed

They are useful for:

- rapid tooling,
- orchestration,
- test automation,
- code generation,
- benchmark control,
- report generation.

Their runtime performance is usually irrelevant to CEP-0 because they operate offline.

However, their output is security-critical and performance-critical.

Therefore:

> Python/Lua tool performance is not important. Tool determinism and output compliance are mandatory.

---

## 28.3 Determinism requirement

Python/Lua tooling must be deterministic.

Required:

- stable iteration order,
- sorted output where order is not semantic,
- fixed seeds,
- fixed locale,
- fixed timezone if time is used,
- no dependence on environment unless explicit,
- no dependence on current working directory unless explicit,
- no dependence on file iteration order,
- no dependence on hash randomization,
- no nondeterministic parallelism unless output is deterministic.

Python-specific:

- set `PYTHONHASHSEED` to fixed value,
- sort directory listings,
- use `pathlib` with explicit normalization,
- avoid `set` iteration for output order,
- avoid `dict` insertion-order nondeterminism if input order varies,
- pin dependency versions.

Lua-specific:

- avoid `pairs` for output where order matters,
- use sorted keys,
- pin Lua version,
- avoid OS-dependent behavior unless configured.

---

## 28.4 Generated code rule

Generated C++, Rust, C, or Zig code is fully subject to CEP&CC.

The generator is not exempt because it is written in Python or Lua.

Generated code must include CEP comments or the generator must emit them.

Generated code must not contain:

- hard-coded assumptions,
- magic numbers,
- commented-out code,
- unvalidated FFI,
- unsafe constructs without comments,
- nondeterministic layout,
- unstable ordering,
- hidden allocation in CEP-0.

Generated code must be reviewed either as source or via golden-file tests.

---

## 28.5 Tool security

Python/Lua tools are a supply-chain attack surface.

Required:

- pinned dependencies,
- lockfiles,
- no arbitrary network fetch during release builds,
- no telemetry,
- no secrets in scripts,
- no undocumented subprocess execution,
- sandboxed code generation where possible,
- validated input files,
- validated output paths.

Banned:

- `eval` of untrusted input,
- `exec` of untrusted input,
- dynamic import of untrusted modules,
- shell injection,
- path traversal,
- unpickling untrusted data in Python,
- loading untrusted Lua bytecode.

---

## 28.6 Python example: deterministic generator

```python
# CEP:WHAT: Generates opcode table header.
# CEP:WHY: Keeps opcode metadata synchronized with specification.
# CEP:STATUS: complete
# CEP:FAILURE: Exits with error if spec file is malformed.
# CEP:ASSUMES: spec file is UTF-8 and sorted by opcode value.
# CEP:COST: offline tool; runtime cost irrelevant.
# CEP:EVIDENCE: golden test cep_opcode_table.hpp.golden.
# CEP:SECURITY: input is trusted repository file; no network.

def generate_opcode_table(spec: list[OpcodeSpec]) -> str:
    lines = []
    lines.append("// Generated by cep_gen_opcode_table.py")
    lines.append("// Do not edit manually.")
    lines.append("")
    lines.append("inline constexpr std::array<OpcodeInfo, 128> kOpcodeTable{{")

    for entry in sorted(spec, key=lambda e: e.value):
        lines.append(f"    OpcodeInfo{{ .opcode = {entry.value}, .flags = {entry.flags} }},")

    lines.append("};")
    lines.append("")
    return "\n".join(lines)
```

---

# 29. FFI and cross-language integration

FFI is both a security boundary and a performance boundary.

Therefore FFI receives special rules.

---

## 29.1 FFI ownership rules

Every FFI API must answer:

- Who allocates?
- Who frees?
- Who owns pointers?
- Who owns buffers?
- Who owns error strings?
- Who owns callbacks?
- What happens on error?
- What is the alignment?
- What is the lifetime?
- What is the calling convention?
- What is the threading policy?
- What is the reentrancy policy?
- What is the signal-safety policy?
- What is the interrupt-safety policy?

If any answer is unknown, the FFI API is incomplete.

---

## 29.2 FFI layout rules

FFI structs must use explicit layout.

C++:

- use standard-layout types,
- avoid virtual functions,
- avoid exceptions in ABI,
- avoid `std::vector`, `std::string`, `std::optional`, etc., in stable ABI unless private implementation is controlled.

Rust:

- use `#[repr(C)]`,
- use explicit integer types,
- avoid `Vec`, `String`, `Box` in ABI unless ownership is documented.

Zig:

- use `extern struct` or `packed struct`,
- explicit integer types.

C:

- fixed-width types,
- no bitfields unless target-defined,
- no flexible array members unless reviewed.

---

## 29.3 FFI error rules

Do not propagate exceptions across FFI.

C++ FFI functions should be `extern "C"` and `noexcept`.

Rust FFI functions must not unwind.

Zig FFI functions must not panic across boundary.

C FFI functions must return error codes.

Error codes must be documented.

---

## 29.4 FFI validation

All FFI inputs must be validated on entry.

Even if the caller is trusted, validate where feasible because FFI callers may be wrong.

Validation includes:

- null checks,
- alignment checks,
- length checks,
- enum range checks,
- flag checks,
- callback validity,
- ownership validity.

FFI validation is security work, not optional performance overhead.

---

# 30. Updated review checklist

Add the following to the existing review checklist.

## Security

- [ ] Threat model documented.
- [ ] Trust boundaries explicit.
- [ ] Untrusted input validated.
- [ ] Integer safety checked.
- [ ] Memory safety checked.
- [ ] Unsafe code isolated.
- [ ] FFI boundary validated.
- [ ] No secrets in logs.
- [ ] Side-channel policy documented.
- [ ] Resource limits explicit.
- [ ] Supply chain pinned.
- [ ] Toolchain pinned.

## Optimality

- [ ] Optimality class declared.
- [ ] Target specified.
- [ ] Cost model specified.
- [ ] Evidence attached.
- [ ] Disassembly reviewed.
- [ ] Alternatives considered.
- [ ] Lower-bound argument provided if claimed.
- [ ] No hidden work.
- [ ] Security checks not removed without proof.
- [ ] Optimality comment present.

## Companion languages

- [ ] Language role justified.
- [ ] CEP comment schema present.
- [ ] Hot-path bans respected.
- [ ] FFI layout explicit.
- [ ] FFI errors explicit.
- [ ] Allocator policy explicit.
- [ ] Panic/abort policy explicit.
- [ ] Generated code reviewed.
- [ ] Tooling deterministic.

---

Added. Below are the new normative chapters for **file structure**, **naming**, and **violation handling**.

This includes the requested extermination policy.

The rule is simple:

> Non-compliant code is not merged.  
> If non-compliant code is discovered after merge, it is quarantined, reverted, or deleted.  
> We exterminate the code, not the person.

---

# 32. File structure

File structure is part of the standard because bad structure hides cost, ownership, assumptions, and security boundaries.

A compliant repository must make the following obvious from the directory tree alone:

- what is hot,
- what is cold,
- what is target-specific,
- what is generated,
- what is third-party,
- what is test code,
- what is benchmark code,
- what is tooling,
- what is configuration,
- what is security-sensitive,
- what is FFI,
- what is documentation.

If a reviewer cannot determine these from the layout, the repository structure is non-compliant.

---

## 32.1 Top-level repository layout

Recommended layout:

```text
repo/
├── modules/
├── source/
├── include/
├── target/
├── hot/
├── cold/
├── ffi/
├── generated/
├── tests/
├── benches/
├── tools/
├── config/
├── security/
├── docs/
├── scripts/
├── third_party/
├── quarantine/
└── .cep/
```

Meaning:

| Directory | Purpose |
|---|---|
| `modules/` | First-party C++26 module interface units |
| `source/` | First-party C++ implementation units |
| `include/` | Public headers only when headers are unavoidable |
| `target/` | Target-specific code: ISA, OS, ABI, hardware |
| `hot/` | CEP-0 hot code only |
| `cold/` | CEP-1/CEP-2 cold code |
| `ffi/` | FFI boundaries for C, Rust, Zig, C++ |
| `generated/` | Machine-generated code |
| `tests/` | Unit, property, contract, and integration tests |
| `benches/` | Benchmarks and cycle-cost evidence |
| `tools/` | Build tools, generators, harnesses |
| `config/` | Target configuration, feature configuration |
| `security/` | Threat models, security policies, audits |
| `docs/` | Human documentation |
| `scripts/` | Python/Lua tooling scripts |
| `third_party/` | Vendored third-party code |
| `quarantine/` | Non-compliant code awaiting deletion or repair |
| `.cep/` | CEP&CC lint config, waivers, evidence metadata |

---

## 32.2 Hot and cold separation

Hot and cold code must not live in the same file unless absolutely unavoidable.

Preferred:

```text
hot/decoder/opcode_decoder.cppm
hot/decoder/opcode_decoder.cpp
cold/decoder/opcode_decoder_diagnostics.cpp
```

Bad:

```text
decoder.cpp
```

where hot decoding, logging, file I/O, and diagnostics are mixed together.

Why:

- hot code must be auditable,
- cold code must not accidentally enter hot paths,
- benchmarking becomes easier,
- security boundaries become clearer,
- compile-time isolation improves.

---

## 32.3 Module directory rules

C++ modules should mirror architecture.

Example:

```text
modules/
├── cep/
│   ├── core/
│   │   ├── core.cppm
│   │   ├── types.cppm
│   │   ├── result.cppm
│   │   └── limits.cppm
│   ├── hot/
│   │   ├── decoder.cppm
│   │   ├── checksum.cppm
│   │   └── dispatch.cppm
│   ├── cold/
│   │   ├── diagnostics.cppm
│   │   └── config_parser.cppm
│   ├── target/
│   │   ├── arm64.cppm
│   │   ├── riscv64.cppm
│   │   └── x86_64.cppm
│   └── ffi/
│       ├── c_abi.cppm
│       └── rust_abi.cppm
```

Module names should use dotted form:

```cpp
export module cep.core.types;
export module cep.hot.decoder;
export module cep.target.arm64;
```

Namespace names should mirror module names:

```cpp
namespace cep::core::types {}
namespace cep::hot::decoder {}
namespace cep::target::arm64 {}
```

---

## 32.4 Source directory rules

Implementation units mirror module units.

Example:

```text
source/
├── cep/
│   ├── core/
│   │   ├── types.cpp
│   │   └── result.cpp
│   ├── hot/
│   │   ├── decoder.cpp
│   │   └── checksum.cpp
│   ├── cold/
│   │   └── config_parser.cpp
│   └── target/
│       └── arm64/
│           ├── cache.cpp
│           └── barriers.cpp
```

Rules:

- One primary component per file.
- No file may contain both CEP-0 and CEP-1 code unless separated by explicit sections and approved.
- No target-specific code may live in generic source directories.
- No generated code may live in handwritten source directories.
- No third-party code may live in first-party source directories.

---

## 32.5 Target directory rules

Target-specific code must be isolated.

Example:

```text
target/
├── arm64/
│   ├── cache.cppm
│   ├── barriers.cppm
│   └── intrinsics.cppm
├── riscv64/
│   ├── cache.cppm
│   └── barriers.cppm
├── x86_64/
│   ├── cache.cppm
│   ├── barriers.cppm
│   └── inline_asm.cppm
└── generic/
    └── fallback.cppm
```

Allowed inside target directories:

- inline assembly,
- intrinsics,
- cache-line constants,
- memory barriers,
- MMIO helpers,
- target-specific alignment rules,
- target-specific performance counters.

Forbidden outside target directories:

- inline assembly,
- target intrinsics,
- target-specific `if` checks,
- target-specific constants.

Bad:

```cpp
#if defined(__x86_64__)
...
#endif
```

inside generic hot code.

Good:

```cpp
#include <cep/target/barriers.hpp>
```

or:

```cpp
import cep.target.barriers;
```

---

## 32.6 FFI directory rules

FFI code must live in dedicated FFI directories.

Example:

```text
ffi/
├── cpp/
│   ├── decoder_api.cppm
│   └── decoder_api.cpp
├── c/
│   ├── cep_decoder.h
│   └── cep_decoder.c
├── rust/
│   ├── decoder_ffi.rs
│   └── Cargo.toml
└── zig/
    └── decoder_ffi.zig
```

FFI directories must contain:

- ABI definitions,
- validation code,
- layout tests,
- ownership documentation,
- error-code documentation.

FFI directories must not contain business logic.

FFI should be thin.

Good FFI:

```text
validate -> convert -> call internal API -> convert result -> return
```

Bad FFI:

```text
parse -> allocate -> optimize -> lower -> emit -> log
```

---

## 32.7 Generated code rules

Generated code must be isolated.

Example:

```text
generated/
├── opcode_table.hpp
├── opcode_table.cpp
├── error_codes.rs
├── ir_nodes.zig
└── diagnostics.c
```

Every generated file must begin with a generator comment.

Example:

```cpp
// GENERATED FILE
// Generator: tools/gen_opcode_table.py
// Generator CEP:STATUS: complete
// Generator CEP:EVIDENCE: golden test opcode_table.golden
// Do not edit manually.
```

Rules:

- generated files must not be manually edited,
- generated files must be reproducible,
- generated files must have golden tests,
- generated files must include generator identity,
- generated files must include CEP comments or emit them,
- generated files must not introduce hard-coded assumptions.

If a generator emits CEP-0 code, the generator itself is performance-critical and security-critical.

---

## 32.8 Test directory rules

Tests must mirror source and modules.

Example:

```text
tests/
├── unit/
│   ├── core/
│   │   ├── result_test.cpp
│   │   └── limits_test.cpp
│   ├── hot/
│   │   ├── decoder_test.cpp
│   │   └── checksum_test.cpp
│   └── ffi/
│       └── c_abi_test.cpp
├── property/
├── contract/
├── fuzz/
├── security/
└── golden/
```

Rules:

- every CEP-0 function requires tests,
- every security validation function requires adversarial tests,
- every FFI boundary requires layout tests,
- every generated file requires golden tests,
- every stub requires a test proving it fails loudly in debug,
- every placeholder requires a test proving it is not used in release.

---

## 32.9 Benchmark directory rules

Benchmarks are evidence, not decoration.

Example:

```text
benches/
├── hot/
│   ├── decoder_bench.cpp
│   └── checksum_bench.cpp
├── target/
│   ├── arm64/
│   └── x86_64/
└── artifacts/
```

Benchmark artifacts must include:

- compiler version,
- flags,
- target CPU,
- input data,
- date or artifact ID,
- measured cycles,
- disassembly hash,
- regression threshold.

Benchmarks must be deterministic.

---

## 32.10 Tools directory rules

Tooling lives here:

```text
tools/
├── gen_opcode_table.py
├── gen_error_codes.py
├── lint_comments.py
├── check_assumptions.py
├── bench_gate.py
└── exterminate.py
```

Python/Lua tools are CEP-2, but their output must be CEP-compliant.

Tools must be deterministic.

---

## 32.11 Config directory rules

Configuration values must live here:

```text
config/
├── target_arm64.hpp
├── target_riscv64.hpp
├── limits.hpp
├── feature_gates.hpp
└── security_policy.hpp
```

Config files must define named constants.

Bad:

```cpp
constexpr int cache_line = 64;
```

Good:

```cpp
namespace cep::target {
    inline constexpr std::size_t cache_line_bytes = CEP_TARGET_CACHE_LINE_BYTES;
}
```

All config constants must have comments:

```cpp
// CEP:WHAT: Target cache line size.
// CEP:WHY: Used for alignment and false-sharing avoidance.
// CEP:STATUS: complete
// CEP:FAILURE: static_assert if target config missing.
// CEP:ASSUMES: provided by target manifest.
// CEP:COST: compile-time only.
// CEP:EVIDENCE: target manual / bench config.
```

---

## 32.12 Security directory rules

Security documentation is normative.

Example:

```text
security/
├── threat_model.md
├── trust_boundaries.md
├── ffi_security.md
├── unsafe_audit.md
├── side_channels.md
└── supply_chain.md
```

Security documents must be versioned and reviewed.

---

## 32.13 Quarantine directory

Non-compliant code may be moved to:

```text
quarantine/
```

Quarantine code:

- is not compiled by default,
- is not linked,
- is not shipped,
- is not benchmarked,
- is not treated as compliant,
- must contain a quarantine manifest.

Quarantine manifest example:

```text
quarantine/
└── old_decoder/
    ├── QUARANTINE.md
    ├── decoder.cpp
    └── decoder.cppm
```

`QUARANTINE.md` must contain:

```text
Reason: hidden allocation in CEP-0
Violation: CEP&CC 9.2
Owner: alice
Ticket: CEP-901
Deadline: 2026-10-15
Disposition: repair or exterminate
```

Quarantine is temporary.

Quarantine is not storage.

If quarantine code is not repaired by its deadline, it is exterminated.

---

# 33. Naming

Naming is normative.

Bad names hide intent. Hidden intent hides cost. Hidden cost violates CEP&CC.

---

## 33.1 General naming principles

Names must be:

- explicit,
- searchable,
- pronounceable,
- consistent,
- non-clever,
- non-ambiguous,
- stable where ABI-stable,
- reflective of cost class where useful.

Avoid:

- jokes,
- puns,
- obscure abbreviations,
- temporary names,
- duplicated names across unrelated concepts,
- names that lie,
- names that imply speed without evidence,
- names that imply safety without proof.

Bad:

```cpp
fast_thing
```

Bad:

```cpp
do_it
```

Bad:

```cpp
helper2
```

Bad:

```cpp
optimized_parser
```

Good:

```cpp
decode_opcode
```

Good:

```cpp
validate_packet_bounds
```

Good:

```cpp
compute_checksum_u32
```

---

## 33.2 File names

Use lowercase snake_case.

Examples:

```text
opcode_decoder.cppm
opcode_decoder.cpp
packet_validator.cpp
arm64_barriers.cppm
checksum_bench.cpp
decoder_test.cpp
```

Rules:

- no spaces,
- no uppercase,
- no version numbers in file names unless ABI versioning requires it,
- no dates in file names,
- no “final”, “new”, “old”, “fixed”,
- no duplicated names in different layers unless intentional and documented.

Bad:

```text
DecoderFinal.cpp
```

Bad:

```text
parser_new.cpp
```

Bad:

```text
checksum2.cpp
```

---

## 33.3 Hot file names

CEP-0 files should be obviously hot.

Recommended:

```text
hot/decoder/opcode_decoder.cppm
hot/checksum/checksum_u32.cppm
```

or file-level tag:

```cpp
// CEP:CLASS: CEP-0
```

If a file contains hot code, the file header must say so.

Example:

```cpp
// CEP:FILE: hot/decoder/opcode_decoder.cppm
// CEP:CLASS: CEP-0
```

---

## 33.4 Cold file names

Cold files should be obviously cold.

Examples:

```text
cold/diagnostics/log_sink.cpp
cold/config/config_parser.cpp
```

File header:

```cpp
// CEP:FILE: cold/config/config_parser.cpp
// CEP:CLASS: CEP-1
```

---

## 33.5 Target file names

Target files must include target identity.

Examples:

```text
target/arm64/barriers.cppm
target/x86_64/cache.cppm
target/riscv64/mmio.cppm
```

Bad:

```text
target/barriers.cppm
```

unless generic.

Good generic fallback:

```text
target/generic/fallback_barriers.cppm
```

---

## 33.6 Test file names

Test files must state what they test.

Pattern:

```text
<component>_test.cpp
```

Examples:

```text
opcode_decoder_test.cpp
checksum_u32_test.cpp
ffi_c_abi_test.cpp
```

For property tests:

```text
opcode_decoder_property_test.cpp
```

For fuzz tests:

```text
packet_validator_fuzz_test.cpp
```

For security tests:

```text
packet_validator_security_test.cpp
```

---

## 33.7 Benchmark file names

Pattern:

```text
<component>_bench.cpp
```

Examples:

```text
opcode_decoder_bench.cpp
checksum_u32_bench.cpp
```

For target-specific benches:

```text
opcode_decoder_arm64_bench.cpp
```

---

## 33.8 Generated file names

Generated files should state their origin.

Examples:

```text
generated/opcode_table.hpp
generated/error_codes.rs
generated/ir_nodes.zig
```

Generated file header:

```cpp
// GENERATED FILE
// Generator: tools/gen_opcode_table.py
// Do not edit manually.
```

---

## 33.9 Namespace names

Namespaces must be lowercase and architectural.

Pattern:

```cpp
namespace cep::<layer>::<component> {}
```

Examples:

```cpp
namespace cep::core {}
namespace cep::hot::decoder {}
namespace cep::cold::config {}
namespace cep::target::arm64 {}
namespace cep::ffi::c {}
namespace cep::detail {}
```

Rules:

- no namespace aliases in public APIs unless stable,
- no `using namespace` in module interfaces,
- no namespace pollution,
- no one-letter namespaces except local lambda/template scope.

---

## 33.10 Module names

Use dotted module names.

Examples:

```cpp
export module cep.core.types;
export module cep.hot.decoder;
export module cep.target.arm64.barriers;
export module cep.ffi.c.decoder;
```

Module names must match directory and namespace structure.

---

## 33.11 Type names

Types use `PascalCase`.

Examples:

```cpp
struct OpcodeInfo {};
class InstructionDecoder {};
enum class DecodeError : std::uint8_t {};
using Checksum = std::uint64_t;
```

Rules:

- no Hungarian notation,
- no `C` prefix,
- no `I` interface prefix unless required by legacy policy,
- no abbreviations unless project-wide glossary defines them.

Bad:

```cpp
struct OpInfo {};
```

unless `Op` is a defined term.

Good:

```cpp
struct OpcodeInfo {};
```

---

## 33.12 Enum names

Enums use `enum class` and `PascalCase` names.

Enumerator names use `snake_case` or `PascalCase` depending on project choice, but one style must be chosen.

Recommended:

```cpp
enum class DecodeError : std::uint8_t {
    none = 0,
    bad_opcode = 1,
    bad_operand = 2,
    unsupported_extension = 3,
};
```

Or:

```cpp
enum class DecodeError : std::uint8_t {
    None = 0,
    BadOpcode = 1,
    BadOperand = 2,
    UnsupportedExtension = 3,
};
```

Do not mix styles.

Rules:

- enums must have explicit underlying type if ABI-stable,
- enums must not be implicitly converted to integers,
- use `std::to_underlying` when conversion is required.

---

## 33.13 Function names

Functions use `snake_case`.

Function names should describe the action.

Examples:

```cpp
auto decode_opcode(std::uint8_t opcode) noexcept -> DecodeResult;
auto validate_packet(PacketView packet) noexcept -> std::expected<Packet, PacketError>;
auto compute_checksum_u32(std::span<const std::uint32_t> data) noexcept -> std::uint64_t;
```

Use verb-first names:

- `decode_`
- `validate_`
- `parse_`
- `compute_`
- `lower_`
- `emit_`
- `schedule_`
- `allocate_`
- `reserve_`
- `flush_`
- `finish_`

Predicates use:

- `is_`
- `has_`
- `can_`
- `should_`

Examples:

```cpp
auto is_aligned(const void* ptr, std::size_t alignment) noexcept -> bool;
auto has_overflow(std::uint32_t a, std::uint32_t b) noexcept -> bool;
```

Factories use:

- `make_`
- `create_`

Examples:

```cpp
auto make_decoder_config() noexcept -> DecoderConfig;
```

Conversions use:

- `to_`
- `as_`
- `into_`

Examples:

```cpp
auto to_wire_format(const Packet& packet) noexcept -> WirePacket;
```

---

## 33.14 Variable names

Variables use `snake_case`.

Examples:

```cpp
std::uint32_t opcode;
std::size_t frame_index;
PacketError parse_error;
```

Rules:

- no single-letter variables except loops, math, template parameters,
- no abbreviations without glossary,
- loop indices may be `i`, `j`, `k` only when conventional,
- iterator names should describe the element.

Bad:

```cpp
auto x = decode();
```

Good:

```cpp
auto decoded_insn = decode();
```

---

## 33.15 Constant names

Constants use `kPascalCase`.

Examples:

```cpp
inline constexpr std::size_t kMaxPacketBytes = CEP_LIMIT_MAX_PACKET_BYTES;
inline constexpr std::uint32_t kInvalidOpcode = 0xFFFFFFFFu;
```

Magic constants are banned.

Bad:

```cpp
constexpr int kMax = 4096;
```

Good:

```cpp
inline constexpr std::size_t kMaxPacketBytes = CEP_LIMIT_MAX_PACKET_BYTES;
```

and the limit itself is documented.

---

## 33.16 Macro names

Macros are discouraged, but if used, they must be prefixed.

Required prefix:

```text
CEP_
```

Examples:

```cpp
#define CEP_TARGET_CACHE_LINE_BYTES 64
#define CEP_HAS_MDSPAN 1
```

Banned:

```cpp
#define MAX 4096
```

Banned:

```cpp
#define DO_DECODE() ...
```

---

## 33.17 Template parameter names

Template parameters use `PascalCase`.

Examples:

```cpp
template <typename Decoder>
auto decode_all(Decoder& decoder) -> DecodeStatus;

template <std::size_t Alignment>
class AlignedBuffer;
```

Concepts use `PascalCase`.

Examples:

```cpp
template <typename T>
concept ContiguousBuffer = requires(T buffer) {
    { buffer.data() } -> std::same_as<std::uint8_t*>;
    { buffer.size() } -> std::convertible_to<std::size_t>;
};
```

---

## 33.18 Error type names

Error types must be explicit.

Patterns:

```cpp
enum class <component>_error;
struct <component>_error;
using <component>_result = std::expected<T, <component>_error>;
```

Examples:

```cpp
enum class DecodeError : std::uint8_t;
using DecodeResult = std::expected<OpcodeInfo, DecodeError>;

enum class PacketError : std::uint8_t;
using PacketParseResult = std::expected<Packet, PacketError>;
```

Do not use generic `Error` unless the scope is tiny.

Bad:

```cpp
enum class Error {};
```

Good:

```cpp
enum class DecoderError {};
```

---

## 33.19 FFI names

FFI symbols must be stable and explicit.

C ABI example:

```c
extern "C" int cep_decode_opcode(uint8_t opcode, cep_opcode_info* out);
```

Rules:

- prefix all public C symbols with project prefix,
- use lowercase snake_case,
- use fixed-width types,
- avoid C++ types in C ABI,
- avoid exceptions,
- avoid allocation,
- document ownership.

Good:

```c
int32_t cep_decoder_validate(const uint8_t* data, size_t size);
```

Bad:

```c
int decode(const char* data, int len);
```

---

## 33.20 Rust naming

Rust follows standard Rust style, but CEP&CC additions apply.

Use:

```rust
snake_case
```

for functions and variables.

Use:

```rust
PascalCase
```

for types.

Use:

```rust
SCREAMING_SNAKE_CASE
```

for constants and statics.

FFI functions must be explicit:

```rust
#[no_mangle]
pub extern "C" fn cep_decode_opcode(opcode: u8) -> DecodeResult
```

Unsafe modules should be named clearly:

```rust
mod unsafe_decoder;
```

or:

```rust
mod ffi;
```

Do not hide unsafe in generic utility modules.

---

## 33.21 C naming

C uses lowercase snake_case.

Public symbols must have project prefix.

Example:

```c
uint32_t cep_mmio_read_u32(volatile uint32_t const* addr);
```

Internal functions may be `static`.

Bad:

```c
uint32_t read_reg(volatile uint32_t* p);
```

Good:

```c
uint32_t cep_mmio_read_u32(volatile uint32_t const* addr);
```

---

## 33.22 Zig naming

Zig uses standard Zig style.

Functions:

```zig
camelCase
```

Types:

```zig
PascalCase
```

Constants:

```zig
camelCase
```

or project-chosen stable style.

FFI functions should use C-compatible names:

```zig
export fn cep_decode_opcode(opcode: u8) callconv(.C) DecodeResult
```

Do not use ambiguous names.

Bad:

```zig
pub fn doThing(...)
```

Good:

```zig
pub fn decodeOpcode(...)
```

---

## 33.23 Python/Lua tool naming

Python:

```python
snake_case
```

for functions and variables.

```python
PascalCase
```

for classes.

```python
UPPER_SNAKE_CASE
```

for constants.

Files:

```text
gen_opcode_table.py
lint_comments.py
bench_gate.py
```

Lua:

```lua
snake_case
```

recommended.

Tool names must describe the tool.

Bad:

```text
gen.py
```

Good:

```text
gen_opcode_table.py
```

---

# 34. Violation handling and extermination

CEP&CC is not advisory.

Violations are defects.

Some defects are style issues.

Some defects are security hazards.

Some defects are performance hazards.

Some defects are all three.

The response is proportional to severity, but the default action for non-compliant code is rejection.

---

## 34.1 Prime extermination rule

> Non-compliant code is not allowed to enter the mainline.  
> If it is detected before merge, it is blocked.  
> If it is detected after merge, it is quarantined, reverted, or deleted.  
> The code is exterminated. The person is not.

This standard is about code hygiene, not punishment.

But code that violates security, cycle-exactness, or assumption rules is not allowed to remain alive in the tree.

---

## 34.2 Violation severity classes

### Severity 0: Exterminate immediately

Severity 0 violations are unacceptable.

Examples:

- undefined behavior,
- memory safety bug,
- buffer overflow,
- use-after-free,
- uninitialized read,
- uninitialized write,
- secret leakage,
- FFI boundary without validation,
- exception crossing FFI,
- panic crossing FFI,
- hidden allocation in CEP-0,
- hidden locking in CEP-0,
- hidden I/O in CEP-0,
- hot-path virtual dispatch,
- hot-path `dyn Trait` in Rust,
- hot-path `std::function`,
- hot-path `std::any`,
- hot-path macro-generated control flow,
- hard-coded target assumption,
- missing layout on FFI type,
- generated code without golden test,
- untrusted input parsed without validation,
- security check removed without proof,
- benchmark evidence falsified,
- stale optimality claim knowingly retained.

Response:

1. Block merge.
2. If already merged, revert or quarantine immediately.
3. Add regression test.
4. Add lint rule if possible.
5. Perform root-cause analysis.

---

### Severity 1: Exterminate unless emergency waiver

Severity 1 violations are serious but may have rare waivers.

Examples:

- missing CEP comment block on hot function,
- missing `CEP:COST` on CEP-0 function,
- missing `CEP:FAILURE` on security-sensitive function,
- missing `CEP:ASSUMES` where assumption exists,
- TODO without owner and ticket,
- stub used in production path without ticket,
- placeholder callable from release path,
- benchmark missing artifact,
- non-deterministic Python/Lua generator output,
- unsafe block without `CEP:UNSAFE`,
- target-specific code outside target directory,
- mixed hot/cold file without waiver.

Response:

1. Block merge.
2. Require repair or deletion.
3. If emergency waiver is granted, record it in `.cep/waivers/`.

Waivers must include:

```text
violation
reason
owner
ticket
security review
performance review
expiration date
```

Waivers without expiration are banned.

---

### Severity 2: Reject and request repair

Severity 2 violations are clean-code violations.

Examples:

- bad naming,
- missing file header,
- missing namespace structure,
- mixed directory placement,
- vague comment,
- stale comment,
- commented-out code,
- missing `[[nodiscard]]`,
- missing explicit constructor,
- magic number in cold code,
- missing test for cold utility.

Response:

1. CI may warn or fail depending on project strictness.
2. Reviewer should reject.
3. Author repairs.

If repeated, code may be quarantined.

---

### Severity 3: Warn and educate

Severity 3 violations are minor style or documentation issues.

Examples:

- inconsistent formatting where formatter is missing,
- minor comment wording issue,
- non-blocking naming preference,
- documentation typo.

Response:

- warn,
- request cleanup,
- do not block unless repeated.

---

## 34.3 Automated extermination pipeline

CI must enforce extermination.

Pipeline stages:

1. **Parse**
   - Check file structure.
   - Check file headers.
   - Check CEP comment tags.
   - Check status tags.
   - Check TODO owner/ticket.

2. **Compile**
   - C++26 mode.
   - Warnings as errors.
   - Feature gates.
   - No banned features in hot code.

3. **Static analysis**
   - Banned functions.
   - Banned types.
   - Banned macros.
   - Hard-coded constants.
   - Missing static assertions.
   - Unsafe block comments.

4. **Sanitizers**
   - ASan.
   - UBSan.
   - TSan.
   - MSan where relevant.

5. **Security**
   - Fuzz tests.
   - FFI validation tests.
   - Secret scanning.
   - Dependency pinning.
   - Supply-chain checks.

6. **Performance**
   - Benchmark gate.
   - Disassembly diff.
   - Allocation check.
   - Virtual call check.
   - Indirect call check.
   - Branch regression check.

7. **Determinism**
   - Generated code reproducibility.
   - Python/Lua tool determinism.
   - Golden file tests.

8. **Verdict**
   - PASS,
   - WARN,
   - FAIL,
   - EXTERMINATE.

If verdict is `EXTERMINATE`, merge is blocked.

---

## 34.4 Extermination actions

When code is exterminated, one of the following actions occurs.

### 34.4.1 Reject

For unmerged code.

The merge request is closed or marked blocked.

Reason must be recorded:

```text
EXTERMINATED: hidden allocation in CEP-0
Rule: CEP&CC 9.2
Evidence: CI run 2026-09-28-1432
```

### 34.4.2 Revert

For merged code that violates Severity 0 or Severity 1.

Revert commit message:

```text
Exterminate commit 8f3a2c1

Reason: hidden allocation in CEP-0 hot decoder.
Rule: CEP&CC 9.2
Ticket: CEP-901
Owner: alice
```

The revert is not optional.

The revert happens before repair if the violation is severe.

### 34.4.3 Quarantine

For code that may be repairable but must not remain active.

Move code to:

```text
quarantine/
```

Add manifest:

```text
QUARANTINE.md
```

Quarantined code is excluded from build.

### 34.4.4 Delete

For code that is not worth repairing.

Delete:

- dead code,
- obsolete stubs,
- expired quarantine,
- repeated violations,
- unowned code,
- code with no tests,
- code with no evidence,
- code with stale security claims.

Deletion is a valid maintenance action.

Dead code is a security liability and a performance lie.

---

## 34.5 Human handling

Do not attack people.

Attack the defect.

Good review comment:

```text
This violates CEP&CC 9.2: hidden allocation in CEP-0.
The code must be quarantined or repaired before merge.
```

Bad review comment:

```text
You wrote terrible code.
```

Repeated violations by a contributor should trigger:

- additional review,
- pairing,
- training,
- reduced merge privileges,
- mandatory CEP&CC checklist sign-off.

But the immediate object of extermination is the code.

---

## 34.6 Violation examples and required responses

### Example 1: Hidden allocation in CEP-0

Violation:

```cpp
auto decode_packet(PacketView view) -> Packet {
    std::vector<std::uint8_t> buffer;
    ...
}
```

Response:

```text
EXTERMINATE
Reason: std::vector allocation in CEP-0.
Rule: CEP&CC 9.2.
Action: revert or replace with caller-provided fixed buffer.
```

---

### Example 2: Missing CEP comments on hot function

Violation:

```cpp
auto decode_opcode(std::uint8_t opcode) noexcept -> OpcodeInfo;
```

No CEP block.

Response:

```text
FAIL
Reason: missing CEP comment block.
Rule: CEP&CC 10.2.
Action: repair before merge.
```

If function is already in CEP-0 and comments are missing after review, quarantine.

---

### Example 3: Hard-coded cache line size

Violation:

```cpp
alignas(64) struct Counter {};
```

No named constant.

Response:

```text
FAIL
Reason: hard-coded cache-line assumption.
Rule: CEP&CC 11.
Action: use cep::target::cache_line_bytes.
```

---

### Example 4: FFI without layout

Rust violation:

```rust
pub struct DecoderState {
    ...
}
```

exported through FFI without `#[repr(C)]`.

Response:

```text
EXTERMINATE
Reason: FFI type lacks explicit layout.
Rule: CEP&CC 25.8.
Action: add #[repr(C)] and layout tests.
```

---

### Example 5: Rust panic in hot path

Violation:

```rust
let value = table[index].unwrap();
```

Response:

```text
EXTERMINATE
Reason: possible panic in CEP-0.
Rule: CEP&CC 25.4.
Action: replace with explicit error handling or proof.
```

---

### Example 6: Zig default allocator in hot path

Violation:

```zig
var list = std.ArrayList(u8).init(std.heap.page_allocator);
```

Response:

```text
EXTERMINATE
Reason: heap allocation in CEP-0.
Rule: CEP&CC 27.4.
Action: replace with fixed buffer or arena.
```

---

### Example 7: C macro logic

Violation:

```c
#define DECODE(x) do { ... } while (0)
```

Response:

```text
EXTERMINATE
Reason: macro-generated control flow.
Rule: CEP&CC 26.5.
Action: replace with static inline function.
```

---

### Example 8: Python generator nondeterminism

Violation:

```python
for key in table.keys():
    emit(key)
```

Output depends on unstable order.

Response:

```text
FAIL
Reason: nondeterministic generated output.
Rule: CEP&CC 28.3.
Action: sort keys or use stable order.
```

If generated code already merged, regenerate golden files or revert.

---

## 34.7 Waiver process

Waivers are rare, temporary, and documented.

Waivers are stored in:

```text
.cep/waivers/
```

Example waiver file:

```text
.cep/waivers/CEP-901.yaml
```

Contents:

```yaml
id: CEP-901
rule: CEP&CC 9.2
component: hot/decoder/opcode_decoder.cpp
violation: inline assembly required due to target errata
owner: alice
security_review: bob
performance_review: carol
created: 2026-09-28
expires: 2026-12-31
status: active
```

Waivers must not:

- be verbal,
- be permanent,
- cover security vulnerabilities without security review,
- cover undefined behavior unless hardware errata is documented,
- apply to generated code without generator fix ticket.

Expired waivers automatically trigger extermination.

---

## 34.8 Extermination report

Every extermination produces a report.

Report location:

```text
.cep/exterminations/
```

Example:

```text
.cep/exterminations/2026-09-28-CEP-901.md
```

Report contents:

```markdown
# Extermination Report CEP-901

Date: 2026-09-28
Component: hot/decoder/opcode_decoder.cpp
Violation: hidden allocation in CEP-0
Rule: CEP&CC 9.2
Severity: 0
Detected by: CI benchmark gate
Action: reverted commit 8f3a2c1
Owner: alice
Follow-up: CEP-902
Regression test: tests/unit/hot/decoder_allocation_test.cpp
```

Extermination reports are not optional.

They are institutional memory.

---

## 34.9 Post-extermination process

After extermination, the team must:

1. Identify the root cause.
2. Add a regression test.
3. Add lint or CI rule if possible.
4. Update documentation.
5. Repair or delete quarantined code.
6. Close the ticket.

If the same violation recurs three times in the same component, the component is considered structurally non-compliant.

Structural non-compliance requires redesign or deletion.

---

# 35. File header requirements

Every first-party source file must begin with a file-level CEP block.

C++ example:

```cpp
// CEP:FILE: hot/decoder/opcode_decoder.cppm
// CEP:WHAT: Module interface for the hot opcode decoder.
// CEP:WHY: Provides allocation-free opcode decoding for CEP-0 dispatch.
// CEP:CLASS: CEP-0
// CEP:STATUS: complete
// CEP:FAILURE: Returns DecodeError for invalid opcodes. No allocation. No throw.
// CEP:ASSUMES: Input opcode is a raw untrusted byte; validated inside.
// CEP:COST: 3 cycles expected on arm64-a78, bench CEP-0019.
// CEP:EVIDENCE: bench CEP-0019, asm artifact a41c9e2.
// CEP:SECURITY: Handles untrusted opcode input.
```

C example:

```c
// CEP:FILE: target/arm64/mmio.c
// CEP:WHAT: MMIO access helpers for arm64 target.
// CEP:WHY: Hardware registers require volatile accesses with explicit ordering.
// CEP:CLASS: CEP-0
// CEP:STATUS: complete
// CEP:FAILURE: none if caller provides valid aligned target address.
// CEP:ASSUMES: address is 4-byte aligned and target-mapped.
// CEP:COST: one volatile load/store per access.
// CEP:EVIDENCE: target manual section 12.4.
// CEP:SECURITY: MMIO values are untrusted and must be validated by caller.
```

Rust example:

```rust
// CEP:FILE: ffi/decoder_ffi.rs
// CEP:WHAT: C ABI FFI for opcode decoder.
// CEP:WHY: Provides stable C interface to Rust decoder.
// CEP:CLASS: FFI
// CEP:STATUS: complete
// CEP:FAILURE: Returns error code; does not panic.
// CEP:ASSUMES: C caller validates pointer validity where possible.
// CEP:COST: one table lookup after validation.
// CEP:EVIDENCE: bench CEP-RS-0019.
// CEP:SECURITY: FFI boundary; all inputs validated.
```

Zig example:

```zig
// CEP:FILE: ffi/decoder_ffi.zig
// CEP:WHAT: C ABI FFI for opcode decoder.
// CEP:WHY: Exposes deterministic decoder to C/C++.
// CEP:CLASS: FFI
// CEP:STATUS: complete
// CEP:FAILURE: Returns error enum; no panic.
// CEP:ASSUMES: caller provides valid slice bounds.
// CEP:COST: one table lookup after validation.
// CEP:EVIDENCE: bench CEP-ZIG-0019.
// CEP:SECURITY: FFI boundary; inputs validated.
```

Python tool example:

```python
# CEP:FILE: tools/gen_opcode_table.py
# CEP:WHAT: Generates opcode table from opcode specification.
# CEP:WHY: Keeps opcode metadata synchronized and deterministic.
# CEP:CLASS: CEP-2
# CEP:STATUS: complete
# CEP:FAILURE: Exits with error if spec is malformed.
# CEP:ASSUMES: spec file is UTF-8 and repository-trusted.
# CEP:COST: offline tool; runtime cost irrelevant.
# CEP:EVIDENCE: golden test generated/opcode_table.hpp.golden.
# CEP:SECURITY: no network; no untrusted input.
```

---

# 36. Updated repository compliance checklist

A repository is compliant only if:

## Structure

- [ ] Hot and cold code are separated.
- [ ] Target-specific code is isolated.
- [ ] FFI code is isolated.
- [ ] Generated code is isolated.
- [ ] Tests mirror source.
- [ ] Benchmarks mirror hot code.
- [ ] Third-party code is vendored and separated.
- [ ] Quarantine exists and is excluded from builds.
- [ ] Config constants are centralized.

## Naming

- [ ] Files use lowercase snake_case.
- [ ] Modules match namespaces and directories.
- [ ] Types use PascalCase.
- [ ] Functions use snake_case.
- [ ] Constants use kPascalCase or equivalent.
- [ ] Macros use CEP_ prefix.
- [ ] FFI symbols use stable project prefix.
- [ ] No clever or misleading names.
- [ ] No dead names like `old`, `new`, `final`.

## Violation handling

- [ ] CI can block non-compliant merges.
- [ ] CI can detect banned hot-path constructs.
- [ ] CI can detect missing CEP comments.
- [ ] CI can detect missing evidence.
- [ ] CI can detect nondeterministic generators.
- [ ] Waivers are written, temporary, and reviewed.
- [ ] Extermination reports exist.
- [ ] Quarantine has deadlines.
- [ ] Expired quarantine is deleted.

---

# 37. Final extermination clause

The final rule is:

> CEP&CC does not negotiate with non-compliant code.

If code cannot prove:

- what it is,
- why it exists,
- what it assumes,
- how it fails,
- what it costs,
- whether it is secure,
- whether it is optimal,
- whether it is complete,

then it does not belong in the codebase.

It is either fixed, quarantined, or exterminated.
