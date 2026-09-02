// Pass 15 — NarrowingTransform (Phase 1)
//
// PURPOSE: Demote ops to narrower types when consumers allow.
// STATUS: SCAFFOLD — not yet implemented. This file is the declared contract
// (ordering, modes, analyses, kill switch, telemetry). It runs as a
// check-only no-op and never claims a transformation it did not perform.
// The design notes below describe the intended mechanism.
//
// DESIGN NOTES:
//   * track trunc users feeding narrow consumers; rewrite op width
//   * requires the narrow-type legality check against all uses
#include "core/son/passes/pass_utils.h"

namespace jules {

class NarrowingTransformPass : public Pass {
public:
    const char* name() const override { return "NarrowingTransform"; }
    int order() const override { return 15; }
    const char* phase_name() const override { return "Phase 1"; }
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

JULES_REGISTER_PASS(NarrowingTransformPass, 15, "Phase 1")

} // namespace jules
