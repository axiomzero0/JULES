// Pass 85 — RegisterAllocation (Phase 8)
//
// MVP policy: spill-everywhere — every virtual value gets a dedicated frame
// slot; the emitter loads operands into scratch registers per operation.
// This is correctness-first (deterministic, trivially correct); the
// linear-scan allocator (JIT) / graph coloring (AOT quality) upgrade path is
// documented in docs/architecture.md and slots into this same pass boundary.
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

namespace jules {

class RegisterAllocationPass : public Pass {
public:
    const char* name() const override { return "RegisterAllocation"; }
    int order() const override { return 85; }
    const char* phase_name() const override { return "Phase 8: Lowering & Machine Optimization"; }
    Stage stage() const override { return Stage::Linear; }
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        bool changed = false;
        for (LFunction& lf : ctx.lin->fns) {
            if (lf.slot_count > 0) changed = true;
            x64_allocate_frame(lf);
        }
        return changed;
    }
};

JULES_REGISTER_PASS(RegisterAllocationPass, 85, "Phase 8")

} // namespace jules
