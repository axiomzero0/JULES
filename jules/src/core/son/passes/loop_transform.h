// Shared machinery for loop-body transforms (passes 42/44/46/47).
//
// Two pieces:
//   1. CountedLoop recognition: the canonical while-shaped loop the pipeline
//      guarantees after SROA — a header Region with preds [entry, latch],
//      exactly one If pinned at the header whose cond is Cmp(iv_phi, bound)
//      with the iv a header phi advanced by a constant step in the latch.
//      Short-circuit (&&/||) conditions produce multi-If headers and are
//      rejected: those loops are early-exit, not counted (pass 39 agrees).
//   2. LoopCloner: subgraph cloning with per-copy phi threading.
//        * Body mode (unrolling): clones the loop body once; every header
//          phi is substituted with the incoming value for that copy (the
//          previous copy's latch output), so memory versions and
//          loop-carried values chain naturally. The body-entry projection
//          maps to a fresh Jump block supplied by the caller.
//        * Whole mode (epilogue duplication): duplicates the entire loop
//          (header Region, phis, cond If, body). The clone's entry edge is
//          rewired to a caller-supplied block head; the cloned phis' entry
//          values reference the ORIGINAL loop phis (their current values at
//          the moment the entry edge is taken — valid because the original
//          header dominates the entry edge).
//
// Soundness notes (mirrored in docs/pass_status.md):
//   * loop-carried phis are never substituted inside the live main loop —
//     only clones receive substituted inputs;
//   * a Store/Call/Alloc pinned at the header rejects the loop: the header
//     executes once per chunk after unrolling, and effects must not have
//     their execution count changed (loads are pure reads of the
//     iteration-start memory version and re-execute soundly once per chunk);
//   * the trip-count arithmetic deliberately falls back to "not computable"
//     on any wraparound rather than guessing.
#pragma once

#include "core/son/passes/pass_utils.h"

