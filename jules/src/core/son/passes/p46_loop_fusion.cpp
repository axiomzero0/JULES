// Pass 46 — LoopFusion (Phase 4)
//
// Merge two ADJACENT loops that run the same iteration sequence:
//
//   var s1 = 0; i = 0; while i < n { s1 += a[i]; i++; }
//   var s2 = 0; i = 0; while i < n { s2 += b[i]; i++; }
//   ==> one loop whose body does both updates
//
// Gains: one loop header/guard/latch instead of two (half the branch
// traffic), one IV update, and a fused body SLP/LICM/the vectorizer can
// see whole.
//
// Compatibility: identical IVs — same entry value (same node or equal
// constants), same constant step, same guard relation against the SAME
// bound node (GVN unifies constants; a param bound is one node), same
// projection polarity. L2's entry predecessor must be L1's EXIT
// projection (back-to-back loops).
//
// Safety (base-level, via alias analysis):
//   * writes(L1) may not touch anything L2 accesses, and vice versa —
//     otherwise interleaving the bodies changes which value a load sees
//   * no calls in either body
//   * L2's carried values (phi entries) and any invariant hoisted into
//     the gap between the loops must be available BEFORE L1's entry —
//     anything derived from L1's RESULTS (post-loop values) rejects the
//     fusion; movable pure nodes are repinned ahead of the fused loop
//   * memory rewiring: B2's first effects read B1's last effect
//     (iteration-ordered chain); post-L2 readers read the fused loop's
//     memory phi
//
// Mechanics: iv2 RAUs to iv1 (identical sequence); L2's header phis
// split into trivial self-latch phis (collapse to their entry), the mem
// phi (rewired as above), and real carried phis (rebuilt at header1 with
// their old entry/latch inputs); the guard's exit projection of L2 RAUs
// to L1's exit projection; B2's first block contents join B1's last
// block; the fused backedge comes from B2's last block.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {

struct LoopShape {
    NodeId header = kNoNode;
    u8 entry_slot = 0, latch_slot = 1;
    NodeId guard_if = kNoNode;
    NodeId body_proj = kNoNode;   // projection entering the body
    NodeId exit_proj = kNoNode;   // projection leaving the loop
    NodeId iv_phi = kNoNode;
    NodeId iv_init = kNoNode;     // entry value node (const or shared node)
    i64 iv_step = 1;
    NodeId bound = kNoNode;       // guard bound NODE (compared by identity)
    CmpOp rel = CmpOp::Lt;
    NodeId mem_phi = kNoNode;
    std::vector<NodeId> blocks;   // body blocks (incl. body_proj's block)
    std::vector<NodeId> phis;     // header phis (all)
};

bool match_shape(Graph& g, LoopInfo& li, DomTree& dom, const Loop& l, LoopShape& out) {
    (void)dom;
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

    // IV phi compared against the bound
    NodeId iv = kNoNode, bound = kNoNode;
    auto phi_here = [&](NodeId n) {
        return n != kNoNode && g.node(n).op == Op::Phi && g.node(n).in[0] == header;
    };
    if (phi_here(cmp.in[1])) { iv = cmp.in[1]; bound = cmp.in[2]; }
    else if (phi_here(cmp.in[2])) { iv = cmp.in[2]; bound = cmp.in[1]; }
    if (iv == kNoNode || bound == kNoNode) return false;
    const Node& ivn = g.node(iv);
    // step: Add(phi, const)
    NodeId upd = ivn.in[latch_slot + 1];
    i64 step = 0;
    if (upd == kNoNode || g.node(upd).op != Op::Bin ||
        static_cast<BinOp>(g.node(upd).sub) != BinOp::Add)
        return false;
    ConstVal sc;
    if (g.node(upd).in[1] == iv && const_of(g, g.node(upd).in[2], sc)) step = sc.iv;
    else if (g.node(upd).in[2] == iv && const_of(g, g.node(upd).in[1], sc)) step = sc.iv;
    else return false;

    // no early exits (inner branches stay inside)
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
    out.iv_init = ivn.in[entry_slot + 1];
    out.iv_step = step;
    out.bound = bound;
    out.rel = static_cast<CmpOp>(cmp.sub);
    out.mem_phi = kNoNode;
    out.blocks.clear();
    for (NodeId b : l.blocks) out.blocks.push_back(b);
    out.phis.clear();
    for (NodeId u : g.uses_of(header))
        if (g.node(u).op == Op::Phi) {
            out.phis.push_back(u);
            if (g.node(u).ty == ty_mem()) out.mem_phi = u;
        }
    return true;
}

class LoopFuser {
public:
    LoopFuser(Graph& g, LoopInfo& li, DomTree& dom, AliasInfo& aa)
        : g_(g), li_(li), dom_(dom), aa_(aa) {}

