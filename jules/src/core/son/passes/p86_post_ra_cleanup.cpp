// Pass 86 — PostRACleanup (Phase 8)
//
// Post-allocation repair on the MIR stream: store-then-load of the same
// slot collapses into a register move; slot-immediate materializations fold
// into register immediates; self-moves die. This is where real register
// copies (the MIR "copy" set CopyPropagation's SoN counterpart works on)
// get eliminated.
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

namespace jules {

class PostRACleanupPass : public Pass {
public:
    const char* name() const override { return "PostRACleanup"; }
    int order() const override { return 86; }
    const char* phase_name() const override { return "Phase 8: Lowering & Machine Optimization"; }
    Stage stage() const override { return Stage::Linear; }
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        bool changed = false;
        for (LFunction& lf : ctx.lin->fns)
            changed |= x64_post_ra_cleanup(lf);
        return changed;
    }
};

JULES_REGISTER_PASS(PostRACleanupPass, 86, "Phase 8")

} // namespace jules