namespace jules {

// ---- counted-loop shape ---------------------------------------------------------

struct CountedLoop {
    NodeId header = kNoNode;      // Region; preds [entry(outside), latch(inside)]
    NodeId preheader = kNoNode;   // the outside pred block head
    NodeId latch = kNoNode;       // the inside pred block head
    u8 entry_slot = 0;            // Region pred slot of the entry edge
    u8 latch_slot = 1;            // Region pred slot of the backedge
    NodeId iv = kNoNode;          // header phi (integer)
    NodeId iv_next = kNoNode;     // Bin(iv + step), pinned inside the loop
    i64 step = 1;                 // signed constant step
    NodeId bound = kNoNode;       // loop-invariant bound
    u8 cmp = 0;                   // normalized CmpOp: cmp(iv, bound)
    NodeId cond_if = kNoNode;     // the single If pinned at the header
    NodeId cmp_node = kNoNode;    // the Cmp feeding cond_if
    NodeId body_entry = kNoNode;  // IfTrue projection (first body block)
    NodeId exit_proj = kNoNode;   // IfFalse projection (the exit edge)
    // exit_target: the outside Region (pred slot exit_slot) or Jump (in[0])
    NodeId exit_target = kNoNode;
    u8 exit_slot = 0;             // slot in exit_target when it is a Region
    bool exit_target_is_region = false;
    NodeId exit_region = kNoNode; // merge region whose phis read the loop phis
    std::vector<NodeId> exit_pinned; // code pinned at the exit projection
    std::vector<NodeId> phis;     // ALL header phis (mem + values), any order
    std::vector<NodeId> blocks;   // loop body blocks (projections/regions/jumps)
    u32 body_nodes = 0;           // nodes pinned in body blocks (size estimate)
};

namespace loop_transform_detail {

inline bool in_blocks(const std::vector<NodeId>& blocks, NodeId b) {
    for (NodeId x : blocks)
        if (x == b) return true;
    return false;
}

// Is `n`'s value available at every loop entry (usable as the bound)?
// Const: yes (block-independent). Pinned outside the loop: yes (dominates).
// Pinned at the header and pure over invariant inputs: yes.
inline bool loop_invariant(Graph& g, NodeId n, NodeId header,
                           const std::vector<NodeId>& blocks) {
    if (n == kNoNode) return false;
    const Node& nd = g.node(n);
    if (nd.op == Op::Const) return true;
    if (!is_pure_op(nd.op)) return false;
    if (nd.in[0] == header) {
        for (u8 i = 1; i < nd.n_in; ++i)
            if (!loop_invariant(g, nd.in[i], header, blocks)) return false;
        return true;
    }
    return !in_blocks(blocks, nd.in[0]);
}

} // namespace loop_transform_detail

// Recognizes the canonical counted form for loop `l`. Returns false for
// early-exit / multi-exit / short-circuit-condition / effectful-header loops.
inline bool recognize_counted(Graph& g, LoopInfo& li, const Loop& l, CountedLoop& out) {
    using namespace loop_transform_detail;
    if (std::getenv("JULES_DBG_UNROLL"))
        std::fprintf(stderr, "[unroll-try] hdr=%u blocks=%zu\n", l.header, l.blocks.size());
    NodeId header = l.header;
    if (g.node(header).op != Op::Region) return false;
    if (g.node(header).n_in != 2) return false; // [entry, latch] only

    // entry vs latch pred slots: default [entry, latch]; flip only when the
    // FIRST pred is the in-loop (backedge) source
    u8 entry_slot = 0, latch_slot = 1;
    bool p0_in = li.block_in_loop(l, g.node(header).in[0]);
    bool p1_in = li.block_in_loop(l, g.node(header).in[1]);
    if (p0_in && !p1_in) {
        entry_slot = 1;
        latch_slot = 0;
    } else if (!p1_in) {
        return false; // neither pred inside: not a natural loop shape
    }
    out.header = header;
    out.entry_slot = entry_slot;
    out.latch_slot = latch_slot;
    out.preheader = g.node(header).in[entry_slot];
    out.latch = g.node(header).in[latch_slot];

    // body blocks = loop blocks minus header; size estimate counts LIVE nodes
    // (killed nodes keep their inputs in the lazy use lists)
    out.blocks.clear();
    for (NodeId b : l.blocks)
        if (b != header) out.blocks.push_back(b);
    out.body_nodes = 0;
    for (NodeId b : out.blocks)
        for (NodeId u : g.uses_of(b))
            if (!g.is_dead(u) && g.node(u).in[0] == b) ++out.body_nodes;

    // header phis
    out.phis.clear();
    for (NodeId u : g.uses_of(header)) {
        const Node& un = g.node(u);
        if (un.op == Op::Phi && un.in[0] == header) out.phis.push_back(u);
    }
    if (out.phis.empty()) return false;

    // exactly one If pinned at the header; no effects at the header
    NodeId cond_if = kNoNode;
    for (NodeId u : g.uses_of(header)) {
        const Node& un = g.node(u);
        if (un.in[0] != header) continue;
        switch (un.op) {
            case Op::If:
                if (cond_if != kNoNode) return false; // short-circuit chain
                cond_if = u;
                break;
            case Op::Store:
            case Op::Call:
            case Op::Alloc:
                return false; // header effects: execution count must not change
            default:
                break; // phis / cond computation / consts / loads
        }
    }
    if (cond_if == kNoNode) return false;
    out.cond_if = cond_if;

    NodeId cmp_node = g.node(cond_if).in[1];
    if (cmp_node == kNoNode || g.node(cmp_node).op != Op::Cmp) return false;
    out.cmp_node = cmp_node;

    // projections
    NodeId tproj = kNoNode, fproj = kNoNode;
    for (NodeId u : g.uses_of(cond_if)) {
        if (g.node(u).op == Op::IfTrue) tproj = u;
        if (g.node(u).op == Op::IfFalse) fproj = u;
    }
    if (tproj == kNoNode || fproj == kNoNode) return false;
    // body on the true side, exit on the false side (builder canonical form)
    if (!li.block_in_loop(l, tproj) || li.block_in_loop(l, fproj)) return false;
    out.body_entry = tproj;
    out.exit_proj = fproj;

    // The exit edge: an outside successor Region (pred slot) or Jump, and/or
    // code pinned directly at the projection (PhiSimplification removes the
    // single-pred merge region after while loops, so the continuation code
    // pins at IfFalse). Both forms are supported: the transforms rewire the
    // successor edge and repin the exit code to the new last block.
    NodeId exit_target = kNoNode;
    u8 exit_slot = 0;
    bool exit_is_region = false;
    out.exit_pinned.clear();
    for (NodeId u : g.uses_of(fproj)) {
        const Node& un = g.node(u);
        if (un.op == Op::Dead) continue;
        if (un.op == Op::Region && !li.block_in_loop(l, u)) {
            for (u8 i = 0; i < un.n_in; ++i)
                if (un.in[i] == fproj) { exit_target = u; exit_slot = i; }
            exit_is_region = true;
        } else if (un.op == Op::Jump && un.in[0] == fproj) {
            if (!li.block_in_loop(l, u)) { exit_target = u; exit_is_region = false; }
        } else if (un.in[0] == fproj) {
            out.exit_pinned.push_back(u); // continuation code on the exit edge
        }
    }
    if (exit_target == kNoNode && out.exit_pinned.empty()) return false;
    out.exit_target = exit_target;
    out.exit_slot = exit_slot;
    out.exit_target_is_region = exit_is_region;
    out.exit_region = exit_is_region ? exit_target : kNoNode;

    // no other exits: every loop block's outside users must be the recognized
    // exit edge or the exit code pinned at the exit projection.
    for (NodeId b : l.blocks) {
        for (NodeId u : g.uses_of(b)) {
            const Node& un = g.node(u);
            if (un.op == Op::Region) {
                if (li.block_in_loop(l, u)) continue;
                if (u == out.exit_target) continue;
                return false; // a second merge outside the loop
            }
            if (un.op == Op::Jump && un.in[0] == b) {
                if (li.block_in_loop(l, u)) continue;
                if (b == fproj && u == out.exit_target) continue;
                return false;
            }
            if (un.op == Op::Return && un.in[0] == b && b != fproj)
                return false; // early return inside the body
            if (un.op == Op::Stop) return false;
        }
    }

    // cond: Cmp(iv_phi, bound) after operand-order normalization
    const Node& cn = g.node(cmp_node);
    NodeId a = cn.in[1], b2 = cn.in[2];
    if (a == kNoNode || b2 == kNoNode) return false;
    bool a_is_phi = false, b_is_phi = false;
    for (NodeId p : out.phis) {
        if (p == a) a_is_phi = true;
        if (p == b2) b_is_phi = true;
    }
    if (a_is_phi == b_is_phi) return false; // both or neither: not canonical
    NodeId iv = a_is_phi ? a : b2;
    NodeId bound = a_is_phi ? b2 : a;
    u8 op = cn.sub;
    if (!a_is_phi) { // mirror: (bound op iv) -> (iv mirror_op bound)
        switch (static_cast<CmpOp>(op)) {
            case CmpOp::Lt: op = static_cast<u8>(CmpOp::Gt); break;
            case CmpOp::Gt: op = static_cast<u8>(CmpOp::Lt); break;
            case CmpOp::Le: op = static_cast<u8>(CmpOp::Ge); break;
            case CmpOp::Ge: op = static_cast<u8>(CmpOp::Le); break;
            default: return false;
        }
    }
    const Node& ivn = g.node(iv);
    if (ivn.ty == ty_mem() || !ty_is_int(ivn.ty)) return false;
    if (g.node(bound).ty != ivn.ty) return false;
    out.iv = iv;
    out.bound = bound;
    out.cmp = op;

    // step: the iv phi's latch input is Bin(iv, const)
    NodeId iv_next = ivn.in[latch_slot + 1];
    if (iv_next == kNoNode || g.node(iv_next).op != Op::Bin) return false;
    const Node& nxt = g.node(iv_next);
    if (nxt.sub != static_cast<u8>(BinOp::Add) &&
        nxt.sub != static_cast<u8>(BinOp::Sub))
        return false;
    NodeId other = kNoNode;
    if (nxt.in[1] == iv) other = nxt.in[2];
    else if (nxt.in[2] == iv) other = nxt.in[1];
    else return false;
    ConstVal step;
    if (!const_of(g, other, step)) return false;
    if (!ty_is_int(step.ty)) return false;
    out.iv_next = iv_next;
    out.step = step.iv * (nxt.sub == static_cast<u8>(BinOp::Sub) ? -1 : 1);

    // step direction must match the comparison form
    bool up = out.step > 0;
    switch (static_cast<CmpOp>(out.cmp)) {
        case CmpOp::Lt:
        case CmpOp::Le:
            if (!up) return false;
            break;
        case CmpOp::Gt:
        case CmpOp::Ge:
            if (up) return false;
            break;
        default:
            return false;
    }

    // bound invariance
    if (!loop_invariant(g, out.bound, header, out.blocks)) return false;

    // exit-region phis (if any) must merge loop phis, consts, or values pinned
    // outside the loop — the epilogue rewiring replaces loop-phi references slot
    // by slot, and anything pinned at the header dies with the header.
    if (out.exit_region != kNoNode) {
        for (NodeId u : g.uses_of(out.exit_region)) {
            const Node& un = g.node(u);
            if (un.op != Op::Phi || un.in[0] != out.exit_region) continue;
            for (u8 i = 1; i < un.n_in; ++i) {
                NodeId v = un.in[i];
                if (v == kNoNode) continue;
                bool is_loop_phi = false;
                for (NodeId p : out.phis)
                    if (p == v) is_loop_phi = true;
                if (is_loop_phi) continue;
                if (g.node(v).op == Op::Const) continue;
                NodeId vpin = g.node(v).in[0];
                if (vpin != header && !in_blocks(out.blocks, vpin)) continue;
                return false;
            }
        }
    }

    // header-pinned non-phi/non-if/non-const nodes (cond computation): every
    // user must stay inside the loop — full unrolling kills the header, and a
    // value computed there but used after the loop would die with it.
    for (NodeId u : g.uses_of(header)) {
        if (g.node(u).in[0] != header) continue;
        Op o = g.node(u).op;
        if (o == Op::Phi || o == Op::If || o == Op::Const) continue;
        for (NodeId w : g.uses_of(u)) {
            if (w == u) continue;
            NodeId wpin = g.node(w).in[0];
            if (wpin == header || in_blocks(out.blocks, wpin)) continue;
            return false; // used outside the loop
        }
    }
    return true;
}

// ---- cloner ---------------------------------------------------------------------

class LoopCloner {
public:
    LoopCloner(Graph& g, const CountedLoop& cl) : g_(g), cl_(cl) {}

