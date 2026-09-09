// Implementation of loop_transforms.h (shared by passes 42/44).
//
// The body cloner duplicates a loop's body blocks `extra` times as a
// trailing chain. Control/data wiring, concretely:
//   BEFORE:  guard-body-projection P -> body_0 ... latch J_0 -> header
//   AFTER:   P -> body_0 ... J_0 -> E_1 -> body_1 ... J_1 -> ... -> J_x -> header
// Each copy m (1..extra) sees the phi values of dynamic iteration base+m:
// the seed map for copy m maps every header phi to copy (m-1)'s clone of
// that phi's update value (copy 0 = the original body). The header's
// latch predecessor and every phi backedge input are retargeted to the
// last copy after the chain is built.
#include "core/son/passes/loop_transforms.h"

namespace jules {
namespace loopx {

namespace {

bool phi_at_header(Graph& g, NodeId n, NodeId header) {
    return n != kNoNode && !g.is_dead(n) && g.node(n).op == Op::Phi &&
           g.node(n).in[0] == header;
}

bool iv_update_shape(Graph& g, NodeId update, NodeId iv_phi, i64& step) {
    if (update == kNoNode || g.is_dead(update)) return false;
    const Node& u = g.node(update);
    if (u.op != Op::Bin || static_cast<BinOp>(u.sub) != BinOp::Add) return false;
    ConstVal c;
    if (u.in[1] == iv_phi && const_of(g, u.in[2], c)) { step = c.iv; return true; }
    if (u.in[2] == iv_phi && const_of(g, u.in[1], c)) { step = c.iv; return true; }
    return false;
}

bool is_head_op(Op o) {
    return o == Op::Jump || o == Op::Region || o == Op::IfTrue || o == Op::IfFalse;
}

} // namespace

bool match_counted(Graph& g, LoopInfo& li, DomTree& dom, const Loop& l,
                   CountedLoop& out) {
    (void)dom;
    if (l.blocks.size() < 2) return false;
    NodeId header = l.header;
    const Node& h = g.node(header);
    if (h.op != Op::Region || h.n_in != 2) return false;

    // Packed (vectorized) loops are owned by pass 56: the scalar unroller/
    // peeler would re-thread their phis with scalar-clone seeds and the
    // double transform corrupts the lane plumbing. Vector loops get their
    // ILP from the packed width; scalar unrolling adds nothing below 128-bit.
    for (NodeId u : g.uses_of(header)) {
        if (g.is_dead(u)) continue;
        if (ty_is_vector(g.node(u).ty)) return false; // packed phi (vacc etc.)
    }
    for (NodeId blk : l.blocks) {
        for (NodeId u : g.uses_of(blk)) {
            if (g.is_dead(u) || g.node(u).in[0] != blk) continue;
            if (ty_is_vector(g.node(u).ty)) return false; // packed body op
        }
    }

    u8 entry_slot = 2, latch_slot = 2;
    for (u8 i = 0; i < 2; ++i) {
        NodeId p = h.in[i];
        bool in_loop = li.block_in_loop(l, p);
        if (in_loop) latch_slot = i; else entry_slot = i;
    }
    if (entry_slot == 2 || latch_slot == 2 || entry_slot == latch_slot) return false;

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

    NodeId iv = kNoNode, bound = kNoNode;
    if (phi_at_header(g, cmp.in[1], header)) { iv = cmp.in[1]; bound = cmp.in[2]; }
    else if (phi_at_header(g, cmp.in[2], header)) { iv = cmp.in[2]; bound = cmp.in[1]; }
    if (iv == kNoNode) return false;
    ConstVal bound_c;
    if (!const_of(g, bound, bound_c)) return false;
    CmpOp rel = static_cast<CmpOp>(cmp.sub);

    const Node& ivn = g.node(iv);
    ConstVal init_c;
    if (!const_of(g, ivn.in[entry_slot + 1], init_c)) return false;
    i64 step = 0;
    if (!iv_update_shape(g, ivn.in[latch_slot + 1], iv, step)) return false;
    if (step <= 0) return false;
    bool lt_form = false;
    switch (rel) {
        case CmpOp::Lt: case CmpOp::Gt: lt_form = true; break;
        case CmpOp::Le: case CmpOp::Ge: lt_form = false; break;
        default: return false;
    }
    // Trip count. Lt (i < B, i += k): iterations = (B - init)/k. Le
    // (i <= B, i += k): iterations = (B - init)/k + 1 — the span must
    // count the inclusive bound, i.e. bound - init + step, NOT bound -
    // init - 1 (the old formula under-counted by two iterations and
    // every const-trip `<=` loop that unrolled executed its body two
    // extra times — sumle(9) returned 66, not 45; found by the PGO
    // session's Le-relation test, regression-locked by t37). Gt/Ge are
    // the countdown forms: span goes negative and the clamp rejects.
    i64 span = bound_c.iv - init_c.iv + (lt_form ? 0 : step);
    if (span < 0) span = 0;
    if (span % step != 0) return false;

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
    out.iv_step = step;
    out.trip = span / step;
    out.blocks.clear();
    out.body_nodes = 0;
    for (NodeId blk : l.blocks) {
        if (blk == header) continue;
        out.blocks.push_back(blk);
        for (NodeId u : g.uses_of(blk)) {
            if (g.node(u).in[0] == blk && !is_control_op(g.node(u).op) &&
                !is_head_op(g.node(u).op))
                ++out.body_nodes;
        }
    }
    out.phis.clear();
    for (NodeId u : g.uses_of(header))
        if (g.node(u).op == Op::Phi) out.phis.push_back(u);
    return true;
}

namespace {

struct Cloner {
    Graph& g;
    const CountedLoop& cl;
    FlatMap<NodeId, NodeId> vmap;   // value node -> clone (this copy)
    FlatMap<NodeId, NodeId> blkmap; // block head -> clone head (this copy)
    NodeId entry;                   // this copy's entry Jump

