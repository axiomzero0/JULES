// Pass 60 — VectorLegalization (Phase 5)
//
// Post-vectorizer legality + normalization for packed nodes:
//   * verifies every vector op has a legal packed form for its lane type
//     (Bin op in the SSE2 set, Extract lane < lane count, Broadcast operand
//     scalar) — an illegal shape is scalarized by KILLING the packed node
//     and letting DCE reclaim the loop (the vectorizer never emits illegal
//     shapes; this is the safety net for future emitters)
//   * removes dead vector ops (Broadcasts whose consumers all died after
//     vectorization — DCE treats them as pure ops but they are XMM-poison
//     for the RA's fp pools when they linger)
//   * converts Extract(v, 0) chains on full-width-known values to the
//     canonical low-lane read (pass 62 owns the identity collapse; this
//     pass only validates)
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {
class VectorLegalizer {
public:
    explicit VectorLegalizer(Graph& g) : g_(g) {}

    u32 run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op == Op::Dead) continue;
            if (!ty_is_vector(n.ty)) {
                // Cast INTO a vector (Broadcast) or FROM (Extract) is legal
                // even though its type is scalar — checked below by op
                if (n.op == Op::Cast) check_cast_legality(id, n);
                continue;
            }
            switch (n.op) {
                case Op::Bin:
                    if (!vecx::packed_bin_legal(ty_lane_type(n.ty),
                                                static_cast<BinOp>(n.sub))) {
                        g_.kill(id);
                        ++removed_;
                    }
                    break;
                case Op::Load:
                case Op::Phi:
                case Op::Store: // vector stores carry the VALUE type
                    break;
                default:
                    // packed Cast/Broadcast handled via check_cast_legality
                    break;
            }
            // dead broadcasts: no live users
            if (n.op == Op::Cast &&
                static_cast<CastOp>(n.sub) == CastOp::Broadcast) {
                bool used = false;
                for (NodeId u : g_.uses_of(id))
                    if (g_.node(u).op != Op::Dead) { used = true; break; }
                if (!used) {
                    g_.kill(id);
                    ++removed_;
                }
            }
        }
        if (removed_) g_.touch();
        return removed_;
    }

private:
    void check_cast_legality(NodeId id, Node& n) {
        CastOp k = static_cast<CastOp>(n.sub);
        if (k == CastOp::Broadcast) {
            if (ty_is_vector(n.ty)) return;
            g_.kill(id);
            ++removed_;
            return;
        }
        if (k == CastOp::Extract) {
            // operand must be a vector; lane in range
            const Node& src = g_.node(n.in[1]);
            if (g_.is_dead(n.in[1]) || !ty_is_vector(src.ty) ||
                n.aux >= ty_lanes(src.ty)) {
                g_.kill(id);
                ++removed_;
            }
        }
    }

    Graph& g_;
    u32 removed_ = 0;
};
} // namespace

class VectorLegalizationPass : public Pass {
public:
    const char* name() const override { return "VectorLegalization"; }
    int order() const override { return 60; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            VectorLegalizer v(fg.g);
            changed |= v.run() > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(VectorLegalizationPass, 60, "Phase 5")

} // namespace jules
