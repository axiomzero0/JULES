// Pass 13 — Reassociation (Phase 1)
//
// Reorders associative ops into canonical form so equivalent expressions
// hash equal in GVN:
//   * commutative ops (add/mul/and/or/xor): operands ordered by node id
//   * constants moved to the right operand
// Floating point is deliberately untouched by default: reassociation
// changes results under IEEE semantics (the side-condition the catalog
// calls out). --fp=fast is the explicit, documented opt-in that allows FP
// reassociation (still no reordering across divisions or mixed signs —
// only operand canonicalization of add/mul pairs).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class Reassociator {
public:
    explicit Reassociator(Graph& g, bool fp_fast) : g_(g), fp_fast_(fp_fast) {}

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
    static constexpr u32 kMaxRounds = 4;

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op != Op::Bin) return false;
        if (ty_is_float(n.ty) && !fp_fast_) return false; // FP semantics preserved

        BinOp op = static_cast<BinOp>(n.sub);
        bool commutative = op == BinOp::Add || op == BinOp::Mul || op == BinOp::And ||
                           op == BinOp::Or || op == BinOp::Xor;
        if (!commutative) return false;

        NodeId a = n.in[1], b = n.in[2];
        ConstVal ca, cb;
        bool ac = const_of(g_, a, ca);
        bool bc = const_of(g_, b, cb);

        // constant on the left -> move right
        if (ac && !bc) {
            g_.set_input(id, 1, b); // b was read into a local first
            g_.set_input(id, 2, a);
            return true;
        }
        // canonical: lower node id on the left
        if (!ac && !bc && a > b) {
            g_.set_input(id, 1, b);
            g_.set_input(id, 2, a);
            return true;
        }
        return false;
    }

    Graph& g_;
    bool fp_fast_ = false;
    bool changed_ = false;
};
} // namespace

class ReassociationPass : public Pass {
public:
    const char* name() const override { return "Reassociation"; }
    int order() const override { return 13; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        bool fp_fast = fp_fast_allowed(ctx.opts.fp);
        for (FunctionGraph& fg : ctx.mod.fns) {
            Reassociator r(fg.g, fp_fast);
            changed |= r.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(ReassociationPass, 13, "Phase 1")

} // namespace jules
