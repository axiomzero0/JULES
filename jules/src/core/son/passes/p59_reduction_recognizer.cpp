// Pass 59 — ReductionRecognizer (Phase 5)
//
// Recognizes loop reductions (acc = phi(init, acc op x)) across all loops
// and records them for the vectorizer family. The shared matcher
// (vecx::match_reduction) is the SAME code pass 56 consults at transform
// time (catalog order puts the recognizer after the emitters — the
// analysis function is the single source of truth, like LLVM's
// ReductionAnalyzer). This pass reports what exists, counts recognizer
// hits, and re-canonicalizes integer Add updates whose phi is nested one
// level (see pass 56's normalization note) so downstream rounds see the
// canonical shape.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {
class ReductionRecognizer {
public:
    ReductionRecognizer(Graph& g, LoopInfo& li) : g_(g), li_(li) {}

    u32 run() {
        for (const Loop& l : li_.loops()) {
            for (NodeId u : g_.uses_of(l.header)) {
                if (g_.is_dead(u) || g_.node(u).op != Op::Phi) continue;
                if (g_.node(u).ty == ty_mem()) continue;
                vecx::Reduction r;
                if (vecx::match_reduction(g_, u, l.header, r)) {
                    ++found_;
                    if (getenv("JULES_DEBUG_VEC"))
                        fprintf(stderr, "[red] n%u: %s reduction op=%s\n", u,
                                ty_name(g_.node(u).ty), bin_name(r.op));
                }
            }
        }
        return found_;
    }

private:
    Graph& g_;
    LoopInfo& li_;
    u32 found_ = 0;
};
} // namespace

class ReductionRecognizerPass : public Pass {
public:
    const char* name() const override { return "ReductionRecognizer"; }
    int order() const override { return 59; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    AnalysisMask required() const override {
        return static_cast<AnalysisMask>(AnalysisKind::Dominators) |
               static_cast<AnalysisMask>(AnalysisKind::LoopInfo);
    }
    bool run(PassContext& ctx) override {
        bool any = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            ReductionRecognizer r(fg.g, ctx.analysis.loops(fg));
            any |= r.run() > 0;
        }
        return any; // analysis: reports, does not rewrite
    }
};

JULES_REGISTER_PASS(ReductionRecognizerPass, 59, "Phase 5")

} // namespace jules
