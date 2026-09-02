// Pass 4 — TypeCanonicalization (Phase 0: Frontend Residue Cleanup)
//
// Normalizes the type layer of the graph so downstream hashing is stable:
//   * collapse cast chains that reduce to a single equivalent cast or none
//     (sext(zext(x)) etc. where widths line up)
//   * remove casts whose target equals the operand type (may appear after
//     other rewrites; IdentityCollapse catches the plain case, this pass
//     also handles chains)
//   * bool-valued Bin(And/Or/Xor) stay canonical (I1 operands)
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class TypeCanonicalizer {
public:
    explicit TypeCanonicalizer(Graph& g) : g_(g) {}
    bool run() {
        bool changed = false;
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (NodeId id = 0; id < g_.size(); ++id) this_round |= visit(id);
            changed |= this_round;
            if (!this_round) break;
        }
        return changed;
    }

private:
    static constexpr u32 kMaxRounds = 6;

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op != Op::Cast) return false;
        NodeId inner = n.in[1];
        const Node& in = g_.node(inner);
        if (in.op != Op::Cast) {
            if (in.ty == n.ty) {
                g_.replace_all_uses(id, inner);
                g_.kill(id);
                return true;
            }
            return false;
        }
        // cast(cast(x)) : x has type tx, inner casts to t1, outer to t2.
        NodeId x = in.in[1];
        TypeId tx = g_.node(x).ty, t1 = in.ty, t2 = n.ty;
        if (t1 == t2) {
            // outer cast is a no-op on the inner's result
            g_.replace_all_uses(id, inner);
            g_.kill(id);
            return true;
        }
        if (tx == t2 && ty_bits(t1) == ty_bits(t2)) {
            // trunc to t1 then re-extend to tx with same width: net identity
            g_.replace_all_uses(id, x);
            g_.kill(id);
            return true;
        }
        // widening then narrowing back to a smaller-or-equal width of x:
        // zext/sext to 64 then trunc to 32 where x is 32 -> identity
        if (ty_bits(t1) > ty_bits(tx) && ty_bits(t2) == ty_bits(tx)) {
            g_.replace_all_uses(id, x);
            g_.kill(id);
            return true;
        }
        // x wider than both: t1 and t2 both narrower-or-equal -> keep the
        // direct trunc to t2 (skip the intermediate)
        if (ty_bits(t1) <= ty_bits(tx) && ty_bits(t2) <= ty_bits(t1)) {
            CastOp kind = static_cast<CastOp>(n.sub);
            NodeId direct = g_.make(Op::Cast, t2, {n.in[0], x}, static_cast<u8>(kind));
            g_.replace_all_uses(id, direct);
            g_.kill(id);
            return true;
        }
        return false;
    }

    Graph& g_;
};
} // namespace

class TypeCanonicalizationPass : public Pass {
public:
    const char* name() const override { return "TypeCanonicalization"; }
    int order() const override { return 4; }
    const char* phase_name() const override { return "Phase 0: Frontend Residue Cleanup"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            TypeCanonicalizer t(fg.g);
            changed |= t.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(TypeCanonicalizationPass, 4, "Phase 0")

} // namespace jules
