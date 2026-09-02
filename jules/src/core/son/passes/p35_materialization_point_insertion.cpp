// Pass 35 — MaterializationPointInsertion (Phase 3)
//
// PURPOSE: Lazy materialization points for PEA (JIT).
// STATUS: SCAFFOLD — not yet implemented. This file is the declared contract
// (ordering, modes, analyses, kill switch, telemetry). It runs as a
// check-only no-op and never claims a transformation it did not perform.
// The design notes below describe the intended mechanism.
//
// DESIGN NOTES:
//   * on non-escaping paths, keep the object virtual
//   * emit materialization recipes into deopt metadata (pass 89)
#include "core/son/passes/pass_utils.h"

namespace jules {

class MaterializationPointInsertionPass : public Pass {
public:
    const char* name() const override { return "MaterializationPointInsertion"; }
    int order() const override { return 35; }
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

JULES_REGISTER_PASS(MaterializationPointInsertionPass, 35, "Phase 3")

} // namespace jules
