// Pass 7 — PhiSimplification (Phase 0: Frontend Residue Cleanup)
//
// Pre-GVN phi cleanup so identical computations don't pollute value hashing:
//   * single-input phi (degenerate region) -> its input
//   * all inputs identical (self-references ignored) -> that input
//   * single-predecessor Region -> repin contents to the predecessor and
//     remove the merge
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class PhiSimplifier {
public:
    explicit PhiSimplifier(Graph& g) : g_(g) {}

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

    bool replace(NodeId from, NodeId to) {
        if (from == to || to == kNoNode) return false;
        // self-referential phis: strip self inputs first (handled by caller)
        g_.replace_all_uses(from, to);
        g_.kill(from);
        return true;
    }

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op == Op::Dead) return false;

        if (n.op == Op::Phi) {
            NodeId region = n.in[0];
            const Node& r = g_.node(region);
            if (r.op == Op::Dead) return false;

            // Single input phi.
            if (n.n_in == 2) return replace(id, n.in[1]);

            // All inputs identical (ignoring self-references).
            NodeId common = kNoNode;
            bool all_same = true;
            for (u8 i = 1; i < n.n_in; ++i) {
                NodeId v = n.in[i];
                if (v == id) continue; // self reference: pass-through
                if (common == kNoNode) common = v;
                else if (v != common) { all_same = false; break; }
            }
            if (all_same && common != kNoNode) {
                // strip self references before replacing
                for (u8 i = 1; i < n.n_in; ++i)
                    if (n.in[i] == id) n.in[i] = common;
                g_.mark_uses_dirty();
                return replace(id, common);
            }
            return false;
        }

        if (n.op == Op::Region) {
            if (n.n_in != 1) return false;
            NodeId pred = n.in[0];
            if (pred == id) return false; // self-loop
            const Node& p = g_.node(pred);
            if (p.op == Op::Dead) return false;
            // repin everything pinned here to the predecessor
            const SmallVec<NodeId, 4> users = g_.uses_of(id);
            for (NodeId u : users) {
                Node& un = g_.node(u);
                if (un.op == Op::Phi && un.in[0] == id) {
                    // phi at degenerate region -> its single input
                    g_.replace_all_uses(u, un.in[1]);
                    g_.kill(u);
                } else if (un.op == Op::Region) {
                    // u references id as a PREDECESSOR (any input slot,
                    // including in[0]): splice the degenerate merge out of
                    // u's edge list in place. Missing this left the killed
                    // region in successor pred lists — loop backedges died,
                    // and SCCP's unreachable-pred trim then decapitated the
                    // loop. In-place splicing preserves phi input alignment
                    // at u (the value for that edge is unchanged).
                    for (u8 j = 0; j < un.n_in; ++j)
                        if (un.in[j] == id) g_.set_input(u, j, pred);
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

class PhiSimplificationPass : public Pass {
public:
    const char* name() const override { return "PhiSimplification"; }
    int order() const override { return 7; }
    const char* phase_name() const override { return "Phase 0: Frontend Residue Cleanup"; }
    bool parallelizable() const override { return true; }
    AnalysisMask invalidated() const override {
        return static_cast<AnalysisMask>(AnalysisKind::Dominators | AnalysisKind::LoopInfo);
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            PhiSimplifier s(fg.g);
            changed |= s.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(PhiSimplificationPass, 7, "Phase 0")

} // namespace jules
