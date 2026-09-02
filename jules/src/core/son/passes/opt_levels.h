// Optimization levels and budget presets (the user-visible -O matrix).
//
// Design contract (docs/opt_levels.md, mirrored from the language spec):
//   * Optimization levels are compile-time budget presets.
//   * PGO, FTO/LTO, and JIT budgets are NOT levels — they are orthogonal
//     modifiers that tune the same unified pipeline.
//   * No -O4, no -Ofast, no hidden runtime tiers.
//
// Level semantics follow the spec's pass-availability matrix (§7) and
// budget table (§8). Gating tiers:
//     Off        pass does not run at this level
//     Limited    pass runs in conservative mode (~)
//     On         pass runs (full for -O2, default production)
//     Aggressive pass runs with the larger budget (✓✓, -O3 only)
// Passes not listed in the spec matrix inherit the phase default.
#pragma once

#include "core/support/common.h"

#include <string>

namespace jules {

enum class OptLevel : u8 {
    O0 = 0, // debug fidelity: required lowering only
    Og = 1, // debug-friendly optimization
    O1 = 2, // fast optimization, obvious wins
    O2 = 3, // default production (the most important level)
    O3 = 4, // peak performance
    Os = 5, // size-aware production
    Oz = 6, // aggressive size reduction
};

constexpr int kOptLevelCount = 7;
const char* opt_level_name(OptLevel l);
bool parse_opt_level(const std::string& s, OptLevel& out); // "-O0".."Oz" forms

// ---- orthogonal modifiers ------------------------------------------------------

enum class FpMode : u8 { Strict, Fast };
enum class PgoMode : u8 { Off, Instrument, Use, Sample, Live };
enum class LtoMode : u8 { None, Fto, Thin, Full };
enum class JitBudget : u8 { Fast, Balanced, Peak };

// ---- availability tiers ---------------------------------------------------------

enum class Avail : u8 { Off, Limited, On, Aggressive };

// Availability of a catalog pass at a level (spec §7 matrix).
Avail pass_avail(int order, OptLevel lvl);

// Scheduler decision: does the pass run at this level? (Off => skip;
// Limited/On/Aggressive all run — passes read the tier to throttle.)
inline bool level_runs(int order, OptLevel lvl) {
    return pass_avail(order, lvl) != Avail::Off;
}

// ---- budgets (spec §8) -----------------------------------------------------------

struct LevelBudgets {
    u32 inline_threshold;   // callee node-estimate ceiling for inlining
    u32 inline_budget;      // per-function cloned-node budget
    u32 cleanup_rounds;     // post-inline SCCP/GVN/DCE re-run rounds
    u32 unroll_factor;      // max unroll factor (loop unrolling pass)
    bool size_biased;       // cost model bias for -Os/-Oz
    bool ra_registers;      // pass 85: real register allocation (vs spill-only)
};

LevelBudgets level_budgets(OptLevel lvl);

// --fp=fast legality gates (checked by value-optimizing passes):
//     reassociation of FP operations and FP algebraic identities are only
//     legal when the user explicitly opted in; default is IEEE-strict.
inline bool fp_fast_allowed(FpMode m) { return m == FpMode::Fast; }

// JIT compile-latency budget caps the effective level (spec §12): the
// user-visible level still applies; the budget only constrains how much
// work the runtime compiler may spend at that moment.
OptLevel cap_level_for_jit(JitBudget b, OptLevel requested);

} // namespace jules
