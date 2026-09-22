// Pass 61 — MaskGeneration (Phase 5)
//
// Generates the ACTIVE-LANE MASK machinery for conditional packed values.
// The loop vectorizer (56) packs if-converted loop bodies: a scalar
// `Select(c, t, f)` whose condition is a body Cmp becomes a packed
// Select(vmask, tvec, fvec), where the packed Cmp produces a per-lane
// all-ones/all-zeros mask in the same vector type (the exact bit shape
// x86 cmpps/cmppd/pcmpeqd produce). This pass lowers every packed Select
// into the EXPLICIT mask blend so machine lowering is a plain pand triple:
//
//   Select(m, t, f)  ->  Or(And(m, t), AndNot(m, f))
//                          pand     pandn(~m & f)   por
//
// Degenerate zero arms collapse the triple bitwise (no lane arithmetic,
// exact for every lane kind including NaN-carrying f64 lanes):
//   Select(m, t, ZERO)  ->  And(m, t)         [AndNot(m,0)=0; Or(x,0)=x]
//   Select(m, ZERO, f)  ->  AndNot(m, f)       [And(m,0)=0; Or(0,x)=x]
// one packed op instead of three — the canonical masked-accumulate /
// clamp-to-zero family (gcc lowers the same source to a single maxpd).
//
// CLAMP-TO-ZERO MIN/MAX: when the mask is a relational compare of the
// nonzero arm against the SAME +0.0 the zero arm broadcasts, the blend
// is a single min/max instruction (gcc's one-maxpd shape for
// `v > 0 ? v : 0`). Exactly two forms are bitwise-identical to the
// select on EVERY lane, including NaN and -0.0:
//   Select(Gt(t, Z), t, Z)  ->  Max(Z, t)   [maxpd dst=t src=Z]
//   Select(Lt(t, Z), t, Z)  ->  Min(t, Z)   [minpd dst=t src=Z]
// Gt/Lt only: the Ge/Le forms differ on -0.0 lanes (the select yields
// the raw -0.0, the instruction yields +0.0 on equal operands).
//
// Soundness is bitwise: no arithmetic touches the lanes, so FP strictness
// is preserved exactly (blend picks t's or f's bits per lane; the packed
// compare uses ORDERED predicates — NaN semantics match the scalar
// ucomis forms bit for bit). SSE2 has no blendv (that is SSE4.1's
// vpblendvb); the and/andn/or triple is the canonical SSE2 masked blend.
//
// Why lower at the SoN level instead of in the isel: the blend becomes
// visible to GVN (shared masks across selects with the same condition),
// DCE, and the machine peepholes as ordinary packed Bins — and the
// instruction selector keeps a safety-net emission for any packed Select
// that survives (kill-switch --disable 61), emitting the same triple.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {
class MaskGenerator {
public:
    explicit MaskGenerator(Graph& g) : g_(g) {}

    u32 run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op != Op::Select || n.op == Op::Dead) continue;
            if (!ty_is_vector(n.ty)) continue; // scalar selects: pass 17's
            Node sn = n; // copy: make() below may grow nodes_
            NodeId pin = sn.in[0];
            if (pin == kNoNode || g_.is_dead(pin)) continue;
            NodeId m = sn.in[1], t = sn.in[2], f = sn.in[3];
            if (m == kNoNode || t == kNoNode || f == kNoNode) continue;
            if (g_.is_dead(m) || g_.is_dead(t) || g_.is_dead(f)) continue;

            NodeId blend;
            if (NodeId mm = try_clamp_minmax(sn, m, t, f); mm != kNoNode) {
                blend = mm;
            } else if (is_packed_zero(t)) {
                // Select(m, ZERO, f): the true lanes are zero
                blend = g_.make(Op::Bin, sn.ty, {pin, m, f},
                                static_cast<u8>(BinOp::AndNot));
            } else if (is_packed_zero(f)) {
                // Select(m, t, ZERO): the false lanes are zero
                blend = g_.make(Op::Bin, sn.ty, {pin, m, t},
                                static_cast<u8>(BinOp::And));
            } else {
                // And(m, t): mask-selected true lanes
                NodeId and_t = g_.make(Op::Bin, sn.ty, {pin, m, t},
                                       static_cast<u8>(BinOp::And));
                // AndNot(m, f) = ~m & f: mask-cleared false lanes
                NodeId andnot_f = g_.make(Op::Bin, sn.ty, {pin, m, f},
                                          static_cast<u8>(BinOp::AndNot));
                // Or: the blend
                blend = g_.make(Op::Bin, sn.ty, {pin, and_t, andnot_f},
                                static_cast<u8>(BinOp::Or));
            }

            g_.replace_all_uses(id, blend);
            g_.kill(id);
            ++lowered_;
        }
        if (lowered_) g_.touch();
        return lowered_;
    }

