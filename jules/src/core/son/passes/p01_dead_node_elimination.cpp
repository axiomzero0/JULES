// Pass 1 — DeadNodeElimination (Phase 0: Frontend Residue Cleanup)
//
// Mark & sweep from liveness roots:
//   * Stop (returns) via full input closure
//   * every Call in a control-reachable block (calls may have side effects
//     and are never DCE'd on value-liveness alone)
// Unmarked nodes are killed. First pass to shrink the graph after lowering.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class DeadCodeElim {
public:
    explicit DeadCodeElim(Graph& g) : g_(g) {}

    bool run() {
        mark_roots();
        sweep();
        return changed_;
    }

private:
    void mark(NodeId n) {
        if (n == kNoNode || live_.contains(n)) return;
        const Node& nd = g_.node(n);
        if (nd.op == Op::Dead) return;
        live_.insert(n, true);
        work_.push_back(n);
    }

    void mark_roots() {
        mark(g_.stop());
        // Control-reachable calls (side effects are roots).
        FlatMap<NodeId, std::vector<NodeId>> succs = control_succs();
        std::vector<NodeId> stack{g_.start()};
        FlatMap<NodeId, bool> reach;
        reach.insert(g_.start(), true);
        while (!stack.empty()) {
            NodeId b = stack.back();
            stack.pop_back();
            // calls pinned to this block are live roots
            for (NodeId u : g_.uses_of(b)) {
                const Node& un = g_.node(u);
                if (un.op == Op::Call && un.in[0] == b) mark(u);
            }
            if (const std::vector<NodeId>* ss = succs.find(b)) {
                for (NodeId s : *ss) {
                    if (!reach.contains(s)) {
                        reach.insert(s, true);
                        stack.push_back(s);
                    }
                }
            }
        }
        // drain: full input closure from all roots
        while (!work_.empty()) {
            NodeId n = work_.back();
            work_.pop_back();
            const Node& nd = g_.node(n);
            for (u8 i = 0; i < nd.n_in; ++i) mark(nd.in[i]);
        }
    }

    FlatMap<NodeId, std::vector<NodeId>> control_succs() {
        FlatMap<NodeId, std::vector<NodeId>> succs;
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op == Op::Dead) continue;
            switch (n.op) {
                case Op::If:
                    for (NodeId p : g_.uses_of(id))
                        if (g_.node(p).op == Op::IfTrue || g_.node(p).op == Op::IfFalse)
                            succs[n.in[0]].push_back(p);
                    break;
                case Op::Jump:
                    succs[n.in[0]].push_back(id);
                    break;
                case Op::Region:
                    for (u8 i = 0; i < n.n_in; ++i) succs[n.in[i]].push_back(id);
                    break;
                default: break;
            }
        }
        return succs;
    }

    void sweep() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            if (g_.node(id).op == Op::Dead) continue;
            if (!live_.contains(id)) {
                g_.kill(id);
                changed_ = true;
            }
        }
    }

    Graph& g_;
    FlatMap<NodeId, bool> live_;
    std::vector<NodeId> work_;
    bool changed_ = false;
};
} // namespace

class DeadNodeEliminationPass : public Pass {
public:
    const char* name() const override { return "DeadNodeElimination"; }
    int order() const override { return 1; }
    const char* phase_name() const override { return "Phase 0: Frontend Residue Cleanup"; }
    bool parallelizable() const override { return true; }
    AnalysisMask invalidated() const override {
        return static_cast<AnalysisMask>(AnalysisKind::Dominators | AnalysisKind::LoopInfo);
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            DeadCodeElim dce(fg.g);
            changed |= dce.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(DeadNodeEliminationPass, 1, "Phase 0")

} // namespace jules
