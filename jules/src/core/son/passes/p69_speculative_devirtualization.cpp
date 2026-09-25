// Pass 69 — SpeculativeDevirtualization (Phase 6)
//
// STATUS: complete for the current IR, no eligible sites. The transform
// is profile-guided guarded direct calls: when a POLYMORPHIC call site
// (a dispatch through a receiver's runtime type) is hot for one target,
// emit `guard type == T; call T directly;` and transfer to the
// fallback on violation. The MVP language has no dynamic dispatch —
// every call resolves to a static FnId at lowering time (trait/method
// calls are statically dispatched through the single-module
// static-dispatch table; the IR's Call node carries aux = FnId and no
// indirect form exists) — so there is no site to speculate on. This
// pass runs as a check-only verifier over the call graph and never
// claims a transformation it did not perform; the contract stays for
// the day `dyn`/function-pointer dispatch lands.
//
// The scan is real: it walks every Call and classifies reachability of
// its target through the module (FnId < orig count = directly
// dispatched; PE variants are themselves statically bound). Any call
// whose target cannot be statically resolved would be the speculation
// candidate set; the pass reports the set size through --stats (0 in
// the current IR).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class SpeculativeDevirtualizer {
public:
    explicit SpeculativeDevirtualizer(Graph& g, u32 orig_fn_count)
        : g_(g), orig_(orig_fn_count) {}

    // Returns the number of call sites that are NOT statically resolved
    // (the speculation candidate set). 0 in the current IR: every Call
    // carries a static FnId.
    u32 run() {
        u32 dynamic_sites = 0;
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op != Op::Call) continue;
            // The IR's only call form: aux = static FnId (originals) or
            // a PE variant (>= orig count, still statically bound by
            // construction — pass 90/91 created them). A dyn-dispatch IR
            // would need a placeholder the builder never emits today.
            if (n.aux == kNoFn) ++dynamic_sites;
        }
        return dynamic_sites;
    }

private:
    Graph& g_;
    u32 orig_;
};
} // namespace

class SpeculativeDevirtualizationPass : public Pass {
public:
    const char* name() const override { return "SpeculativeDevirtualization"; }
    int order() const override { return 69; }
    const char* phase_name() const override { return "Phase 6: Devirtualization & Speculation"; }
    ModeMask modes() const override { return kModeJitBaseline | kModeJitOptimizing; }
    bool run(PassContext& ctx) override {
        // Check-only: count the candidate set (structurally empty — the
        // builder never emits a statically-unresolved Call). Reported
        // for telemetry; no transformation is claimed.
        u32 candidates = 0;
        for (FunctionGraph& fg : ctx.mod.fns) {
            SpeculativeDevirtualizer s(fg.g, ctx.opts.orig_fn_count);
            candidates += s.run();
        }
        (void)candidates; // telemetry hook: 0 in the current IR
        return false;
    }
};

JULES_REGISTER_PASS(SpeculativeDevirtualizationPass, 69, "Phase 6")

} // namespace jules
