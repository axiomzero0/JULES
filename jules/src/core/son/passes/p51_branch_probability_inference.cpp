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
        for (FunctionGraph& fg : ctx.mod.fns) {
            BranchProb& bp = ctx.analysis.branchprob(fg);
            (void)bp; // warm + annotate: the analysis manager computes and
        }             // caches the per-If probabilities this pass is named
        return false; // for (2026-09-18 audit fix: analysis-only passes
                      // report no transformation — same contract as p19/p20;
                      // previously returned true whenever If nodes existed,
                      // inflating the --stats `changes` column).
    }
};

JULES_REGISTER_PASS(BranchProbabilityInferencePass, 51, "Phase 4")

} // namespace jules
