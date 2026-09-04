// Pass 88 — MachineLICM (Phase 8)
//
// Machine-level loop optimization, post-RA:
//   * LOOP ROTATION: the emitted while-loop shape takes two branches per
//     iteration (the guard's taken jcc into the body + the unconditional
//     latch jump back to the head). Rotation moves the guard to the bottom
//     of the loop and replaces the latch with the guard's backedge:
//         [L: cond; jcc body][exit code][body; jmp L]
//       -> [L: jmp check][body][check: cond; jcc body][exit code]
//     Every entry path re-evaluates the guard before the body (semantics
//     preserved); the per-iteration cost drops to ONE taken branch — the
//     shape every production compiler's loop rotation pass produces.
//   * LOOP-INVARIANT HOISTING (machine view): the isel constant pool
//     materializes f64/f32 constants at first use, which for loop-carried
//     constants sits INSIDE the loop body. Hoisting moves the
//     [movq $bits, %rax][movq %rax, %xmmN] pair out of the backedge region
//     (once per loop entry instead of once per iteration).
// The SoN-level LICM (pass 40) already hoists invariant computations the
// frontend can see; these two transforms are only visible after isel/RA.
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

namespace jules {

class MachineLICMPass : public Pass {
public:
    const char* name() const override { return "MachineLICM"; }
    int order() const override { return 88; }
    const char* phase_name() const override { return "Phase 8: Lowering & Machine Optimization"; }
    Stage stage() const override { return Stage::Linear; }
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        bool changed = false;
        for (LFunction& lf : ctx.lin->fns) {
            changed |= x64_loop_rotate(lf);          // rotation first: regions
                                                    // become the rotated spans
            changed |= x64_hoist_loop_constants(lf); // then hoist out of them
            changed |= x64_loop_entry_fallthrough(lf); // entry/leaf-exit edges
                                                    // become fallthrough
        }
        return changed;
    }
};

JULES_REGISTER_PASS(MachineLICMPass, 88, "Phase 8")

} // namespace jules
