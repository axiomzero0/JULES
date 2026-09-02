// Pass 50 — TailRecursionElimination (Phase 4)
//
// Detects self tail calls (Return value is a Call to the same function, the
// call is the final effect in its block, and the return sits in that same
// block) and marks them kFlagTailCall. The x86-64 emitter honors the flag by
// jumping to the function entry instead of calling (no stack growth).
// Runs before inlining/loop opts per the catalog ordering contract.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class TailCallMarker {
public:
    TailCallMarker(Graph& g, FnId fid) : g_(g), fid_(fid) {}

    bool run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op != Op::Return || n.n_in != 3) continue;
            NodeId val = n.in[2];
            const Node& vn = g_.node(val);
            if (vn.op != Op::Call || vn.aux != fid_) continue;
            // the call must be the last memory effect and share the return's block
            if (n.in[1] != val) continue;         // something happened after the call
            if (n.in[0] != vn.in[0]) continue;    // control drifted
            g_.node(val).flags |= kFlagTailCall;
            changed_ = true;
        }
        return changed_;
    }

private:
    Graph& g_;
    FnId fid_;
    bool changed_ = false;
};
} // namespace

class TailRecursionEliminationPass : public Pass {
public:
    const char* name() const override { return "TailRecursionElimination"; }
    int order() const override { return 50; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::CallGraph); }
    bool run(PassContext& ctx) override {
        (void)ctx.analysis.callgraph();
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            TailCallMarker m(fg.g, fg.fid);
            changed |= m.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(TailRecursionEliminationPass, 50, "Phase 4")

} // namespace jules
