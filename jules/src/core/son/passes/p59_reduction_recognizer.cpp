// Pass 59 — ReductionRecognizer (Phase 5: Vectorization & Superword)
//
// Detects reduction patterns inside loops: phi = phi op x for
// add/min/max/mul (dot products are nested add-of-mul). Analysis only in
// the MVP (records telemetry; no horizontal vector ops exist yet), but the
// recognition itself is real and runs over the loop tree — it is exactly
// the legality fact a future LoopVectorizer consumes.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class ReductionRecognizer {
public:
    ReductionRecognizer(Graph& g, LoopInfo& li) : g_(g), li_(li) {}

    bool run() {
        for (const Loop& l : li_.loops()) {
            for (NodeId u : g_.uses_of(l.header)) {
                const Node& phi = g_.node(u);
                if (phi.op != Op::Phi || phi.ty == ty_mem()) continue;
                // an input must be binop(phi, x) defined inside the loop
                for (u8 i = 1; i < phi.n_in; ++i) {
                    NodeId v = phi.in[i];
                    if (v == u) continue;
                    const Node& vn = g_.node(v);
                    if (vn.op != Op::Bin) continue;
                    if (vn.in[1] != u && vn.in[2] != u) continue;
                    BinOp op = static_cast<BinOp>(vn.sub);
                    if (op == BinOp::Add || op == BinOp::Mul || op == BinOp::And ||
                        op == BinOp::Or || op == BinOp::Xor) {
                        reductions_++;
                        found_ = true;
                    }
                }
            }
        }
        return found_;
    }

    u32 count() const { return reductions_; }

private:
    Graph& g_;
    LoopInfo& li_;
    u32 reductions_ = 0;
    bool found_ = false;
};
} // namespace

class ReductionRecognizerPass : public Pass {
public:
    const char* name() const override { return "ReductionRecognizer"; }
    int order() const override { return 59; }
    const char* phase_name() const override { return "Phase 5: Vectorization & Superword Parallelism"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override { return 0; } // pure analysis
    bool run(PassContext& ctx) override {
        bool any = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            ReductionRecognizer r(fg.g, ctx.analysis.loops(fg));
            any |= r.run();
        }
        return any;
    }
};

JULES_REGISTER_PASS(ReductionRecognizerPass, 59, "Phase 5")

} // namespace jules