    bool ok() const { return ok_; }
    const FlatMap<NodeId, NodeId>& map() const { return map_; }

    // Nodes at or above this id are this transform's own creations (entry
    // Jumps of previous copies): they must not be rooted as body contents.
    void set_foreign_floor(u32 floor) { foreign_floor_ = floor; }

    NodeId lookup(NodeId n) const {
        if (const NodeId* m = map_.find(n)) return *m;
        return n;
    }

    // Body mode: clone the body once. entry_block is a fresh Jump head
    // playing the IfTrue projection's role; phi_in maps each header phi to
    // this copy's incoming value. Roots: every LIVE node pinned at a body
    // block (block heads alone do not carry their contents) plus the block
    // heads themselves. Returns the cloned latch block head (== entry_block
    // itself for single-block bodies).
    NodeId clone_body(NodeId entry_block, const FlatMap<NodeId, NodeId>& phi_in) {
        mode_ = Mode::Body;
        entry_ = entry_block;
        phi_in_ = &phi_in;
        for (NodeId b : cl_.blocks) {
            for (NodeId u : g_.uses_of(b)) {
                if (u >= foreign_floor_) continue; // this transform's Jump
                if (g_.is_dead(u) || g_.node(u).in[0] != b) continue;
                if (u == cl_.cond_if) continue;
                clone(u); // pinned data / effects / control
            }
            if (b != cl_.body_entry && b < foreign_floor_) clone(b); // block head
        }
        NodeId latch_clone = clone(cl_.latch); // == entry_ for single-block bodies
        if (!ok_) return kNoNode;
        return latch_clone;
    }

