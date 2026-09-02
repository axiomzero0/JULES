# Optimization Levels

The user-visible optimization system is an orthogonal matrix: **levels** are
compile-time budget presets, and PGO / LTO / FP policy / JIT budgets are
**modifiers** that tune the same unified pipeline. There is no `-O4`, no
`-Ofast`, and no hidden runtime tiers.

```text
-O level        how aggressively to optimize
--fp            whether FP identities are legal
--pgo           where profile evidence comes from
--lto / --fto   how much cross-module visibility the optimizer gets
--jit-budget    runtime compilation latency class (JIT modes only)
```

## Levels

| Level | Goal | Register allocator | Cleanup rounds | Inline budget |
|---|---|---|---|---|
| `-O0` | Debug fidelity | spill-everywhere | 0 | trivial sites only |
| `-Og` | Debug-friendly opt | spill-everywhere | 0 | trivial sites only |
| `-O1` | Fast optimization | linear scan | 1 | 50 |
| `-O2` | Default production (default) | linear scan + folds | 2 | 225 |
| `-O3` | Peak performance | linear scan + folds | 3 | 600 |
| `-Os` | Size-aware production | linear scan (size-biased) | 1 | 100 |
| `-Oz` | Aggressive size | linear scan (size-biased) | 1 | 40 |

Bare `-O` means `-O1` (GCC convention); **no level flag defaults to `-O2`**
(the release preset). Level availability per pass follows the catalog's
§7-style matrix in `src/core/son/passes/opt_levels.cpp`: required lowering
passes (TypeCanonicalization, ComptimeResidueFold, PhiSimplification,
MemorySSARepair, linearization, isel) run at every level; optimization passes
gate by tier (Off / Limited / On / Aggressive). Levels are budget presets —
they never change program semantics: the test suite runs every program at
every level and diffs byte-identical output.

## Modifiers

- `--fp=strict` (default) — IEEE-strict; FP reassociation stays off.
  `--fp=fast` — explicit opt-in; allows FP operand canonicalization in
  Reassociation. No other numeric semantics change.
- `--pgo=off | instrument | use=<file> | sample=<file> | live` — accepted and
  validated; profile plumbing (counters, versioned profile format, evidence
  thresholds) is not implemented yet, so non-off modes compile without
  profile data and print a note. Profiles are a force multiplier, not new
  logic: no pass becomes legal from a profile alone.
- `--lto=none | thin | full`, `--fto` — visibility modes. The compiler is
  single-module, so the default is full whole-program visibility: all
  functions are in one graph and cross-"module" inlining/devirtualization is
  just inlining. `--lto=none` disables cross-function inlining (the
  separate-compilation semantics) and pass 82's summaries.
- `--jit-budget=fast | balanced | peak` — JIT compile-latency classes that
  cap the effective level (fast → -O1, balanced → -O2, peak → uncapped).
  These are compilation latency budgets, not execution tiers: the modes
  remain `aot | jit-baseline | jit-optimizing`.

## What -O2/-O3 actually buy today

On the six benchmark kernels, `-O2` and `-O3` produce near-identical code
because the input graphs are already simple after the -O2 pass set; the level
differences concentrate in compile time, pass activity, and larger programs
(where the inline budget and cleanup fixpoint rounds matter). The measured
end-to-end picture is in `bench/results/summary.md` and the PDF report.
