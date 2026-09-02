// Pass 72 — GuardHoisting (Phase 6)
//
// PURPOSE: Move guards to cold points (JIT).
// STATUS: SCAFFOLD — not yet implemented. This file is the declared contract
// (ordering, modes, analyses, kill switch, telemetry). It runs as a
// check-only no-op and never claims a transformation it did not perform.
// The design notes below describe the intended mechanism.
//
// DESIGN NOTES:
//   * LICM over guard nodes once they exist in the IR
#include "core/son/passes/pass_utils.h"

namespace jules {

class GuardHoistingPass : public Pass {
public:
    const char* name() const override { return "GuardHoisting"; }
    int order() const override { return 72; }
    const char* phase_name() const override { return "Phase 6"; }
    ModeMask modes() const override { return kModeJitBaseline | kModeJitOptimizing; }
    bool run(PassContext& ctx) override {
        // Scaffold: validate preconditions, record telemetry, change nothing.
        bool preconditions = true;
        for (FunctionGraph& fg : ctx.mod.fns) {
            if (fg.g.live_count() == 0) preconditions = false;
        }
        (void)preconditions; // telemetry hook
        return false;
    }
};

JULES_REGISTER_PASS(GuardHoistingPass, 72, "Phase 6")

} // namespace jules
