// Pass 9 — GlobalValueNumbering (Phase 1)
//
// Dominance-checked hash-consing CSE over the whole function:
//   * pure ops hashed ctrl-insensitively; a hit replaces the new node with
//     the dominating prior definition
//   * phis keyed by (region, inputs)
//   * loads keyed by (memory version, address): identical versions + address
//     provably yield the same value; dominance still enforced
// Preserves Dominators (pure data replacement, no control change) — the
// contract the scheduler relies on for subsequent passes.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class Gvn {
public:
    Gvn(Graph& g, DomTree& dom) : g_(g), dom_(dom) {}

    bool run() {
        for (NodeId b : dom_.rpo()) {
            // deterministic order: node id ascending within the block
            std::vector<NodeId> pinned;
            for (NodeId u : g_.uses_of(b))
                if (g_.node(u).in[0] == b) pinned.push_back(u);
            std::sort(pinned.begin(), pinned.end());
            for (NodeId n : pinned) visit(n);
        }
        return changed_;
    }

private:
    NodeId block_of(NodeId n) { return g_.node(n).in[0]; }

    void visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op == Op::Dead) return;

        if (n.op == Op::Phi) {
            u64 h = phi_hash(id);
            auto& bucket = phi_buckets_[h];
            for (NodeId cand : bucket) {
                if (cand == id || g_.node(cand).op == Op::Dead) continue;
                if (phi_equals(cand, id)) {
                    g_.replace_all_uses(id, cand);
                    g_.kill(id);
                    changed_ = true;
                    return;
                }
            }
            bucket.push_back(id);
            return;
        }

        if (is_pure_op(n.op) || n.op == Op::Load) {
            u64 h = g_.pure_hash(id);
            auto& bucket = buckets_[h];
            for (NodeId cand : bucket) {
                if (cand == id || g_.node(cand).op == Op::Dead) continue;
                if (!g_.pure_equals(cand, id)) continue;
                // loads: memory version must be identical (it is part of the
                // hash via in[1]; pure_equals compares in[1..] too).
                if (!dom_.dominates(block_of(cand), block_of(id))) continue;
                g_.replace_all_uses(id, cand);
                g_.kill(id);
                changed_ = true;
                return;
            }
            bucket.push_back(id);
        }
    }

    u64 phi_hash(NodeId id) {
        const Node& n = g_.node(id);
        u64 h = g_.pure_hash(id);
        h = hash_mix(h, n.in[0]); // region identity matters for phis
        return h;
    }

    bool phi_equals(NodeId a, NodeId b) {
        const Node& x = g_.node(a);
        const Node& y = g_.node(b);
        if (x.op != Op::Phi || y.op != Op::Phi) return false;
        if (x.in[0] != y.in[0] || x.n_in != y.n_in || x.ty != y.ty) return false;
        for (u8 i = 1; i < x.n_in; ++i)
            if (x.in[i] != y.in[i]) return false;
        return true;
    }

    Graph& g_;
    DomTree& dom_;
    FlatMap<u64, std::vector<NodeId>> buckets_;
    FlatMap<u64, std::vector<NodeId>> phi_buckets_;
    bool changed_ = false;
};
} // namespace

class GlobalValueNumberingPass : public Pass {
public:
    const char* name() const override { return "GlobalValueNumbering"; }
    int order() const override { return 9; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::Dominators); }
    // Pure data replacement: control structure untouched, Dominators preserved.
    AnalysisMask invalidated() const override { return 0; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Gvn g(fg.g, ctx.analysis.doms(fg));
            changed |= g.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(GlobalValueNumberingPass, 9, "Phase 1")

} // namespace jules
