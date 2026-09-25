// Partial Evaluation & Partial Deoptimization (PE/PD family).
//
// Design (see docs/pass_status.md §Partial Evaluation):
//   * A VARIANT is a specialized clone of a function under an ASSUMPTION SET
//     (parameter -> constant bindings). Static PE (pass 90) derives bindings
//     from provable call-site constants; guarded PE (pass 91) derives them
//     from PGO argument sketches and protects each binding with a runtime
//     guard whose failure transfers to the less-specialized rung — the
//     version ladder: V{n} (all assumptions) -> ... -> V0 (generic).
//   * BINDING-TIME ANALYSIS (BTA) classifies the callee's nodes under the
//     bindings WITHOUT rewriting — the benefit gate. Effectful nodes
//     (Load/Call/Alloc) are conservatively dynamic: memory state is the #1
//     staticness blocker and this MVP does not split memory chains on
//     alias facts (documented gap).
//   * TERMINATION is by construction: single-generation specialization
//     (variants are never themselves specialized), a per-function variant
//     cap, a per-variant node-growth budget tied to the optimization
//     level, and a global node budget. Context merging beyond exact-value
//     dedup (range/enum lattices) is future work; the table dedups exact
//     assumption sets via structural keys.
//   * CORRECTNESS of the ladder: every guard sits at a call boundary (a
//     checkpoint), so a failed assumption transfers BEFORE any of the
//     specialized body's effects execute — no OSR, no compensation, no
//     side-effect replay. The generic call runs the whole function; the
//     variant runs the whole function; exactly one runs.
//
// Honesty: this is the Phase-1 (function-entry) subset of the design.
// Loop-header deopt, OSR, materialization, and alias-splitting are NOT
// implemented; guards are emitted at call boundaries only.
#pragma once

#include "core/son/passes/pass_utils.h"

#include <string>
#include <vector>