    // Whole mode: duplicate the entire loop. The clone's entry pred edge is
    // `entry_pred`; cloned phis reference the ORIGINAL phis as their entry
    // values. Returns the cloned header Region. After the call, map() holds
    // clones for the header, every phi, the cond If, both projections and
    // the whole body.
    NodeId clone_loop(NodeId entry_pred) {
        mode_ = Mode::Whole;
        entry_pred_ = entry_pred;

        NodeId hdr = cl_.header;
        NodeId hdr_shell = g_.make(Op::Region, ty_ctrl());
        map_.insert(hdr, hdr_shell);
        for (NodeId p : cl_.phis) {
            NodeId pshell = g_.make(Op::Phi, g_.node(p).ty);
            map_.insert(p, pshell);
        }
        // fill phis positionally: the cloned region's preds are ALWAYS
        // [entry_pred, cloned-latch], so phi inputs are
        // [pshell, entry-value, cloned-latch-value] regardless of the
        // original slot order. The entry value is the ORIGINAL phi — its
        // current value at the moment the entry edge is taken (the original
        // header dominates that edge).
        for (NodeId p : cl_.phis) {
            NodeId pshell = *map_.find(p);
            const Node& pn = g_.node(p);
            NodeId latch_value = pn.in[cl_.latch_slot + 1];
            g_.set_input(pshell, 0, hdr_shell);
            g_.append_input(pshell, p);
            g_.append_input(pshell, clone(latch_value));
        }
        // header preds: entry edge -> entry_pred, backedge -> cloned latch
        g_.set_input(hdr_shell, 0, entry_pred);
        g_.append_input(hdr_shell, clone(cl_.latch));

        // cond If (its Cmp and header-pinned cond inputs clone recursively)
        clone(cl_.cond_if);
        // both projections of the cloned If (body entry + clone's exit edge)
        for (NodeId u : g_.uses_of(cl_.cond_if)) {
            if (g_.node(u).op == Op::IfTrue || g_.node(u).op == Op::IfFalse) clone(u);
        }
        // remaining header-pinned cond computations (casts etc.)
        for (NodeId u : g_.uses_of(hdr)) {
            if (g_.node(u).in[0] != hdr) continue;
            const Node& un = g_.node(u);
            if (un.op == Op::Phi || un.op == Op::If) continue;
            if (map_.find(u)) continue;
            clone(u);
        }
        // body: root every LIVE original node pinned at a body block, then the
        // block heads (body_entry clones as a real projection of the cloned
        // If). The transform's own Jump chains must not be re-cloned.
        for (NodeId b : cl_.blocks) {
            for (NodeId u : g_.uses_of(b)) {
                if (u >= foreign_floor_) continue;
                if (g_.is_dead(u) || g_.node(u).in[0] != b) continue;
                if (u == cl_.cond_if) continue;
                clone(u);
            }
            if (b < foreign_floor_) clone(b);
        }
        if (!ok_) return kNoNode;
        return hdr_shell;
    }

