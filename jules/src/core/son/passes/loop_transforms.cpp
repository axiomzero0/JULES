// Implementation of loop_transforms.h (shared by passes 42/43/44).
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
//
// NESTED-LOOP CYCLES (the t_dp_lea2 miscompile): a body containing an
// inner loop is a CYCLE — the inner header Region's latch pred is
// downstream of the projections of its own guard. Two rules make the
// cloner cycle-correct:
//   1. body_order never deadlocks: a Region waits only for preds that
//      are NOT reachable from the Region itself (backedge preds do not
//      block placement; they are cloned later in the sweep).
//   2. remap() misses on in-body nodes are DEFERRED: the clone is built
//      with the original as a placeholder, and every deferred input is
//      patched to the proper clone after the whole copy is swept. The
//      old cloner silently wired such inputs to the ORIGINAL nodes —
//      the cloned inner loop lost its backedge, its body died, and the
//      outer loop kept running empty guard shells that summed a quarter
//      of the iterations (found by mini3: fill + RMW + 2D sum printing
//      3066 instead of 12282; the asm showed copies 2-4 of the outer
//      unroll as cmp/jge shells with no bodies).
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
    // Deopt-guard ladders (pass 91) are family-owned: the cloner re-threads
    // merge-region phis per copy, which severs the fallback rung's merge
    // (observed: the generic rung cut out of the loop's ladder). Loops
    // containing a guard site stay scalar-shaped; their ILP comes from
    // the specialization itself, not from unrolling.
    for (NodeId blk : l.blocks) {
        for (NodeId u : g.uses_of(blk)) {
            if (g.is_dead(u)) continue;
            if ((g.node(u).flags & kFlagGuardSite) != 0) return false;
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

    // In-body originals (blocks + nodes pinned at them), shared across
    // copies. remap() misses on these are backedges / forward refs within
    // the copy: defer, patch post-sweep. Misses on anything else are loop
    // invariants: the original is correct.
    const FlatMap<NodeId, bool>* in_body = nullptr;

    struct Deferred {
        NodeId node; // the clone holding a placeholder input
        u8 slot;
        NodeId orig; // the in-body original not yet cloned at defer time
    };
    std::vector<Deferred> deferred_;

    bool body_has(NodeId n) const {
        return n != kNoNode && in_body && in_body->contains(n);
    }

    NodeId remap(NodeId n) const {
        if (n == kNoNode) return kNoNode;
        if (const NodeId* m = vmap.find(n)) return *m;
        if (const NodeId* m = blkmap.find(n)) return *m;
        return n; // invariant/external: unchanged
    }

    // After the copy is swept, every deferred input is re-remapped with
    // the completed maps. Cycle-correct by construction: backedge
    // originals are cloned later in the sweep, so the patch lands on the
    // copy's own latch/phi-update clones.
    void patch_deferred() {
        for (const Deferred& d : deferred_) {
            NodeId m = remap(d.orig);
            if (m != d.orig) g.set_input(d.node, d.slot, m);
            // else: unreachable given the ordering gate plus the
            // unconditional content sweep (every in-body original is
            // cloned exactly once per copy); keeping the original would
            // reproduce the old broken wiring, so this stays loud in the
            // comment rather than silent in the code.
        }
        deferred_.clear();
    }

    void defer_misses(const Node& un, NodeId clone, u8 from_slot) {
        for (u8 i = from_slot; i < un.n_in; ++i) {
            NodeId o = un.in[i];
            if (o == kNoNode) continue;
            NodeId got = remap(o);
            if (got == o && body_has(o)) deferred_.push_back(Deferred{clone, i, o});
        }
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
        defer_misses(un, c, 1);
        return c;
    }

    // Clone all non-head nodes pinned at block `old_blk` onto `new_blk`.
    // Single pass: intra-block order does not matter — an input whose
    // producer is cloned later in the pass defers and is patched after
    // the sweep. (The old dependency-progress loop existed so remap()
    // would hit in order; deferral removes that requirement — and the
    // old loop could never have handled a cyclic body anyway.)
    void clone_block_contents(NodeId old_blk, NodeId new_blk, u32& count) {
        const SmallVec<NodeId, 4> users = g.uses_of(old_blk);
        for (NodeId u : users) {
            if (u == new_blk) continue;
            const Node& un = g.node(u);
            if (un.in[0] != old_blk) continue;
            if (is_head_op(un.op) || un.op == Op::Region) continue;
            clone_node(u, new_blk);
            ++count;
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
        defer_misses(bn, c, 0);
        return c;
    }
};

// Body block order: control reachability from the body projection,
// predecessor-before-successor. Projections follow their If; Regions wait
// only for FORWARD preds — a pred reachable FROM the Region (a backedge
// into a nested loop header, or an irreducible cycle) does not block
// placement: it is cloned later in the sweep and its input is patched via
// the deferred mechanism.
//
// Returns FEWER than cl.blocks.size() entries when the body cannot be
// linearized; callers treat that as "do not transform". No safety-append:
// an arbitrary append order is exactly how the nested-loop miscompile
// was born.
std::vector<NodeId> body_order(Graph& g, const CountedLoop& cl) {
    auto in_blocks = [&](NodeId b) {
        return std::find(cl.blocks.begin(), cl.blocks.end(), b) != cl.blocks.end();
    };

    // Body-internal successor edges: B -> H when control flows B to H
    // (Region pred lists, Jump ctrl inputs, projection pin blocks).
    FlatMap<NodeId, std::vector<NodeId>> succ;
    auto add_edge = [&](NodeId from, NodeId to) {
        if (from == to || !in_blocks(from) || !in_blocks(to)) return;
        if (std::vector<NodeId>* v = succ.find(from)) {
            if (std::find(v->begin(), v->end(), to) == v->end()) v->push_back(to);
        } else {
            succ.insert(from, std::vector<NodeId>{to});
        }
    };
    for (NodeId b : cl.blocks) {
        const Node& bn = g.node(b);
        switch (bn.op) {
            case Op::Region:
                for (u8 i = 0; i < bn.n_in; ++i)
                    if (bn.in[i] != kNoNode) add_edge(bn.in[i], b);
                break;
            case Op::Jump:
                if (bn.in[0] != kNoNode) add_edge(bn.in[0], b);
                break;
            case Op::IfTrue:
            case Op::IfFalse: {
                NodeId if_pin = g.node(bn.in[0]).in[0];
                if (if_pin != kNoNode) add_edge(if_pin, b);
                break;
            }
            default:
                break;
        }
    }
    // Per-Region downstream set (backedge-pred detection). Bodies are
    // small (match_counted's 60-node guard): the O(B^2) scan is fine.
    FlatMap<NodeId, std::vector<NodeId>> down_of;
    auto downstream = [&](NodeId r) -> const std::vector<NodeId>& {
        if (const std::vector<NodeId>* v = down_of.find(r)) return *v;
        std::vector<NodeId> seen;
        std::vector<NodeId> work{r};
        while (!work.empty()) {
            NodeId n = work.back();
            work.pop_back();
            const std::vector<NodeId>* s = succ.find(n);
            if (!s) continue;
            for (NodeId t : *s) {
                if (std::find(seen.begin(), seen.end(), t) == seen.end()) {
                    seen.push_back(t);
                    work.push_back(t);
                }
            }
        }
        down_of.insert(r, std::move(seen));
        return *down_of.find(r);
    };

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
                const std::vector<NodeId>& down = downstream(b);
                for (u8 i = 0; i < bn.n_in; ++i) {
                    NodeId p = bn.in[i];
                    if (p == kNoNode || placed.contains(p)) continue;
                    if (!in_blocks(p)) continue; // external pred
                    if (std::find(down.begin(), down.end(), p) != down.end())
                        continue;                // backedge pred: cloned later
                    ready = false; break;
                }
            } else if (bn.op == Op::IfTrue || bn.op == Op::IfFalse) {
                // the projection's input is an internal If; the If is
                // PINNED at a block — that block must be placed first.
                // No body_proj special case here (the Jump one below is
                // safe because clone_head DEFERS unremapped inputs; the
                // projection head-clone has no deferral, so a projection
                // swept before its pin's contents clones against the
                // ORIGINAL If — observed on the empty false arm of an
                // in-body diamond: three IfFalse copies all pointing at
                // the original If, the joins fed by the wrong path, and
                // the loop miscompiled under -O2).
                NodeId if_pin = g.node(bn.in[0]).in[0];
                if (if_pin != kNoNode && in_blocks(if_pin) &&
                    !placed.contains(if_pin))
                    ready = false;
            } else {
                NodeId ctrl = bn.in[0];
                if (ctrl != kNoNode && ctrl != cl.body_proj && in_blocks(ctrl) &&
                    !placed.contains(ctrl))
                    ready = false;
            }
            if (!ready) continue;
            placed.insert(b, 1);
            order.push_back(b);
            progress = true;
        }
    }
    return order;
}

} // namespace

