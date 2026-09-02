// Pass 10 — RedundantPhiElimination (Phase 1)
//
// Post-GVN phi cleanup:
//   * all inputs identical (self-references ignored) -> that input
//   * single-input phi -> input
//   * degenerate (single-pred) regions collapse, repinning their contents
// GVN's phi hashing can leave these behind after value replacement.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class RedundantPhiEliminator {
public:
    explicit RedundantPhiEliminator(Graph& g) : g_(g) {}

    bool run() {
        bool changed = false;
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (NodeId id = 0; id < g_.size(); ++id) this_round |= visit(id);
            changed |= this_round;
            if (!this_round) break;
        }
        return changed;
    }

private:
    static constexpr u32 kMaxRounds = 8;

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op == Op::Dead) return false;

        if (n.op == Op::Phi) {
            if (n.n_in == 2) {
                g_.replace_all_uses(id, n.in[1]);
                g_.kill(id);
                return true;
            }
            NodeId common = kNoNode;
            bool all_same = true;
            for (u8 i = 1; i < n.n_in; ++i) {
                NodeId v = n.in[i];
                if (v == id) continue;
                if (common == kNoNode) common = v;
                else if (v != common) { all_same = false; break; }
            }
            if (all_same && common != kNoNode) {
                for (u8 i = 1; i < n.n_in; ++i)
                    if (n.in[i] == id) n.in[i] = common;
                g_.mark_uses_dirty();
                g_.replace_all_uses(id, common);
                g_.kill(id);
                return true;
            }
            return false;
        }

        if (n.op == Op::Region && n.n_in == 1 && n.in[0] != id) {
            NodeId pred = n.in[0];
            if (g_.node(pred).op == Op::Dead) return false;
            const SmallVec<NodeId, 4> users = g_.uses_of(id);
            for (NodeId u : users) {
                Node& un = g_.node(u);
                if (un.op == Op::Phi && un.in[0] == id) {
                    g_.replace_all_uses(u, un.in[1]);
                    g_.kill(u);
                } else if (un.in[0] == id) {
                    g_.set_input(u, 0, pred);
                }
            }
            g_.kill(id);
            return true;
        }
        return false;
    }

    Graph& g_;
};
} // namespace

class RedundantPhiEliminationPass : public Pass {
public:
    const char* name() const override { return "RedundantPhiElimination"; }
    int order() const override { return 10; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    AnalysisMask invalidated() const override {
        return static_cast<AnalysisMask>(AnalysisKind::Dominators | AnalysisKind::LoopInfo);
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            RedundantPhiEliminator e(fg.g);
            changed |= e.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(RedundantPhiEliminationPass, 10, "Phase 1")

} // namespace jules
