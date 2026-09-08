// Pass 45 — LoopInterchange (Phase 4)
//
// Swap a perfectly-nested loop pair when the INNER index strides the
// wider gap — the classic column-major walk:
//
//   while i < W { while j < H { s += a[j*W + i]; j++; } i++; }
//   ==> while j < H { while i < W { s += a[j*W + i]; i++; } j++; }
//
// The body nodes are untouched: the same address expression `j*W+i`
// becomes stride-1 in the new INNER loop (i). Loads whose j-coefficient
// is non-zero become invariant in the new inner loop (LICM hoists them).
//
// Legality gates:
//   * perfect nest: the outer guard's body projection feeds the inner
//     header directly, and nothing else is pinned in that gap
//   * the whole nest is READ-ONLY (loads + pure ops; no stores/calls) —
//     dependence direction cannot matter for pure reads
//   * carried values thread the standard pair pattern
//     (X_o = Phi(ext, X_i), X_i = Phi(X_o, upd)); accumulating updates
//     must be integer commutative folds whose feed never reads the
//     accumulator (fp folds change rounding order: gated on --fp=fast)
//   * every load's element index is affine in (i, j) with the i
//     coefficient exactly 1 (unit inner stride after the swap) and some
//     load's j coefficient > 1 (otherwise there is nothing to gain)
//
// Mechanics: the two headers swap ROLES — control edges re-thread
// (entry into the new outer, inner entry from the outer's body
// projection, backedges cross), carried phi pairs ROTATE their inputs
// (the outer pair takes the external entry + inner pair; the inner pair
// takes the outer phi + body update), the IV updates move to the blocks
// their new loops need (i++ into the inner body, j++ onto the outer
// latch path), and post-nest readers of the outer phis re-target to the
// new outer phis. Both guards keep testing their own IVs, unchanged.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {

struct InterShape {
    NodeId header = kNoNode;
    u8 entry_slot = 0, latch_slot = 1;
    NodeId guard_if = kNoNode;
    NodeId body_proj = kNoNode;
    NodeId exit_proj = kNoNode;
    NodeId iv_phi = kNoNode;
    i64 iv_step = 1;
};

bool match_inter_shape(Graph& g, LoopInfo& li, const Loop& l, InterShape& out) {
    if (l.blocks.size() < 2) return false;
    NodeId header = l.header;
    const Node& h = g.node(header);
    if (h.op != Op::Region || h.n_in != 2) return false;
    u8 entry_slot = 2, latch_slot = 2;
    for (u8 i = 0; i < 2; ++i) {
        if (li.block_in_loop(l, h.in[i])) latch_slot = i;
        else entry_slot = i;
    }
    if (entry_slot == 2 || latch_slot == 2) return false;

    NodeId guard_if = kNoNode;
    for (NodeId u : g.uses_of(header)) {
        if (g.node(u).op == Op::If && g.node(u).in[0] == header) {
            if (guard_if != kNoNode) return false;
            guard_if = u;
        }
    }
    if (guard_if == kNoNode) return false;
    const Node& gi = g.node(guard_if);
    if (gi.in[1] == kNoNode || g.node(gi.in[1]).op != Op::Cmp) return false;
    const Node& cmp = g.node(gi.in[1]);

    NodeId body_proj = kNoNode, exit_proj = kNoNode;
    for (NodeId u : g.uses_of(guard_if)) {
        Op o = g.node(u).op;
        if (o != Op::IfTrue && o != Op::IfFalse) continue;
        if (li.block_in_loop(l, u)) {
            if (body_proj != kNoNode) return false;
            body_proj = u;
        } else {
            if (exit_proj != kNoNode) return false;
            exit_proj = u;
        }
    }
    if (body_proj == kNoNode || exit_proj == kNoNode) return false;

    auto phi_here = [&](NodeId n) {
        return n != kNoNode && g.node(n).op == Op::Phi && g.node(n).in[0] == header;
    };
    NodeId iv = kNoNode;
    if (phi_here(cmp.in[1])) iv = cmp.in[1];
    else if (phi_here(cmp.in[2])) iv = cmp.in[2];
    if (iv == kNoNode) return false;
    const Node& ivn = g.node(iv);
    NodeId upd = ivn.in[latch_slot + 1];
    if (upd == kNoNode || g.node(upd).op != Op::Bin ||
        static_cast<BinOp>(g.node(upd).sub) != BinOp::Add)
        return false;
    ConstVal sc;
    if (g.node(upd).in[1] == iv && const_of(g, g.node(upd).in[2], sc)) { /* ok */ }
    else if (g.node(upd).in[2] == iv && const_of(g, g.node(upd).in[1], sc)) { /* ok */ }
    else return false;
    if (sc.iv <= 0) return false;

    // no early exits
    for (NodeId blk : l.blocks) {
        if (blk == header) continue;
        for (NodeId u : g.uses_of(blk)) {
            const Node& un = g.node(u);
            if (un.op != Op::If || un.in[0] != blk) continue;
            if (u == guard_if) continue;
            for (NodeId p : g.uses_of(u)) {
                Op po = g.node(p).op;
                if (po != Op::IfTrue && po != Op::IfFalse) continue;
                if (!li.block_in_loop(l, p)) return false;
            }
        }
    }

    out.header = header;
    out.entry_slot = entry_slot;
    out.latch_slot = latch_slot;
    out.guard_if = guard_if;
    out.body_proj = body_proj;
    out.exit_proj = exit_proj;
    out.iv_phi = iv;
    out.iv_step = sc.iv;
    return true;
}

