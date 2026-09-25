// Pass 74 — GuardMerging (Phase 6: Devirtualization & Speculation)
//
// Same-value guards within a dominator subtree merge: when a guard site
// (an If flagged kFlagGuardSite, created by pass 91's deopt ladder) is
// dominated by another guard site with a structurally identical condition
// (same compared SSA value and same constant), the dominated re-check is
// provably redundant — SSA values are immutable, so the dominating guard
// already established the fact on every path reaching the dominated one.
//
// The REWRITE resolves the arm split, not by graph surgery here but by
// FACT INJECTION: the dominated guard's condition input is replaced with
// a Const pinned at its block, and the cleanup sweep's SCCP — which runs
// AFTER this pass in kCleanupOrders — performs the arm-split elimination
// (kill the dead projection, trim the ladder merge's dead predecessor,
// realign the phis, cascade the dead rung's call chain). That is exactly
// the machinery SCCP already exercises on the versioned loops of pass 72,
// so the "split-elimination of the false arm's merge contribution" the
// t41 JIT round found hazardous as a hand-rolled re-point is instead the
// pred-trim path with phi realignment, the one shape SCCP proves sound.
//
// Soundness of the resolution: block-level domination alone is NOT
// enough — the dominated guard could also be reachable through the
// dominating guard's FALSE arm (a second call site's ladder under the
// first site's fallback), where the condition is false. The rewrite only
// fires when one of the dominating guard's PROJECTIONS dominates the
// dominated guard's block:
//   * IfTrue dominates  -> the condition was true on every path there
//   * IfFalse dominates -> false on every path
// which pins the dominated guard's value on ALL paths reaching it.
//
// Execution slot: pass 91 creates guards at the END of the SoN stage, so
// this pass runs in the post-inline cleanup sweep at the SoN/Linear
// boundary (see pass_manager.cpp kCleanupOrders), positioned BEFORE the
// folding set so the same round's SCCP prunes what this pass resolves —
// a single-round sweep (-Os; the spec family is Off at -O1) must not
// carry resolved-but-unpruned guards into the linearizer.
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

// The projections of a guard If (kNoNode when absent).
void guard_projs(Graph& g, NodeId gif, NodeId& t, NodeId& f) {
    t = kNoNode;
    f = kNoNode;
    for (NodeId u : g.uses_of(gif)) {
        Op uo = g.node(u).op;
        if (uo == Op::IfTrue) t = u;
        if (uo == Op::IfFalse) f = u;
    }
}

class GuardMerger {
public:
    explicit GuardMerger(Graph& g, DomTree& dom) : g_(g), dom_(dom) {}

    // Resolves every dominated same-key guard whose value is pinned by a
    // dominating guard's ARM (true -> Const(1), false -> Const(0)). Returns
    // the number of guards resolved; the cleanup sweep's SCCP prunes them.
    u32 run() {
        std::vector<NodeId> guards;
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op == Op::If && (n.flags & kFlagGuardSite) != 0 &&
                guard_key(g_, id).valid)
                guards.push_back(id);
        }
        if (guards.size() < 2) return 0;

        u32 resolved = 0;
        for (NodeId g2 : guards) {
            if (g_.node(g2).op != Op::If) continue; // resolved earlier this run
            const GuardKey k2 = guard_key(g_, g2);
            if (!k2.valid) continue;
            NodeId b2 = g_.node(g2).in[0];
            if (b2 == kNoNode) continue;
            for (NodeId g1 : guards) {
                if (g1 == g2 || g_.node(g1).op != Op::If) continue;
                const GuardKey k1 = guard_key(g_, g1);
                if (!same_key(k1, k2)) continue;
                // arm-level domination pins the value on every path to b2
                NodeId t1 = kNoNode, f1 = kNoNode;
                guard_projs(g_, g1, t1, f1);
                NodeId b1 = g_.node(g1).in[0];
                if (b1 == kNoNode) continue;
                bool pin_true = t1 != kNoNode && dom_.dominates(t1, b2);
                bool pin_false = f1 != kNoNode && dom_.dominates(f1, b2);
                if (!pin_true && !pin_false) continue;
                // A chain root must not resolve itself against a guard it
                // itself dominates transitively through BOTH arms (a block
                // below a merge is dominated by neither arm alone) — the
                // arm check above already excludes that; resolve g2.
                if (!resolve(g2, pin_true)) continue;
                ++resolved;
                break;
            }
        }
        return resolved;
    }

private:
    // Replace the guard's condition with a Const pinned at its block. The
    // guard keeps its kFlagGuardSite flag (harmless: SCCP folds the If and
    // the cascade removes it within the same sweep round).
    bool resolve(NodeId g2, bool value) {
        NodeId b2 = g_.node(g2).in[0];
        if (b2 == kNoNode) return false;
        NodeId c = g_.make(Op::Const, ty_i1(), {b2});
        g_.node(c).ival = value ? 1 : 0;
        g_.set_input(g2, 1, c);
        g_.touch();
        return true;
    }

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
    AnalysisMask invalidated() const override {
        // The pass injects value facts only (a condition input becomes a
        // Const); the control graph is untouched — SCCP, which consumes
        // the fact, invalidates the structural analyses itself.
        return 0;
    }
    bool run(PassContext& ctx) override {
        u32 resolved = 0;
        for (FunctionGraph& fg : ctx.mod.fns) {
            GuardMerger m(fg.g, ctx.analysis.doms(fg));
            resolved += m.run();
        }
        return resolved > 0;
    }
};

JULES_REGISTER_PASS(GuardMergingPass, 74, "Phase 6")

} // namespace jules
