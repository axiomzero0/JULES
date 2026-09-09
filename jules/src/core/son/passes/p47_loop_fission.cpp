// Pass 47 — LoopFission (Phase 4)
//
// Split a counted loop carrying TWO INDEPENDENT reduction clusters into two
// loops, one per cluster — the inverse of pass 46's fusion, applied where
// fusion would not: the clusters share the iteration sequence but nothing
// else, so per-loop register pressure halves, and each split loop is the
// clean single-reduction shape the LOOP VECTORIZER (pass 56) requires (a
// two-reduction loop is rejected there with `second-loop-carried-phi`;
// fission is the enabler — the same interaction GCC/LLVM get from loop
// distribution before vectorization).
//
//   while i < N { s = s + a[i]; h = h ^ b[i]; i++; }
//      ==>  while i < N { s = s + a[i]; i++; }      (vectorizes)
//           while i < N { h = h ^ b[i]; i++; }      (vectorizes)
//
// FIRST FAMILY (documented scope): READ-ONLY counted loops — the memory phi
// self-latches (no store in the body; the proof is the phi itself), a
// single body block, no calls, exactly two carried value phis, each a
// FLAT reduction (acc = phi(init, Bin(op, acc, feed))) whose feed is a
// subtree of loads over ONE cluster array plus pure ops; the two clusters'
// load bases are pairwise NoAlias (distinct allocations), and the feeds
// never reference the other cluster's accumulator (dependent pairs stay
// one loop — the y = y + x shape below). Every body load must feed one of
// the two reductions. Write loops, calls, and dependent clusters carry
// real re-ordering proofs (the memory chain must be split and re-threaded
// per cluster) and stay out of this family's contract.
//
// Mechanics: the second cluster gets a FRESH loop after the first's exit
// projection — new header Region, guard Cmp/If (same bound node and
// relation, fresh IV phi with the same init/step), self-latching memory
// phi (read-only), and a cloned second-cluster subtree (loads re-pinned
// with the new memory phi, IV references remapped to the new IV,
// loop-external leaves reused). Post-loop readers of the moved
// accumulator read the new loop's phi; the old phi and its original
// subtree die with the next DCE sweep. Nodes pinned in the loop-exit gap
// that consume the moved accumulator re-pin after the new loop's exit.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {

struct FissionShape {
    NodeId header = kNoNode;
    u8 entry_slot = 0, latch_slot = 1;
    NodeId guard_if = kNoNode;
    NodeId body_proj = kNoNode;   // projection entering the body
    NodeId exit_proj = kNoNode;   // projection leaving the loop
    NodeId iv_phi = kNoNode;
    NodeId iv_init = kNoNode;
    NodeId iv_add = kNoNode;      // Add(iv, k) at the latch
    i64 iv_step = 1;
    NodeId bound = kNoNode;
    CmpOp rel = CmpOp::Lt;
    bool iv_on_lhs = true;        // Cmp operand side of the IV
    NodeId mem_phi = kNoNode;
    NodeId body = kNoNode;        // the single body block head
    NodeId phi_a = kNoNode;       // kept cluster
    NodeId phi_b = kNoNode;       // moved cluster
    NodeId feed_a = kNoNode;
    NodeId feed_b = kNoNode;
    bool b_on_lhs = true;         // update side of the accumulator
    BinOp op_b = BinOp::Add;
};

class LoopFissioner {
public:
    LoopFissioner(Graph& g, LoopInfo& li, DomTree& dom, AliasInfo& aa)
        : g_(g), li_(li), dom_(dom), aa_(aa) {}

    u32 run() {
        u32 hits = 0;
        for (int round = 0; round < 6; ++round) {
            bool any = false;
            for (const Loop& l : li_.loops()) {
                if (try_fission(l)) { ++hits; any = true; break; }
            }
            if (!any) break;
        }
        return hits;
    }

private:
    bool in_body(NodeId n) const {
        NodeId pin = g_.node(n).in[0];
        return pin == sh_.header || pin == sh_.body;
    }

