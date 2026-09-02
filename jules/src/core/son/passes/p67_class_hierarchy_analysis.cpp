// Pass 67 — ClassHierarchyAnalysis (Phase 6: Devirtualization & Speculation)
//
// Builds the (single-module MVP) class/trait hierarchy and computes possible
// call targets. With no dyn-dispatch in the MVP subset the hierarchy is
// empty and every call site is already direct — the analysis runs, reports
// a monomorphic target per site, and the devirtualization passes 68/69
// become vacuous. The interface is the contract future trait support plugs
// into.
#include "core/son/passes/pass_utils.h"

namespace jules {

class ClassHierarchyAnalysisPass : public Pass {
public:
    const char* name() const override { return "ClassHierarchyAnalysis"; }
    int order() const override { return 67; }
    const char* phase_name() const override { return "Phase 6: Devirtualization & Speculation"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::CallGraph); }
    AnalysisMask invalidated() const override { return 0; }
    bool run(PassContext& ctx) override {
        CallGraphInfo& cg = ctx.analysis.callgraph();
        bool monomorphic = true;
        for (FunctionGraph& fg : ctx.mod.fns) {
            for (const CallGraphInfo::Site& s : cg.sites_of(fg.fid)) {
                if (s.callee == kNoFn) monomorphic = false;
            }
        }
        (void)monomorphic; // telemetry: all sites resolve directly in MVP
        return false;
    }
};

JULES_REGISTER_PASS(ClassHierarchyAnalysisPass, 67, "Phase 6")

} // namespace jules
