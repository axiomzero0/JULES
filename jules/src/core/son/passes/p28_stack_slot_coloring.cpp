// Pass 28 — StackSlotColoring (Phase 2)
//
// Merge non-overlapping stack slots. Pass 85's finalize() already colors
// memory-resident slots by their live HULL (the machine-level coloring
// the 2026-09-18 audit recorded as "delegated"); THIS pass is the
// catalog entry performing the gen-precise refinement over the FINAL
// machine stream (execution slot 885: after the register allocator and
// the machine LICM/peephole tier, before the deopt manifest reads the
// frame layout):
//
//   * slots whose exact generation sub-intervals (per-linear-redefinition
//     liveness — the same gens the RA's coalescing used) are provably
//     disjoint share one rbp offset even when their hulls overlap
//     through a loop backedge (loop phi slot vs inner temporary);
//   * sharing is realized by UNIFYING SLOT IDS: aliased slots become the
//     color representative in every operand, so no downstream consumer
//     (pass 89 manifest, pass 92 window liveness) ever models two ids at
//     one physical offset;
//   * addr-taken slots never share (LeaSlot identity is allocation
//     identity); wide 16-byte vector slots keep their own aligned area;
//   * the frame is re-laid out above the callee-save area and the
//     FrameSub immediate + frame_size are re-patched.
//
// Telemetry: lf.ra_colored (offsets shared by disjoint-range slots).
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

namespace jules {

class StackSlotColoringPass : public Pass {
public:
    const char* name() const override { return "StackSlotColoring"; }
    int order() const override { return 28; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    Stage stage() const override { return Stage::Linear; }
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        bool changed = false;
        for (LFunction& lf : ctx.lin->fns)
            changed |= x64_slot_recolor(lf) > 0;
        return changed;
    }
};

JULES_REGISTER_PASS(StackSlotColoringPass, 28, "Phase 2")

} // namespace jules