    u32 run() {
        u32 hits = 0;
        for (int round = 0; round < 8; ++round) {
            bool any = false;
            auto loops = li_.loops();
            for (size_t i = 0; i < loops.size() && !any; ++i) {
                LoopShape s1;
                if (!match_shape(g_, li_, dom_, loops[i], s1)) continue;
                for (size_t j = 0; j < loops.size() && !any; ++j) {
                    if (i == j) continue;
                    LoopShape s2;
                    if (!match_shape(g_, li_, dom_, loops[j], s2)) continue;
                    if (try_fuse(s1, s2)) { ++hits; any = true; }
                }
            }
            if (!any) break;
        }
        return hits;
    }

private:
    // value defined before the fused loop (dominates L1's entry)?
    bool available_before(NodeId n, NodeId entry, NodeId header, int depth = 0) {
        if (n == kNoNode || g_.is_dead(n) || depth > 64) return true;
        if (seen_.contains(n)) return true;
        seen_.insert(n, true);
        const Node& nd = g_.node(n);
        if (nd.op == Op::Phi && nd.in[0] == header) return false;
        if (is_block_head(nd.op) || is_control_op(nd.op))
            return n == entry || dom_.dominates(n, entry);
        NodeId pin = nd.in[0];
        if (pin == kNoNode) return false;
        if (pin == entry || dom_.dominates(pin, entry)) {
            for (u8 k = 1; k < nd.n_in; ++k)
                if (!available_before(nd.in[k], entry, header, depth + 1)) return false;
            return true;
        }
        return false;
    }

    void collect_accesses(const LoopShape& s, std::vector<NodeId>& writes,
                          std::vector<NodeId>& reads, bool& calls) {
        for (NodeId blk : s.blocks) {
            for (NodeId u : g_.uses_of(blk)) {
                if (g_.is_dead(u) || g_.node(u).in[0] != blk) continue;
                Op o = g_.node(u).op;
                if (o == Op::Store) writes.push_back(g_.node(u).in[2]);
                if (o == Op::Load) reads.push_back(g_.node(u).in[2]);
                if (o == Op::Call) calls = true;
            }
        }
    }