    NodeId remap(NodeId n) const {
        if (n == kNoNode) return kNoNode;
        if (const NodeId* m = vmap.find(n)) return *m;
        if (const NodeId* m = blkmap.find(n)) return *m;
        return n; // invariant/external: unchanged
    }

    NodeId clone_node(NodeId u, NodeId pin) {
        Node un = g.node(u); // COPY: make_arr below grows nodes_ and would
                             // invalidate any reference into the vector
                             // (use-after-free; caught by ASAN on t24)
        NodeId ins[kMaxInputs];
        ins[0] = pin;
        for (u8 i = 1; i < un.n_in; ++i) ins[i] = remap(un.in[i]);
        NodeId c = g.make_arr(un.op, un.ty, ins, un.n_in, un.sub, un.aux);
        Node& cn = g.node(c);
        cn.ival = un.ival;
        cn.fval = un.fval;
        cn.flags = un.flags;
        vmap.insert(u, c);
        if (un.op == Op::Return) g.append_input(g.stop(), c);
        return c;
    }

    // Clone all non-head nodes pinned at block `old_blk` onto `new_blk`,
    // in DEPENDENCY order: a node whose in-block inputs are not yet cloned
    // waits for a later sweep (users lists are arbitrary order; cloning an
    // Add before the Mul it reads would remap the Mul to the ORIGINAL).
    void clone_block_contents(NodeId old_blk, NodeId new_blk, u32& count) {
        std::vector<NodeId> pinned;
        const SmallVec<NodeId, 4> users = g.uses_of(old_blk);
        for (NodeId u : users) {
            if (u == new_blk) continue;
            const Node& un = g.node(u);
            if (un.in[0] != old_blk) continue;
            if (is_head_op(un.op) || un.op == Op::Region) continue;
            pinned.push_back(u);
        }
        FlatMap<NodeId, bool> done;
        bool progress = true;
        while (progress && done.size() < pinned.size()) {
            progress = false;
            for (NodeId u : pinned) {
                if (done.contains(u)) continue;
                const Node& un = g.node(u);
                bool ready = true;
                for (u8 i = 1; i < un.n_in; ++i) {
                    NodeId d = un.in[i];
                    if (d == kNoNode || d == u) continue;
                    bool in_block = false;
                    for (NodeId q : pinned)
                        if (q == d) { in_block = true; break; }
                    if (in_block && !done.contains(d)) { ready = false; break; }
                }
                if (!ready) continue;
                clone_node(u, new_blk);
                done.insert(u, true);
                ++count;
                progress = true;
            }
        }
    }

