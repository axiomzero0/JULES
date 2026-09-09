// Pass 56 — LoopVectorizer (Phase 5: Vectorization & Superword Parallelism)
//
// Top-down vectorization of counted loops with contiguous unit-stride array
// accesses (SSE2 128-bit baseline: 2 lanes x 8B, 4 lanes x 4B):
//
//   [preheader] --guard(bound >= VF)--> [vector loop] --exit--\
//        \----- guard false --------------------------------> [merge]
//   vector loop: k = 0..nv (nv = bound / VF), one packed iteration
//   computes VF scalar iterations of the ORIGINAL body.
//   [merge] feeds the ORIGINAL loop, which stays intact as the scalar
//   remainder: its iv entry becomes Phi(merge, [init, k*VF]) and the
//   reduction entry becomes Phi(merge, [init, horizontal(vacc)]).
//
// Body mapping (see vecx:: for shared width/legality/cost decisions):
//   Load(a[i])   -> packed Load(vty, a + k*16)
//   Store(a[i])  -> packed Store, same base
//   Bin over packed values -> packed Bin (SSE2-legal ops only)
//   loop-invariant scalar operands -> Broadcast
//   acc = phi(init, acc op load) -> vector phi + horizontal lane tree at
//     the exit (integer: exact; FP: gated on --fp=fast because the lane
//     tree reassociates the reduction order)
// Any other shape (iv-derived stored values, calls, inner branches, a
// second loop-carried phi, non-unit strides) makes the loop SKIP.
#include "core/son/passes/vector_utils.h"

#include <cstdio>
#include <cstdlib>

namespace jules {

namespace {

struct VecLoop {
    NodeId header = kNoNode;
    u8 entry_slot = 0, latch_slot = 1;
    NodeId guard_if = kNoNode;
    NodeId body_proj = kNoNode;
    NodeId exit_proj = kNoNode;
    NodeId iv_phi = kNoNode;
    NodeId bound = kNoNode; // i < bound (bound: const or dynamic, i32/i64)
    TypeId iv_ty = ty_i64();
    i64 trip = -1; // const trip (bound - 0), -1 = dynamic
    NodeId body_proj_ctrl = kNoNode;
    std::vector<NodeId> body; // non-control nodes pinned at the body block
};

bool match_vec_loop(Graph& g, LoopInfo& li, const Loop& l, VecLoop& out) {
    if (l.blocks.size() != 2) return false; // header + exactly one body block
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
    CmpOp rel = static_cast<CmpOp>(cmp.sub);
    // canonical `iv < bound` or mirrored `bound > iv` (reassociation and
    // branch inversion rewrite guards to the constant-on-left form)
    bool mirrored = false;
    if (rel != CmpOp::Lt && rel != CmpOp::Le) {
        if (rel == CmpOp::Gt || rel == CmpOp::Ge) mirrored = true;
        else return false;
    }

    NodeId iv = kNoNode, bound = kNoNode;
    bool iv_on_lhs = g.node(cmp.in[1]).op == Op::Phi && g.node(cmp.in[1]).in[0] == header;
    bool iv_on_rhs = g.node(cmp.in[2]).op == Op::Phi && g.node(cmp.in[2]).in[0] == header;
    if (iv_on_lhs && !iv_on_rhs) {
        iv = cmp.in[1];
        bound = cmp.in[2];
        if (mirrored) return false; // (bound > iv) has iv on the RIGHT
    } else if (iv_on_rhs && !iv_on_lhs) {
        iv = cmp.in[2];
        bound = cmp.in[1];
        if (!mirrored) return false; // (iv < bound) has iv on the LEFT
    } else {
        return false; // both phis or neither
    }
    if (iv == kNoNode || bound == kNoNode || g.is_dead(bound)) return false;

    const Node& ivn = g.node(iv);
    if (ivn.ty != ty_i64() && ivn.ty != ty_i32()) return false;
    // init == 0, step == +1: the vector path covers [0, k*VF) exactly
    ConstVal init_c, step_c;
    if (!const_of(g, ivn.in[entry_slot + 1], init_c) || init_c.iv != 0) return false;
    NodeId upd = ivn.in[latch_slot + 1];
    if (g.is_dead(upd)) return false;
    const Node& un = g.node(upd);
    if (un.op != Op::Bin || static_cast<BinOp>(un.sub) != BinOp::Add) return false;
    if (un.in[1] == iv && const_of(g, un.in[2], step_c)) { /* canonical */ }
    else if (un.in[2] == iv && const_of(g, un.in[1], step_c)) { /* mirrored */ }
    else return false;
    if (step_c.iv != 1) return false;
    // the guard compares the SAME type on both sides
    if (g.node(bound).ty != ivn.ty) return false;

    NodeId body_proj = kNoNode, exit_proj = kNoNode;
    for (NodeId u : g.uses_of(guard_if)) {
        Op o = g.node(u).op;
        if (o != Op::IfTrue && o != Op::IfFalse) continue;
        if (li.block_in_loop(l, u)) body_proj = u;
        else exit_proj = u;
    }
    if (body_proj == kNoNode || exit_proj == kNoNode) return false;

    // The preheader (entry pred block) must not already end in a branch:
    // the vector guard If needs to become its terminator.
    NodeId entry_head = h.in[entry_slot];
    if (entry_head == kNoNode || g.is_dead(entry_head)) return false;
    for (NodeId u : g.uses_of(entry_head)) {
        if (g.is_dead(u)) continue;
        if (g.node(u).op == Op::If && g.node(u).in[0] == entry_head) return false;
    }

    std::vector<NodeId> body;
    for (NodeId u : g.uses_of(body_proj)) {
        if (g.is_dead(u) || g.node(u).in[0] != body_proj) continue;
        if (is_control_op(g.node(u).op) || is_block_head(g.node(u).op)) continue;
        body.push_back(u);
    }
    std::sort(body.begin(), body.end());

    out.header = header;
    out.entry_slot = entry_slot;
    out.latch_slot = latch_slot;
    out.guard_if = guard_if;
    out.body_proj = body_proj;
    out.exit_proj = exit_proj;
    out.iv_phi = iv;
    out.bound = bound;
    out.iv_ty = ivn.ty;
    ConstVal bnd;
    out.trip = const_of(g, bound, bnd) ? bnd.iv - (rel == CmpOp::Le ? 1 : 0) : -1;
    out.body = std::move(body);
    return true;
}

class LoopVectorizer {
public:
    LoopVectorizer(Graph& g, LoopInfo& li, FpMode fp, bool size_biased)
        : g_(g), li_(li), fp_(fp), size_biased_(size_biased) {}