    // Is `n` defined OUTSIDE the loop (available before its entry)? The
    // leaves of the moved cluster's subtree that satisfy this are reused,
    // not cloned. Header phis are the loop-carried values themselves.
    bool loop_external(NodeId n) const {
        if (n == kNoNode || g_.is_dead(n)) return true;
        const Node& nd = g_.node(n);
        if (nd.op == Op::Phi && nd.in[0] == sh_.header) return false;
        NodeId pin = nd.in[0];
        if (pin == kNoNode) return false;
        if (pin == sh_.header || pin == sh_.body) return false;
        return true; // pinned to a block outside the loop
    }

    // walk a feed subtree: collect load bases; reject stores/calls/control,
    // references to the OTHER cluster's phi, or nodes that are neither
    // loop-external nor body-pinned pure ops. The IV phi is EXPECTED (the
    // feeds index with it; the clone remaps it to the new IV).
    bool scan_feed(NodeId n, NodeId self_phi, NodeId other_phi,
                   std::vector<NodeId>& bases, u32 depth = 0) {
        if (depth > 256) return false;
        if (n == kNoNode) return true; // absent operand slot (arity < 3)
        if (g_.is_dead(n)) return true;
        if (n == other_phi) return false;      // dependent clusters rejected
        if (n == self_phi) return false;       // accumulator inside the feed
        if (n == sh_.iv_phi) return true;      // the shared induction var
        if (n == sh_.mem_phi) return true;     // the loop's memory version
        if (loop_external(n)) return true;     // invariant leaf: reuse
        const Node& nd = g_.node(n);
        switch (nd.op) {
            case Op::Load: {
                if (in_body(n)) {
                    vecx::AddrPattern ap = vecx::match_addr(g_, nd.in[2], 0);
                    if (!ap.ok) return false;
                    bases.push_back(ap.base);
                }
                return scan_feed(nd.in[2], self_phi, other_phi, bases, depth + 1);
            }
            case Op::Bin:
            case Op::Cast:
                if (!in_body(n)) return false; // pinned inside the loop only
                return scan_feed(nd.in[1], self_phi, other_phi, bases, depth + 1) &&
                       scan_feed(nd.in[2], self_phi, other_phi, bases, depth + 1);
            case Op::Const:
                return true;
            default:
                return false; // store/call/phi/control: outside the family
        }
    }

