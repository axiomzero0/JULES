// Pass 22 — StoreToLoadForwarding (Phase 2: Memory Optimization)
//
// Forwards the stored value directly to subsequent loads of the same base
// (no memory round-trip). Uses MemDep::reaching_store with phi meets; the
// forwarded store dominates the load by construction of the chain.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class StoreToLoadForwarder {
public:
    StoreToLoadForwarder(Graph& g, MemDep& md) : g_(g), md_(md) {}

    bool run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op != Op::Load) continue;
            NodeId def = kNoNode;
            MemDepResult r = md_.reaching_store(id, def);
            if (r == MemDepResult::Def && def != kNoNode) {
                NodeId val = g_.node(def).in[3];
                if (g_.node(val).op != Op::Dead && val != id) {
                    g_.replace_all_uses(id, val);
                    g_.kill(id);
                    changed_ = true;
                }
            }
        }
        return changed_;
    }

private:
    Graph& g_;
    MemDep& md_;
    bool changed_ = false;
};
} // namespace

class StoreToLoadForwardingPass : public Pass {
public:
    const char* name() const override { return "StoreToLoadForwarding"; }
    int order() const override { return 22; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    AnalysisMask required() const override {
        return AnalysisKind::AliasInfo | AnalysisKind::MemDep;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            StoreToLoadForwarder f(fg.g, ctx.analysis.memdep(fg));
            changed |= f.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(StoreToLoadForwardingPass, 22, "Phase 2")

} // namespace jules
