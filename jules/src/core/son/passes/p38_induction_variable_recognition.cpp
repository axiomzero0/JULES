// Pass 38 — InductionVariableRecognition (Phase 4)
//
// Finds basic induction variables: header phis of the form
//     iv = phi(init, iv + step)   [step a constant]
// plus their trip-count-relevant comparisons. Recorded as telemetry and
// available for re-computation by consumers (LoopClassification, the
// vectorizer scaffolds). Analysis only.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
struct IvFact {
    NodeId phi = kNoNode;
    NodeId init = kNoNode;
    i64 step = 0;
    BinOp step_op = BinOp::Add;
};

class IvRecognizer {
public:
    IvRecognizer(Graph& g, LoopInfo& li) : g_(g), li_(li) {}

    bool run() {
        for (const Loop& l : li_.loops()) {
            for (NodeId u : g_.uses_of(l.header)) {
                const Node& phi = g_.node(u);
                if (phi.op != Op::Phi || phi.ty == ty_mem()) continue;
                IvFact fact;
                if (recognize(l, u, fact)) facts_.push_back(fact);
            }
        }
        return !facts_.empty();
    }

    size_t count() const { return facts_.size(); }

private:
    bool recognize(const Loop& l, NodeId phi_id, IvFact& out) {
        const Node& phi = g_.node(phi_id);
        for (u8 i = 1; i < phi.n_in; ++i) {
            NodeId v = phi.in[i];
            if (v == phi_id) continue; // self pass-through
            const Node& vn = g_.node(v);
            if (vn.op != Op::Bin) continue;
            if (vn.sub != static_cast<u8>(BinOp::Add) && vn.sub != static_cast<u8>(BinOp::Sub))
                continue;
            // one operand is the phi itself, the other a constant
            NodeId other = kNoNode;
            if (vn.in[1] == phi_id) other = vn.in[2];
            else if (vn.in[2] == phi_id) other = vn.in[1];
            else continue;
            ConstVal step;
            if (!const_of(g_, other, step)) continue;
            // the update must live inside the loop
            if (!li_.block_in_loop(l, g_.node(v).in[0]) && g_.node(v).in[0] != l.header) continue;

            out.phi = phi_id;
            out.step_op = static_cast<BinOp>(vn.sub);
            out.step = step.iv * (vn.sub == static_cast<u8>(BinOp::Sub) ? -1 : 1);
            // init: the other phi input (not the update chain)
            for (u8 j = 1; j < phi.n_in; ++j)
                if (j != i) out.init = phi.in[j];
            return true;
        }
        return false;
    }

    Graph& g_;
    LoopInfo& li_;
    std::vector<IvFact> facts_;
};
} // namespace

class InductionVariableRecognitionPass : public Pass {
public:
    const char* name() const override { return "InductionVariableRecognition"; }
    int order() const override { return 38; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override { return 0; }
    bool run(PassContext& ctx) override {
        bool any = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            IvRecognizer r(fg.g, ctx.analysis.loops(fg));
            any |= r.run();
        }
        return any;
    }
};

JULES_REGISTER_PASS(InductionVariableRecognitionPass, 38, "Phase 4")

} // namespace jules