    bool match(const Loop& l, FissionShape& out) {
        if (l.blocks.size() != 2) return false; // header + ONE body block
        NodeId header = l.header;
        const Node& h = g_.node(header);
        if (h.op != Op::Region || h.n_in != 2) return false;

        u8 entry_slot = 2, latch_slot = 2;
        for (u8 i = 0; i < 2; ++i) {
            if (li_.block_in_loop(l, h.in[i])) latch_slot = i;
            else entry_slot = i;
        }
        if (entry_slot == 2) return false;

        NodeId guard_if = kNoNode;
        for (NodeId u : g_.uses_of(header)) {
            if (g_.node(u).op == Op::If && g_.node(u).in[0] == header) {
                if (guard_if != kNoNode) return false;
                guard_if = u;
            }
        }
        if (guard_if == kNoNode) return false;
        const Node& gi = g_.node(guard_if);
        if (gi.in[1] == kNoNode || g_.node(gi.in[1]).op != Op::Cmp) return false;
        const Node& cmp = g_.node(gi.in[1]);

        NodeId body_proj = kNoNode, exit_proj = kNoNode;
        for (NodeId u : g_.uses_of(guard_if)) {
            Op o = g_.node(u).op;
            if (o != Op::IfTrue && o != Op::IfFalse) continue;
            if (li_.block_in_loop(l, u)) {
                if (body_proj != kNoNode) return false;
                body_proj = u;
            } else {
                if (exit_proj != kNoNode) return false;
                exit_proj = u;
            }
        }
        if (body_proj == kNoNode || exit_proj == kNoNode) return false;

        auto phi_here = [&](NodeId n) {
            return n != kNoNode && g_.node(n).op == Op::Phi && g_.node(n).in[0] == header;
        };
        NodeId iv = kNoNode, bound = kNoNode;
        bool iv_on_lhs = true;
        if (phi_here(cmp.in[1])) { iv = cmp.in[1]; bound = cmp.in[2]; }
        else if (phi_here(cmp.in[2])) { iv = cmp.in[2]; bound = cmp.in[1]; iv_on_lhs = false; }
        if (iv == kNoNode || bound == kNoNode) return false;
        const Node& ivn = g_.node(iv);
        NodeId upd = ivn.in[latch_slot + 1];
        i64 step = 0;
        if (upd == kNoNode || g_.node(upd).op != Op::Bin ||
            static_cast<BinOp>(g_.node(upd).sub) != BinOp::Add)
            return false;
        ConstVal sc;
        if (g_.node(upd).in[1] == iv && const_of(g_, g_.node(upd).in[2], sc)) step = sc.iv;
        else if (g_.node(upd).in[2] == iv && const_of(g_, g_.node(upd).in[1], sc)) step = sc.iv;
        else return false;
        if (step <= 0) return false;

        // header phis: iv + (optional mem) + exactly two carried value phis.
        // The mem phi may be ABSENT: a read-only loop whose loads read an
        // external version (the memory phi collapses when nothing in the
        // body produces a new version — e.g. loads reading a PRECEDING
        // loop's exit memory).
        NodeId mem_phi = kNoNode, phi_a = kNoNode, phi_b = kNoNode;
        u32 value_phis = 0;
        for (NodeId u : g_.uses_of(header)) {
            if (g_.is_dead(u) || g_.node(u).op != Op::Phi || u == iv) continue;
            if (g_.node(u).ty == ty_mem()) {
                if (mem_phi != kNoNode) return false;
                mem_phi = u;
                continue;
            }
            if (++value_phis > 2) return false;
            if (phi_a == kNoNode) phi_a = u; else phi_b = u;
        }
        if (value_phis != 2) return false;
        // READ-ONLY proof: the memory phi self-latches (nothing in the body
        // produces a new memory version); with no phi, the body scan below
        // proves it directly (no stores/calls)
        if (mem_phi != kNoNode &&
            g_.node(mem_phi).in[latch_slot + 1] != mem_phi) return false;
        // flat reductions (match_reduction accepts the latch-update shape)
        vecx::Reduction ra, rb;
        if (!vecx::match_reduction(g_, phi_a, header, ra)) return false;
        if (!vecx::match_reduction(g_, phi_b, header, rb)) return false;

        // no calls/stores in the body (the read-only proof when there is
        // no memory phi; with a self-latching phi the stores are already
        // excluded, calls still are effects)
        for (NodeId u : g_.uses_of(body_proj)) {
            if (g_.is_dead(u) || g_.node(u).in[0] != body_proj) continue;
            Op o = g_.node(u).op;
            if (o == Op::Call || o == Op::Store) return false;
            if ((o == Op::If) && u != guard_if) return false;
        }

        out.header = header;
        out.entry_slot = entry_slot;
        out.latch_slot = latch_slot;
        out.guard_if = guard_if;
        out.body_proj = body_proj;
        out.exit_proj = exit_proj;
        out.iv_phi = iv;
        out.iv_init = ivn.in[entry_slot + 1];
        out.iv_add = upd;
        out.iv_step = step;
        out.bound = bound;
        out.rel = static_cast<CmpOp>(cmp.sub);
        out.iv_on_lhs = iv_on_lhs;
        out.mem_phi = mem_phi;
        out.body = body_proj;
        out.phi_a = phi_a;
        out.phi_b = phi_b;
        out.feed_a = ra.feed;
        out.feed_b = rb.feed;
        out.b_on_lhs = rb.phi_on_lhs;
        out.op_b = rb.op;
        return true;
    }

