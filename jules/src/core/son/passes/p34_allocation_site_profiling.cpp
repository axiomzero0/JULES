// Pass 34 — AllocationSiteProfiling (Phase 3)
//
// PURPOSE: Per-site escape behavior instrumentation (JIT).
// STATUS: SCAFFOLD — not yet implemented. This file is the declared contract
// (ordering, modes, analyses, kill switch, telemetry). It runs as a
// check-only no-op and never claims a transformation it did not perform.
// The design notes below describe the intended mechanism.
//
// DESIGN NOTES:
//   * instrument alloc sites with counters + escape flags
//   * profile format shared with pass 69's type profiles
#include "core/son/passes/pass_utils.h"

namespace jules {

class AllocationSiteProfilingPass : public Pass {
public:
    const char* name() const override { return "AllocationSiteProfiling"; }
    int order() const override { return 34; }
    const char* phase_name() const override { return "Phase 3"; }
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

JULES_REGISTER_PASS(AllocationSiteProfilingPass, 34, "Phase 3")

} // namespace jules