namespace jules {

enum class PeKind : u8 { Const, Range };

// One assumption on a parameter slot. Two kinds:
//   * Const  — param == value (p90's proven call-site constants; p91's
//     sticky-hot values). The bound param is DROPPED from the variant's
//     signature (replaced by an entry Const).
//   * Range  — param stays a runtime value, but provably within
//     [range_lo, range_hi] on profiled invocations (p91 only). The param
//     is KEPT in the variant; branches that cannot be taken anywhere in
//     the interval fold away (bounds checks, saturating clamps). This is
//     the Const -> Range -> Dynamic widening ladder: the sketch reports
//     [first..match] hot or the [min..max] hull, never both.
struct PeAssumption {
    u8 param = 0;
    PeKind kind = PeKind::Const;
    ConstVal value;             // Const: the exact value (ty = param type)
    i64 range_lo = 0;           // Range: inclusive lower bound
    i64 range_hi = 0;           // Range: inclusive upper bound
};

// Level-tied budgets (the spec's termination & code-size safety).
struct PeBudgets {
    u32 max_variants_per_fn = 0; // ladder width per function
    u32 growth_num = 1;          // variant live nodes <= origin * growth_num
    u32 growth_den = 1;          //   ... / growth_den
    u32 total_nodes = 0;         // global node budget across all variants
    u32 range_max_span = 0;      // max (hi - lo) for Range assumptions
};
PeBudgets pe_budgets(OptLevel lvl);

// BTA summary (analysis only; no rewriting).
struct PeBta {
    u32 static_values = 0;  // value nodes that become compile-time
    u32 dynamic_values = 0; // value nodes that stay runtime
    u32 static_ifs = 0;     // branches whose condition becomes compile-time
};
PeBta pe_binding_time_analysis(Graph& g, const std::vector<PeAssumption>& bindings);

// Fold engine: iterate {reachability, constant propagation, dead-branch
// pruning, phi collapse} to a fixpoint on a freshly bound graph. Shared by
// variant creation (the "late-stage peephole on specialized graphs").
// Returns true when any rewrite fired. `bindings` (optional) seed Range
// assumptions onto the KEPT param nodes of the cloned graph — indices are
// the CLONE's compact indices (see pe_make_variant), not the origin's.
bool pe_fold(Graph& g, u32 round_limit,
            const std::vector<PeAssumption>* bindings = nullptr);

// Variant creation. Clones `origin`, binds the assumptions, folds, checks
// budgets (level-tied), dedups against previously created variants, and
// appends the accepted variant to the module. Returns the variant's FnId,
// or kNoFn when rejected (no benefit / budget / duplicate of the origin).
// `changed` (optional) reports whether a usable VARIANT materialized —
// created fresh OR reused via the exact-assumption-set dedup (identical
// binding sets across call sites share one variant; the fold telemetry
// of a reused variant belongs to its creation, not the reuse).
//
// `extra_slots` (default 0): additional ladder-width slots the caller has
// ALREADY retired in the same action — the per-fn cap becomes
// (existing + extra) < max_variants_per_fn. Contract: only a caller that
// kills an existing rung's calls for this origin in the same rewrite may
// spend its slot (the weakened rung REPLACES the retired one's ladder
// position, so the ladder width per call site is unchanged). Termination
// holds: pass 73 weakens each site at most once per run and the global
// node budget still bounds the total.
FnId pe_make_variant(Module& mod, SymbolTable& syms, FnId origin,
                     const std::vector<PeAssumption>& bindings, const PeBudgets& b,
                     bool* changed = nullptr, u32 extra_slots = 0);

// Re-derive the variant's assumptions (empty = not a PE variant).
const std::vector<PeAssumption>* pe_variant_assumptions(FnId fid);

// The origin function a PE variant was cloned from (kNoFn = not a
// variant). Guard-family passes need the origin to re-derive PGO sketch
// keys — profiles enumerate ORIGINAL functions only.
FnId pe_variant_origin(FnId fid);

// ---- PGO argument sketches (pass 91) ---------------------------------------
//
// Instrument mode pins one sticky-value sketch per (function, integer
// parameter) at the function entry: counters [first, total, match, min,
// max] where `first` is the first observed argument value, `total` the
// invocation count, `match` the count of invocations equal to `first`,
// and [min, max] the observed hull. A parameter is hot when either
//   * match/total >= 95% with total >= 64 (sticky value -> Const), or
//   * total >= 64 and (max - min) fits the level's range budget
//     (-> Range assumption; 64-bit int params only — narrower widths
//     load zero-extended and the recorded hull would not be the source
//     domain's signed order).
// The sketch is one-sided on stickiness (it cannot see a value hotter
// than the first one); that only costs missed specializations, never
// correctness: the guard protects every use.
//
// Counter layout in jules.prof: [pass-43 loop pairs (2 each)]
//                               [sketches (5 each), (fn, param) order].
// The sketch enumeration covers ORIGINAL functions only (fid < the
// module's entry-time function count) so appended PE variants cannot
// shift indices between the instrument and use builds.
inline constexpr u64 kPeSketchMinSamples = 64;
inline constexpr u64 kPeSketchMinMatchPct = 95; // match * 100 >= pct * total
inline constexpr u64 kPeSketchSlots = 5;         // [first,total,match,min,max]

// Number of sketch slots over the module's original functions
// (integer-typed parameters of fids < orig_fn_count).
u32 pe_sketch_slots(const Module& mod, u32 orig_fn_count);

// Sketch base index (first counter of the sketch region).
inline u64 pe_sketch_base(const PassOptions& o) { return 2ull * o.pgo_loop_pairs; }

// Rewire a replaced call's users onto a (value, memory) pair — Calls are
// both a value and a memory version; users split by slot kind exactly like
// the inliner's exit rewire (see inline_util.cpp for the miscompile this
// prevents). `new_val` may be kNoNode for void calls.
void pe_rewire_call_users(Graph& g, NodeId old_call, NodeId new_val, NodeId new_mem);

} // namespace jules
