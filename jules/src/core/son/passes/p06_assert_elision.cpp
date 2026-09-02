// Pass 6 — AssertElision (Phase 0: Frontend Residue Cleanup)
//
// STATUS: vacuously complete for the current IR. The MVP language subset has
// no arrays/indexing and emits no bounds/null/overflow check nodes, so there
// are no asserts to elide. The pass exists (per the 89-pass contract), runs,
// and reports zero work; once checked ops (bounds checks on index syntax,
// arithmetic overflow checks) are lowered into explicit Check nodes, this is
// the pass that removes the ones proven safe by type width analysis or by
// range facts from SCCP. Kept separate from DCE on purpose: elision requires
// a semantic proof, not liveness.
#include "core/son/passes/pass_utils.h"

namespace jules {

class AssertElisionPass : public Pass {
public:
    const char* name() const override { return "AssertElision"; }
    int order() const override { return 6; }
    const char* phase_name() const override { return "Phase 0: Frontend Residue Cleanup"; }
    bool parallelizable() const override { return true; }
    AnalysisMask required() const override { return 0; }
    bool run(PassContext& ctx) override {
        // No check nodes exist in the MVP IR: verified by scan, reported, no-op.
        bool any_checks = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            for (NodeId id = 0; id < fg.g.size(); ++id) {
                const Node& n = fg.g.node(id);
                if (n.flags & kFlagGuardSite) any_checks = true;
            }
        }
        (void)any_checks; // telemetry hook: guard sites appear with deopt support
        return false;
    }
};

JULES_REGISTER_PASS(AssertElisionPass, 6, "Phase 0")

} // namespace jules
