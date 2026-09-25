// Pass 27 — BitfieldLowering (Phase 2)
//
// As-built design (two layers):
//   1. FRONTEND (eager): the language's bitfield values ARE their backing
//      integers in the IR; `f.seg` reads lower to (x >> shift) & mask and
//      `f.seg = v` writes lower to the read-modify-write chain
//      Or(And(backing, ~M), (v & mask) << shift) — emitted by the
//      builder at lowering time (see builder.cpp bitfield_store).
//   2. THIS PASS (canonicalization): consecutive segment writes on the
//      same backing slot nest: with pass 22's store-to-load forwarding,
//      the second write's value is
//          Or(And(Or(And(base, ~M1), S1), ~M2), S2)
//      This pass flattens the nest to the canonical single-RMW form
//          Or(And(base, ~(M1|M2)), Or(S1, S2))
//      PROOF OF EXACTNESS (value identity, no control assumptions):
//        * masks disjoint: M1 & M2 == 0 (bitfield segments never
//          overlap — checked as consts here, not assumed)
//        * inserted values bounded: S1's bits ⊆ M1 and S2's bits ⊆ M2
//          (matched structurally: (v & m) << s shapes and constants)
//        then And(And(base,~M1),~M2) == And(base, ~(M1|M2)) and
//        S1 & ~M2 == S1, so the distribution is bit-exact.
//      The rewrite is a pure value-tree identity — sound regardless of
//      intervening observers (an intermediate load keeps the dead store
//      alive via its own chain; DSE decides that separately). Only
//      contiguous-run masks (the segment fingerprint: M = (2^w - 1) <<
//      s) participate, keeping the transform on bitfield-shaped code.
//
// The lowering layer of the contract (mask/shift before SROA) is
// satisfied by the frontend eagerly lowering at AST time; there is no
// IR-level bitfield node to lower. This pass carries the residual
// canonicalization job of that contract.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {

// A matched segment insert: positioned value S with proven S ⊆ mask M.
struct SegInsert {
    u64 mask = 0;    // positioned segment mask M (contiguous run of 1s)
    NodeId value = kNoNode;
};

// Extract a const u64 payload (i64/u64 domains carry the same bits).
bool const_of_u64(const Graph& g, NodeId x, u64& out) {
    const Node& n = g.node(x);
    if (n.op != Op::Const) return false;
    if (n.ty != ty_u64() && n.ty != ty_i64()) return false;
    out = static_cast<u64>(n.ival);
    return true;
}

// An And with a u64 constant in either operand position (mask shape).
bool is_and_const(const Graph& g, NodeId x) {
    const Node& n = g.node(x);
    if (n.op != Op::Bin || static_cast<BinOp>(n.sub) != BinOp::And)
        return false;
    u64 c;
    return const_of_u64(g, n.in[1], c) || const_of_u64(g, n.in[2], c);
}

u64 and_const_of(const Graph& g, NodeId x) {
    u64 c = 0;
    if (!const_of_u64(g, g.node(x).in[1], c))
        const_of_u64(g, g.node(x).in[2], c);
    return c;
}

// Is `m` a single contiguous run of set bits (the segment shape)?
// (sum must be a NONZERO power of two: a high-run mask like ~7 wraps to
// zero and is not a segment — it is a CLEAR constant.)
bool contiguous_mask(u64 m) {
    if (m == 0) return false;
    u64 low = m & (~m + 1);       // lowest set bit
    u64 sum = m + low;            // power of two iff single run
    return sum != 0 && (sum & (sum - 1)) == 0;
}

// Prove `v`'s bits ⊆ `m` structurally.
bool value_bounded(const Graph& g, NodeId v, u64 m) {
    if (m == ~u64(0)) return true; // full-width segment: anything fits
    const Node& n = g.node(v);
    if (n.op == Op::Const) {
        return (static_cast<u64>(n.ival) & ~m) == 0;
    }
    if (n.op != Op::Bin) return false;
    BinOp op = static_cast<BinOp>(n.sub);
    if (op == BinOp::And) {
        // And(x, c) bounds to c (either operand order)
        u64 c;
        if (const_of_u64(g, n.in[1], c)) return (c & ~m) == 0;
        if (const_of_u64(g, n.in[2], c)) return (c & ~m) == 0;
        return false;
    }
    if (op == BinOp::Shl) {
        // Shl(w, s): bounds to (bound(w) << s). IR shift counts are masked
        // to 6 bits exactly like the machine does (pass_utils folds y & 63;
        // x86 masks CL), so Shl(w, 64) == Shl(w, 0) == w — "everything
        // shifted out" was wrong. Any bit shifted out of the top rejects
        // the proof (the earlier shifted<inner heuristic missed
        // wrap-with-growth shapes like inner=0x6000..0, s=2).
        u64 s;
        if (!const_of_u64(g, n.in[2], s)) return false;
        s &= 63;
        u64 inner;
        if (!const_of_u64(g, n.in[1], inner)) {
            // Shl(And(x, c), s)
            const Node& w = g.node(n.in[1]);
            if (w.op == Op::Bin && static_cast<BinOp>(w.sub) == BinOp::And) {
                if (!const_of_u64(g, w.in[1], inner) &&
                    !const_of_u64(g, w.in[2], inner))
                    return false;
            } else {
                return false;
            }
        }
        if (s > 0 && inner > (~u64(0) >> s)) return false; // top bit would leave
        u64 shifted = inner << s;
        return (shifted & ~m) == 0;
    }
    return false;
}

