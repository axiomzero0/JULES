// Pass 65 — SIMDIntrinsicMatching (Phase 5)
//
// PURPOSE: Map source idioms to single SSE2 instructions the generic Bin
// lowering would never find. First family: FP min/max.
//
//   `if a < b { m = a } else { m = b }`  (and the 7 other relational/arm
//    permutations) — IfConversion (p49) makes it a Select over a Cmp; the
//    generic FP select lowering is TWO branches + two loads (emit_select).
//    This pass recognizes the shape and rewrites it to a single Min/Max
//    Bin node, which the isel emits as one `minsd`/`maxsd` (or `minpd`/
//    `maxpd` when a later vectorization sweep packs it).
//
// NaN EXACTNESS: x86 minsd/maxsd return the DST operand on unordered
// compares. The matcher therefore keeps operand order load-bearing: every
// matched source shape maps to one of the two canonical IR forms whose
// select semantics (including the NaN arm) are bit-identical:
//   Min(a,b) = Select(Lt(a,b), a, b)   -> minsd src=a dst=b
//   Max(a,b) = Select(Lt(a,b), b, a)   -> maxsd src=b dst=a
// Machine passes must never commute Min/Max operands (NaN asymmetry).
//
// Integer selects are left alone: the Cmov lowering is already one
// instruction (no SSE integer min/max exists below SSE4.1's pminsd).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {

// Which canonical op does `Select(Cmp(rel p q), t, f)` with {t,f}=={p,q}
// equal EXACTLY (ordered + unordered)? After normalizing Gt/Ge to Lt/Le
// over swapped operands, p is the "smaller side" candidate: rel true
// means p <= q. The true arm then either keeps the smaller side (Min) or
// the larger side (Max); the false arm takes the other operand.
bool match_minmax(Graph& g, NodeId sel, NodeId& p_out, NodeId& q_out, bool& is_min) {
    const Node& s = g.node(sel);
    if (s.op != Op::Select) return false;
    if (!ty_is_float(s.ty)) return false; // FP only (ints: Cmov is fine)
    NodeId c = s.in[1];
    if (c == kNoNode || g.node(c).op != Op::Cmp) return false;
    const Node& cn = g.node(c);
    NodeId p = cn.in[1], q = cn.in[2];
    if (p == kNoNode || q == kNoNode || p == q) return false;
    if (!ty_is_float(g.node(p).ty) || !ty_is_float(g.node(q).ty)) return false;

    CmpOp rel = static_cast<CmpOp>(cn.sub);
    // normalize to the Lt/Le family over (p, q): Gt(p,q) == Lt(q,p),
    // Ge(p,q) == Le(q,p). Eq/Ne select neither side of an order.
    if (rel == CmpOp::Gt || rel == CmpOp::Ge) {
        std::swap(p, q);
        rel = (rel == CmpOp::Gt) ? CmpOp::Lt : CmpOp::Le;
    }
    if (rel != CmpOp::Lt && rel != CmpOp::Le) return false;

    NodeId t = s.in[2], f = s.in[3];
    if (t == p && f == q) {          // true (p<=q) keeps the smaller side
        is_min = true;
    } else if (t == q && f == p) {   // true keeps the larger side
        is_min = false;
    } else {
        return false;                // arms not the compare operands
    }
    p_out = p;
    q_out = q;
    return true;
}

} // namespace

class SIMDIntrinsicMatchingPass : public Pass {
public:
    const char* name() const override { return "SIMDIntrinsicMatching"; }
    int order() const override { return 65; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Graph& g = fg.g;
            for (NodeId id = 0; id < g.size(); ++id) {
                NodeId p, q;
                bool is_min = false;
                if (!match_minmax(g, id, p, q, is_min)) continue;
                // Rewrite the select in place: same pin, Min/Max binop.
                Node& s = g.node(id);
                NodeId pin = s.in[0];
                s.op = Op::Bin;
                s.sub = static_cast<u8>(is_min ? BinOp::Min : BinOp::Max);
                s.n_in = 3;
                s.in[0] = pin;
                s.in[1] = p;
                s.in[2] = q;
                s.in[3] = kNoNode;
                g.mark_uses_dirty();
                g.touch();
                changed = true;
            }
        }
        return changed;
    }
};

JULES_REGISTER_PASS(SIMDIntrinsicMatchingPass, 65, "Phase 5")

} // namespace jules
