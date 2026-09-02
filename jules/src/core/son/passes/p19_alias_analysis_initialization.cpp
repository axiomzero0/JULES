// Pass 19 — AliasAnalysisInitialization (Phase 2: Memory Optimization)
//
// Analysis-only pass: builds the type-based, field-sensitive (MVP: base-of
// provenance) alias structures and registers them in the analysis manager.
// Every memory transform below consumes this. Declares required analyses and
// preserves the dominator tree.
#include "core/son/passes/pass_utils.h"

namespace jules {

class AliasAnalysisInitializationPass : public Pass {
public:
    const char* name() const override { return "AliasAnalysisInitialization"; }
    int order() const override { return 19; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::Dominators); }
    AnalysisMask invalidated() const override { return 0; } // pure analysis
    bool run(PassContext& ctx) override {
        for (FunctionGraph& fg : ctx.mod.fns) {
            (void)ctx.analysis.alias(fg); // warm the cache; telemetry only
        }
        return false;
    }
};

JULES_REGISTER_PASS(AliasAnalysisInitializationPass, 19, "Phase 2")

} // namespace jules
