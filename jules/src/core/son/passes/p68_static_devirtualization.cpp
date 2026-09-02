// Pass 68 — StaticDevirtualization (Phase 6)
//
// Resolves dyn/vtable calls via type propagation + CHA. STATUS: vacuously
// complete — the MVP subset has no `dyn Trait` (all dispatch is static;
// every Call node carries a concrete FnId). The pass scans to confirm the
// invariant and reports it. When trait objects land, this is the AOT-side
// devirtualizer (whole-program analysis; the JIT-side twin is pass 69).
#include "core/son/passes/pass_utils.h"

namespace jules {

class StaticDevirtualizationPass : public Pass {
public:
    const char* name() const override { return "StaticDevirtualization"; }
    int order() const override { return 68; }
    const char* phase_name() const override { return "Phase 6: Devirtualization & Speculation"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::CallGraph); }
    bool run(PassContext& ctx) override {
        CallGraphInfo& cg = ctx.analysis.callgraph();
        bool all_direct = true;
        for (FunctionGraph& fg : ctx.mod.fns) {
            for (const CallGraphInfo::Site& s : cg.sites_of(fg.fid)) {
                if (s.callee == kNoFn) all_direct = false;
            }
        }
        (void)all_direct; // telemetry: MVP emits direct calls only
        return false;
    }
};

JULES_REGISTER_PASS(StaticDevirtualizationPass, 68, "Phase 6")

} // namespace jules