    u32 vectorize_all() {
        // innermost-first loop order (LoopInfo sorts by body size asc)
        for (const Loop& l : li_.loops()) {
            VecLoop vl;
            if (!match_vec_loop(g_, li_, l, vl)) {
                if (getenv("JULES_DEBUG_VEC"))
                    fprintf(stderr, "[vec] no-match loop n%u\n", l.header);
                continue;
            }
            if (analyze_body(vl)) {
                build_vector_loop(vl);
            } else {
                ++skipped_;
                if (getenv("JULES_DEBUG_VEC"))
                    fprintf(stderr, "[vec] skip loop n%u: %s\n", vl.header, skip_reason_);
            }
        }
        return built_;
    }

    u32 skipped() const { return skipped_; }

private:
    // ---- phase 1: classify every body node ---------------------------------
    bool analyze_body(const VecLoop& vl) {
        skip_reason_ = "shape";

        // Integer-Add reduction normalization: reassociation (level-
        // dependent canonical order) may leave the phi nested one level
        // inside the update — update = Add(Add(phi, X), Y). Rewriting to
        // Add(phi, Add(X, Y)) is exact for integers (associativity) and
        // gives the reduction matcher the canonical shape. FP stays
        // untouched (rounding order is semantic under strict).
        for (NodeId u : g_.uses_of(vl.header)) {
            if (g_.is_dead(u) || g_.node(u).op != Op::Phi) continue;
            Node& p = g_.node(u);
            if (p.ty != ty_i64() && p.ty != ty_i32()) continue;
            if (p.n_in != 3) continue;
            NodeId upd_id = p.in[2];
            if (upd_id == kNoNode || g_.is_dead(upd_id)) continue;
            Node upd = g_.node(upd_id);
            if (upd.op != Op::Bin || static_cast<BinOp>(upd.sub) != BinOp::Add) continue;
            bool phi_direct = upd.in[1] == u || upd.in[2] == u;
            if (phi_direct) continue;
            // find the operand that is an Add containing the phi
            for (u8 side = 1; side <= 2; ++side) {
                NodeId a_id = upd.in[side];
                if (a_id == kNoNode || g_.is_dead(a_id)) continue;
                Node a = g_.node(a_id);
                if (a.op != Op::Bin || static_cast<BinOp>(a.sub) != BinOp::Add) continue;
                if (a.in[1] != u && a.in[2] != u) continue;
                NodeId x = (a.in[1] == u) ? a.in[2] : a.in[1];
                NodeId y = upd.in[side == 1 ? 2 : 1];
                // inner = X + Y (pinned where the update was)
                NodeId inner = g_.make(Op::Bin, p.ty, {upd.in[0], x, y},
                                       static_cast<u8>(BinOp::Add));
                // new update = phi + inner
                NodeId newupd = g_.make(Op::Bin, p.ty, {upd.in[0], u, inner},
                                        static_cast<u8>(BinOp::Add));
                g_.set_input(u, 2, newupd);
                g_.touch();
                break;
            }
        }

        // (re)collect body nodes: the normalization above may have created
        // new pinned nodes the matcher's snapshot never saw
        body_.clear();
        for (NodeId n : g_.uses_of(vl.body_proj)) {
            if (g_.is_dead(n) || g_.node(n).in[0] != vl.body_proj) continue;
            if (is_control_op(g_.node(n).op) || is_block_head(g_.node(n).op)) continue;
            body_.push_back(n);
        }
        std::sort(body_.begin(), body_.end());

        // Every header phi must be the iv, the mem phi, ONE reduction, or a
        // loop-INVARIANT pass-through (SROA's value phis for pointer locals
        // that never change inside the loop: inputs are the phi itself or
        // values defined outside the loop).
        reduction_phi_ = kNoNode;
        for (NodeId u : g_.uses_of(vl.header)) {
            if (g_.is_dead(u) || g_.node(u).op != Op::Phi) continue;
            if (u == vl.iv_phi) continue;
            if (g_.node(u).ty == ty_mem()) continue;
            if (reduction_phi_ == kNoNode &&
                vecx::match_reduction(g_, u, vl.header, red_)) {
                reduction_phi_ = u;
                continue;
            }
            if (passthrough_phi(u, vl)) continue;
            skip_reason_ = "second-loop-carried-phi";
            return false;
        }

        loads_.clear();
        stores_.clear();
        pure_.clear();
        addr_pieces_.clear();
        elem_ty_ = ty_none();

        // address chains of all memory ops are rebuilt, not mapped — mark
        // them BEFORE classification (id order visits the index Shl before
        // the Load it feeds)
        auto mark_addr_chain = [&](NodeId mem_op) {
            std::vector<NodeId> stack{g_.node(mem_op).in[2]};
            while (!stack.empty()) {
                NodeId cur = stack.back();
                stack.pop_back();
                if (cur == kNoNode || g_.is_dead(cur)) continue;
                const Node& cn = g_.node(cur);
                if (cn.op == Op::Cast || cn.op == Op::Bin) {
                    addr_pieces_.insert(cur, true);
                    for (u8 i = 1; i < cn.n_in; ++i) stack.push_back(cn.in[i]);
                }
            }
        };
        for (NodeId n : body_) {
            Op o = g_.node(n).op;
            if (o == Op::Load || o == Op::Store) mark_addr_chain(n);
        }

        // phi-update nodes are loop machinery (iv/reduction backedges), not
        // body computation to pack
        NodeId iv_update = g_.node(vl.iv_phi).in[vl.latch_slot + 1];
        NodeId red_update = kNoNode;
        if (reduction_phi_ != kNoNode)
            red_update = g_.node(reduction_phi_).in[g_.node(reduction_phi_).n_in - 1];

        for (NodeId n : body_) {
            const Node& nd = g_.node(n);
            if (addr_pieces_.contains(n)) continue; // rebuilt by vec_addr
            if (n == iv_update || n == red_update) continue; // loop machinery
            switch (nd.op) {
                case Op::Load: {
                    vecx::AddrPattern ap = vecx::match_addr(g_, nd.in[2]);
                    if (!ap.ok || ap.idx != vl.iv_phi) {
                        skip_reason_ = "load-not-unit-stride";
                        return false;
                    }
                    if (!register_elem(nd.ty, ap.esz)) return false;
                    loads_.push_back(n);
                    load_addr_.insert(n, ap);
                    break;
                }
                case Op::Store: {
                    vecx::AddrPattern ap = vecx::match_addr(g_, nd.in[2]);
                    if (!ap.ok || ap.idx != vl.iv_phi) {
                        skip_reason_ = "store-not-unit-stride";
                        return false;
                    }
                    if (!register_elem(g_.node(nd.in[3]).ty, ap.esz)) return false;
                    stores_.push_back(n);
                    store_addr_.insert(n, ap);
                    break;
                }
                case Op::Bin: {
                    BinOp op = static_cast<BinOp>(nd.sub);
                    if (!vecx::packed_bin_legal(nd.ty, op)) {
                        skip_reason_ = "packed-illegal-bin";
                        return false;
                    }
                    if (vecx::vector_ty_for(nd.ty) == ty_none()) {
                        skip_reason_ = "bin-type-no-packed-form";
                        return false;
                    }
                    if (elem_ty_ == ty_none()) elem_ty_ = nd.ty;
                    else if (elem_ty_ != nd.ty) {
                        skip_reason_ = "mixed-element-types";
                        return false;
                    }
                    pure_.push_back(n);
                    break;
                }
                case Op::Cast: {
                    // non-address casts (e.g. f32->f64 per element) are not
                    // packed in the MVP; address casts never reach here
                    skip_reason_ = "cast-in-body";
                    return false;
                }
                default:
                    skip_reason_ = op_name(nd.op);
                    return false;
            }
        }
        // re-filter pure_: safety against late-marked address pieces
        std::vector<NodeId> filtered;
        for (NodeId n : pure_)
            if (!addr_pieces_.contains(n)) filtered.push_back(n);
        pure_ = std::move(filtered);

        if (loads_.empty() && stores_.empty()) {
            skip_reason_ = "no-memory-ops";
            return false;
        }

        // pure-op operands: mapped loads, other pure ops, the reduction phi,
        // or loop invariants (fixpoint order — ids are not dependency order)
        {
            FlatMap<NodeId, bool> mapped;
            for (NodeId l : loads_) mapped.insert(l, true);
            bool progress = true;
            while (progress) {
                progress = false;
                for (NodeId p : pure_) {
                    if (mapped.contains(p)) continue;
                    bool ok = true;
                    for (u8 i = 1; i < g_.node(p).n_in && ok; ++i) {
                        NodeId d = g_.node(p).in[i];
                        if (d == kNoNode || g_.is_dead(d)) continue;
                        if (mapped.contains(d) || d == reduction_phi_) continue;
                        if (!loop_invariant(d, vl)) ok = false;
                    }
                    if (ok) {
                        mapped.insert(p, true);
                        progress = true;
                    }
                }
            }
            for (NodeId p : pure_) {
                if (mapped.contains(p)) continue;
                skip_reason_ = "pure-operand-unmappable";
                return false;
            }
            // invariant operands must be broadcastable: elem-typed or const
            for (NodeId p : pure_) {
                const Node& pd = g_.node(p);
                for (u8 i = 1; i < pd.n_in; ++i) {
                    NodeId d = pd.in[i];
                    if (d == kNoNode || g_.is_dead(d)) continue;
                    if (d == reduction_phi_) continue;
                    if (loads_.end() != std::find(loads_.begin(), loads_.end(), d)) continue;
                    if (pure_.end() != std::find(pure_.begin(), pure_.end(), d)) continue;
                    if (!broadcastable(d)) {
                        skip_reason_ = "operand-not-broadcastable";
                        return false;
                    }
                }
            }
        }

        // stored values: mapped loads, mapped pure ops, or broadcastable
        // LOOP-INVARIANT leaves. The invariance half is load-bearing: the
        // raw IV as a stored value (`a[i] = i`) is elem-typed (broadcastable
        // by type) but loop-VARYING — broadcasting a header phi reads a
        // stale merged value into every lane (the fill miscompiled: each
        // element got the same value; found by the fission test's dependent
        // companion). The header comment's "iv-derived stored values are
        // rejected" contract now actually holds for the raw-phi form.
        for (NodeId s : stores_) {
            NodeId v = g_.node(s).in[3];
            if (v == kNoNode || g_.is_dead(v)) return false;
            if (loads_.end() != std::find(loads_.begin(), loads_.end(), v)) continue;
            if (pure_.end() != std::find(pure_.begin(), pure_.end(), v)) continue;
            if (!broadcastable(v) || !loop_invariant(v, vl)) {
                skip_reason_ = "stored-value-unmappable";
                return false;
            }
        }

        // reduction checks
        if (reduction_phi_ != kNoNode) {
            NodeId feed = red_.feed;
            bool feed_ok = (feed != kNoNode && !g_.is_dead(feed)) &&
                           ((loads_.end() != std::find(loads_.begin(), loads_.end(), feed)) ||
                            (pure_.end() != std::find(pure_.begin(), pure_.end(), feed)));
            if (!feed_ok) {
                skip_reason_ = "reduction-feed-unmappable";
                return false;
            }
            if (ty_is_float(elem_ty_) && !fp_fast_allowed(fp_)) {
                skip_reason_ = "fp-reduction-strict";
                return false;
            }
            if (!vecx::packed_bin_legal(elem_ty_, red_.op)) {
                skip_reason_ = "reduction-op-illegal";
                return false;
            }
            // the update must be the phi's backedge value (match_reduction
            // already guarantees update == phi's latch input)
        }

        // array bases must be loop-invariant
        for (NodeId ldn : loads_) {
            if (!loop_invariant(load_addr_.find(ldn)->base, vl)) {
                skip_reason_ = "load-base-variant";
                return false;
            }
        }
        for (NodeId stn : stores_) {
            if (!loop_invariant(store_addr_.find(stn)->base, vl)) {
                skip_reason_ = "store-base-variant";
                return false;
            }
        }

        vecx::CostVerdict v = vecx::vector_cost_ok(elem_ty_, vl.trip,
                                                   reduction_phi_ != kNoNode,
                                                   size_biased_);
        if (!v.profitable) {
            skip_reason_ = v.reason;
            return false;
        }
        return true;
    }