class BitfieldCanonicalizer {
public:
    explicit BitfieldCanonicalizer(Graph& g) : g_(g) {}

    bool run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op != Op::Bin || static_cast<BinOp>(n.sub) != BinOp::Or)
                continue; // roots are the Or of the outermost insert
            changed_ |= canonicalize(id);
        }
        return changed_;
    }

private:
    // (const_of_u64 lives at namespace scope above — shared by
    // value_bounded and the matcher.)
    // Match `v` as a chain of segment inserts:
    //   v = Or(And(rest, ~M), S)  |  v = Or(S, And(rest, ~M))
    // `rest` recursing until it stops matching (the base value).
    // Appends to `segs`; returns the base node (kNoNode = no match at
    // this level, i.e. v IS the base).
    NodeId match_chain(NodeId v, std::vector<SegInsert>& segs) {
        const Node& n = g_.node(v);
        if (n.op != Op::Bin || static_cast<BinOp>(n.sub) != BinOp::Or ||
            n.ty != ty_u64() || n.n_in < 3)
            return v; // base case (only u64 backing slots participate)

        // the And(...) side (commutative). An unshifted segment makes the
        // INSERT side an And too (Or(And(base,~m), And(v,m))): the clear
        // const is a high-run (~m), the insert const is the segment run
        // itself — and they are complements. Disambiguate on that.
        NodeId and_side = kNoNode, ins_side = kNoNode;
        bool a1 = is_and_const(g_, n.in[1]);
        bool a2 = is_and_const(g_, n.in[2]);
        if (a1 && a2) {
            u64 c1 = and_const_of(g_, n.in[1]);
            u64 c2 = and_const_of(g_, n.in[2]);
            if (contiguous_mask(c2) && ~c2 == c1) {
                and_side = n.in[1]; // clear side
                ins_side = n.in[2]; // insert: And(v, m)
            } else if (contiguous_mask(c1) && ~c1 == c2) {
                and_side = n.in[2];
                ins_side = n.in[1];
            } else {
                return v; // two clears / unrelated masks: not an insert
            }
        } else {
            for (NodeId cand : {n.in[1], n.in[2]}) {
                const Node& c = g_.node(cand);
                if (c.op == Op::Bin && static_cast<BinOp>(c.sub) == BinOp::And &&
                    c.n_in >= 3) {
                    and_side = cand;
                } else {
                    ins_side = cand;
                }
            }
        }
        if (and_side == kNoNode || ins_side == kNoNode) return v;

        // And(P, ~M) with the const in either position
        const Node& a = g_.node(and_side);
        NodeId rest = kNoNode;
        u64 not_m = 0;
        u64 c1, c2;
        if (const_of_u64(g_, a.in[1], c1) && !const_of_u64(g_, a.in[2], c2)) {
            not_m = c1; rest = a.in[2];
        } else if (const_of_u64(g_, a.in[2], c2) && !const_of_u64(g_, a.in[1], c1)) {
            not_m = c2; rest = a.in[1];
        } else {
            return v; // no clear-mask const: not a segment insert
        }
        u64 m = ~not_m;
        if (!contiguous_mask(m)) return v;           // segment fingerprint
        if (!value_bounded(g_, ins_side, m)) return v; // insertion ⊆ segment

        NodeId base = match_chain(rest, segs);
        if (base == kNoNode) return kNoNode;
        // Disjointness with every earlier segment (segments never
        // overlap; a repeat writer would alias itself).
        for (const SegInsert& s : segs)
            if (s.mask & m) return kNoNode;
        segs.push_back(SegInsert{m, ins_side});
        return base;
    }

    bool canonicalize(NodeId root) {
        Node sc = g_.node(root);
        if (sc.n_in < 3) return false;
        std::vector<SegInsert> segs;
        NodeId base = match_chain(root, segs);
        if (base == kNoNode || segs.size() < 2) return false;

        // Rebuild: Or(And(base, ~(M1|M2|...)), Or(S1, S2, ...))
        u64 combined = 0;
        for (const SegInsert& s : segs) combined |= s.mask;
        NodeId pin = sc.in[0];
        NodeId clr = g_.make(Op::Const, ty_u64(), {pin});
        g_.node(clr).ival = static_cast<i64>(~combined);
        NodeId kept = g_.make(Op::Bin, ty_u64(), {pin, base, clr},
                              static_cast<u8>(BinOp::And));
        NodeId acc = segs[0].value;
        for (size_t i = 1; i < segs.size(); ++i)
            acc = g_.make(Op::Bin, ty_u64(), {pin, acc, segs[i].value},
                          static_cast<u8>(BinOp::Or));
        NodeId flat = g_.make(Op::Bin, ty_u64(), {pin, kept, acc},
                              static_cast<u8>(BinOp::Or));
        g_.replace_all_uses(root, flat);
        g_.kill(root);
        g_.touch();
        return true;
    }

    Graph& g_;
    bool changed_ = false;
};
} // namespace

class BitfieldLoweringPass : public Pass {
public:
    const char* name() const override { return "BitfieldLowering"; }
    int order() const override { return 27; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            BitfieldCanonicalizer c(fg.g);
            changed |= c.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(BitfieldLoweringPass, 27, "Phase 2")

} // namespace jules
