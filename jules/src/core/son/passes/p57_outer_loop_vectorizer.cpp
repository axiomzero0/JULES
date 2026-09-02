// Pass 57 — OuterLoopVectorizer (Phase 5)
//
// PURPOSE: Vectorize outer nest when inner is short.
// STATUS: SCAFFOLD — not yet implemented. This file is the declared contract
// (ordering, modes, analyses, kill switch, telemetry). It runs as a
// check-only no-op and never claims a transformation it did not perform.
// The design notes below describe the intended mechanism.
//
// DESIGN NOTES:
//   * when inner nest is short, vectorize the outer instead
#include "core/son/passes/pass_utils.h"

namespace jules {

class OuterLoopVectorizerPass : public Pass {
public:
    const char* name() const override { return "OuterLoopVectorizer"; }
    int order() const override { return 57; }
    const char* phase_name() const override { return "Phase 5"; }
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

JULES_REGISTER_PASS(OuterLoopVectorizerPass, 57, "Phase 5")

} // namespace jules
