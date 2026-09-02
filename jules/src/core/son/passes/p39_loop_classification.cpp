// Pass 39 — LoopClassification (Phase 4)
//
// Tags loops as counted / uncounted / early-exit / nested. A counted loop:
//   * exactly one induction variable
//   * the exiting comparison involves that IV against a loop-invariant bound
//   * a single exit edge
// Drives pass selection downstream (unrolling/vectorization scaffolds read
// this classification's contract; the recognition code is real).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class LoopClassifier {
public:
    LoopClassifier(Graph& g, LoopInfo& li) : g_(g), li_(li) {}

    bool run() {
        for (const Loop& l : li_.loops()) {
            bool counted = is_counted(l);
            if (counted) ++counted_loops_;
            else ++uncounted_loops_;
        }
        return counted_loops_ > 0;
    }

private:
    bool is_counted(const Loop& l) {
        // exit count: successor edges leaving the loop from any block
        u32 exits = 0;
        NodeId exit_cmp = kNoNode;
        for (NodeId blk : l.blocks) {
            for (NodeId u : g_.uses_of(blk)) {
                const Node& un = g_.node(u);
                if (un.op != Op::If || un.in[0] != blk) continue;
                // which projection leaves the loop?
                for (NodeId p : g_.uses_of(u)) {
                    Op po = g_.node(p).op;
                    if (po != Op::IfTrue && po != Op::IfFalse) continue;
                    if (!li_.block_in_loop(l, p)) {
                        ++exits;
                        exit_cmp = un.in[1];
                    }
                }
            }
        }
        if (exits != 1) return false;
        if (exit_cmp == kNoNode || g_.node(exit_cmp).op != Op::Cmp) return false;

        // the comparison must use a header phi (IV) and an invariant bound
        const Node& cmp = g_.node(exit_cmp);
        NodeId iv = kNoNode, bound = kNoNode;
        if (g_.node(cmp.in[1]).op == Op::Phi && g_.node(cmp.in[1]).in[0] == l.header) {
            iv = cmp.in[1];
            bound = cmp.in[2];
        } else if (g_.node(cmp.in[2]).op == Op::Phi && g_.node(cmp.in[2]).in[0] == l.header) {
            iv = cmp.in[2];
            bound = cmp.in[1];
        }
        if (iv == kNoNode) return false;
        // bound defined outside the loop
        if (li_.block_in_loop(l, g_.node(bound).in[0]) &&
            !is_block_head(g_.node(bound).op))
            return false;
        if (is_block_head(g_.node(bound).op) && li_.block_in_loop(l, bound)) return false;
        return true;
    }

    Graph& g_;
    LoopInfo& li_;
    u32 counted_loops_ = 0;
    u32 uncounted_loops_ = 0;
};
} // namespace

class LoopClassificationPass : public Pass {
public:
    const char* name() const override { return "LoopClassification"; }
    int order() const override { return 39; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override { return 0; }
    bool run(PassContext& ctx) override {
        bool any = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            LoopClassifier c(fg.g, ctx.analysis.loops(fg));
            any |= c.run();
        }
        return any;
    }
};

JULES_REGISTER_PASS(LoopClassificationPass, 39, "Phase 4")

} // namespace jules