    // clone the moved cluster's subtree into the new body; IV references
    // remap to iv2, loop-external leaves are reused (recorded in reused_ —
    // they must stay available BEFORE the new loop)
    NodeId clone_value(NodeId n, NodeId iv2, NodeId body2, NodeId mem2,
                       NodeId self_phi2, NodeId other_phi) {
        if (n == kNoNode || g_.is_dead(n)) return n;
        if (n == sh_.iv_phi) return iv2;
        if (n == other_phi) return kNoNode; // rejected earlier; unreachable
        if (n == sh_.mem_phi) return mem2;  // the loop's own memory phi
        if (loop_external(n)) {
            reused_.insert(n, true);
            return n;
        }
        const Node* nd = &g_.node(n);
        if (const NodeId* c = vmap_.find(n)) return *c;
        NodeId out = kNoNode;
        switch (nd->op) {
            case Op::Load: {
                NodeId addr = clone_value(nd->in[2], iv2, body2, mem2, self_phi2, other_phi);
                // the memory input: the original loop's phi maps to mem2; an
                // EXTERNAL version (no mem phi — loads read pre-loop memory)
                // is reused as-is
                NodeId mem_in = clone_value(nd->in[1], iv2, body2, mem2, self_phi2,
                                            other_phi);
                if (mem_in == kNoNode) return kNoNode;
                out = g_.make(Op::Load, nd->ty, {body2, mem_in, addr});
                break;
            }
            case Op::Bin: {
                NodeId a = clone_value(nd->in[1], iv2, body2, mem2, self_phi2, other_phi);
                NodeId b = clone_value(nd->in[2], iv2, body2, mem2, self_phi2, other_phi);
                out = g_.make(Op::Bin, nd->ty, {body2, a, b}, nd->sub);
                break;
            }
            case Op::Cast: {
                NodeId a = clone_value(nd->in[1], iv2, body2, mem2, self_phi2, other_phi);
                out = g_.make(Op::Cast, nd->ty, {body2, a}, nd->sub);
                break;
            }
            case Op::Const: {
                out = g_.make(Op::Const, nd->ty, {body2});
                g_.node(out).ival = nd->ival;
                g_.node(out).fval = nd->fval;
                break;
            }
            default:
                return kNoNode; // not in the family
        }
        vmap_.insert(n, out);
        return out;
    }

