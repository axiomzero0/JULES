// Pass 2 — IdentityCollapse (Phase 0: Frontend Residue Cleanup)
//
// x+0, x*1, x&~0, x|0, x^0, x<<0, x/1, x-0, redundant casts (same type),
// select(x,x), double negation/not. Canonical form for downstream hashing
// (GVN) — kept separate from ConstantFolding by design: no arithmetic is
// evaluated here, only algebraic identities are rewritten.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {

bool int_all_ones(const ConstVal& c) { return !c.is_fp && (static_cast<u64>(c.iv) == ~0ull); }
bool int_zero(const ConstVal& c) { return !c.is_fp && c.iv == 0; }
bool int_one(const ConstVal& c) { return !c.is_fp && c.iv == 1; }

class IdentityCollapser {
public:
    explicit IdentityCollapser(Graph& g) : g_(g) {}

    bool run() {
        bool changed = false;
        // iterate to fixpoint: replacements can expose new identities
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (NodeId id = 0; id < g_.size(); ++id) {
                this_round |= visit(id);
            }
            changed |= this_round;
            if (!this_round) break;
        }
        return changed;
    }

private:
    static constexpr u32 kMaxRounds = 8; // named fixpoint bound

    bool replace(NodeId from, NodeId to) {
        if (from == to || to == kNoNode) return false;
        g_.replace_all_uses(from, to);
        g_.kill(from);
        return true;
    }

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op == Op::Dead) return false;
        switch (n.op) {
            case Op::Bin: {
                BinOp op = static_cast<BinOp>(n.sub);
                NodeId a = n.in[1], b = n.in[2];
                ConstVal ca, cb;
                bool ba = const_of(g_, a, ca);
                bool bb = const_of(g_, b, cb);
                bool fp = ty_is_float(n.ty);
                if (op == BinOp::Min || op == BinOp::Max) {
                    // min(x,x) = max(x,x) = x (NaN: both arms identical)
                    if (a == b) return replace(id, a);
                    return false;
                }
                if (op == BinOp::Add) {
                    if (bb && int_zero(cb)) return replace(id, a);
                    if (ba && int_zero(ca)) return replace(id, b);
                } else if (op == BinOp::Sub) {
                    if (bb && int_zero(cb)) return replace(id, a);
                } else if (op == BinOp::Mul) {
                    if (bb && int_one(cb)) return replace(id, a);
                    if (ba && int_one(ca)) return replace(id, b);
                    if (!fp && bb && int_zero(cb)) return fold_to_zero(id);
                    if (!fp && ba && int_zero(ca)) return fold_to_zero(id);
                } else if (op == BinOp::Div) {
                    if (bb && int_one(cb)) return replace(id, a);
                } else if (op == BinOp::Mod) {
                    if (bb && int_one(cb) && !fp) return fold_to_zero(id);
                } else if (op == BinOp::And) {
                    if (bb && int_all_ones(cb)) return replace(id, a);
                    if (ba && int_all_ones(ca)) return replace(id, b);
                    if (bb && int_zero(cb)) return fold_to_zero(id);
                    if (ba && int_zero(ca)) return fold_to_zero(id);
                } else if (op == BinOp::Or) {
                    if (bb && int_zero(cb)) return replace(id, a);
                    if (ba && int_zero(ca)) return replace(id, b);
                    if (bb && int_all_ones(cb)) return replace(id, b);
                    if (ba && int_all_ones(ca)) return replace(id, a);
                } else if (op == BinOp::Xor) {
                    if (bb && int_zero(cb)) return replace(id, a);
                    if (ba && int_zero(ca)) return replace(id, b);
                    if (bb && int_all_ones(cb)) {
                        NodeId nn = g_.make(Op::Un, n.ty, {n.in[0], a}, static_cast<u8>(UnOp::BNot));
                        return replace(id, nn);
                    }
                } else if (op == BinOp::Shl || op == BinOp::Shr) {
                    if (bb && int_zero(cb)) return replace(id, a);
                }
                return false;
            }
            case Op::Cast: {
                // no-op cast: target type == operand type
                if (g_.node(n.in[1]).ty == n.ty) return replace(id, n.in[1]);
                return false;
            }
            case Op::Select: {
                if (n.in[2] == n.in[3]) return replace(id, n.in[2]);
                return false;
            }
            case Op::Un: {
                UnOp op = static_cast<UnOp>(n.sub);
                const Node& x = g_.node(n.in[1]);
                if (x.op == Op::Un && x.sub == n.sub &&
                    (op == UnOp::Not || op == UnOp::BNot)) {
                    // !!x -> x ; ~~x -> x   (logical not / bitwise not)
                    return replace(id, x.in[1]);
                }
                if (x.op == Op::Un && x.sub == static_cast<u8>(UnOp::Neg) && op == UnOp::Neg) {
                    return replace(id, x.in[1]);
                }
                return false;
            }
            default:
                return false;
        }
    }

    bool fold_to_zero(NodeId id) {
        Node& n = g_.node(id);
        ConstVal z;
        z.ty = n.ty;
        z.is_fp = ty_is_float(n.ty);
        z.iv = 0;
        z.fv = 0.0;
        NodeId zero = make_const_node(g_, n.in[0], z);
        return replace(id, zero);
    }

    Graph& g_;
};
} // namespace

class IdentityCollapsePass : public Pass {
public:
    const char* name() const override { return "IdentityCollapse"; }
    int order() const override { return 2; }
    const char* phase_name() const override { return "Phase 0: Frontend Residue Cleanup"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            IdentityCollapser c(fg.g);
            changed |= c.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(IdentityCollapsePass, 2, "Phase 0")

} // namespace jules
