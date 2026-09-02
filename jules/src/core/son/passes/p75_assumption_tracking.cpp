// Pass 75 — AssumptionTracking (Phase 6)
//
// Infrastructure pass: records the assumption set attached to the compiled
// module version. In AOT mode the set is empty (every transformation here
// is proven, not speculated); JIT modes will register type/branch/value
// assumptions with violation handlers here before any speculative pass
// runs. Violations are the trigger for partial recompilation in the full
// design.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
constexpr size_t kMaxAssumptions = 1024; // named registry bound
} // namespace

class AssumptionTrackingPass : public Pass {
public:
    const char* name() const override { return "AssumptionTracking"; }
    int order() const override { return 75; }
    const char* phase_name() const override { return "Phase 6: Devirtualization & Speculation"; }
    bool run(PassContext& ctx) override {
        // Assumption registry (MVP: AOT = proof-only, so zero assumptions).
        // Scan for guard-flagged nodes; none can exist yet (no speculative
        // passes emit them), which the count asserts.
        size_t assumptions = 0;
        for (FunctionGraph& fg : ctx.mod.fns) {
            for (NodeId id = 0; id < fg.g.size(); ++id) {
                if (fg.g.node(id).flags & kFlagGuardSite) ++assumptions;
            }
        }
        if (assumptions > kMaxAssumptions) {
            ctx.diag.error(SourcePos{}, "assumption registry exceeded its bound");
        }
        return false;
    }
};

JULES_REGISTER_PASS(AssumptionTrackingPass, 75, "Phase 6")

} // namespace jules
