// Pass 51 — BranchProbabilityInference (Phase 4)
//
// Analysis pass: annotates If nodes with static probabilities (loop
// backedges hot at 0.90, others 0.50 default) stored in BranchProb through
// the analysis manager. The linearizer consumes it for hot-path fallthrough
// layout (pass 52's contract).
#include "core/son/passes/pass_utils.h"

namespace jules {

class BranchProbabilityInferencePass : public Pass {
public:
    const char* name() const override { return "BranchProbabilityInference"; }
    int order() const override { return 51; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override { return 0; } // pure analysis
    bool run(PassContext& ctx) override {
        bool any = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            BranchProb& bp = ctx.analysis.branchprob(fg);
            // warm + telemetry: count annotated branches
            for (NodeId id = 0; id < fg.g.size(); ++id)
                if (fg.g.node(id).op == Op::If) any = true;
            (void)bp;
        }
        return any;
    }
};

JULES_REGISTER_PASS(BranchProbabilityInferencePass, 51, "Phase 4")

} // namespace jules