    // After clone_body: the value this copy produced for a header phi.
    NodeId latch_out(NodeId phi) const {
        return lookup(g_.node(phi).in[cl_.latch_slot + 1]);
    }

private:
    enum class Mode { Body, Whole };

    NodeId clone(NodeId n) {
        if (n == kNoNode) return kNoNode;
        if (const NodeId* m = map_.find(n)) return *m;

        const Node& nd = g_.node(n);
        if (nd.op == Op::Dead) return kNoNode;

        // body-entry projection remap (body mode): the fresh Jump plays the
        // role of the IfTrue projection as the copy's first block.
        if (mode_ == Mode::Body && n == cl_.body_entry) {
            map_.insert(n, entry_);
            return entry_;
        }
        // per-copy phi substitution (body mode)
        if (mode_ == Mode::Body && phi_in_) {
            if (const NodeId* m = phi_in_->find(n)) {
                map_.insert(n, *m);
                return *m;
            }
        }
        if (n == g_.start()) return n;
        if (nd.op == Op::Const) {
            map_.insert(n, n);
            return n; // block-independent value
        }
        // the header Region: only ever referenced as a pin
        if (n == cl_.header) {
            if (mode_ == Mode::Body) {
                map_.insert(n, entry_);
                return entry_;
            }
            return *map_.find(n);
        }
        // the main loop's cond If is never cloned in body mode
        if (mode_ == Mode::Body && n == cl_.cond_if) return n;

        NodeId pin = nd.in[0];
        if (!is_block_head(nd.op)) {
            if (pin == kNoNode) return n;
            // data/effect node pinned outside the loop: available as-is
            bool pinned_outside =
                pin != cl_.header && !loop_transform_detail::in_blocks(cl_.blocks, pin);
            if (pinned_outside) {
                map_.insert(n, n);
                return n;
            }
        }

        // shell first (cycle-safe), then fill inputs
        NodeId shell = g_.make_arr(nd.op, nd.ty, nullptr, 0, nd.sub, nd.aux);
        g_.node(shell).ival = nd.ival;
        g_.node(shell).fval = nd.fval;
        g_.node(shell).flags = nd.flags;
        map_.insert(n, shell);

        for (u8 i = 0; i < nd.n_in; ++i) {
            NodeId in = nd.in[i];
            NodeId mapped;
            if (i == 0) {
                if (mode_ == Mode::Body && in == cl_.header) {
                    mapped = entry_; // header-pinned computation re-pins to J
                } else if (mode_ == Mode::Body && in == cl_.body_entry) {
                    mapped = entry_;
                } else {
                    mapped = clone(in);
                }
                if (mapped == kNoNode && nd.op != Op::Region) {
                    ok_ = false;
                    return kNoNode;
                }
                if (mapped == kNoNode) mapped = in;
                g_.set_input(shell, 0, mapped);
            } else {
                mapped = clone(in);
                if (mapped == kNoNode) mapped = in;
                g_.append_input(shell, mapped);
            }
            if (g_.node(shell).n_in >= kMaxInputs && i + 1 < nd.n_in) {
                ok_ = false; // arity overflow guard
                return kNoNode;
            }
        }
        return shell;
    }

