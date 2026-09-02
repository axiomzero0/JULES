// Pass 3 — ConstantFolding (Phase 0: Frontend Residue Cleanup)
//
// Evaluates pure ops (Bin/Cmp/Un/Cast/Select) whose operands are all
// constants. Deliberately does NOT propagate through control flow — that is
// SCCP's job (pass 8). Division by zero stays runtime (UB model documented).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class ConstantFolder {
public:
    explicit ConstantFolder(Graph& g) : g_(g) {}

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
    static constexpr u32 kMaxRounds = 8;

    bool visit(NodeId id) {
        Node& n = g_.node(id);
        if (n.op == Op::Dead) return false;
        ConstVal out;
        switch (n.op) {
            case Op::Bin: {
                ConstVal a, b;
                if (!const_of(g_, n.in[1], a) || !const_of(g_, n.in[2], b)) return false;
                if (!eval_bin_const(static_cast<BinOp>(n.sub), a, b, out)) return false;
                break;
            }
            case Op::Cmp: {
                ConstVal a, b;
                if (!const_of(g_, n.in[1], a) || !const_of(g_, n.in[2], b)) return false;
                if (!eval_cmp_const(static_cast<CmpOp>(n.sub), a, b, out)) return false;
                break;
            }
            case Op::Un: {
                ConstVal a;
                if (!const_of(g_, n.in[1], a)) return false;
                if (!eval_un_const(static_cast<UnOp>(n.sub), a, out)) return false;
                break;
            }
            case Op::Cast: {
                ConstVal a;
                if (!const_of(g_, n.in[1], a)) return false;
                if (!eval_cast_const(static_cast<CastOp>(n.sub), a, n.ty, out)) return false;
                break;
            }
            case Op::Select: {
                ConstVal c;
                if (!const_of(g_, n.in[1], c)) return false;
                NodeId pick = c.iv != 0 ? n.in[2] : n.in[3];
                g_.replace_all_uses(id, pick);
                g_.kill(id);
                return true;
            }
            default:
                return false;
        }
        NodeId folded = make_const_node(g_, n.in[0], out);
        g_.replace_all_uses(id, folded);
        g_.kill(id);
        return true;
    }

    Graph& g_;
};
} // namespace

class ConstantFoldingPass : public Pass {
public:
    const char* name() const override { return "ConstantFolding"; }
    int order() const override { return 3; }
    const char* phase_name() const override { return "Phase 0: Frontend Residue Cleanup"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            ConstantFolder f(fg.g);
            changed |= f.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(ConstantFoldingPass, 3, "Phase 0")

} // namespace jules
