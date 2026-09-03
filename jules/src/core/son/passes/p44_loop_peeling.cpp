// Pass 44 — LoopPeeling (Phase 4)
//
// First-iteration peeling as a standalone transform, used where a peeled
// prologue is profitable on its own:
//   * REMAINDER PEELING toward the unroll factor: when a counted loop's
//     constant trip T does not divide the factor F, the first T mod F
//     iterations are peeled into a straight-line prologue so the residual
//     loop (T - T mod F iterations) unrolls exactly (pass 42 runs again
//     in the post-inline cleanup round, after this pass, and finds it).
//     Peeled copies execute unconditionally: T is an exact constant, so
//     those iterations always ran.
// The peeled chain attaches at the loop ENTRY: [entry] -> E1 -> body1
// ... J1 -> ... -> Jr -> header, and every header phi's entry input
// becomes the last peeled copy's value (the IV starts at init + r*k).
#include "core/son/passes/loop_transforms.h"

namespace jules {

class LoopPeelingPass : public Pass {
public:
    const char* name() const override { return "LoopPeeling"; }
    int order() const override { return 44; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        u32 factor = level_budgets(ctx.opts.level).unroll_factor;
        if (factor < 2) return false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Graph& g = fg.g;
            for (int round = 0; round < 8; ++round) {
                bool this_round = false;
                auto li = LoopInfo::compute(g, ctx.analysis.doms(fg));
                for (const Loop& l : li->loops()) {
                    loopx::CountedLoop cl;
                    if (!loopx::match_counted(g, *li, ctx.analysis.doms(fg), l, cl)) continue;
                    if (cl.trip < 6) continue;
                    if (cl.body_nodes > 60) continue;
                    u64 r = static_cast<u64>(cl.trip) % factor;
                    if (r == 0) continue; // already divisible: pass 42's job
                    u64 residual = static_cast<u64>(cl.trip) - r;
                    if (residual < 4) continue;
                    if (cl.body_nodes * (r + 1) > 240) continue;
                    if (loopx::clone_body_chain(g, cl, static_cast<u32>(r), true) == 0)
                        continue;
                    this_round = true;
                    break; // structure changed: recompute
                }
                changed |= this_round;
                if (!this_round) break;
            }
        }
        return changed;
    }
};

JULES_REGISTER_PASS(LoopPeelingPass, 44, "Phase 4")

} // namespace jules