    bool register_elem(TypeId ety, u32 esz) {
        if (vecx::vector_ty_for(ety) == ty_none()) {
            skip_reason_ = "element-no-packed-form";
            return false;
        }
        u32 lanes = vecx::vector_lanes_for(ety);
        if (lanes * esz != 16) {
            skip_reason_ = "element-size-mismatch";
            return false;
        }
        if (elem_ty_ == ty_none()) elem_ty_ = ety;
        else if (elem_ty_ != ety) {
            skip_reason_ = "mixed-element-types";
            return false;
        }
        return true;
    }

    bool broadcastable(NodeId d) const {
        const Node& dn = g_.node(d);
        if (dn.op == Op::Const) return true; // re-typed copy at broadcast
        return dn.ty == elem_ty_;
    }

    // A phi at `vl.header` whose value never changes per iteration: every
    // input is the phi itself or a node defined outside this loop.
    bool passthrough_phi(NodeId u, const VecLoop& vl) const {
        const Node& p = g_.node(u);
        for (u8 i = 1; i < p.n_in; ++i) {
            NodeId v = p.in[i];
            if (v == u) continue; // self-backedge: unchanged
            if (v == kNoNode || g_.is_dead(v)) return false;
            const Node& vn = g_.node(v);
            if (vn.op == Op::Phi) {
                if (vn.in[0] == vl.header) return false; // varies in-loop
                continue;                                 // outer phi: constant here
            }
            if (is_control_op(vn.op) || is_block_head(vn.op)) return false;
            if (!defined_outside_loop(v, vl)) return false;
        }
        return true;
    }

