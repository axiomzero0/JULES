// Pass 62 — ShuffleOptimization (Phase 5)
//
// Minimizes lane traffic (the SoN analog of shuffle cleanup):
//   * Extract(Broadcast(x), lane) -> x : a broadcast followed by a lane
//     extraction of the SAME origin is the identity — the scalar is
//     already in the low lane (lane 0) or was never packed usefully.
//     Vectorization and SLP create these pairs on the reduction exits and
//     around broadcast operands; collapsing them removes a punpcklqdq +
//     extraction pair per site.
//   * Extract chains whose consumer died are removed by DCE; this pass
//     owns the identity rewrite.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {
class ShuffleOptimizer {
public:
    explicit ShuffleOptimizer(Graph& g) : g_(g) {}

    u32 run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op != Op::Cast || n.op == Op::Dead) continue;
            if (static_cast<CastOp>(n.sub) != CastOp::Extract) continue;
            NodeId src = n.in[1];
            if (src == kNoNode || g_.is_dead(src)) continue;
            const Node& sn = g_.node(src);
            if (sn.op != Op::Cast || static_cast<CastOp>(sn.sub) != CastOp::Broadcast)
                continue;
            // Extract(Broadcast(x), k) == x (every lane IS x)
            NodeId origin = sn.in[1];
            if (origin == kNoNode || g_.is_dead(origin)) continue;
            if (g_.node(origin).ty != n.ty) continue; // type mismatch: keep
            g_.replace_all_uses(id, origin);
            g_.kill(id);
            ++folded_;
            changed_ = true;
        }
        if (changed_) g_.touch();
        return folded_;
    }

private:
    Graph& g_;
    u32 folded_ = 0;
    bool changed_ = false;
};
} // namespace

class ShuffleOptimizationPass : public Pass {
public:
    const char* name() const override { return "ShuffleOptimization"; }
    int order() const override { return 62; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            ShuffleOptimizer s(fg.g);
            changed |= s.run() > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(ShuffleOptimizationPass, 62, "Phase 5")

} // namespace jules
