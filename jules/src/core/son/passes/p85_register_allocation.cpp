// Pass 85 — RegisterAllocation (Phase 8)
//
// Level-dispatched allocator (spec §8):
//   -O0/-Og  "simple": spill-everywhere frame layout (every value in a
//            dedicated frame slot — deterministic, debug-friendly).
//   -O1+     linear scan over slot live ranges with callee-saved /
//            caller-saved / XMM pools (x64_ra.cpp). Values that find no
//            register keep their memory operands, so spilling degenerates
//            to the spill-everywhere behavior for that value.
// The budgeted-IRC grade (graph coloring with move coalescing) is the
// documented upgrade path; linear scan is the honest current mechanism.
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
        LevelBudgets b = level_budgets(ctx.opts.level);
        bool changed = false;
        for (LFunction& lf : ctx.lin->fns) {
            const FunctionGraph* fg = ctx.mod.find_fn(lf.fid);
            changed |= x64_allocate_registers(lf, fg ? &fg->g : nullptr,
                                              b.ra_registers,
                                              ctx.opts.level == OptLevel::O3,
                                              b.size_biased);
        }
        return changed;
    }
};

JULES_REGISTER_PASS(RegisterAllocationPass, 85, "Phase 8")

} // namespace jules