    bool defined_outside_loop(NodeId n, const VecLoop& vl) const {
        const Node& nd = g_.node(n);
        for (const Loop& l : li_.loops()) {
            if (l.header != vl.header) continue;
            for (NodeId blk : l.blocks)
                if (nd.in[0] == blk) return false;
        }
        return true;
    }

    bool loop_invariant(NodeId n, const VecLoop& vl) const {
        if (n == kNoNode || g_.is_dead(n)) return false;
        const Node& nd = g_.node(n);
        if (nd.op == Op::Phi) {
            // a phi at THIS header is invariant only if pass-through; a phi
            // anywhere else (outer header, preheader merge) is constant for
            // the whole execution of this loop
            if (nd.in[0] == vl.header) return passthrough_phi(n, vl);
            return true;
        }
        if (is_control_op(nd.op) || is_block_head(nd.op)) return false;
        return defined_outside_loop(n, vl);
    }

    // ---- phase 2: build the vector loop -------------------------------------
    void build_vector_loop(const VecLoop& vl) {
        Graph& g = g_;
        const u32 lanes = vecx::vector_lanes_for(elem_ty_);
        TypeId vty = vecx::vector_ty_for(elem_ty_);
        TypeId ity = vl.iv_ty; // i64 or i32 (original iv type)

        NodeId entry_head = g.node(vl.header).in[vl.entry_slot];
        NodeId mphi = mem_phi(vl);
        // The loop's memory entry: the mem phi's entry input, or (when SROA
        // removed every memory op and RedundantPhiElimination collapsed the
        // pass-through phi) the version the surviving loads read directly.
        NodeId entry_mem = kNoNode;
        if (mphi != kNoNode) {
            entry_mem = g.node(mphi).in[vl.entry_slot + 1];
        } else {
            for (NodeId ldn : loads_) {
                entry_mem = g.node(ldn).in[1];
                break;
            }
            if (entry_mem == kNoNode)
                for (NodeId stn : stores_) { entry_mem = g.node(stn).in[1]; break; }
        }
        if (entry_mem == kNoNode) return; // no memory traffic: unreachable here

        // ---- entry guard: if (bound >= lanes) ----
        NodeId lanes_e = const_of_ty(entry_head, lanes, ity);
        NodeId gcond = g.make(Op::Cmp, ty_i1(), {entry_head, vl.bound, lanes_e},
                              static_cast<u8>(CmpOp::Ge));
        NodeId gif = g.make(Op::If, ty_ctrl(), {entry_head, gcond});
        NodeId gtrue = g.make(Op::IfTrue, ty_ctrl(), {gif});
        NodeId gfalse = g.make(Op::IfFalse, ty_ctrl(), {gif});

        // ---- vector loop skeleton (classic while shape) ----
        NodeId vhead = g.make(Op::Region, ty_ctrl(), {gtrue});
        NodeId vmphi = g.make(Op::Phi, ty_mem(), {vhead, entry_mem});
        NodeId kphi = g.make(Op::Phi, ty_i64(), {vhead, int64_const(gtrue, 0)});
        NodeId vacc = kNoNode;
        if (reduction_phi_ != kNoNode) {
            NodeId init = g.node(reduction_phi_).in[vl.entry_slot + 1];
            NodeId bcast = broadcast_val(init, gtrue, vty);
            vacc = g.make(Op::Phi, vty, {vhead, bcast});
        }

        // vector trip count nv = sext/zext(bound)/lanes, all i64
        NodeId bound64 = widen(g, vhead, vl.bound);
        NodeId nv = g.make(Op::Bin, ty_i64(),
                           {vhead, bound64, int64_const(vhead, lanes)},
                           static_cast<u8>(BinOp::Div));

        // loop guard: if (k < nv) — pinned AT the header
        NodeId lcond = g.make(Op::Cmp, ty_i1(), {vhead, kphi, nv},
                              static_cast<u8>(CmpOp::Lt));
        NodeId lif = g.make(Op::If, ty_ctrl(), {vhead, lcond});
        NodeId ltrue = g.make(Op::IfTrue, ty_ctrl(), {lif});
        NodeId lfalse = g.make(Op::IfFalse, ty_ctrl(), {lif});

        // ---- packed body (pinned at ltrue) ----
        FlatMap<NodeId, NodeId> vmap; // scalar -> packed
        // packed address: Cast(Ptr, Add(Cast(i64, base), Shl(k, 4)))
        auto vec_addr = [&](vecx::AddrPattern ap, NodeId orig_addr) {
            NodeId base64 = g.make(Op::Cast, ty_i64(), {ltrue, ap.base},
                                   static_cast<u8>(CastOp::Ptr));
            NodeId off = g.make(Op::Bin, ty_i64(),
                                {ltrue, kphi, int64_const(ltrue, 4)},
                                static_cast<u8>(BinOp::Shl));
            NodeId addr64 = g.make(Op::Bin, ty_i64(), {ltrue, base64, off},
                                   static_cast<u8>(BinOp::Add));
            return g.make(Op::Cast, g.node(orig_addr).ty, {ltrue, addr64},
                          static_cast<u8>(CastOp::Ptr));
        };
        for (NodeId ldn : loads_) {
            NodeId vaddr = vec_addr(*load_addr_.find(ldn), g.node(ldn).in[2]);
            NodeId vload = g.make(Op::Load, vty, {ltrue, vmphi, vaddr});
            vmap.insert(ldn, vload);
        }
        // pure ops in dependency fixpoint (ids are not dep order)
        {
            FlatMap<NodeId, bool> done;
            bool progress = true;
            while (progress) {
                progress = false;
                for (NodeId p : pure_) {
                    if (done.contains(p)) continue;
                    bool ready = true;
                    for (u8 i = 1; i < g.node(p).n_in && ready; ++i) {
                        NodeId d = g.node(p).in[i];
                        if (d == kNoNode || g_.is_dead(d)) continue;
                        if (vmap.find(d)) continue;
                        if (d == reduction_phi_) continue;
                        if (pure_.end() != std::find(pure_.begin(), pure_.end(), d) &&
                            !done.contains(d))
                            ready = false;
                    }
                    if (!ready) continue;
                    Node pd = g.node(p); // copy: makes below may grow nodes_
                    NodeId ins[kMaxInputs];
                    ins[0] = ltrue;
                    u8 n = 1;
                    for (u8 i = 1; i < pd.n_in; ++i) {
                        NodeId d = pd.in[i];
                        if (d == reduction_phi_) { ins[n++] = vacc; continue; }
                        if (const NodeId* m = vmap.find(d)) { ins[n++] = *m; continue; }
                        ins[n++] = broadcast_operand(d, ltrue, vty);
                    }
                    NodeId vp = g.make_arr(pd.op, vty, ins, n, pd.sub, pd.aux);
                    vmap.insert(p, vp);
                    done.insert(p, true);
                    progress = true;
                }
            }
        }

        // reduction update: Bin(op, vacc, feed-vector)
        NodeId vupdate = kNoNode;
        if (reduction_phi_ != kNoNode) {
            NodeId feed = red_.feed;
            NodeId vfeed = kNoNode;
            if (const NodeId* m = vmap.find(feed)) vfeed = *m;
            NodeId lhs = red_.phi_on_lhs ? vacc : vfeed;
            NodeId rhs = red_.phi_on_lhs ? vfeed : vacc;
            vupdate = g.make(Op::Bin, vty, {ltrue, lhs, rhs},
                             static_cast<u8>(red_.op));
        }

        // packed stores chain on the memory
        NodeId cur_mem = vmphi;
        for (NodeId stn : stores_) {
            NodeId vaddr = vec_addr(*store_addr_.find(stn), g.node(stn).in[2]);
            NodeId sv = g.node(stn).in[3];
            NodeId vval = kNoNode;
            if (const NodeId* m = vmap.find(sv)) vval = *m;
            else vval = broadcast_operand(sv, ltrue, vty);
            NodeId vs = g.make(Op::Store, ty_mem(), {ltrue, cur_mem, vaddr, vval});
            cur_mem = vs;
        }

        // k' = k + 1 (i64)
        NodeId knew = g.make(Op::Bin, ty_i64(), {ltrue, kphi, int64_const(ltrue, 1)},
                             static_cast<u8>(BinOp::Add));

        // backedge wiring
        g.append_input(vhead, ltrue);
        g.append_input(vmphi, cur_mem);
        g.append_input(kphi, knew);
        if (vacc != kNoNode) g.append_input(vacc, vupdate);

        // ---- merge: guard-false + vector exit ----
        NodeId merge = g.make(Op::Region, ty_ctrl(), {gfalse, lfalse});
        NodeId merge_mem = g.make(Op::Phi, ty_mem(), {merge, entry_mem, vmphi});

        // scalar iv entry: 0 (guard false) | k*lanes (vector exit)
        NodeId done64 = g.make(Op::Bin, ty_i64(), {lfalse, kphi, int64_const(lfalse, lanes)},
                               static_cast<u8>(BinOp::Mul));
        NodeId done_val = done64;
        if (ity == ty_i32())
            done_val = g.make(Op::Cast, ty_i32(), {lfalse, done64},
                              static_cast<u8>(CastOp::Trunc));
        NodeId zero_entry = const_of_ty(gfalse, 0, ity);
        NodeId iv_entry = g.make(Op::Phi, ity, {merge, zero_entry, done_val});

        // scalar reduction entry: init | horizontal(vacc)
        NodeId acc_entry = kNoNode;
        if (reduction_phi_ != kNoNode) {
            NodeId init = g.node(reduction_phi_).in[vl.entry_slot + 1];
            NodeId horiz = horizontal_reduce(vacc, lfalse, red_.op);
            acc_entry = g.make(Op::Phi, elem_ty_, {merge, init, horiz});
        }

        // ---- rewire the ORIGINAL loop entry (it becomes the remainder) ----
        g.set_input(vl.header, vl.entry_slot, merge);
        if (mphi != kNoNode) {
            g.set_input(mphi, vl.entry_slot + 1, merge_mem);
        } else {
            // no mem phi: the body's memory ops read entry_mem directly —
            // retarget them to the merge phi (slot 1 only, inside the body)
            for (NodeId n : body_) {
                Node& nd = g.node(n);
                if (nd.op == Op::Load || is_effect_op(nd.op)) {
                    if (nd.n_in > 1 && nd.in[1] == entry_mem)
                        g.set_input(n, 1, merge_mem);
                }
            }
        }
        g.set_input(vl.iv_phi, vl.entry_slot + 1, iv_entry);
        if (reduction_phi_ != kNoNode)
            g.set_input(reduction_phi_, vl.entry_slot + 1, acc_entry);

        ++built_;
        g_.touch();
    }

