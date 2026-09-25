// Pass 71 — GuardInsertion (Phase 6: Devirtualization & Speculation)
//
// The speculation family's guard sites are created by the PE/PD ladder
// (pass 91: PGO-driven Const/Range assumption sets protected by runtime
// guards whose failure transfers to the less-specialized rung). Those
// guards live in the SoN as If nodes flagged kFlagGuardSite. This pass
// runs at the Linear stage and materializes every flagged guard into the
// linear module's guard-site table — the table pass 89's deopt manifest
// emits. Without it the manifest's guard list was structurally always
// empty even when ladders were real.
//
// Guard kinds are derived from the guard's comparison:
//   Cmp Eq  -> "const"   (param == hot value; the p91 Const rung)
//   Cmp Ge  -> "range_lo"(param >= lo; the p91 Range rung, lower hinge)
//   Cmp Le  -> "range_hi"(param <= hi; the p91 Range rung, upper hinge)
// Any other shape is recorded as "check" (reserved for future guard
// producers).
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

namespace jules {

class GuardInsertionPass : public Pass {
public:
    const char* name() const override { return "GuardInsertion"; }
    int order() const override { return 71; }
    const char* phase_name() const override { return "Phase 6: Devirtualization & Speculation"; }
    Stage stage() const override { return Stage::Linear; }
    ModeMask modes() const override { return kModeAll; } // runs wherever guards exist
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        u32 before = static_cast<u32>(ctx.lin->guard_sites.size());
        for (LFunction& lf : ctx.lin->fns) {
            const FunctionGraph* fg = ctx.mod.find_fn(lf.fid);
            if (!fg) continue;
            for (LBlock& b : lf.blocks) {
                if (b.terminator_if == kNoNode) continue;
                const Node& gif = fg->g.node(b.terminator_if);
                if ((gif.flags & kFlagGuardSite) == 0) continue;
                const char* kind = "check";
                if (gif.n_in >= 2) {
                    const Node& cond = fg->g.node(gif.in[1]);
                    if (cond.op == Op::Cmp) {
                        switch (static_cast<CmpOp>(cond.sub)) {
                            case CmpOp::Eq: kind = "const"; break;
                            case CmpOp::Ge: kind = "range_lo"; break;
                            case CmpOp::Le: kind = "range_hi"; break;
                            default: break;
                        }
                    }
                }
                ctx.lin->guard_sites.push_back(
                    LinearModule::GuardSite{lf.fid, b.index, kind});
            }
        }
        return ctx.lin->guard_sites.size() != before;
    }
};

JULES_REGISTER_PASS(GuardInsertionPass, 71, "Phase 6")

} // namespace jules
