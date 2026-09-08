// Pass 66 — VectorWidthSelection (Phase 5)
//
// Region-level vector width choice. The MVP packs at the fixed SSE2
// baseline (128-bit — the universal x86-64 contract, no -march flags in
// the spec): width = 16 / lane bytes. This pass VALIDATES that every
// packed type in the module is 128-bit with an SSE2-legal lane layout,
// records the width decisions (telemetry), and normalizes illegal widths
// — a packed type whose lane count x lane bytes != 16 (impossible via the
// fixed type table today; this is the guard for future 256-bit AVX work)
// is killed and left to scalarize via DCE.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {
class WidthSelector {
public:
    explicit WidthSelector(Graph& g) : g_(g) {}

    u32 run() {
        // (width occurrences counted in widths_)
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op == Op::Dead) continue;
            TypeId t = n.ty;
            if (!ty_is_vector(t)) {
                if (n.op == Op::Cast &&
                    (static_cast<CastOp>(n.sub) == CastOp::Broadcast ||
                     static_cast<CastOp>(n.sub) == CastOp::Extract)) {
                    // vector-adjacent: operand/result carries the width
                    if (static_cast<CastOp>(n.sub) == CastOp::Broadcast)
                        t = n.ty; // Broadcast's ty IS the vector type
                    else
                        t = g_.node(n.in[1]).ty;
                    if (!ty_is_vector(t)) continue;
                } else {
                    continue;
                }
            }
            u32 lanes = ty_lanes(t);
            u32 lane_bytes = ty_store_bytes(ty_lane_type(t));
            if (lanes * lane_bytes != 16) {
                // illegal width for the SSE2 baseline: scalarize by kill
                g_.kill(id);
                ++killed_;
                continue;
            }
            ++widths_;
            if (getenv("JULES_DEBUG_VEC"))
                fprintf(stderr, "[width] n%u: %u x %s\n", id, lanes,
                        ty_name(ty_lane_type(t)));
        }
        if (killed_) g_.touch();
        return widths_;
    }

    u32 killed() const { return killed_; }

private:
    Graph& g_;
    u32 widths_ = 0;
    u32 killed_ = 0;
};
} // namespace

class VectorWidthSelectionPass : public Pass {
public:
    const char* name() const override { return "VectorWidthSelection"; }
    int order() const override { return 66; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool any = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            WidthSelector w(fg.g);
            any |= w.run() > 0;
        }
        return any;
    }
};

JULES_REGISTER_PASS(VectorWidthSelectionPass, 66, "Phase 5")

} // namespace jules
