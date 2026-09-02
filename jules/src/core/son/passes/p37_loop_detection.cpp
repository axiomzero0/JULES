// Pass 37 — LoopDetection (Phase 4: Loop Analysis & Transforms)
//
// Analysis pass: identifies natural loops via backedge detection (succ u->h
// where h dominates u), builds the loop tree (inner-first, nesting depth,
// preheaders). Warm LoopInfo in the analysis manager for every consumer.
#include "core/son/passes/pass_utils.h"

namespace jules {

class LoopDetectionPass : public Pass {
public:
    const char* name() const override { return "LoopDetection"; }
    int order() const override { return 37; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::Dominators); }
    AnalysisMask invalidated() const override { return 0; } // pure analysis
    bool run(PassContext& ctx) override {
        bool found = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            LoopInfo& li = ctx.analysis.loops(fg);
            found |= !li.loops().empty();
        }
        return found; // telemetry: loop structure present
    }
};

JULES_REGISTER_PASS(LoopDetectionPass, 37, "Phase 4")

} // namespace jules