// How an index expression depends on a given IV. Coefficients may be
// runtime values (matrix width params), so kinds replace constants:
//   Unit — the IV appears exactly once, unscaled (stride 1)
//   Wide — the IV is scaled/offset by something not provably 1
//   Absent — no dependence
enum class CK { Absent, Unit, Wide };

CK combine_ck(CK a, CK b) {
    if (a == CK::Absent) return b;
    if (b == CK::Absent) return a;
    return CK::Wide; // Unit+Unit = 2x, Unit+Wide, Wide+Wide
}

bool refs_iv(Graph& g, NodeId n, NodeId iv, int depth = 0);

CK coeff_kind(Graph& g, NodeId idx, NodeId iv, int depth = 0) {
    if (idx == kNoNode || g.is_dead(idx) || depth > 64) return CK::Absent;
    if (idx == iv) return CK::Unit;
    const Node& n = g.node(idx);
    if (n.op == Op::Const) return CK::Absent;
    if (n.op == Op::Bin) {
        BinOp op = static_cast<BinOp>(n.sub);
        if (op == BinOp::Add) {
            CK a = coeff_kind(g, n.in[1], iv, depth + 1);
            CK b = coeff_kind(g, n.in[2], iv, depth + 1);
            return combine_ck(a, b);
        }
        if (op == BinOp::Mul || op == BinOp::Shl) {
            // iv * anything-not-iv: unit only when the scale is provably 1
            if (n.in[1] == iv || n.in[2] == iv) {
                NodeId other = (n.in[1] == iv) ? n.in[2] : n.in[1];
                if (coeff_kind(g, other, iv, depth + 1) != CK::Absent) return CK::Wide;
                ConstVal c;
                if (op == BinOp::Mul && const_of(g, other, c) && c.iv == 1)
                    return CK::Unit;
                if (op == BinOp::Shl) {
                    if (const_of(g, n.in[2], c) && c.iv == 0) return CK::Unit; // x << 0
                }
                return CK::Wide;
            }
        }
    }
    // unknown shape: conservatively wide if it references the iv at all
    if (refs_iv(g, idx, iv, 0)) return CK::Wide;
    return CK::Absent;
}

