// Pass 77 — AlwaysInlineEnforcement (Phase 7)
//
// Force-inlines #[inline(always)] functions and trivially small bodies
// (policy: node estimate <= kTrivialBodyNodes) at every call site.
#include "core/son/passes/inline.h"

namespace jules {

namespace {
constexpr u32 kTrivialBodyNodes = 10; // named policy constant
} // namespace

class AlwaysInlineEnforcementPass : public Pass {
public:
    const char* name() const override { return "AlwaysInlineEnforcement"; }
    int order() const override { return 77; }
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
                    const FunctionGraph* callee = ctx.mod.find_fn(n.aux);
                    if (!callee) continue;
                    if (callee->no_inline) continue;
                    bool force = callee->always_inline || callee->node_estimate <= kTrivialBodyNodes;
                    if (!force) continue;
                    if (!inline_recursion_ok(caller.fid, callee->fid)) continue;
                    if (inline_call(caller, id, *const_cast<FunctionGraph*>(callee))) {
                        this_round = true;
                        break; // node ids shifted roles; rescan this function
                    }
                }
            }
            changed |= this_round;
            if (!this_round) break;
        }
        return changed;
    }

private:
    static constexpr u32 kMaxRounds = 16; // bounded expansion
};

JULES_REGISTER_PASS(AlwaysInlineEnforcementPass, 77, "Phase 7")

} // namespace jules