u32 clone_body_chain(Graph& g, const CountedLoop& cl, u32 extra, bool at_entry) {
    if (extra == 0 || cl.blocks.empty()) return 0;

    std::vector<NodeId> order = body_order(g, cl);
    // ORDERING GATE: a body that cannot be linearized is not transformed
    // at all. This runs BEFORE any mutation — a partially-rewired clone
    // chain is exactly the failure mode being guarded against.
    if (order.size() != cl.blocks.size()) return 0;
    NodeId header_latch = g.node(cl.header).in[cl.latch_slot]; // J_0
    if (header_latch == kNoNode || g.is_dead(header_latch)) return 0;

    // In-body node set (blocks + everything pinned at them), shared by
    // all copies: remap() misses on these defer; misses on anything else
    // are loop invariants and stay original.
    FlatMap<NodeId, bool> in_body;
    for (NodeId b : cl.blocks) {
        in_body.insert(b, true);
        for (NodeId u : g.uses_of(b))
            if (g.node(u).in[0] == b) in_body.insert(u, true);
    }

    // Seed bookkeeping: copy m's seed is copy (m-1)'s clone of the phi's
    // ORIGINAL seed input at the attach slot. Chaining through the original
    // ids keeps remap() hits (its maps are keyed by original ids): chaining
    // the live CLONE ids left trailing copies 3+ seeded from copy 1
    // (observed: 4x-unrolled print loop duplicating its third lane's value
    // and memory effects skipping a chain link).
    std::vector<NodeId> live = cl.phis;
    std::vector<NodeId> live_vals;
    std::vector<NodeId> seed_orig;
    std::vector<NodeId> latch_orig; // the phis' in-body updates (IV step,
                                    // memory chain) — the peeling chain seed
    u8 seed_slot = at_entry ? cl.entry_slot : cl.latch_slot;
    for (NodeId phi : live) {
        NodeId v = g.node(phi).in[seed_slot + 1];
        live_vals.push_back(v);
        seed_orig.push_back(v);
        latch_orig.push_back(g.node(phi).in[cl.latch_slot + 1]);
    }
    NodeId prev_latch = at_entry ? g.node(cl.header).in[cl.entry_slot] : header_latch;

    for (u32 copy = 1; copy <= extra; ++copy) {
        Cloner c{g, cl, {}, {}, kNoNode};
        c.in_body = &in_body;
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
                    // CLONE (the If was pinned at an earlier block — the
                    // ordering guarantees the pin was swept first)
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
        c.patch_deferred(); // cycle inputs: originals -> this copy's clones

        // next copy's seed: this copy's clone of the ORIGINAL seed value
        // (remap-of-original: the copy's vmap keys originals). PEELING
        // chains through the LATCH value — the in-body update — because
        // copy m+1's entry seed is copy m's COMPUTED value; the entry-side
        // seed is an external init constant that never remaps, so chaining
        // through it re-ran iteration 0 in every peeled copy (observed:
        // const-trip store loop writing a[0] six times, residual loop
        // restarting its IV at init). The unroll path keeps the latch
        // seed as before.
        for (size_t i = 0; i < live.size(); ++i)
            live_vals[i] = c.remap(at_entry ? latch_orig[i] : seed_orig[i]);
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
