// Pass 44 — LoopPeeling (Phase 4)
//
// PURPOSE: Extract first/last iterations.
// STATUS: SCAFFOLD — not yet implemented. This file is the declared contract
// (ordering, modes, analyses, kill switch, telemetry). It runs as a
// check-only no-op and never claims a transformation it did not perform.
// The design notes below describe the intended mechanism.
//
// DESIGN NOTES:
//   * clone the first iteration out of the loop; peel-phi alignment
#include "core/son/passes/pass_utils.h"

namespace jules {

class LoopPeelingPass : public Pass {
public:
    const char* name() const override { return "LoopPeeling"; }
    int order() const override { return 44; }
    const char* phase_name() const override { return "Phase 4"; }
    ModeMask modes() const override { return kModeAll; }
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

JULES_REGISTER_PASS(LoopPeelingPass, 44, "Phase 4")

} // namespace jules
