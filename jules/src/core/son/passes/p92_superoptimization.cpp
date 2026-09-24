// Pass 92 — Superoptimization (the third machine-optimization tier).
//
// A bounded, deterministic search over post-88 MIR windows: the engine
// (src/superopt/, target-agnostic) explores instruction sequences built from
// the ISA descriptor table (src/targets/x86_64/x64_super_isa.cpp — the only
// place instruction semantics live, as data) and replaces a window when it
// finds a strictly cheaper sequence that is vector-equivalent on the
// window's live-out contract. Nothing about any optimization is encoded
// here, in the engine, or in the table: lea fusion, xor-zeroing, dead-code
// elimination and every other win are SEARCH RESULTS.
//
// Gating (spec §7 availability matrix): -O3 only, Aggressive tier. The
// search budget is pop-count based (deterministic — never wall-clock).
// JULES_SUPEROPT=0 disables; JULES_SUPEROPT_STATS=1 prints per-function
// telemetry; JULES_SUPEROPT_POPS / JULES_SUPEROPT_FN_POPS tune budgets.
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

namespace jules {

class SuperoptimizationPass : public Pass {
public:
    const char* name() const override { return "Superoptimization"; }
    int order() const override { return 92; }
    const char* phase_name() const override {
        return "Phase 8: Lowering & Machine Optimization";
    }
    Stage stage() const override { return Stage::Linear; }
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        return x64_superopt_module(*ctx.lin);
    }
};

JULES_REGISTER_PASS(SuperoptimizationPass, 92, "Phase 8")

} // namespace jules
