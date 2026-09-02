// Pass 5 — ComptimeResidueFold (Phase 0, AOT)
//
// Re-evaluates partially-resolved comptime expressions AFTER specialization
// and inlining have run. Distinct from plain ConstantFolding: this pass runs
// a bottom-up memoized evaluator over the pure subgraph (the same evaluation
// core the comptime interpreter uses in sema), which folds transitive
// constant chains even when intermediate nodes were not written as constants
// by the frontend (e.g. values exposed by argument specialization).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class ComptimeResidueFolder {
public:
    struct OptVal {
        bool has = false;
        ConstVal v;
    };

    explicit ComptimeResidueFolder(Graph& g) : g_(g) {}

    bool run() {
        // snapshot the size: folding appends Const nodes that must not be
        // revisited by this pass (they are already canonical).
        const NodeId end = static_cast<NodeId>(g_.size());
        for (NodeId id = 0; id < end; ++id) {
            const Node& n = g_.node(id);
            if (n.op == Op::Dead) continue;
            if (!is_pure_op(n.op)) continue;
            ConstVal v;
            if (eval(id, v)) {
                NodeId c = make_const_node(g_, block_pin(g_, id), v);
                if (c != id) {
                    g_.replace_all_uses(id, c);
                    g_.kill(id);
                    changed_ = true;
                }
            }
        }
        return changed_;
    }

private:
    // Bottom-up memoized pure evaluation. TOP = no value.
    bool eval(NodeId id, ConstVal& out) {
        if (const OptVal* m = memo_.find(id)) {
            if (!m->has) return false;
            out = m->v;
            return true;
        }
        OptVal result;
        result.has = false;
        do_eval(id, result);
        memo_.insert(id, result);
        if (!result.has) return false;
        out = result.v;
        return true;
    }

    void do_eval(NodeId id, OptVal& result) {
        const Node& n = g_.node(id);
        switch (n.op) {
            case Op::Const: {
                result.has = const_of(g_, id, result.v);
                return;
            }
            case Op::Param:
                return; // unknown at compile time
            case Op::Bin: {
                ConstVal a, b;
                if (!eval(n.in[1], a) || !eval(n.in[2], b)) return;
                result.has = eval_bin_const(static_cast<BinOp>(n.sub), a, b, result.v);
                return;
            }
            case Op::Cmp: {
                ConstVal a, b;
                if (!eval(n.in[1], a) || !eval(n.in[2], b)) return;
                result.has = eval_cmp_const(static_cast<CmpOp>(n.sub), a, b, result.v);
                return;
            }
            case Op::Un: {
                ConstVal a;
                if (!eval(n.in[1], a)) return;
                result.has = eval_un_const(static_cast<UnOp>(n.sub), a, result.v);
                return;
            }
            case Op::Cast: {
                ConstVal a;
                if (!eval(n.in[1], a)) return;
                result.has = eval_cast_const(static_cast<CastOp>(n.sub), a, n.ty, result.v);
                return;
            }
            case Op::Select: {
                ConstVal c, t, f;
                if (!eval(n.in[1], c)) return;
                if (c.iv != 0) { result.has = eval(n.in[2], result.v); return; }
                result.has = eval(n.in[3], result.v);
                return;
            }
            default:
                return;
        }
    }

    Graph& g_;
    FlatMap<NodeId, OptVal> memo_;
    bool changed_ = false;
};
} // namespace

class ComptimeResidueFoldPass : public Pass {
public:
    const char* name() const override { return "ComptimeResidueFold"; }
    int order() const override { return 5; }
    const char* phase_name() const override { return "Phase 0: Frontend Residue Cleanup"; }
    ModeMask modes() const override { return kModeAOT; } // AOT aggressive fold
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            ComptimeResidueFolder f(fg.g);
            changed |= f.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(ComptimeResidueFoldPass, 5, "Phase 0")

} // namespace jules