    bool try_fission(const Loop& l) {
        if (!match(l, sh_)) return false;

        // cluster independence: feeds reference neither accumulator; load
        // base sets pairwise NoAlias; every body load belongs to a feed
        std::vector<NodeId> bases_a, bases_b;
        if (!scan_feed(sh_.feed_a, sh_.phi_a, sh_.phi_b, bases_a)) return false;
        if (!scan_feed(sh_.feed_b, sh_.phi_b, sh_.phi_a, bases_b)) return false;
        if (bases_a.empty() || bases_b.empty()) return false;
        for (NodeId ba : bases_a)
            for (NodeId bb : bases_b)
                if (aa_.alias(ba, bb) != AliasResult::NoAlias) return false;
        std::vector<NodeId> feeds{sh_.feed_a, sh_.feed_b};
        for (NodeId u : g_.uses_of(sh_.body)) {
            if (g_.is_dead(u) || g_.node(u).op != Op::Load) continue;
            if (g_.node(u).in[0] != sh_.body) continue;
            bool fed = false;
            for (NodeId f : feeds)
                if (subtree_contains(f, u, 0)) { fed = true; break; }
            if (!fed) return false; // a load feeding neither reduction
        }

        // PRE-VALIDATION (before any mutation): the exit-gap nodes the clone
        // will reuse — loop-external leaves of the moved subtree and their
        // input closure — must not read the moved accumulator (they would
        // have to stay before L2 for the clone while reading a value only
        // available after it; checking after the clone would leave an
        // orphaned half-built loop on bail)
        {
            FlatMap<NodeId, bool> ext_leaves;
            std::vector<NodeId> wstack{sh_.feed_b, sh_.iv_add};
            while (!wstack.empty()) {
                NodeId n = wstack.back();
                wstack.pop_back();
                if (n == kNoNode || g_.is_dead(n) || ext_leaves.contains(n)) continue;
                if (n == sh_.iv_phi || n == sh_.phi_a || n == sh_.phi_b) continue;
                if (n == sh_.mem_phi) continue;
                if (loop_external(n)) {
                    ext_leaves.insert(n, true);
                    const Node& en = g_.node(n);
                    for (u8 k = 1; k < en.n_in; ++k) wstack.push_back(en.in[k]);
                    continue;
                }
                const Node& nd = g_.node(n);
                for (u8 k = 1; k < nd.n_in; ++k) wstack.push_back(nd.in[k]);
            }
            for (const auto& kv : ext_leaves.entries()) {
                NodeId u = kv.first;
                if (g_.node(u).in[0] != sh_.exit_proj) continue;
                if (reads_transitively(u, sh_.phi_b)) return false;
            }
        }

        // ---- build the second loop after L1's exit projection ----
        NodeId entry_mem = sh_.mem_phi != kNoNode
                               ? g_.node(sh_.mem_phi).in[sh_.entry_slot + 1]
                               : kNoNode;

        // guard: Cmp + If at a fresh header; projections
        vmap_.clear();
        reused_.clear();
        NodeId h2 = g_.make(Op::Region, ty_ctrl(), {sh_.exit_proj});
        NodeId phi_b_entry = g_.node(sh_.phi_b).in[sh_.entry_slot + 1];
        NodeId acc2 = g_.make(Op::Phi, g_.node(sh_.phi_b).ty, {h2, phi_b_entry});
        NodeId iv2 = g_.make(Op::Phi, g_.node(sh_.iv_phi).ty, {h2, sh_.iv_init});
        NodeId mem2 = kNoNode;
        if (sh_.mem_phi != kNoNode) mem2 = g_.make(Op::Phi, ty_mem(), {h2, entry_mem});

        NodeId cmp_lhs = iv2, cmp_rhs = sh_.bound;
        if (!sh_.iv_on_lhs) std::swap(cmp_lhs, cmp_rhs);
        NodeId cmp2 = g_.make(Op::Cmp, ty_i1(), {h2, cmp_lhs, cmp_rhs},
                               static_cast<u8>(sh_.rel));
        NodeId if2 = g_.make(Op::If, ty_ctrl(), {h2, cmp2});
        Op proj_op = g_.node(sh_.body_proj).op; // same polarity as L1
        NodeId true2 = g_.make(proj_op, ty_ctrl(), {if2});
        NodeId false2 = g_.make(proj_op == Op::IfTrue ? Op::IfFalse : Op::IfTrue,
                                 ty_ctrl(), {if2});
        NodeId body2 = true2; // the new body block head
        g_.append_input(h2, body2); // latch pred (backedge)

        // memory: read-only family — the new loop's memory self-latches
        // too (only when the original loop threaded one)
        if (mem2 != kNoNode) g_.append_input(mem2, mem2);

        // IV latch: clone Add(iv, k) with the remapped IV
        NodeId add2 = clone_value(sh_.iv_add, iv2, body2, mem2, acc2, sh_.phi_a);
        if (add2 == kNoNode) return false;
        g_.append_input(iv2, add2);

        // second cluster's feed + update
        NodeId feed2 = clone_value(sh_.feed_b, iv2, body2, mem2, acc2, sh_.phi_a);
        if (feed2 == kNoNode) return false;
        NodeId lhs = sh_.b_on_lhs ? acc2 : feed2;
        NodeId rhs = sh_.b_on_lhs ? feed2 : acc2;
        NodeId upd2 = g_.make(Op::Bin, g_.node(sh_.phi_b).ty, {body2, lhs, rhs},
                               static_cast<u8>(sh_.op_b));
        g_.append_input(acc2, upd2);

        // post-loop readers of the moved accumulator read the new phi; the
        // old phi's update subtree dies with the next DCE sweep
        g_.replace_all_uses(sh_.phi_b, acc2);
        g_.kill(sh_.phi_b);

        // post-loop control: L1's exit projection is now L2's entry pred.
        // The POST-LOOP CODE — everything pinned to the exit projection
        // (the return, the consumers of both accumulators) — must move
        // after L2 (re-pin to its exit projection); nodes the CLONED
        // subtree reuses (invariants computed in the gap) stay before L2,
        // along with the transitive input closure they read. Regions
        // entered from the exit get their pred retargeted to L2's exit.
        FlatMap<NodeId, bool> keep;
        std::vector<NodeId> stack;
        for (const auto& kv : reused_.entries()) {
            if (!keep.contains(kv.first)) { keep.insert(kv.first, true); stack.push_back(kv.first); }
        }
        while (!stack.empty()) {
            NodeId n = stack.back();
            stack.pop_back();
            if (n == kNoNode || g_.is_dead(n)) continue;
            const Node& nd = g_.node(n);
            for (u8 k = 1; k < nd.n_in && k < kMaxInputs; ++k) {
                NodeId v = nd.in[k];
                if (v == kNoNode || g_.is_dead(v)) continue;
                const Node& vn = g_.node(v);
                // only follow data deps (skipping pins); memory-chain
                // links do not gate placement (versions dominate)
                if (vn.op == Op::Store || vn.op == Op::Call || vn.op == Op::Phi) continue;
                if (!keep.contains(v)) { keep.insert(v, true); stack.push_back(v); }
            }
        }
        const SmallVec<NodeId, 4> ex_users = g_.uses_of(sh_.exit_proj);
        for (NodeId u : ex_users) {
            if (g_.is_dead(u) || u == h2) continue;
            Node& un = g_.node(u);
            bool touches = false;
            for (u8 k = 0; k < un.n_in; ++k)
                if (un.in[k] == sh_.exit_proj) touches = true;
            if (!touches) continue;
            if (un.op == Op::Region) {
                // post-loop merge region: its pred becomes L2's exit
                for (u8 k = 0; k < un.n_in; ++k)
                    if (un.in[k] == sh_.exit_proj) g_.set_input(u, k, false2);
                continue;
            }
            if (un.in[0] == sh_.exit_proj) {
                // pinned at the exit block: post-loop code moves after L2
                // unless the clone consumes it (the keep set)
                if (keep.contains(u)) {
                    if (reads_transitively(u, acc2)) return false; // conflict
                    continue;
                }
                g_.set_input(u, 0, false2);
            }
        }

        g_.touch();
        return true;
    }

