// Pass 42 — LoopUnrolling (Phase 4)
//
// PURPOSE: Duplicate loop bodies (static heuristic).
// STATUS: SCAFFOLD — not yet implemented. This file is the declared contract
// (ordering, modes, analyses, kill switch, telemetry). It runs as a
// check-only no-op and never claims a transformation it did not perform.
// The design notes below describe the intended mechanism.
//
// DESIGN NOTES:
//   * duplicate the loop body subgraph k times, rewiring header phis
//   * guarded by loop classification (pass 39) + cost model
#include "core/son/passes/pass_utils.h"

namespace jules {

class LoopUnrollingPass : public Pass {
public:
    const char* name() const override { return "LoopUnrolling"; }
    int order() const override { return 42; }
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

JULES_REGISTER_PASS(LoopUnrollingPass, 42, "Phase 4")

} // namespace jules