    bool try_fuse(const LoopShape& s1, const LoopShape& s2) {
        // adjacency: L2's entry pred is L1's exit projection
        if (g_.node(s2.header).in[s2.entry_slot] != s1.exit_proj) return false;
        // same projection polarity + relation
        if (g_.node(s2.body_proj).op != g_.node(s1.body_proj).op) return false;
        if (s2.rel != s1.rel) return false;
        // identical IVs
        if (s2.iv_step != s1.iv_step) return false;
        if (s2.bound != s1.bound) return false; // same node (GVN-unified)
        if (s1.iv_init != s2.iv_init) {
            ConstVal c1, c2;
            if (!const_of(g_, s1.iv_init, c1) || !const_of(g_, s2.iv_init, c2) ||
                c1.is_fp != c2.is_fp || c1.iv != c2.iv)
                return false;
        }
        if (s1.iv_step <= 0) return false;

        // base-level write/read independence
        std::vector<NodeId> w1, r1, w2, r2;
        bool calls = false;
        collect_accesses(s1, w1, r1, calls);
        collect_accesses(s2, w2, r2, calls);
        if (calls) return false;
        auto disjoint = [&](const std::vector<NodeId>& ws, const std::vector<NodeId>& as) {
            for (NodeId w : ws)
                for (NodeId a : as)
                    if (aa_.alias(w, a) != AliasResult::NoAlias) return false;
            return true;
        };
        if (!disjoint(w1, w2) || !disjoint(w1, r2) || !disjoint(w2, r1)) return false;

        NodeId E1 = g_.node(s1.header).in[s1.entry_slot];

        // nodes hoisted into the gap (pinned at L1's exit block) that B2
        // consumes must move ahead of the fused loop — pure, and their
        // INPUTS defined before L1's entry (the node itself is in the gap,
        // which is exactly why it is moving)
        const SmallVec<NodeId, 4> gap_users = g_.uses_of(s1.exit_proj);
        for (NodeId u : gap_users) {
            if (g_.is_dead(u) || u == s2.header) continue;
            const Node& un = g_.node(u);
            if (un.in[0] != s1.exit_proj) continue; // region pred etc.
            if (is_control_op(un.op) || is_block_head(un.op)) return false;
            if (!is_pure_op(un.op)) return false;   // effects in the gap: reject
            seen_.clear();
            bool ok = true;
            for (u8 k = 1; k < un.n_in && ok; ++k)
                ok = available_before(un.in[k], E1, s1.header);
            if (!ok) return false;
            g_.set_input(u, 0, E1);                 // hoist ahead of the loop
            g_.touch();
        }

        // IVs identical: every use of iv2 becomes iv1
        g_.replace_all_uses(s2.iv_phi, s1.iv_phi);

        // B2's header phis: classify and rebuild
        NodeId b1_last = g_.node(s1.header).in[s1.latch_slot];
        NodeId b2_first = s2.body_proj;
        NodeId b2_last = g_.node(s2.header).in[s2.latch_slot];
        FlatMap<NodeId, bool> in_b2;
        for (NodeId b : s2.blocks) in_b2.insert(b, true);

        for (NodeId phi : s2.phis) {
            if (phi == s2.iv_phi || phi == s2.mem_phi) continue;
            const Node& pn = g_.node(phi);
            NodeId latch_v = pn.in[s2.latch_slot + 1];
            NodeId entry_v = pn.in[s2.entry_slot + 1];
            if (latch_v == phi) {
                // trivial thread-through: value unchanged across L2
                g_.replace_all_uses(phi, entry_v);
                g_.kill(phi);
                continue;
            }
            // real carried phi: rebuild at header1 (inputs aligned to
            // header1's entry/latch pred slots)
            seen_.clear();
            if (!available_before(entry_v, E1, s1.header)) return false;
            NodeId a = entry_v, b = latch_v;
            if (s1.entry_slot == 1) std::swap(a, b);
            NodeId np = g_.make(Op::Phi, pn.ty, {s1.header, a, b});
            g_.replace_all_uses(phi, np);
            g_.kill(phi);
        }

        // memory: B2's effects read B1's last effect; post-L2 readers read
        // the fused loop's memory phi
        if (s2.mem_phi != kNoNode && s1.mem_phi != kNoNode) {
            NodeId m1 = s1.mem_phi;
            NodeId m1_latch = g_.node(m1).in[s1.latch_slot + 1];
            NodeId inside_v = (m1_latch != kNoNode && !g_.is_dead(m1_latch)) ? m1_latch : m1;
            const SmallVec<NodeId, 4> musers = g_.uses_of(s2.mem_phi);
            for (NodeId u : musers) {
                if (g_.is_dead(u)) continue;
                Node& un = g_.node(u);
                bool inside = in_b2.contains(un.in[0]);
                NodeId repl = inside ? inside_v : m1;
                if (repl == kNoNode || g_.is_dead(repl)) continue;
                for (u8 k = 1; k < un.n_in; ++k)
                    if (un.in[k] == s2.mem_phi) g_.set_input(u, k, repl);
            }
            g_.kill(s2.mem_phi);
        }

        // control: B2's first contents join B1's last block; the fused
        // backedge comes from B2's last block; post-L2 code re-pins at L1's
        // exit projection
        g_.replace_all_uses(b2_first, b1_last);
        if (b2_last != b2_first) g_.set_input(s1.header, s1.latch_slot, b2_last);
        g_.replace_all_uses(s2.exit_proj, s1.exit_proj);

        g_.kill(s2.header);
        g_.kill(s2.guard_if);
        // the old guard's Cmp: its only user was the killed If; leave it
        // and the verifier flags "Cmp uses a killed node" (the IV phi it
        // compared is dead) — dead-code hygiene, found by --verify on t32
        {
            NodeId old_cmp = g_.node(s2.guard_if).in[1];
            if (old_cmp != kNoNode && !g_.is_dead(old_cmp) &&
                g_.node(old_cmp).op == Op::Cmp) {
                bool live_user = false;
                for (NodeId cu : g_.uses_of(old_cmp))
                    if (!g_.is_dead(cu)) live_user = true;
                if (!live_user) g_.kill(old_cmp);
            }
        }
        g_.kill(s2.exit_proj);
        g_.kill(b2_first);
        g_.kill(s2.iv_phi);
        g_.touch();
        return true;
    }

    Graph& g_;
    LoopInfo& li_;
    DomTree& dom_;
    AliasInfo& aa_;
    FlatMap<NodeId, bool> seen_;
};

} // namespace

class LoopFusionPass : public Pass {
public:
    const char* name() const override { return "LoopFusion"; }
    int order() const override { return 46; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo | AnalysisKind::AliasInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo | AnalysisKind::AliasInfo |
               AnalysisKind::MemDep;
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Graph& g = fg.g;
            auto li = LoopInfo::compute(g, ctx.analysis.doms(fg));
            LoopFuser f(g, *li, ctx.analysis.doms(fg), ctx.analysis.alias(fg));
            changed |= f.run() > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(LoopFusionPass, 46, "Phase 4")

} // namespace jules
