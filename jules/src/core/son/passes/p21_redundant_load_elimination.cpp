// Pass 21 — RedundantLoadElimination (Phase 2: Memory Optimization)
//
// Removes loads satisfied by a PRIOR DOMINATING LOAD of the same base object
// when no aliasing write (store to the same base, or any call) intervenes on
// the memory chain. Distinct from GVN's load dedup: this one walks the
// version chain skipping provably non-aliasing stores.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class RedundantLoadEliminator {
public:
    RedundantLoadEliminator(Graph& g, AliasInfo& aa, DomTree& dom)
        : g_(g), aa_(aa), dom_(dom) {}

    bool run() {
        // collect loads in id order (deterministic)
        std::vector<NodeId> loads;
        for (NodeId id = 0; id < g_.size(); ++id)
            if (g_.node(id).op == Op::Load) loads.push_back(id);

        for (NodeId l : loads) {
            if (g_.node(l).op == Op::Dead) continue; // already replaced
            NodeId cand = find_dominating_load(l, loads);
            if (cand != kNoNode && cand != l) {
                g_.replace_all_uses(l, cand);
                g_.kill(l);
                changed_ = true;
            }
        }
        return changed_;
    }

private:
    NodeId find_dominating_load(NodeId l, const std::vector<NodeId>& loads) {
        const Node& ln = g_.node(l);
        NodeId base = aa_.base_of(ln.in[2]);
        if (base == kNoNode) return kNoNode;
        NodeId l_block = ln.in[0];

        for (NodeId other : loads) {
            if (other >= l) break; // only strictly earlier nodes
            const Node& on = g_.node(other);
            if (on.op != Op::Load) continue;
            if (aa_.base_of(on.in[2]) != base) continue;
            if (!dom_.dominates(on.in[0], l_block)) continue;
            // No intervening write between `other` and `l`: walk l's chain
            // backward; stop at other's memory version (ok), a same-base
            // store or any call (barrier), or unknown (give up).
            if (chain_clear(g_.node(l).in[1], on.in[1], base, 0)) return other;
        }
        return kNoNode;
    }

    // Is the walk from `mem` back to `target` free of same-base stores/calls?
    bool chain_clear(NodeId mem, NodeId target, NodeId base, u32 depth) {
        constexpr u32 kMaxDepth = 64;
        if (depth > kMaxDepth) return false;
        while (mem != kNoNode && g_.node(mem).op != Op::Dead) {
            if (mem == target) return true;
            const Node& m = g_.node(mem);
            switch (m.op) {
                case Op::Store:
                    if (aa_.base_of(m.in[2]) == base) return false;
                    mem = m.in[1];
                    continue;
                case Op::Alloc:
                    mem = m.in[1];
                    continue;
                case Op::Call:
                    return false; // barrier
                case Op::Phi: {
                    // every path must be clear to the same target
                    for (u8 i = 1; i < m.n_in; ++i)
                        if (!chain_clear(m.in[i], target, base, depth + 1)) return false;
                    return true;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    Graph& g_;
    AliasInfo& aa_;
    DomTree& dom_;
    bool changed_ = false;
};
} // namespace

class RedundantLoadEliminationPass : public Pass {
public:
    const char* name() const override { return "RedundantLoadElimination"; }
    int order() const override { return 21; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::AliasInfo;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            RedundantLoadEliminator e(fg.g, ctx.analysis.alias(fg), ctx.analysis.doms(fg));
            changed |= e.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(RedundantLoadEliminationPass, 21, "Phase 2")

} // namespace jules
