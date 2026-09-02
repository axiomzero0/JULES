// Pass 87 — MachinePeephole (Phase 8)
//
// Target-specific combining post-RA:
//   * `cmp reg, $0` -> `test reg, reg`
//   * self-moves (`movq %rax, %rax`) removed
//   * redundant instruction stream pruning (Nop elimination)
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

namespace jules {

class MachinePeepholePass : public Pass {
public:
    const char* name() const override { return "MachinePeephole"; }
    int order() const override { return 87; }
    const char* phase_name() const override { return "Phase 8: Lowering & Machine Optimization"; }
    Stage stage() const override { return Stage::Linear; }
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        bool changed = false;
        for (LFunction& lf : ctx.lin->fns)
            changed |= x64_machine_peephole(lf);
        return changed;
    }
};

JULES_REGISTER_PASS(MachinePeepholePass, 87, "Phase 8")

} // namespace jules
