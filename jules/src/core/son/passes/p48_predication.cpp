// Pass 48 — Predication (Phase 4)
//
// General branch-to-select conversion beyond the diamond case pass 49
// handles. The real target in this language: SHORT-CIRCUIT CONDITIONS.
// The builder lowers `a && b` / `a || b` as full control flow
//     If(a) -> [rhs: b] / [const: 0|1] -> Region -> vphi(b, 0|1)
// so every `while c1 && c2` loop evaluates its condition through TWO
// branches per iteration. When the RHS is PURE (no loads, stores, calls,
// nested control — only pure data ops), both operands can be evaluated
// unconditionally and merged arithmetically:
//     vphi  ->  Bin(And|Or, c1, c2)   pinned in the pre-branch block
// The If/projections/Region dissolve; the guard branch count halves.
// Nested short-circuits flatten iteratively (inner first, fixpoint):
//     (a && b) && c  ->  And(And(a, b), c)
// Purity is the whole soundness argument: the RHS executes on paths where
// it originally did not, which is observable only through effects — and
// there are none. The memory phi is verified to have identical inputs
// (RHS purity guarantees it; the check stays as a hard gate).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class Predicator {
public:
    Predicator(Graph& g, DomTree& dom) : g_(g), dom_(dom) {}

    bool run() {
        bool changed = false;
        for (u32 round = 0; round < 8; ++round) {
            bool this_round = false;
            for (NodeId id = 0; id < g_.size(); ++id) this_round |= flatten(id);
            changed |= this_round;
            if (!this_round) break;
        }
        return changed;
    }

