// Pass 78 — CostBasedInlining (Phase 7)
//
// Threshold-driven inlining for the remaining call sites. Iterates: each
// successful inline exposes new inlinable sites inside the cloned body,
// until nothing qualifies or the per-function budget (cloned nodes) is
// exhausted. Recursion is bounded by pass 79's policy.
//
// Level budgets (spec §8): threshold and per-function budget are presets —
// -O1 16/50, -O2 24/225, -O3 48/600, -Os/-Oz smaller (size-biased).
#include "core/son/passes/inline.h"

namespace jules {

class CostBasedInliningPass : public Pass {
public:
    const char* name() const override { return "CostBasedInlining"; }
    int order() const override { return 78; }
    const char* phase_name() const override { return "Phase 7: Inlining & Interprocedural"; }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo |
               AnalysisKind::AliasInfo | AnalysisKind::MemDep | AnalysisKind::CallGraph;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        LevelBudgets lb = level_budgets(ctx.opts.level);
        u32 threshold = lb.inline_threshold;
        u32 budget = lb.inline_budget;
        for (u32 round = 0; round < kMaxRounds && budget > 0; ++round) {
            bool this_round = false;
            for (FunctionGraph& caller : ctx.mod.fns) {
                for (NodeId id = 0; id < caller.g.size(); ++id) {
                    if (budget == 0) break;
                    const Node& n = caller.g.node(id);
                    if (n.op != Op::Call || n.aux == kFnPrint || n.aux == kFnFree) continue;
                    const FunctionGraph* callee = ctx.mod.find_fn(n.aux);
                    if (!callee || callee->no_inline) continue;
                    if (callee->node_estimate > threshold) continue;
                    if (!inline_recursion_ok(caller.fid, callee->fid)) continue;
                    // constant arguments make the clone cheap (pass 80 policy hook)
                    u32 effective = callee->node_estimate;
                    bool all_const = n.n_in > 2;
                    for (u8 i = 2; i < n.n_in; ++i)
                        if (caller.g.node(n.in[i]).op != Op::Const) { all_const = false; break; }
                    if (all_const) effective /= 2;
                    if (effective > budget) continue;

                    if (inline_call(caller, id, *const_cast<FunctionGraph*>(callee))) {
                        budget -= effective;
                        this_round = true;
                        break; // rescan the caller (ids changed roles)
                    }
                }
            }
            changed |= this_round;
            if (!this_round) break;
        }
        return changed;
    }

private:
    static constexpr u32 kMaxRounds = 16;
};

JULES_REGISTER_PASS(CostBasedInliningPass, 78, "Phase 7")

} // namespace jules
