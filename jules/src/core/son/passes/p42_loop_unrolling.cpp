// Pass 42 — LoopUnrolling (Phase 4)
//
// Body-duplication unrolling for COUNTED loops with a compile-time-constant
// trip count: `while i < N` with i from a constant, step +k, N constant,
// no early exits. Factor F comes from the level budget (O2: 4, O3: 8 —
// see opt_levels). When T % F != 0 the pass peels the remainder FIRST
// (pass 44's machinery, invoked here so the transform is self-contained
// in one pipeline slot) and then duplicates the body F-1 times:
//     [guard][body][latch]  ->  [guard][body x F][latch]
// The IV phi steps F*k per unrolled iteration; each copy m sees the IV
// value of dynamic iteration base+m (the cloner's seed remap). The guard
// is unchanged: with T' = T - r divisible by F, `i < N` exits exactly
// after T/F unrolled iterations.
// Dynamic-trip loops (unknown N) do not unroll: the guarded-epilogue
// construction (pre-header test + two-exit loop) is documented as the
// roadmap item.
#include "core/son/passes/loop_transforms.h"

namespace jules {

class LoopUnrollingPass : public Pass {
public:
    const char* name() const override { return "LoopUnrolling"; }
    int order() const override { return 42; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            changed |= unroll_fn(fg, ctx);
        }
        return changed;
    }

private:
    static bool unroll_fn(FunctionGraph& fg, PassContext& ctx) {
        Graph& g = fg.g;
        u32 factor = level_budgets(ctx.opts.level).unroll_factor;
        if (factor < 2) return false;
        bool changed = false;
        // innermost-first (LoopInfo orders by body size ascending); loop
        // invalidation after a rewrite forces a re-compute
        for (int round = 0; round < 8; ++round) {
            bool this_round = false;
            auto li = LoopInfo::compute(g, ctx.analysis.doms(fg));
            for (const Loop& l : li->loops()) {
                loopx::CountedLoop cl;
                if (!loopx::match_counted(g, *li, ctx.analysis.doms(fg), l, cl)) continue;
                if (cl.trip < 6) continue;               // too small to matter
                if (cl.body_nodes > 60) continue;        // size guard
                u32 f = factor;
                if (cl.trip % f != 0) {
                    // remainder: peel r copies so the residual trip divides
                    u64 r = static_cast<u64>(cl.trip) % f;
                    u64 residual = static_cast<u64>(cl.trip) - r;
                    if (residual < 4) continue;           // would not unroll after all
                    if (cl.body_nodes * (r + 1) > 240) continue;
                    if (loopx::clone_body_chain(g, cl, static_cast<u32>(r), true) == 0)
                        continue;
                    this_round = true;
                    break; // structure changed: recompute loop info
                }
                if (static_cast<u64>(cl.trip) / f < 2) continue; // nothing gained
                if (cl.body_nodes * f > 240) continue;
                if (loopx::clone_body_chain(g, cl, f - 1, false) == 0) continue;
                this_round = true;
                break; // recompute
            }
            changed |= this_round;
            if (!this_round) break;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(LoopUnrollingPass, 42, "Phase 4")

} // namespace jules
