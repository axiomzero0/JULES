// Pass 14 — SignExtensionElimination (Phase 1)
//
// Removes redundant sign/zero extends via producer/consumer width analysis:
//   * extend-of-extend chains collapse to a single extend from the base
//   * trunc(extend(x)) == x when widths cancel
//   * extend to the operand's own width is a no-op
// Critical for 32/64 target normalization (i64 math on i32 sources).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class SignExtensionEliminator {
public:
    explicit SignExtensionEliminator(Graph& g) : g_(g) {}

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
        if (from == to) return false;
        g_.replace_all_uses(from, to);
        g_.kill(from);
        return true;
    }

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op != Op::Cast) return false;
        CastOp kind = static_cast<CastOp>(n.sub);
        if (kind != CastOp::SExt && kind != CastOp::ZExt && kind != CastOp::Trunc) return false;
        NodeId x = n.in[1];
        const Node& xn = g_.node(x);
        if (xn.op == Op::Dead) return false;

        // extend to own width: no-op
        if ((kind == CastOp::SExt || kind == CastOp::ZExt) && xn.ty == n.ty)
            return replace(id, x);

        if (xn.op == Op::Cast) {
            CastOp inner_kind = static_cast<CastOp>(xn.sub);
            NodeId base = xn.in[1];
            const Node& basen = g_.node(base);
            if (basen.op == Op::Dead) return false;

            // trunc(extend(x)) with canceling widths -> x
            if (kind == CastOp::Trunc &&
                (inner_kind == CastOp::SExt || inner_kind == CastOp::ZExt) &&
                ty_bits(basen.ty) == ty_bits(n.ty))
                return replace(id, base);

            // extend(extend(x)) -> single extend base -> target (sign chosen
            // by the OUTER op only when the inner widened first; if the inner
            // was a trunc, keep the outer semantics from the base)
            if ((kind == CastOp::SExt || kind == CastOp::ZExt) &&
                (inner_kind == CastOp::SExt || inner_kind == CastOp::ZExt)) {
                CastOp single = kind; // outer extension dominates
                NodeId direct = g_.make(Op::Cast, n.ty, {n.in[0], base},
                                        static_cast<u8>(single));
                return replace(id, direct);
            }

            // trunc(trunc(x)) -> trunc base -> target directly
            if (kind == CastOp::Trunc && inner_kind == CastOp::Trunc)
                return replace(id, x); // inner trunc already <= target width
        }
        return false;
    }

    Graph& g_;
    bool changed_ = false;
};
} // namespace

class SignExtensionEliminationPass : public Pass {
public:
    const char* name() const override { return "SignExtensionElimination"; }
    int order() const override { return 14; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            SignExtensionEliminator e(fg.g);
            changed |= e.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(SignExtensionEliminationPass, 14, "Phase 1")

} // namespace jules