    NodeId broadcast_val(NodeId scalar, NodeId pin, TypeId vty) {
        // constants are re-materialized with the element (lane) type so the
        // broadcast reads a same-typed value; everything else broadcasts as-is
        Node sn = g_.node(scalar); // COPY: make() below may grow nodes_
        if (sn.op == Op::Const && sn.ty != ty_lane_type(vty)) {
            NodeId c = g_.make(Op::Const, ty_lane_type(vty), {pin});
            if (ty_is_float(ty_lane_type(vty))) {
                g_.node(c).fval = sn.fval;
                g_.node(c).ival = 0;
            } else {
                g_.node(c).ival = sn.ival;
            }
            scalar = c;
        }
        return g_.make(Op::Cast, vty, {pin, scalar},
                       static_cast<u8>(CastOp::Broadcast));
    }
    NodeId broadcast_operand(NodeId d, NodeId pin, TypeId vty) {
        return broadcast_val(d, pin, vty);
    }

    NodeId horizontal_reduce(NodeId vec, NodeId pin, BinOp op) {
        TypeId scalar = ty_lane_type(g_.node(vec).ty);
        NodeId r = g_.make(Op::Cast, scalar, {pin, vec},
                           static_cast<u8>(CastOp::Extract));
        g_.node(r).aux = 0;
        const u32 lanes = vecx::vector_lanes_for(scalar);
        for (u32 l = 1; l < lanes; ++l) {
            NodeId e = g_.make(Op::Cast, scalar, {pin, vec},
                               static_cast<u8>(CastOp::Extract));
            g_.node(e).aux = l;
            r = g_.make(Op::Bin, scalar, {pin, r, e}, static_cast<u8>(op));
        }
        return r;
    }

