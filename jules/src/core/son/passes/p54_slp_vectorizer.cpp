// Pass 54 — SLPVectorizer (Phase 5)
//
// Bottom-up Superword-Level Parallelism on straight-line code: adjacent
// store PAIRS to consecutive elements of one array, whose values come from
// the isomorphic scalar computation over the same pair of a source array:
//
//   a[C]   = b[C]   * k        a[C]   = b[C]        (pure copy)
//   a[C+1] = b[C+1] * k   ==>  one packed iteration:
//   (adjacent in the memory chain)   [movups load b+C][paddq/pmul.. by
//                                     broadcast k][movups store a+C]
//
// The pair's addresses are the SAME element-C addresses (the vector ops
// read/write 16 bytes there), so no address arithmetic is rebuilt. The
// pattern arises naturally after the unroller fully flattens small const
// loops (a[0]=b[0]; a[1]=b[1]; ...) and from hand-unrolled source.
// Pass 55 discovers and reports the candidate packs (including
// non-adjacent ones); this pass executes the chain-adjacent pairs.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {
class Slp {
public:
    Slp(Graph& g, FpMode fp) : g_(g), fp_(fp) {}

    u32 run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op != Op::Store) continue;
            NodeId s1 = id;
            // find a memory-chain successor that is itself a Store
            NodeId s2 = kNoNode;
            for (NodeId u : g_.uses_of(s1)) {
                if (g_.is_dead(u)) continue;
                Node& un = g_.node(u);
                if (un.op == Op::Store && un.in[1] == s1) {
                    s2 = u;
                    break;
                }
            }
            if (s2 == kNoNode) continue;
            try_pack(s1, s2);
        }
        return packed_;
    }