    bool subtree_contains(NodeId root, NodeId target, u32 depth) const {
        if (root == kNoNode || depth > 256) return false;
        if (root == target) return true;
        if (g_.is_dead(root)) return false;
        if (loop_external(root)) return false;
        const Node& nd = g_.node(root);
        if (nd.op == Op::Load)
            return subtree_contains(nd.in[2], target, depth + 1);
        if (nd.op == Op::Bin || nd.op == Op::Cast)
            return subtree_contains(nd.in[1], target, depth + 1) ||
                   subtree_contains(nd.in[2], target, depth + 1);
        return false;
    }

    bool reads_transitively(NodeId n, NodeId want, u32 depth = 0) const {
        if (n == kNoNode || depth > 128) return false;
        if (n == want) return true;
        if (g_.is_dead(n)) return false;
        const Node& nd = g_.node(n);
        for (u8 k = 1; k < nd.n_in; ++k)
            if (reads_transitively(nd.in[k], want, depth + 1)) return true;
        return false;
    }

    Graph& g_;
    LoopInfo& li_;
    DomTree& dom_;
    AliasInfo& aa_;
    FissionShape sh_;
    FlatMap<NodeId, NodeId> vmap_;
    FlatMap<NodeId, bool> reused_;
};

} // namespace

class LoopFissionPass : public Pass {
public:
    const char* name() const override { return "LoopFission"; }
    int order() const override { return 47; }
    const char* phase_name() const override {
        return "Phase 4: Loop Analysis & Transforms";
    }
    ModeMask modes() const override { return kModeAll; }
    AnalysisMask required() const override {
        return AnalysisKind::LoopInfo | AnalysisKind::Dominators | AnalysisKind::AliasInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::LoopInfo | AnalysisKind::Dominators | AnalysisKind::AliasInfo |
               AnalysisKind::MemDep;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            LoopFissioner f(fg.g, ctx.analysis.loops(fg), ctx.analysis.doms(fg),
                            ctx.analysis.alias(fg));
            changed |= f.run() > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(LoopFissionPass, 47, "Phase 4")

} // namespace jules