    NodeId widen(Graph& g, NodeId pin, NodeId v) {
        TypeId t = g.node(v).ty;
        if (t == ty_i64()) return v;
        CastOp k = ty_is_signed(t) ? CastOp::SExt : CastOp::ZExt;
        return g.make(Op::Cast, ty_i64(), {pin, v}, static_cast<u8>(k));
    }

    NodeId int64_const(NodeId pin, i64 v) {
        NodeId c = g_.make(Op::Const, ty_i64(), {pin});
        g_.node(c).ival = v;
        return c;
    }
    NodeId const_of_ty(NodeId pin, i64 v, TypeId ty) {
        NodeId c = g_.make(Op::Const, ty, {pin});
        g_.node(c).ival = v;
        return c;
    }

    NodeId mem_phi(const VecLoop& vl) const {
        for (NodeId u : g_.uses_of(vl.header)) {
            if (!g_.is_dead(u) && g_.node(u).op == Op::Phi && g_.node(u).ty == ty_mem())
                return u;
        }
        return kNoNode;
    }

    Graph& g_;
    LoopInfo& li_;
    FpMode fp_;
    bool size_biased_;

    std::vector<NodeId> loads_, stores_, pure_, body_;
    FlatMap<NodeId, vecx::AddrPattern> load_addr_, store_addr_;
    FlatMap<NodeId, bool> addr_pieces_;
    NodeId reduction_phi_ = kNoNode;
    vecx::Reduction red_;
    TypeId elem_ty_ = ty_none();
    const char* skip_reason_ = "";

