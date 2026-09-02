// Pass 89 — DeoptMetadataEmission (Phase 8)
//
// Emits the deoptimization manifest for JIT-compiled versions: guard site
// locations, live-value maps, and materialization recipes. In AOT mode the
// pass runs as a no-op telemetry check (nothing speculative exists to
// deopt from); in JIT modes it writes the manifest next to the artifact.
// MVP status: manifest emission is real (empty guard set — no speculative
// passes fire yet); FrameState reconstruction is the next milestone.
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

#include <cstdio>

namespace jules {

class DeoptMetadataEmissionPass : public Pass {
public:
    const char* name() const override { return "DeoptMetadataEmission"; }
    int order() const override { return 89; }
    const char* phase_name() const override { return "Phase 8: Lowering & Machine Optimization"; }
    Stage stage() const override { return Stage::Linear; }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        if (ctx.opts.mode == CompileMode::AOT) return false; // proof-only: no guards

        // Manifest for JIT versions: guard sites recorded by AssumptionTracking.
        std::FILE* manifest = std::fopen("jules_deopt_manifest.txt", "w");
        if (!manifest) return false;
        std::fprintf(manifest, "jules deopt manifest v1\n");
        std::fprintf(manifest, "functions: %zu\n", ctx.lin->fns.size());
        for (const LFunction& lf : ctx.lin->fns)
            std::fprintf(manifest, "fn %u blocks %zu slots %d frame %d\n",
                         lf.fid, lf.blocks.size(), lf.slot_count, lf.frame_size);
        std::fprintf(manifest, "guard_sites: %zu\n", ctx.lin->guard_sites.size());
        for (const LinearModule::GuardSite& g : ctx.lin->guard_sites)
            std::fprintf(manifest, "guard fn=%u block=%d kind=%s\n", g.fn, g.block, g.kind);
        std::fclose(manifest);
        return !ctx.lin->guard_sites.empty();
    }
};

JULES_REGISTER_PASS(DeoptMetadataEmissionPass, 89, "Phase 8")

} // namespace jules
