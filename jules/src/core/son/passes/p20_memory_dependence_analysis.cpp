// Pass 20 — MemoryDependenceAnalysis (Phase 2: Memory Optimization)
//
// Analysis-only pass: computes precise store->load dependence along memory
// chains (with phi meets), which feeds RLE / STLF / DSE / LoadLICM.
#include "core/son/passes/pass_utils.h"

namespace jules {

class MemoryDependenceAnalysisPass : public Pass {
public:
    const char* name() const override { return "MemoryDependenceAnalysis"; }
    int order() const override { return 20; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::AliasInfo;
    }
    AnalysisMask invalidated() const override { return 0; } // pure analysis
    bool run(PassContext& ctx) override {
        for (FunctionGraph& fg : ctx.mod.fns) {
            (void)ctx.analysis.memdep(fg); // warm the cache
        }
        return false;
    }
};

JULES_REGISTER_PASS(MemoryDependenceAnalysisPass, 20, "Phase 2")

} // namespace jules