    u32 built_ = 0;
    u32 skipped_ = 0;
};

} // namespace

class LoopVectorizerPass : public Pass {
public:
    const char* name() const override { return "LoopVectorizer"; }
    int order() const override { return 56; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    AnalysisMask required() const override {
        return static_cast<AnalysisMask>(AnalysisKind::Dominators) |
               static_cast<AnalysisMask>(AnalysisKind::LoopInfo);
    }
    AnalysisMask invalidated() const override {
        return static_cast<AnalysisMask>(AnalysisKind::MemDep);
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        bool size_biased = level_budgets(ctx.opts.level).size_biased;
        for (FunctionGraph& fg : ctx.mod.fns) {
            LoopVectorizer v(fg.g, ctx.analysis.loops(fg), ctx.opts.fp, size_biased);
            u32 built = v.vectorize_all();
            if (built > 0) {
                changed = true;
                // The inliner's repin/clone closure predates packed control
                // shapes (guard-If at a Jump entry block, merge regions
                // feeding a scalar remainder loop, vector phis). Outline
                // vectorized functions — same policy as the accumulator
                // transform (pass 50).
                fg.no_inline = true;
            }
        }
        return changed;
    }
};

JULES_REGISTER_PASS(LoopVectorizerPass, 56, "Phase 5")

} // namespace jules
