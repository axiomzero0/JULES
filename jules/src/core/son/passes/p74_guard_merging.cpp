// Pass 74 — GuardMerging (Phase 6: Devirtualization & Speculation)
//
// Same-value guards within a dominator subtree merge: when a guard site
// (an If flagged kFlagGuardSite, created by pass 91's deopt ladder) is
// DOMINATED by another guard site with a structurally identical condition
// (same compared SSA value and same constant), the dominated re-check is
// provably redundant — SSA values are immutable, so the dominating guard
// already established the fact on every path reaching the dominated one.
//
// STATUS: complete detection, deferred rewrite. The pass proves the
// redundancy set (and reports its size through --stats changes) but does
// not yet rewrite the control graph: collapsing a guard whose arms carry
// the ladder's variant/fallback CALL chains requires split-elimination of
// the false arm's merge contribution (re-pointing region predecessors
// naively executes the fallback rung alongside the variant — a miscompile
// found by the t41 JIT round). The transform lands with the control
// surgery; the proof machinery is live and exercised.
//
// Execution slot: pass 91 creates guards at the END of the SoN stage, so
// this pass runs in the post-inline cleanup sweep at the SoN/Linear
// boundary (see pass_manager.cpp kCleanupOrders).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {

struct GuardKey {
    NodeId value = kNoNode;
    u8 cmp = 0;
    i64 iv = 0;
    f64 fv = 0;
    bool valid = false;
};

GuardKey guard_key(const Graph& g, NodeId gif) {
    GuardKey k;
    const Node& n = g.node(gif);
    if (n.op != Op::If || (n.flags & kFlagGuardSite) == 0 || n.n_in < 2)
        return k;
    const Node& cond = g.node(n.in[1]);
    if (cond.op != Op::Cmp || cond.n_in < 3) return k;
    const Node& rhs = g.node(cond.in[2]);
    if (rhs.op != Op::Const) return k; // non-const guards never merge here
    k.value = cond.in[1];
    k.cmp = cond.sub;
    k.iv = rhs.ival;
    k.fv = rhs.fval;
    k.valid = true;
    return k;
}

bool same_key(const GuardKey& a, const GuardKey& b) {
    return a.valid && b.valid && a.value == b.value && a.cmp == b.cmp &&
           a.iv == b.iv && a.fv == b.fv;
}

class GuardMerger {
public:
    explicit GuardMerger(Graph& g, DomTree& dom) : g_(g), dom_(dom) {}

    // Returns the number of provably redundant (dominated, identical)
    // guard sites. Reported as pass changes; the control rewrite is the
    // next milestone (see file comment).
    u32 run() {
        std::vector<NodeId> guards;
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op == Op::If && (n.flags & kFlagGuardSite) != 0 &&
                guard_key(g_, id).valid)
                guards.push_back(id);
        }
        if (guards.size() < 2) return 0;
        u32 redundant = 0;
        for (NodeId g2 : guards) {
            const GuardKey k2 = guard_key(g_, g2);
            NodeId b2 = g_.node(g2).in[0];
            if (b2 == kNoNode) continue;
            for (NodeId g1 : guards) {
                if (g1 == g2) continue;
                const GuardKey k1 = guard_key(g_, g1);
                if (!same_key(k1, k2)) continue;
                NodeId b1 = g_.node(g1).in[0];
                if (b1 == kNoNode) continue;
                if (dom_.dominates(b1, b2)) { ++redundant; break; }
            }
        }
        return redundant;
    }

private:
    Graph& g_;
    DomTree& dom_;
};

} // namespace

class GuardMergingPass : public Pass {
public:
    const char* name() const override { return "GuardMerging"; }
    int order() const override { return 74; }
    const char* phase_name() const override { return "Phase 6: Devirtualization & Speculation"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::Dominators); }
    AnalysisMask invalidated() const override { return 0; } // analysis-only
    bool run(PassContext& ctx) override {
        u32 total = 0;
        for (FunctionGraph& fg : ctx.mod.fns) {
            GuardMerger m(fg.g, ctx.analysis.doms(fg));
            total += m.run();
        }
        return total > 0; // proof count recorded through --stats
    }
};

JULES_REGISTER_PASS(GuardMergingPass, 74, "Phase 6")

} // namespace jules