    Graph& g_;
    const CountedLoop& cl_;
    Mode mode_ = Mode::Body;
    NodeId entry_ = kNoNode;         // body mode: fresh Jump head
    NodeId entry_pred_ = kNoNode;    // whole mode: entry edge source
    const FlatMap<NodeId, NodeId>* phi_in_ = nullptr;
    FlatMap<NodeId, NodeId> map_;
    u32 foreign_floor_ = 0xFFFFFFFFu;
    bool ok_ = true;
};

// ---- constant folding sweep over a set of nodes (used after full unroll) ----------

inline void fold_created(Graph& g, const std::vector<NodeId>& created) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (NodeId n : created) {
            if (g.is_dead(n)) continue;
            const Node& nd = g.node(n);
            if (nd.op != Op::Bin && nd.op != Op::Cmp && nd.op != Op::Un &&
                nd.op != Op::Cast)
                continue;
            ConstVal a, b, out;
            switch (nd.op) {
                case Op::Bin:
                    if (!const_of(g, nd.in[1], a) || !const_of(g, nd.in[2], b)) continue;
                    if (!eval_bin_const(static_cast<BinOp>(nd.sub), a, b, out)) continue;
                    break;
                case Op::Cmp:
                    if (!const_of(g, nd.in[1], a) || !const_of(g, nd.in[2], b)) continue;
                    if (!eval_cmp_const(static_cast<CmpOp>(nd.sub), a, b, out)) continue;
                    break;
                case Op::Un:
                    if (!const_of(g, nd.in[1], a)) continue;
                    if (!eval_un_const(static_cast<UnOp>(nd.sub), a, out)) continue;
                    break;
                case Op::Cast:
                    if (!const_of(g, nd.in[1], a)) continue;
                    if (!eval_cast_const(static_cast<CastOp>(nd.sub), a, nd.ty, out)) continue;
                    break;
                default:
                    continue;
            }
            NodeId cn = make_const_node(g, nd.in[0], out);
            g.replace_all_uses(n, cn);
            g.kill(n);
            changed = true;
        }
    }
}

