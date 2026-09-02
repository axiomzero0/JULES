// Pass 84 — InstructionSelection (Phase 8)
//
// Pattern-matches the linear SoN schedule into x86-64 MIR: every value gets
// a virtual stack slot, operands load into scratch registers (rax/rcx/rdx),
// calls classify args into the SysV register sequences, tail calls (pass 50)
// become jumps to the entry label, print lowers to printf with per-type
// format strings in .rodata.
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

namespace jules {

class InstructionSelectionPass : public Pass {
public:
    const char* name() const override { return "InstructionSelection"; }
    int order() const override { return 84; }
    const char* phase_name() const override { return "Phase 8: Lowering & Machine Optimization"; }
    Stage stage() const override { return Stage::Linear; }
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        bool ok = true;
        for (size_t i = 0; i < ctx.lin->fns.size(); ++i) {
            LFunction& lf = ctx.lin->fns[i];
            FunctionGraph* fg = ctx.mod.find_fn(lf.fid);
            if (!fg) { ok = false; continue; }
            ok &= x64_select_instructions(lf, *fg, ctx.syms);
        }
        return ok;
    }
};

JULES_REGISTER_PASS(InstructionSelectionPass, 84, "Phase 8")

} // namespace jules
