// Pass 80 — ArgumentSpecialization (Phase 7)
//
// Calls whose arguments are ALL constants get priority inlining: the cloned
// body folds to constants immediately (the post-inline cleanup re-runs
// ConstantFolding/ComptimeResidueFold/SROA), which is the callee-side
// optimization exposure the catalog describes.
#include "core/son/passes/inline.h"

namespace jules {

namespace {
constexpr u32 kSpecializedMaxNodes = 40; // size guard for full specialization
} // namespace

class ArgumentSpecializationPass : public Pass {
public:
    const char* name() const override { return "ArgumentSpecialization"; }
    int order() const override { return 80; }
    const char* phase_name() const override { return "Phase 7: Inlining & Interprocedural"; }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo |
               AnalysisKind::AliasInfo | AnalysisKind::MemDep | AnalysisKind::CallGraph;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (FunctionGraph& caller : ctx.mod.fns) {
                for (NodeId id = 0; id < caller.g.size(); ++id) {
                    const Node& n = caller.g.node(id);
                    if (n.op != Op::Call || n.aux == kFnPrint || n.aux == kFnFree) continue;
                    if (n.n_in <= 2) continue;
                    const FunctionGraph* callee = ctx.mod.find_fn(n.aux);
                    if (!callee || callee->no_inline) continue;
                    if (callee->node_estimate > kSpecializedMaxNodes) continue;
                    if (!inline_recursion_ok(caller.fid, callee->fid)) continue;
                    bool all_const = true;
                    for (u8 i = 2; i < n.n_in; ++i)
                        if (caller.g.node(n.in[i]).op != Op::Const) { all_const = false; break; }
                    if (!all_const) continue;

                    if (inline_call(caller, id, *const_cast<FunctionGraph*>(callee))) {
                        this_round = true;
                        break;
                    }
                }
            }
            changed |= this_round;
            if (!this_round) break;
        }
        return changed;
    }

private:
    static constexpr u32 kMaxRounds = 8;
};

JULES_REGISTER_PASS(ArgumentSpecializationPass, 80, "Phase 7")

} // namespace jules
