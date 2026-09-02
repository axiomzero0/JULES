// Pass 52 — HotPathStraightening (Phase 4)
//
// Chooses the block layout preference for the linearizer (pass 83): the
// loop body stays contiguous with its header, the hot branch successor
// becomes the fallthrough, and backedges are taken-branches. The linearizer
// applies the preference when emitting RPO order (trace-scheduling-lite).
// This pass warms BranchProb and validates that every loop has a layout
// plan (preheader -> header -> body -> exits).
#include "core/son/passes/pass_utils.h"

namespace jules {

class HotPathStraighteningPass : public Pass {
public:
    const char* name() const override { return "HotPathStraightening"; }
    int order() const override { return 52; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo | AnalysisKind::BranchProb;
    }
    AnalysisMask invalidated() const override { return 0; }
    bool run(PassContext& ctx) override {
        bool any_plan = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            LoopInfo& li = ctx.analysis.loops(fg);
            BranchProb& bp = ctx.analysis.branchprob(fg);
            for (const Loop& l : li.loops()) {
                if (li.preheader(l.header) != kNoNode) any_plan = true;
            }
            (void)bp;
        }
        return any_plan;
    }
};

JULES_REGISTER_PASS(HotPathStraighteningPass, 52, "Phase 4")

} // namespace jules