bool refs_iv(Graph& g, NodeId n, NodeId iv, int depth) {
    if (n == kNoNode || g.is_dead(n) || depth > 64) return false;
    if (n == iv) return true;
    const Node& nd = g.node(n);
    if (is_control_op(nd.op)) return false;
    for (u8 k = 1; k < nd.n_in; ++k)
        if (refs_iv(g, nd.in[k], iv, depth + 1)) return true;
    return false;
}

class Interchanger {
public:
    Interchanger(Graph& g, LoopInfo& li, FpMode fp) : g_(g), li_(li), fp_(fp) {}

    u32 run() {
        u32 hits = 0;
        // pre-pass: collapse self-thread phis at every 2-pred loop header
        // (SROA's per-version value threads — e.g. the outer IV read
        // through the inner loop) so shape matching sees clean IV updates.
        for (int fix = 0; fix < 4; ++fix) {
            bool collapsed = false;
            for (const Loop& l : li_.loops()) {
                const Node& h = g_.node(l.header);
                if (h.op != Op::Region || h.n_in != 2) continue;
                u8 lslot = li_.block_in_loop(l, h.in[1]) ? 1 : 0;
                const SmallVec<NodeId, 4> users = g_.uses_of(l.header);
                for (NodeId u : users) {
                    Node& un = g_.node(u);
                    if (un.op != Op::Phi || un.in[0] != l.header) continue;
                    u8 eslot = 1 - lslot;
                    if (un.in[lslot + 1] == u) {
                        g_.replace_all_uses(u, un.in[eslot + 1]);
                        g_.kill(u);
                        collapsed = true;
                    }
                }
            }
            if (!collapsed) break;
        }
        for (int round = 0; round < 6; ++round) {
            bool any = false;
            for (const Loop& outer : li_.loops()) {
                InterShape so;
                if (!match_inter_shape(g_, li_, outer, so)) continue;
                for (const Loop& inner : li_.loops()) {
                    if (inner.parent_header != outer.header) continue;
                    InterShape si;
                    if (!match_inter_shape(g_, li_, inner, si)) continue;
                    if (try_interchange(outer, so, inner, si)) {
                        ++hits;
                        any = true;
                        break;
                    }
                }
                if (any) break;
            }
            if (!any) break;
        }
        return hits;
    }

private:
    bool refs_closure(NodeId n, NodeId a, NodeId b, int depth = 0) {
        if (n == kNoNode || g_.is_dead(n) || depth > 64) return false;
        if (n == a || n == b) return true;
        const Node& nd = g_.node(n);
        if (is_control_op(nd.op)) return false;
        for (u8 k = 1; k < nd.n_in; ++k)
            if (refs_closure(nd.in[k], a, b, depth + 1)) return true;
        return false;
    }

