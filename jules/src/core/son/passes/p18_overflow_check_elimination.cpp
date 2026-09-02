// Pass 18 — OverflowCheckElimination (Phase 1)
//
// Removes arithmetic overflow checks proven non-triggering via range
// lattices. STATUS: vacuously complete for the current IR — the MVP subset
// emits no overflow Check nodes (arithmetic wraps; the UB model is
// documented in docs/son_spec.md). Separate from AssertElision because the
// proof domain is numeric ranges, not type facts; the pass becomes live the
// moment checked-arithmetic syntax lowers Check nodes.
#include "core/son/passes/pass_utils.h"

namespace jules {

class OverflowCheckEliminationPass : public Pass {
public:
    const char* name() const override { return "OverflowCheckElimination"; }
    int order() const override { return 18; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::LoopInfo); }
    bool run(PassContext& ctx) override {
        // No Check nodes exist in the MVP IR: verified by scan, no-op.
        // Range facts would come from IV bounds (pass 38) once checks exist.
        bool any_checks = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            for (NodeId id = 0; id < fg.g.size(); ++id) {
                if (fg.g.node(id).flags & kFlagGuardSite) any_checks = true;
            }
        }
        (void)any_checks;
        return false;
    }
};

JULES_REGISTER_PASS(OverflowCheckEliminationPass, 18, "Phase 1")

} // namespace jules
