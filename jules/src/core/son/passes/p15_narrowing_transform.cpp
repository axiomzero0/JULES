// Pass 15 — NarrowingTransform (Phase 1)
//
// Demotes 64-bit integer ops to 32-bit when every consumer truncates:
//   Trunc(Bin64(ext(x32), ext(y32)))  ->  Bin32(x32, y32)
//
// The legality rule is the contract's "narrow-type check against all
// uses": EVERY use of the wide Bin must be a Trunc to the same 32-bit
// type. Each input must either (a) be a SExt/ZExt from a 32-bit source
// (the extension cancels against the trailing truncation — the same
// cancellation p14 proves for the extend-free chain trunc(extend(x))),
// or (b) be a constant (retyped: only the low 32 bits can reach any
// consumer through the truncation).
//
// Commuting ops (low 32 bits of the wide op == the narrow op, two's
// complement modular arithmetic): Add, Sub, Mul, And, Or, Xor, Shl
// (constant shift < 32 only — the machine masks i64 shift counts to 63
// and i32 counts to 31, so a runtime count would change semantics).
// Excluded (documented): Div/Mod (a/b truncated != (a mod 2^32)/(b mod
// 2^32) — the quotient of the low bits is not the low bits of the
// quotient), Shr (shifted-in bits differ), Min/Max (signed order of the
// low 32 bits is not the order of the full values), Cmp/Select uses
// (not truncations; a comparison of full-width values is not a
// comparison of the truncated ones).
//
// Profitability is structural: the rewrite adds zero nodes (inputs are
// reused; constants are retyped in place) and deletes one Trunc per
// consumer plus the wide Bin, so it fires exactly when the shape matches.
// The pass is sound for the isel because it only produces node shapes
// that ordinary 32-bit source arithmetic already produces — the
// all-uses-trunc rule means no 64-bit consumer ever observes the
// retyped op.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class NarrowingTransformer {
public:
    explicit NarrowingTransformer(Graph& g) : g_(g) {}

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

    // Narrowable commutative-with-truncation binops.
    static bool narrow_op(BinOp op) {
        switch (op) {
            case BinOp::Add: case BinOp::Sub: case BinOp::Mul:
            case BinOp::And: case BinOp::Or: case BinOp::Xor:
                return true;
            default:
                return false; // Shl handled separately (const count gate)
        }
    }

    // May `n` serve as a narrow (32-bit) input? Ext (extends cancel
    // against the consumers' truncation) and Const (only low bits can
    // survive) qualify; anything else keeps the op wide.
    bool narrow_input(NodeId n, TypeId narrow_ty, NodeId& out_input, bool& made_const) {
        const Node& x = g_.node(n);
        if (x.op == Op::Cast) {
            CastOp k = static_cast<CastOp>(x.sub);
            // EXACT type identity (not just bit width): matching by bits
            // let a ZExt(u32) feed i32 consumers (or SExt(i32) feed u32
            // consumers) and emitted a narrow Bin whose operand types
            // differed from its result — verifier-invalid IR on otherwise
            // sign-agnostic shapes. The low-32 result is identical either
            // way, but the graph must stay type-homogeneous.
            if ((k == CastOp::SExt || k == CastOp::ZExt) &&
                g_.node(x.in[1]).ty == narrow_ty) {
                out_input = x.in[1];
                made_const = false;
                return true;
            }
            return false;
        }
        if (x.op == Op::Const) {
            // Retype into the narrow domain: an i32 constant keeps the
            // sign-extended low 32 bits, a u32 constant the zero-extended
            // ones. Either representation carries the exact low-32 bit
            // pattern the machine imm emits at 32-bit width.
            i64 iv = x.ival;
            if (narrow_ty == ty_u32()) iv = static_cast<i64>(static_cast<u64>(iv) & 0xFFFFFFFFull);
            else iv = static_cast<i64>(static_cast<i32>(static_cast<u32>(iv)));
            NodeId c = g_.make(Op::Const, narrow_ty, {x.in[0]});
            g_.node(c).ival = iv;
            out_input = c;
            made_const = true;
            return true;
        }
        return false;
    }

    bool visit(NodeId id) {
        // Snapshot: narrow_input/make below can reallocate the node
        // vector (see p91's NOTE) — no Node& may survive across them.
        Node nc = g_.node(id);
        if (nc.op != Op::Bin) return false;
        if (!ty_is_int(nc.ty) || ty_is_vector(nc.ty) || ty_bits(nc.ty) != 64)
            return false;

        BinOp bop = static_cast<BinOp>(nc.sub);
        bool is_shl = bop == BinOp::Shl;
        if (!narrow_op(bop) && !is_shl) return false;

        // Shl: the count must be a constant below the narrow width —
        // both widths mask runtime counts differently (63 vs 31), and a
        // count >= 32 truncates everything away while the narrow op
        // would keep low bits.
        if (is_shl) {
            const Node& cnt = g_.node(nc.in[2]);
            if (cnt.op != Op::Const) return false;
            if (static_cast<u64>(cnt.ival & 63) >= 32) return false;
        }

        // All consumers must truncate to the SAME 32-bit type (copied:
        // the cached use list mutates as consumers are rewired).
        std::vector<NodeId> uses;
        {
            const SmallVec<NodeId, 4>& live = g_.uses_of(id);
            if (live.empty()) return false;
            uses.assign(live.begin(), live.end());
        }
        TypeId narrow_ty = ty_none();
        for (NodeId u : uses) {
            const Node& un = g_.node(u);
            if (un.op != Op::Cast || un.sub != static_cast<u8>(CastOp::Trunc))
                return false;
            if (!ty_is_int(un.ty) || ty_bits(un.ty) != 32) return false;
            if (narrow_ty == ty_none()) narrow_ty = un.ty;
            else if (narrow_ty != un.ty) return false; // mixed i32/u32 domains
        }

        // Inputs must narrow cleanly.
        NodeId a = kNoNode, b = kNoNode;
        bool ac = false, bc = false;
        if (!narrow_input(nc.in[1], narrow_ty, a, ac)) return false;
        if (!narrow_input(nc.in[2], narrow_ty, b, bc)) return false;
        if (a == kNoNode || b == kNoNode) return false;

        // Build the narrow op at the original pin and swap every
        // consumer's truncation for it.
        NodeId pin = nc.in[0];
        NodeId narrow = g_.make(Op::Bin, narrow_ty, {pin, a, b}, nc.sub);
        for (NodeId u : uses) {
            g_.replace_all_uses(u, narrow);
            g_.kill(u);
        }
        g_.kill(id); // every use was a killed truncation
        return true;
    }

    Graph& g_;
    bool changed_ = false;
};
} // namespace

class NarrowingTransformPass : public Pass {
public:
    const char* name() const override { return "NarrowingTransform"; }
    int order() const override { return 15; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            NarrowingTransformer t(fg.g);
            changed |= t.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(NarrowingTransformPass, 15, "Phase 1")

} // namespace jules
