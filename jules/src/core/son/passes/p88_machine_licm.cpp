// Pass 88 — MachineLICM (Phase 8)
//
// Hoists invariant machine instructions visible only post-isel (constant
// materializations repeated across loop-body blocks). STATUS: simplified —
// the SoN-level LICM (pass 40) already hoists the invariant computations the
// frontend can produce; what remains here is the machine-level rematerial-
// ization scan, which requires machine-loop metadata the current MIR does
// not yet track. Documented design:
//   * build machine loops from block backedges in the MIR
//   * hoist `movq $imm, %r` whose destination is dead outside the loop body
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
        bool any_loops = false;
        for (const LFunction& lf : ctx.lin->fns) {
            // machine backedge detection (Jmp/Jcc targeting an earlier label)
            FlatMap<int, int> label_site;
            for (size_t i = 0; i < lf.code.size(); ++i) {
                const Inst& in = lf.code[i];
                if (in.op == IOp::Label) label_site.insert(in.a.label, static_cast<int>(i));
                if (in.op == IOp::Jmp || in.op == IOp::Jcc) {
                    const int* site = label_site.find(in.a.label);
                    if (site && *site < static_cast<int>(i)) any_loops = true; // backedge
                }
            }
        }
        return any_loops; // telemetry: machine loops detected, hoisting = next step
    }
};

JULES_REGISTER_PASS(MachineLICMPass, 88, "Phase 8")

} // namespace jules