// ---- trip-count computation (constant bounds only) --------------------------------

// Number of iterations for cmp(iv, bound) starting at iv=init with step s.
// Returns false when not statically computable (non-constant operands or
// wraparound-adjacent arithmetic the model refuses to guess at).
inline bool trip_count(const Graph& g, const CountedLoop& cl, u64& trips) {
    ConstVal init, bound;
    NodeId init_node = g.node(cl.iv).in[cl.entry_slot + 1];
    if (!const_of(g, init_node, init)) return false;
    if (!const_of(g, cl.bound, bound)) return false;
    if (init.is_fp || bound.is_fp) return false;
    if (init.ty != bound.ty) return false;
    i64 i = init.iv, bnd = bound.iv, s = cl.step;
    if (s == 0) return false;
    bool sgn = ty_is_signed(init.ty);
    auto lt = [&](i64 p, i64 q) { return sgn ? p < q : static_cast<u64>(p) < static_cast<u64>(q); };
    bool runs = false;
    switch (static_cast<CmpOp>(cl.cmp)) {
        case CmpOp::Lt: runs = lt(i, bnd); break;
        case CmpOp::Le: runs = !lt(bnd, i); break;
        case CmpOp::Gt: runs = lt(bnd, i); break;
        case CmpOp::Ge: runs = !lt(i, bnd); break;
        default: return false;
    }
    if (!runs) {
        trips = 0;
        return true;
    }
    u64 dist;
    if (sgn) {
        // distance from init to bound along the step direction; refuse any
        // case where the i64 subtraction could wrap
        i64 d;
        if (s > 0) {
            if (bnd < i) return false;
            d = bnd - i;
        } else {
            if (i < bnd) return false;
            d = i - bnd;
        }
        if (d < 0) return false;
        dist = static_cast<u64>(d);
    } else {
        if (s > 0) {
            if (static_cast<u64>(bnd) < static_cast<u64>(i)) return false;
            dist = static_cast<u64>(bnd) - static_cast<u64>(i);
        } else {
            if (static_cast<u64>(i) < static_cast<u64>(bnd)) return false;
            dist = static_cast<u64>(i) - static_cast<u64>(bnd);
        }
    }
    u64 ab = static_cast<u64>(s > 0 ? s : -s);
    u64 steps = dist / ab;
    bool rem = dist % ab != 0;
    switch (static_cast<CmpOp>(cl.cmp)) {
        case CmpOp::Lt:
        case CmpOp::Gt:
            trips = steps + (rem ? 1 : 0);
            break;
        case CmpOp::Le:
        case CmpOp::Ge:
            trips = steps + 1;
            break;
        default:
            return false;
    }
    return true;
}

} // namespace jules
