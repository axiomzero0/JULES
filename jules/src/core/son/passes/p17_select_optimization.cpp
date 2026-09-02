// Pass 17 — SelectOptimization (Phase 1)
//
// Ternary/select node specific rewrites (Select is a distinct SoN node):
//   * Select(!c, a, b) -> Select(c, b, a)
//   * Select(c, Select(c, x, y), z) -> Select(c, x, z)   (nested same-cond)
//   * Select(c, x, Select(c, y, z)) -> Select(c, x, z)
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class SelectOptimizer {
public:
    explicit SelectOptimizer(Graph& g) : g_(g) {}

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
        if (n.op != Op::Select) return false;
        NodeId c = n.in[1], t = n.in[2], f = n.in[3];
        const Node& cn = g_.node(c);

        // inverted condition: swap arms and strip the Not
        if (cn.op == Op::Un && cn.sub == static_cast<u8>(UnOp::Not)) {
            g_.set_input(id, 1, cn.in[1]);
            g_.set_input(id, 2, f);
            g_.set_input(id, 3, t);
            return true;
        }

        const Node& tn = g_.node(t);
        const Node& fn = g_.node(f);

        // nested selects on the same condition
        if (tn.op == Op::Select && tn.in[1] == c && t != id) {
            g_.set_input(id, 2, tn.in[2]);
            return true;
        }
        if (fn.op == Op::Select && fn.in[1] == c && f != id) {
            g_.set_input(id, 3, fn.in[3]);
            return true;
        }
        return false;
    }

    Graph& g_;
    bool changed_ = false;
};
} // namespace

class SelectOptimizationPass : public Pass {
public:
    const char* name() const override { return "SelectOptimization"; }
    int order() const override { return 17; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            SelectOptimizer s(fg.g);
            changed |= s.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(SelectOptimizationPass, 17, "Phase 1")

} // namespace jules