private:
    // Backward slice of a value: every pure node feeding it, plus the pins
    // that need moving. Returns false if anything impure appears — the
    // operand is not unconditionally evaluable.
    //   * phis are leaves: a phi whose merge block DOMINATES the branch
    //     block (loop headers, outer scopes) is defined on every path to
    //     the branch — reading it unconditionally is sound. A phi merged
    //     BELOW the branch (the short-circuit's own region, nested &&)
    //     would be read before its definition: reject.
    //   * loads/calls/stores/allocs and control ops: reject (effects or
    //     path-dependent control).
    bool pure_slice(NodeId v, NodeId h, std::vector<NodeId>& out) {
        if (v == kNoNode || g_.is_dead(v)) return false;
        const Node& n = g_.node(v);
        if (is_control_op(n.op)) return false;   // nested control (inner && etc.)
        if (n.op == Op::Phi) {
            NodeId pin = n.in[0];
            if (pin == kNoNode || g_.is_dead(pin)) return false;
            return dom_.dominates(pin, h);       // loop-header phi: safe leaf
        }
        if (n.op == Op::Load || n.op == Op::Call ||
            n.op == Op::Store || n.op == Op::Alloc)
            return false;                        // effect-carrying
        if (!is_pure_op(n.op)) return false;
        // already collected?
        for (NodeId c : out) if (c == v) return true;
        NodeId pin = n.in[0];
        if (pin == kNoNode || g_.is_dead(pin)) return false;
        // nodes pinned at-or-dominating the branch block need no move
        if (dom_.dominates(pin, h)) return true;
        out.push_back(v);
        for (u8 i = 1; i < n.n_in; ++i) {
            NodeId d = n.in[i];
            if (d == kNoNode || g_.is_dead(d)) continue;
            if (is_block_head(g_.node(d).op)) continue; // pinned data
            if (!pure_slice(d, h, out)) return false;
        }
        return true;
    }

    bool flatten(NodeId if_id) {
        const Node& ifn = g_.node(if_id);
        if (ifn.op != Op::If) return false;
        NodeId h = ifn.in[0]; // branching block
        if (h == kNoNode || g_.is_dead(h)) return false;
        NodeId c1 = ifn.in[1];

        NodeId tproj = kNoNode, fproj = kNoNode;
        for (NodeId u : g_.uses_of(if_id)) {
            if (g_.node(u).op == Op::IfTrue) tproj = u;
            if (g_.node(u).op == Op::IfFalse) fproj = u;
        }
        if (tproj == kNoNode || fproj == kNoNode) return false;

        // both projections must fall into the SAME merge region (exactly
        // one Region user each)
        NodeId region = kNoNode;
        {
            NodeId rt = kNoNode, rf = kNoNode;
            for (NodeId u : g_.uses_of(tproj))
                if (g_.node(u).op == Op::Region) {
                    if (rt != kNoNode) return false; // multiple merges
                    rt = u;
                }
            for (NodeId u : g_.uses_of(fproj))
                if (g_.node(u).op == Op::Region) {
                    if (rf != kNoNode) return false;
                    rf = u;
                }
            if (rt == kNoNode || rf == kNoNode || rt != rf) return false;
            region = rt;
        }
        const Node& r = g_.node(region);
        if (r.n_in != 2) return false;

        // value phi + memory phi at the merge; nothing else
        NodeId vphi = kNoNode, memphi = kNoNode;
        u32 extra_phis = 0;
        for (NodeId u : g_.uses_of(region)) {
            const Node& un = g_.node(u);
            if (un.op != Op::Phi) continue;
            if (un.ty == ty_mem()) {
                if (memphi != kNoNode) return false;
                memphi = u;
            } else if (un.ty == ty_i1()) {
                if (vphi != kNoNode) return false;
                vphi = u;
            } else {
                ++extra_phis;
            }
        }
        if (vphi == kNoNode || extra_phis > 0) return false;
        if (g_.node(vphi).n_in != 3) return false;

        // Orientation from the phi inputs: one edge carries the short-
        // circuit constant (0 for &&, 1 for || — earlier passes may have
        // re-pinned the Const node itself to Start, so identify it by
        // VALUE, not by pin); the other edge carries the RHS value.
        // region.in[k] <-> vphi.in[k+1].
        NodeId vals[2] = {g_.node(vphi).in[1], g_.node(vphi).in[2]};
        int const_side = -1;
        i64 cv = 0;
        for (int k = 0; k < 2; ++k) {
            NodeId v = vals[k];
            if (v == kNoNode || g_.node(v).op != Op::Const) continue;
            i64 x = g_.node(v).ival;
            if (x != 0 && x != 1) continue;
            const_side = k;
            cv = x;
        }
        if (const_side < 0) return false; // no short-circuit constant
        // the constant must arrive on the matching projection: 0 via
        // IfFalse (&&), 1 via IfTrue (||) — anything else is a shape the
        // builder never produces; refuse it
        {
            NodeId const_pred = r.in[const_side];
            NodeId want = (cv == 0) ? fproj : tproj;
            if (const_pred != want) return false;
        }
        int rhs_side = 1 - const_side;
        NodeId rhs = vals[rhs_side];
        if (rhs == kNoNode || g_.is_dead(rhs)) return false;
        u8 bin_sub = static_cast<u8>(cv == 0 ? BinOp::And : BinOp::Or);

        // memory merge must be trivially identical (RHS purity implies it;
        // the gate stays so a surprise never silently changes semantics)
        if (memphi != kNoNode) {
            const Node& mp = g_.node(memphi);
            if (mp.in[1] != mp.in[2]) return false;
        }

        // RHS purity: every node in its backward slice is a pure data op
        // (phis allowed as dominating leaves — loop headers)
        std::vector<NodeId> slice;
        if (!pure_slice(rhs, h, slice)) return false;
        // safety: a successor region that already has h as a predecessor
        // would end up with h twice after the dissolve — refuse (rare
        // nested shapes; the next fixpoint round may find another path)
        for (NodeId u : g_.uses_of(region)) {
            const Node& un = g_.node(u);
            if (un.op != Op::Region) continue;
            bool has_region = false, has_h = false;
            for (u8 i = 0; i < un.n_in; ++i) {
                if (un.in[i] == region) has_region = true;
                if (un.in[i] == h && un.n_in > 1) has_h = true;
            }
            if (has_region && has_h) return false;
        }
        // every collected node's pin sits inside the RHS path (below h):
        // repin to h — pure ops, so earlier execution is unobservable
        for (NodeId v : slice) g_.set_input(v, 0, h);

        // c1 must be available at h (it is: it fed the If)
        NodeId flat = g_.make(Op::Bin, ty_i1(), {h, c1, rhs}, bin_sub);
        g_.replace_all_uses(vphi, flat);
        g_.kill(vphi);

        if (memphi != kNoNode) {
            g_.replace_uses_as_memory(memphi, g_.node(memphi).in[1]);
            g_.kill(memphi);
        }

        // dissolve the merge: repin region users to h, repoint region preds
        const SmallVec<NodeId, 4> region_users = g_.uses_of(region);
        for (NodeId u : region_users) {
            Node& un = g_.node(u);
            if (un.op == Op::Region) {
                for (u8 i = 0; i < un.n_in; ++i)
                    if (un.in[i] == region) g_.set_input(u, i, h);
            } else if (un.in[0] == region) {
                g_.set_input(u, 0, h);
            }
        }
        // repin the projections' remaining contents — most importantly the
        // SHORT-CIRCUIT CONSTANT ITSELF (the builder pins it at const_pin):
        // killing the projections with the const still pinned there leaves
        // a live node reading a killed control input (the verifier catches
        // exactly this shape). Pure nodes move freely to h.
        for (NodeId proj : {tproj, fproj}) {
            const SmallVec<NodeId, 4> pusers = g_.uses_of(proj);
            for (NodeId u : pusers) {
                if (g_.node(u).in[0] == proj) g_.set_input(u, 0, h);
            }
        }
        g_.kill(region);
        g_.kill(tproj);
        g_.kill(fproj);
        g_.kill(if_id);
        return true;
    }

    Graph& g_;
    DomTree& dom_;
};
} // namespace

class PredicationPass : public Pass {
public:
    const char* name() const override { return "Predication"; }
    int order() const override { return 48; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::Dominators); }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Predicator p(fg.g, ctx.analysis.doms(fg));
            changed |= p.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(PredicationPass, 48, "Phase 4")

} // namespace jules
