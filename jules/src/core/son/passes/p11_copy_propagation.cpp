// Pass 11 — CopyPropagation (Phase 1)
//
// Replaces uses of copy-like pass-through nodes with the original value.
// In the SoN MVP the "copy" set is: pointer casts (Ptr kind), same-width
// truncations, and self-referential pass-through phis. Richer copies
// (register moves) appear at MIR level and are handled by pass 86.
// Separate from GVN because copies may carry distinct metadata in general
// compilers (debug/lifetime); here the distinction is honored structurally.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class CopyPropagator {
public:
    explicit CopyPropagator(Graph& g) : g_(g) {}

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

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op == Op::Cast) {
            CastOp k = static_cast<CastOp>(n.sub);
            NodeId src = n.in[1];
            const Node& s = g_.node(src);
            if (s.op == Op::Dead) return false;
            bool is_copy = (k == CastOp::Ptr) ||
                           (k == CastOp::Trunc && ty_bits(s.ty) == ty_bits(n.ty));
            if (is_copy && s.ty == n.ty) {
                g_.replace_all_uses(id, src);
                g_.kill(id);
                return true;
            }
        }
        if (n.op == Op::Phi) {
            // pass-through phi: every input is either the phi itself or a
            // single other node -> that node
            NodeId other = kNoNode;
            bool ok = true;
            for (u8 i = 1; i < n.n_in; ++i) {
                NodeId v = n.in[i];
                if (v == id) continue;
                if (other == kNoNode) other = v;
                else if (v != other) { ok = false; break; }
            }
            if (ok && other != kNoNode && other != id) {
                for (u8 i = 1; i < n.n_in; ++i)
                    if (n.in[i] == id) n.in[i] = other;
                g_.mark_uses_dirty();
                g_.replace_all_uses(id, other);
                g_.kill(id);
                return true;
            }
        }
        return false;
    }

    Graph& g_;
    bool changed_ = false;
};
} // namespace

class CopyPropagationPass : public Pass {
public:
    const char* name() const override { return "CopyPropagation"; }
    int order() const override { return 11; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            CopyPropagator p(fg.g);
            changed |= p.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(CopyPropagationPass, 11, "Phase 1")

} // namespace jules
