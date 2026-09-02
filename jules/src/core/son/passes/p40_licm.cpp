// Pass 40 — LoopInvariantCodeMotion (Phase 4)
//
// Hoists loop-invariant PURE computations to the preheader (repinning the
// control input). Invariance: every data input is defined outside the loop
// (or already hoisted). Inner loops first; fixpoint within each loop.
// Loads are excluded here — LoadLICM (41) owns them with alias proofs.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class Licm {
public:
    Licm(Graph& g, LoopInfo& li) : g_(g), li_(li) {}

    bool run() {
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (const Loop& l : li_.loops()) // inner-first ordering
                this_round |= hoist_loop(l);
            changed_ |= this_round;
            if (!this_round) break;
        }
        return changed_;
    }

private:
    static constexpr u32 kMaxRounds = 6;

    bool hoist_loop(const Loop& l) {
        NodeId pre = li_.preheader(l.header);
        if (pre == kNoNode) return false;

        bool changed = false;
        for (NodeId blk : l.blocks) {
            for (NodeId u : g_.uses_of(blk)) {
                Node& un = g_.node(u);
                if (un.op == Op::Dead || un.in[0] != blk) continue;
                if (!is_pure_op(un.op)) continue;
                if (!invariant(u, l)) continue;
                g_.set_input(u, 0, pre); // repin to the preheader block
                changed = true;
            }
        }
        return changed;
    }

    bool invariant(NodeId n, const Loop& l) {
        const Node& nd = g_.node(n);
        for (u8 i = 1; i < nd.n_in; ++i) { // skip the ctrl pin
            NodeId v = nd.in[i];
            if (v == kNoNode) continue;
            const Node& vn = g_.node(v);
            if (vn.op == Op::Dead) continue;
            if (is_block_head(vn.op)) {
                if (li_.block_in_loop(l, v) && v != l.header) return false;
                continue;
            }
            NodeId def_blk = vn.in[0];
            if (def_blk == kNoNode) continue;
            if (li_.block_in_loop(l, def_blk)) return false;
            if (is_block_head(vn.op) == false && def_blk == l.header) return false;
        }
        return true;
    }

    Graph& g_;
    LoopInfo& li_;
    bool changed_ = false;
};
} // namespace

class LoopInvariantCodeMotionPass : public Pass {
public:
    const char* name() const override { return "LoopInvariantCodeMotion"; }
    int order() const override { return 40; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override {
        return static_cast<AnalysisMask>(AnalysisKind::LoopInfo); // repinning changes block contents
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Licm l(fg.g, ctx.analysis.loops(fg));
            changed |= l.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(LoopInvariantCodeMotionPass, 40, "Phase 4")

} // namespace jules
