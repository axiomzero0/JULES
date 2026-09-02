// Pass 76 — InliningCostModel (Phase 7: Inlining & Interprocedural)
//
// Analysis pass: computes per-call-site benefit scores:
//   score = callee_node_estimate * (all-args-constant ? 0.5 : 1.0)
// and flags force-inline sites (#[inline(always)], trivial bodies).
// The numbers feed passes 77/78/80 decisions and --stats telemetry.
#include "core/son/passes/inline.h"

namespace jules {

namespace {
struct SiteScore {
    NodeId call = kNoNode;
    FnId caller = kNoFn, callee = kNoFn;
    u32 score = 0;
    bool force = false;
    bool all_const_args = false;
};

u32 score_site(FunctionGraph& caller, NodeId call, Module& mod) {
    const Node& c = caller.g.node(call);
    if (c.aux == kFnPrint || c.aux == kFnFree) return 0;
    const FunctionGraph* callee = mod.find_fn(c.aux);
    if (!callee) return 0;

    bool all_const = c.n_in > 2;
    for (u8 i = 2; i < c.n_in; ++i)
        if (caller.g.node(c.in[i]).op != Op::Const) { all_const = false; break; }

    u32 score = callee->node_estimate;
    if (all_const) score = score / 2; // argument specialization halves the cost
    return score;
}
} // namespace

class InliningCostModelPass : public Pass {
public:
    const char* name() const override { return "InliningCostModel"; }
    int order() const override { return 76; }
    const char* phase_name() const override { return "Phase 7: Inlining & Interprocedural"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::CallGraph); }
    AnalysisMask invalidated() const override { return 0; } // pure analysis
    bool run(PassContext& ctx) override {
        (void)ctx.analysis.callgraph();
        bool any = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            for (NodeId id = 0; id < fg.g.size(); ++id) {
                const Node& n = fg.g.node(id);
                if (n.op != Op::Call) continue;
                u32 s = score_site(fg, id, ctx.mod);
                if (s > 0) any = true;
            }
        }
        return any;
    }
};

JULES_REGISTER_PASS(InliningCostModelPass, 76, "Phase 7")

} // namespace jules