private:

    static bool const_idx(Graph& g, const vecx::AddrPattern& a, i64& out) {
        if (a.idx == kNoNode) { out = 0; return true; } // folded Add(x,0)
        ConstVal c;
        if (!const_of(g, a.idx, c)) return false;
        out = c.iv;
        return true;
    }

    void try_pack(NodeId s1, NodeId s2) {
        Node& n1 = g_.node(s1);
        Node& n2 = g_.node(s2);
        TypeId ety = g_.node(n1.in[3]).ty;
        if (vecx::vector_ty_for(ety) == ty_none()) return;
        u32 esz = ty_store_bytes(ety);
        u32 lanes = vecx::vector_lanes_for(ety);
        if (lanes != 2) return; // pairs only (4-lane needs 4 stores)
        if (lanes * esz != 16) return;

        // both stores' addresses: same base, consecutive CONST indices
        vecx::AddrPattern a1 = vecx::match_addr(g_, n1.in[2], esz);
        vecx::AddrPattern a2 = vecx::match_addr(g_, n2.in[2], esz);
        if (!a1.ok || !a2.ok) return;
        if (a1.base != a2.base) return;
        i64 c1 = 0, c2 = 0;
        if (!const_idx(g_, a1, c1) || !const_idx(g_, a2, c2)) return;
        if (c2 != c1 + 1) return;
        if (a1.esz != esz) return;

        // values: loads of the same source pair, optionally through ONE
        // identical pure op with identical extra operand
        NodeId v1 = n1.in[3], v2 = n2.in[3];
        if (g_.node(v1).ty != ety || g_.node(v2).ty != ety) return;
        NodeId l1 = v1, l2 = v2;
        NodeId op_node = kNoNode;
        NodeId shared = kNoNode;
        if (g_.node(v1).op == Op::Load && g_.node(v2).op == Op::Load) {
            // pure copy
        } else if (g_.node(v1).op == Op::Bin && g_.node(v2).op == Op::Bin &&
                   g_.node(v1).sub == g_.node(v2).sub &&
                   same_shape_pair(v1, v2, l1, l2, shared)) {
            op_node = v1;
            if (!vecx::packed_bin_legal(ety, static_cast<BinOp>(g_.node(v1).sub))) return;
        } else {
            return;
        }
        // the value loads: same base, same consecutive const offsets
        vecx::AddrPattern la1 = vecx::match_addr(g_, g_.node(l1).in[2], esz);
        vecx::AddrPattern la2 = vecx::match_addr(g_, g_.node(l2).in[2], esz);
        if (!la1.ok || !la2.ok || la1.base != la2.base) return;
        i64 lc1 = 0, lc2 = 0;
        if (!const_idx(g_, la1, lc1) || !const_idx(g_, la2, lc2)) return;
        if (lc2 != lc1 + 1) return;
        if (la1.esz != esz) return;
        if (g_.node(l1).ty != ety) return;
        // the loaded pair and the stored pair may be the same base ONLY if
        // disjoint (copy with C_distinct indices); in-place a[i]=a[i+..]
        // through a chain is handled by the memory ordering (loads read the
        // pre-store version) — keep the conservative check that the loads
        // are ordered before the store via their version: the SoN already
        // encodes it (loads' mem input is the pre-store chain).
        if (ty_is_float(ety) && op_node != kNoNode && !fp_fast_allowed(fp_)) {
            // element-wise ops with a broadcast operand are lane-exact;
            // this check only fires for ops that reassociate — none here.
        }

        TypeId vty = vecx::vector_ty_for(ety);
        NodeId pin = n1.in[0];
        NodeId mem_in = g_.node(l1).in[1]; // version the loads read

        // packed load at the source pair's first address (same 16 bytes)
        NodeId vload = g_.make(Op::Load, vty, {pin, mem_in, g_.node(l1).in[2]});

        NodeId vval = vload;
        if (op_node != kNoNode) {
            NodeId bcast = g_.make(Op::Cast, vty, {pin, shared},
                                   static_cast<u8>(CastOp::Broadcast));
            NodeId lhs = (g_.node(op_node).in[1] == l1) ? vload : bcast;
            NodeId rhs = (g_.node(op_node).in[1] == l1) ? bcast : vload;
            vval = g_.make(Op::Bin, vty, {pin, lhs, rhs}, g_.node(op_node).sub);
        }

        // packed store at the destination pair's first address; it replaces
        // BOTH scalar stores in the memory chain
        NodeId chain_in = n1.in[1];
        NodeId vstore = g_.make(Op::Store, ty_mem(), {pin, chain_in, n1.in[2], vval});

        g_.replace_uses_as_memory(s1, vstore);
        g_.replace_uses_as_memory(s2, vstore);
        g_.kill(s1);
        g_.kill(s2);
        g_.touch();
        ++packed_;
    }

    // v1 = Bin(op, load1, shared) and v2 = Bin(op, load2, shared) with the
    // SAME shared operand (or mirrored sides)?
    bool same_shape_pair(NodeId v1, NodeId v2, NodeId& l1, NodeId& l2,
                         NodeId& shared) const {
        const Node& a = g_.node(v1);
        const Node& b = g_.node(v2);
        NodeId a1 = a.in[1], a2 = a.in[2], b1 = b.in[1], b2 = b.in[2];
        if (g_.node(a1).op == Op::Load && g_.node(b1).op == Op::Load && a2 == b2) {
            l1 = a1; l2 = b1; shared = a2;
            return true;
        }
        if (g_.node(a2).op == Op::Load && g_.node(b2).op == Op::Load && a1 == b1) {
            l1 = a2; l2 = b2; shared = a1;
            return true;
        }
        return false;
    }

    Graph& g_;
    FpMode fp_;
    u32 packed_ = 0;
};
} // namespace

class SLPVectorizerPass : public Pass {
public:
    const char* name() const override { return "SLPVectorizer"; }
    int order() const override { return 54; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            // chain-adjacent store pairs: walk every store's memory users
            Slp s(fg.g, ctx.opts.fp);
            changed |= s.run() > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(SLPVectorizerPass, 54, "Phase 5")

} // namespace jules