private:
    Graph& g_;
    u32 lowered_ = 0;

    // The vectorizer packs invariant scalar arms as Broadcast(Const);
    // a zero arm is either that broadcast form or a raw vector const.
    bool is_packed_zero(NodeId x) {
        if (x == kNoNode || g_.is_dead(x)) return false;
        const Node& xn = g_.node(x);
        if (xn.op == Op::Cast &&
            xn.sub == static_cast<u8>(CastOp::Broadcast) &&
            xn.in[1] != kNoNode) {
            const Node& src = g_.node(xn.in[1]);
            if (src.op == Op::Const)
                return ty_is_float(src.ty) ? src.fval == 0.0 : src.ival == 0;
        }
        if (xn.op == Op::Const && ty_is_vector(xn.ty))
            return xn.ival == 0 && xn.fval == 0.0;
        return false;
    }

    // Clamp-to-zero min/max (see the header comment). Returns the packed
    // Bin when the shape is one of the two exact forms, kNoNode otherwise.
    // `sn` is a COPY of the select node (the graph may grow inside).
    NodeId try_clamp_minmax(const Node& sn, NodeId m, NodeId t, NodeId f) {
        if (g_.node(m).op != Op::Cmp) return kNoNode;
        const Node& c = g_.node(m);
        // FP lanes only: SSE2 has no packed integer min/max
        if (!ty_is_float(ty_lane_type(sn.ty))) return kNoNode;
        // the compared value must BE the nonzero arm (mirrored compares
        // swap both operands and the relation)
        NodeId ct = c.in[1], cz = c.in[2];
        CmpOp rel = static_cast<CmpOp>(c.sub);
        if (ct != t) {
            if (cz != t) return kNoNode;
            std::swap(ct, cz);
            if (rel == CmpOp::Gt) rel = CmpOp::Lt;
            else if (rel == CmpOp::Lt) rel = CmpOp::Gt;
        }
        if (rel != CmpOp::Gt && rel != CmpOp::Lt) return kNoNode;
        // the compared constant must be the same +0.0 the zero arm holds
        if (!is_packed_zero(cz)) return kNoNode;
        if (!is_packed_zero(f)) return kNoNode;
        if (is_packed_zero(t)) return kNoNode; // degenerate: both arms zero
        NodeId pin = sn.in[0];
        if (rel == CmpOp::Gt)
            // Max(Z, t): maxpd dst=t src=Z (NaN/-0 lanes -> +0.0)
            return g_.make(Op::Bin, sn.ty, {pin, f, t},
                           static_cast<u8>(BinOp::Max));
        // Min(t, Z): minpd dst=t src=Z (NaN/-0 lanes -> +0.0)
        return g_.make(Op::Bin, sn.ty, {pin, t, f},
                       static_cast<u8>(BinOp::Min));
    }
};
} // namespace

class MaskGenerationPass : public Pass {
public:
    const char* name() const override { return "MaskGeneration"; }
    int order() const override { return 61; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            MaskGenerator mg(fg.g);
            changed |= mg.run() > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(MaskGenerationPass, 61, "Phase 5")

} // namespace jules