    bool try_interchange(const Loop& lo, const InterShape& so,
                         const Loop& lin, const InterShape& si) {
        Graph& g = g_;
        (void)lo;
        // perfect nest: the outer body projection feeds the inner header
        // directly and pins nothing else
        if (g.node(si.header).in[si.entry_slot] != so.body_proj) return false;
        const SmallVec<NodeId, 4> pot_users = g.uses_of(so.body_proj);
        for (NodeId u : pot_users)
            if (u != si.header) return false;

        // read-only nest
        for (NodeId blk : lo.blocks) {
            for (NodeId u : g.uses_of(blk)) {
                if (g.is_dead(u) || g.node(u).in[0] != blk) continue;
                Op o = g.node(u).op;
                if (o == Op::Store || o == Op::Call || o == Op::Alloc) return false;
            }
        }

        // affine strides over the inner-body loads: the OUTER iv walks
        // with unit stride in every address, and at least one address
        // gives the INNER iv a wide stride (the interchange gain)
        bool any_wide = false;
        for (NodeId blk : lin.blocks) {
            for (NodeId u : g.uses_of(blk)) {
                if (g.is_dead(u) || g.node(u).in[0] != blk) continue;
                if (g.node(u).op != Op::Load) continue;
                vecx::AddrPattern ap = vecx::match_addr(g, g.node(u).in[2], 0);
                if (!ap.ok) return false; // non-array access: bail
                if (ap.idx == kNoNode) continue;
                if (coeff_kind(g, ap.idx, so.iv_phi) != CK::Unit) return false;
                if (coeff_kind(g, ap.idx, si.iv_phi) == CK::Wide) any_wide = true;
            }
        }
        if (!any_wide) return false;

        // collapse trivial self-thread phis at both headers (values the
        // loop carries unchanged — SROA's per-version threads for the
        // outer IV and, in read-only nests, the memory phis): their value
        // IS the entry input everywhere. Fixpoint: inner collapses can
        // turn outer phis self-threading.
        const std::pair<NodeId, u8> hdrs[2] = {{so.header, so.latch_slot},
                                               {si.header, si.latch_slot}};
        for (int fix = 0; fix < 4; ++fix) {
            bool collapsed = false;
            for (const auto& [hdr, lslot] : hdrs) {
                const SmallVec<NodeId, 4> users = g.uses_of(hdr);
                for (NodeId u : users) {
                    Node& un = g.node(u);
                    if (un.op != Op::Phi || un.in[0] != hdr) continue;
                    u8 eslot = (lslot == 0) ? 1 : 0;
                    if (un.in[lslot + 1] == u) { // self-latch: carries entry
                        g.replace_all_uses(u, un.in[eslot + 1]);
                        g.kill(u);
                        collapsed = true;
                    }
                }
            }
            if (!collapsed) break;
        }

        // carried pairs + reduction legality
        std::vector<NodeId> phis_o, phis_i;
        for (NodeId u : g.uses_of(so.header)) {
            if (g.node(u).op == Op::Phi && u != so.iv_phi) phis_o.push_back(u);
        }
        for (NodeId u : g.uses_of(si.header)) {
            if (g.node(u).op == Op::Phi && u != si.iv_phi) phis_i.push_back(u);
        }
        // every inner non-IV phi threads from an outer non-IV phi
        for (NodeId pi : phis_i) {
            NodeId entry_v = g.node(pi).in[si.entry_slot + 1];
            bool paired = false;
            for (NodeId po : phis_o)
                if (entry_v == po) { paired = true; break; }
            if (!paired) return false;
        }
        for (NodeId po : phis_o) {
            // the outer phi must be threaded by some inner phi
            bool paired = false;
            for (NodeId pi : phis_i)
                if (g.node(pi).in[si.entry_slot + 1] == po) { paired = true; break; }
            if (!paired) return false;
        }
        for (NodeId pi : phis_i) {
            NodeId upd = g.node(pi).in[si.latch_slot + 1];
            if (upd == pi) continue; // trivial thread
            if (upd == kNoNode || g.is_dead(upd)) continue;
            // accumulating update? feed must not read the pair
            NodeId po = g.node(pi).in[si.entry_slot + 1];
            const Node& un = g.node(upd);
            if (un.op == Op::Bin) {
                BinOp op = static_cast<BinOp>(un.sub);
                bool comm = op == BinOp::Add || op == BinOp::Mul || op == BinOp::And ||
                            op == BinOp::Or || op == BinOp::Xor;
                NodeId other = kNoNode;
                if (un.in[1] == pi) other = un.in[2];
                else if (un.in[2] == pi) other = un.in[1];
                if (other != kNoNode) {
                    // reduction: other side must not read the pair
                    if (refs_closure(other, pi, po)) return false;
                    bool fp = ty_is_float(g.node(upd).ty);
                    bool comm_ok = comm && (!fp || fp_ == FpMode::Fast);
                    if (!comm_ok) return false;
                }
            } else if (refs_closure(upd, pi, po)) {
                return false; // non-accumulating carried value may not read itself
            }
        }

        NodeId E = g.node(so.header).in[so.entry_slot];
        NodeId P_oT = so.body_proj, P_oF = so.exit_proj;
        NodeId P_iT = si.body_proj, P_iF = si.exit_proj;

        // capture the pre-rotation wiring
        std::vector<std::pair<NodeId, NodeId>> pairs; // (X_o, X_i)
        for (NodeId pi : phis_i) {
            NodeId po = g.node(pi).in[si.entry_slot + 1];
            pairs.push_back({po, pi});
        }

        // ---- control rewiring -------------------------------------------------
        g.set_input(si.header, si.entry_slot, E);          // new outer entry
        g.set_input(so.header, so.entry_slot, P_iT);       // new inner entry
        g.set_input(so.header, so.latch_slot, P_oT);       // inner backedge = body
        g.set_input(si.header, si.latch_slot, P_oF);       // outer backedge = inner exit

        // body contents (pinned at P_iT) move to P_oT (the new inner entry)
        {
            const SmallVec<NodeId, 4> users = g.uses_of(P_iT);
            for (NodeId u : users) {
                if (u == si.header || u == so.header) continue;
                Node& un = g.node(u);
                if (un.in[0] == P_iT && !is_control_op(un.op) && !is_block_head(un.op))
                    g.set_input(u, 0, P_oT);
            }
        }
        // post-nest nodes (pinned at P_oF) move to P_iF (the new outer exit)
        {
            const SmallVec<NodeId, 4> users = g.uses_of(P_oF);
            for (NodeId u : users) {
                if (u == si.header) continue;
                Node& un = g.node(u);
                if (un.in[0] == P_oF && !is_control_op(un.op) && !is_block_head(un.op))
                    g.set_input(u, 0, P_iF);
            }
        }
        // the outer-body residue (i++ tree, pinned at P_iF) moves into the
        // inner body (it now steps per inner iteration)
        {
            const SmallVec<NodeId, 4> users = g.uses_of(P_iF);
            for (NodeId u : users) {
                Node& un = g.node(u);
                if (un.in[0] == P_iF && !is_control_op(un.op) && !is_block_head(un.op))
                    g.set_input(u, 0, P_oT);
            }
        }
        // the inner IV update (j++) moves from the (now moved) body to the
        // outer latch path — it must step once per OUTER iteration
        {
            NodeId j_upd = g.node(si.iv_phi).in[si.latch_slot + 1];
            if (j_upd != kNoNode && !g.is_dead(j_upd))
                g.set_input(j_upd, 0, P_oF);
            // i++ (outer IV update) stays wherever the residue loop put it
            // (P_oT, the inner body) — already correct
        }

        // ---- phi rotation -----------------------------------------------------
        for (auto [xo, xi] : pairs) {
            NodeId ext = g.node(xo).in[so.entry_slot + 1];
            NodeId upd = g.node(xi).in[si.latch_slot + 1];
            g.set_input(xi, si.entry_slot + 1, ext);   // new outer entry
            g.set_input(xi, si.latch_slot + 1, xo);    // new outer latch
            g.set_input(xo, so.entry_slot + 1, xi);    // new inner entry
            g.set_input(xo, so.latch_slot + 1, upd);   // new inner latch
        }

        // post-nest readers of the outer phis read the new outer phis
        for (auto [xo, xi] : pairs) {
            const SmallVec<NodeId, 4> users = g.uses_of(xo);
            for (NodeId u : users) {
                if (u == xi) continue; // the new latch wiring
                Node& un = g.node(u);
                for (u8 k = 1; k < un.n_in; ++k)
                    if (un.in[k] == xo) g.set_input(u, k, xi);
            }
        }

        g.touch();
        return true;
    }

    Graph& g_;
    LoopInfo& li_;
    FpMode fp_;
    FlatMap<NodeId, bool> seen_;
};

} // namespace

class LoopInterchangePass : public Pass {
public:
    const char* name() const override { return "LoopInterchange"; }
    int order() const override { return 45; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo | AnalysisKind::MemDep;
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Graph& g = fg.g;
            auto li = LoopInfo::compute(g, ctx.analysis.doms(fg));
            Interchanger x(g, *li, ctx.opts.fp);
            changed |= x.run() > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(LoopInterchangePass, 45, "Phase 4")

} // namespace jules
