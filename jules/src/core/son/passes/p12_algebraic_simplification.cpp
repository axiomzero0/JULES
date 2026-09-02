// Pass 12 — AlgebraicSimplification (Phase 1)
//
// Multi-pattern rewrites beyond identities, each with side conditions:
//   * (a+b)-b -> a, (a-b)+b -> a            (int only: FP is not associative)
//   * (x*C)/C -> x                           (unsigned only, C != 0)
//   * comparison canonicalization: x < y -> y > x, const-on-left flipped
//   * x*2^k -> x << k                        (int strength reduction)
//   * x - x -> 0, x ^ x -> 0, x & x -> x     (same-node operands)
// Complexity and side-condition checking are why this is separate from
// IdentityCollapse.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class AlgebraicSimplifier {
public:
    explicit AlgebraicSimplifier(Graph& g) : g_(g) {}

    bool run() {
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (NodeId id = 0; id < g_.size(); ++id) this_round |= visit(id);
            changed_ |= this_round;
            if (!this_round) break;
        }
        return changed_;
    }

private:
    static constexpr u32 kMaxRounds = 6;

    bool replace(NodeId from, NodeId to) {
        g_.replace_all_uses(from, to);
        g_.kill(from);
        return true;
    }

    bool fold_zero(NodeId id) {
        Node& n = g_.node(id);
        ConstVal z;
        z.ty = n.ty;
        z.is_fp = ty_is_float(n.ty);
        z.iv = 0;
        z.fv = 0.0;
        return replace(id, make_const_node(g_, n.in[0], z));
    }

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op == Op::Dead) return false;

        if (n.op == Op::Cmp) {
            CmpOp op = static_cast<CmpOp>(n.sub);
            ConstVal l, r;
            bool lc = const_of(g_, n.in[1], l);
            bool rc = const_of(g_, n.in[2], r);
            // constant on the left: flip to the right (canonical for GVN)
            if (lc && !rc) {
                CmpOp flipped;
                switch (op) {
                    case CmpOp::Lt: flipped = CmpOp::Gt; break;
                    case CmpOp::Le: flipped = CmpOp::Ge; break;
                    case CmpOp::Gt: flipped = CmpOp::Lt; break;
                    case CmpOp::Ge: flipped = CmpOp::Le; break;
                    default: return false;
                }
                NodeId a = n.in[1], b = n.in[2]; // read BEFORE mutating (swap bug guard)
                g_.set_input(id, 1, b);
                g_.set_input(id, 2, a);
                n.sub = static_cast<u8>(flipped);
                return true;
            }
            // x < y  ->  y > x (pick greater-than form as canonical)
            if (op == CmpOp::Lt || op == CmpOp::Le) {
                CmpOp flipped = (op == CmpOp::Lt) ? CmpOp::Gt : CmpOp::Ge;
                NodeId a = n.in[1], b = n.in[2]; // swap via temporaries
                g_.set_input(id, 1, b);
                g_.set_input(id, 2, a);
                n.sub = static_cast<u8>(flipped);
                return true;
            }
            return false;
        }

        if (n.op != Op::Bin) return false;
        BinOp op = static_cast<BinOp>(n.sub);
        bool fp = ty_is_float(n.ty);
        NodeId a = n.in[1], b = n.in[2];
        const Node& an = g_.node(a);
        const Node& bn = g_.node(b);

        // same-node cancellation (int only)
        if (!fp && a == b) {
            if (op == BinOp::Sub || op == BinOp::Xor) return fold_zero(id);
            if (op == BinOp::And) return replace(id, a);
            if (op == BinOp::Or) return replace(id, a);
            if (op == BinOp::Mod) return fold_zero(id);
            // note: x/x is NOT folded (x == 0 is runtime UB; no nonzero proof)
        }

        if (!fp && an.op == Op::Bin && an.sub == static_cast<u8>(BinOp::Add) && op == BinOp::Sub) {
            // (a+b)-b -> a ; (a+b)-a -> b
            if (an.in[1] == b) return replace(id, an.in[2]);
            if (an.in[2] == b) return replace(id, an.in[1]);
        }
        if (!fp && an.op == Op::Bin && an.sub == static_cast<u8>(BinOp::Sub) && op == BinOp::Add) {
            // (a-b)+b -> a
            if (an.in[1] == b) return replace(id, an.in[2]);
        }
        if (!fp && an.op == Op::Bin && an.sub == static_cast<u8>(BinOp::Sub) && op == BinOp::Sub) {
            // (a-b)-a -> ... no safe form; skip
        }

        // (x*C)/C -> x, unsigned with constant C != 0
        ConstVal cb;
        if (!fp && ty_is_signed(n.ty) == false && op == BinOp::Div &&
            bn.op == Op::Const && const_of(g_, b, cb) && cb.iv != 0 &&
            an.op == Op::Bin && an.sub == static_cast<u8>(BinOp::Mul)) {
            ConstVal mulc;
            if (const_of(g_, an.in[2], mulc) && mulc.iv == cb.iv && mulc.iv != 0)
                return replace(id, an.in[1]);
            if (const_of(g_, an.in[1], mulc) && mulc.iv == cb.iv && mulc.iv != 0)
                return replace(id, an.in[2]);
        }

        // x * 2^k -> x << k (int, k in 1..6)
        ConstVal m;
        if (!fp && op == BinOp::Mul && bn.op == Op::Const && const_of(g_, b, m) && m.iv > 0) {
            for (u64 k = 1; k <= 6; ++k) {
                if (static_cast<u64>(m.iv) == (1ull << k)) {
                    ConstVal shift;
                    shift.ty = n.ty;
                    shift.is_fp = false;
                    shift.iv = static_cast<i64>(k);
                    NodeId sn = make_const_node(g_, n.in[0], shift);
                    g_.set_input(id, 2, sn);
                    n.sub = static_cast<u8>(BinOp::Shl);
                    return true;
                }
            }
        }
        return false;
    }

    Graph& g_;
    bool changed_ = false;
};
} // namespace

class AlgebraicSimplificationPass : public Pass {
public:
    const char* name() const override { return "AlgebraicSimplification"; }
    int order() const override { return 12; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            AlgebraicSimplifier s(fg.g);
            changed |= s.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(AlgebraicSimplificationPass, 12, "Phase 1")

} // namespace jules
