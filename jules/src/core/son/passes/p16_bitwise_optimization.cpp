// Pass 16 — BitwiseOptimization (Phase 1)
//
// Bit-level rewrites (a distinct domain from arithmetic):
//   * bitfield extract: (x << k) >> k -> x & mask  (logical shift, const k)
//   * absorption: x & (x|y) -> x ; x | (x&y) -> x
//   * complement pairs: x & ~x -> 0 ; x | ~x -> all-ones ; x ^ ~x -> all-ones
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class BitwiseOptimizer {
public:
    explicit BitwiseOptimizer(Graph& g) : g_(g) {}

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

    bool fold_const(NodeId id, i64 value) {
        Node& n = g_.node(id);
        ConstVal c;
        c.ty = n.ty;
        c.is_fp = false;
        c.iv = value;
        return replace(id, make_const_node(g_, n.in[0], c));
    }

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op == Op::Dead || n.op != Op::Bin) return false;
        if (ty_is_float(n.ty) || ty_is_bool(n.ty)) return false;
        BinOp op = static_cast<BinOp>(n.sub);
        NodeId a = n.in[1], b = n.in[2];
        const Node& an = g_.node(a);
        const Node& bn = g_.node(b);

        // complement pairs
        bool a_not_b = (bn.op == Op::Un && bn.sub == static_cast<u8>(UnOp::BNot) && bn.in[1] == a);
        bool b_not_a = (an.op == Op::Un && an.sub == static_cast<u8>(UnOp::BNot) && an.in[1] == b);
        if (a_not_b || b_not_a) {
            if (op == BinOp::And) return fold_const(id, 0);
            if (op == BinOp::Or) {
                // all-ones in the operand width
                u64 ones = ty_bits(n.ty) == 32 ? 0xFFFFFFFFull : ~0ull;
                return fold_const(id, static_cast<i64>(ones));
            }
            if (op == BinOp::Xor) {
                u64 ones = ty_bits(n.ty) == 32 ? 0xFFFFFFFFull : ~0ull;
                return fold_const(id, static_cast<i64>(ones));
            }
        }

        // absorption: x & (x|y) -> x ; x | (x&y) -> x
        if (op == BinOp::And && bn.op == Op::Bin && bn.sub == static_cast<u8>(BinOp::Or) &&
            (bn.in[1] == a || bn.in[2] == a))
            return replace(id, a);
        if (op == BinOp::Or && bn.op == Op::Bin && bn.sub == static_cast<u8>(BinOp::And) &&
            (bn.in[1] == a || bn.in[2] == a))
            return replace(id, a);
        if (op == BinOp::And && an.op == Op::Bin && an.sub == static_cast<u8>(BinOp::Or) &&
            (an.in[1] == b || an.in[2] == b))
            return replace(id, b);
        if (op == BinOp::Or && an.op == Op::Bin && an.sub == static_cast<u8>(BinOp::And) &&
            (an.in[1] == b || an.in[2] == b))
            return replace(id, b);

        // bitfield extract: (x << k) >> k  ->  x & ((1<<(w-k))-1)
        ConstVal k;
        if (op == BinOp::Shr && an.op == Op::Bin && an.sub == static_cast<u8>(BinOp::Shl) &&
            const_of(g_, b, k) && an.in[1] == n.in[2] && !ty_is_signed(n.ty)) {
            u32 w = ty_bits(n.ty);
            i64 shift = k.iv & 63;
            if (shift > 0 && shift < static_cast<i64>(w)) {
                u64 mask = (1ull << (w - shift)) - 1;
                ConstVal mc;
                mc.ty = n.ty;
                mc.is_fp = false;
                mc.iv = static_cast<i64>(mask);
                NodeId maskn = make_const_node(g_, n.in[0], mc);
                NodeId andn = g_.make(Op::Bin, n.ty, {n.in[0], an.in[1], maskn},
                                      static_cast<u8>(BinOp::And));
                return replace(id, andn);
            }
        }
        return false;
    }

    Graph& g_;
    bool changed_ = false;
};
} // namespace

class BitwiseOptimizationPass : public Pass {
public:
    const char* name() const override { return "BitwiseOptimization"; }
    int order() const override { return 16; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            BitwiseOptimizer o(fg.g);
            changed |= o.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(BitwiseOptimizationPass, 16, "Phase 1")

} // namespace jules