    NodeId clone_head(NodeId b) {
        Node bn = g.node(b); // copy (vector growth below)
        NodeId ins[kMaxInputs];
        for (u8 i = 0; i < bn.n_in; ++i) ins[i] = remap(bn.in[i]);
        NodeId c = g.make_arr(bn.op, bn.ty, ins, bn.n_in, bn.sub, bn.aux);
        Node& cn = g.node(c);
        cn.ival = bn.ival;
        cn.fval = bn.fval;
        cn.flags = bn.flags;
        blkmap.insert(b, c);
        return c;
    }
};

// Body block order: control reachability from the body projection,
// predecessor-before-successor. Projections follow their If; Regions are
// deferred until all their preds are placed.
std::vector<NodeId> body_order(Graph& g, const CountedLoop& cl) {
    FlatMap<NodeId, u32> placed;
    std::vector<NodeId> order;
    std::vector<NodeId> pending = cl.blocks; // worklist of heads
    bool progress = true;
    while (progress && order.size() < cl.blocks.size()) {
        progress = false;
        for (NodeId b : pending) {
            if (placed.contains(b)) continue;
            const Node& bn = g.node(b);
            if (b == cl.body_proj) {
                placed.insert(b, 1); order.push_back(b); progress = true; continue;
            }
            bool ready = true;
            if (bn.op == Op::Region) {
                for (u8 i = 0; i < bn.n_in; ++i)
                    if (bn.in[i] != kNoNode && !placed.contains(bn.in[i]) &&
                        std::find(cl.blocks.begin(), cl.blocks.end(), bn.in[i]) !=
                            cl.blocks.end())
                        { ready = false; break; }
            } else if (bn.op == Op::IfTrue || bn.op == Op::IfFalse) {
                // the projection's input is an internal If; the If is
                // PINNED at a block — that block must be placed first
                NodeId if_pin = g.node(bn.in[0]).in[0];
                if (if_pin != cl.body_proj && !placed.contains(if_pin)) ready = false;
            } else {
                NodeId ctrl = bn.in[0];
                if (ctrl != kNoNode && ctrl != cl.body_proj &&
                    std::find(cl.blocks.begin(), cl.blocks.end(), ctrl) !=
                        cl.blocks.end() &&
                    !placed.contains(ctrl))
                    ready = false;
            }
            if (!ready) continue;
            placed.insert(b, 1);
            order.push_back(b);
            progress = true;
        }
    }
    // safety: append anything unreachable by the ordering (should not
    // happen for matched loops)
    for (NodeId b : cl.blocks)
        if (!placed.contains(b)) order.push_back(b);
    return order;
}

} // namespace

u32 clone_body_chain(Graph& g, const CountedLoop& cl, u32 extra, bool at_entry) {
    if (extra == 0 || cl.blocks.empty()) return 0;

    std::vector<NodeId> order = body_order(g, cl);
    NodeId header_latch = g.node(cl.header).in[cl.latch_slot]; // J_0
    if (header_latch == kNoNode || g.is_dead(header_latch)) return 0;

    // Seed bookkeeping: copy m's seed is copy (m-1)'s clone of the phi's
    // ORIGINAL seed input at the attach slot. Chaining through the original
    // ids keeps remap() hits (its maps are keyed by original ids): chaining
    // the live CLONE ids left trailing copies 3+ seeded from copy 1
    // (observed: 4x-unrolled print loop duplicating its third lane's value
    // and memory effects skipping a chain link).
    std::vector<NodeId> live = cl.phis;
    std::vector<NodeId> live_vals;
    std::vector<NodeId> seed_orig;
    u8 seed_slot = at_entry ? cl.entry_slot : cl.latch_slot;
    for (NodeId phi : live) {
        NodeId v = g.node(phi).in[seed_slot + 1];
        live_vals.push_back(v);
        seed_orig.push_back(v);
    }
    NodeId prev_latch = at_entry ? g.node(cl.header).in[cl.entry_slot] : header_latch;

    for (u32 copy = 1; copy <= extra; ++copy) {
        Cloner c{g, cl, {}, {}, kNoNode};
        // seeds: header phis -> previous copy's values
        for (size_t i = 0; i < live.size(); ++i) c.vmap.insert(live[i], live_vals[i]);
        // the copy's entry block: a fresh Jump fed by the previous latch
        c.entry = g.make(Op::Jump, ty_ctrl(), {prev_latch});
        c.blkmap.insert(cl.body_proj, c.entry);

        u32 copied = 0;
        for (NodeId b : order) {
            if (b == cl.body_proj) {
                c.clone_block_contents(b, c.entry, copied);
            } else {
                const Node& bn = g.node(b);
                NodeId nb;
                if (bn.op == Op::IfTrue || bn.op == Op::IfFalse) {
                    // projection of an internal If: its input is the If's
                    // CLONE (the If was pinned at an earlier block)
                    NodeId ifn = bn.in[0];
                    NodeId cif = c.remap(ifn);
                    nb = g.make(bn.op, bn.ty, {cif});
                    c.blkmap.insert(b, nb);
                } else {
                    nb = c.clone_head(b);
                }
                (void)nb;
                c.clone_block_contents(b, nb, copied);
            }
        }

        // next copy's seed: this copy's clone of the ORIGINAL seed value
        // (remap-of-original: the copy's vmap keys originals)
        for (size_t i = 0; i < live.size(); ++i)
            live_vals[i] = c.remap(seed_orig[i]);
        prev_latch = c.remap(header_latch);
    }

    // rewire the loop: the attach-point pred and the matching phi inputs
    // now come from the last copy
    u8 attach_slot = at_entry ? cl.entry_slot : cl.latch_slot;
    g.set_input(cl.header, attach_slot, prev_latch);
    for (size_t i = 0; i < live.size(); ++i)
        g.set_input(live[i], attach_slot + 1, live_vals[i]);

    return extra * (cl.body_nodes + cl.blocks.size());
}

} // namespace loopx
} // namespace jules
