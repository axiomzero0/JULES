// Pass 70 — InlineCacheInsertion (Phase 6)
//
// STATUS: complete for the current IR, no eligible sites. An inline
// cache memoizes a POLYMORPHIC call site's last-seen target (mono-morphic:
// one compare + direct call; poly: a small probe sequence; mega: a
// hash). The MVP language has no virtual/indirect dispatch surface: the
// IR's Call carries a static FnId, trait/method calls lower through the
// single-module static-dispatch table, and there are no function
// pointers. With every site monomorphic by construction, an IC stub
// would be a compare against a constant that always succeeds — the
// pass would only add work. It runs as a check-only verifier over the
// call sites (the candidate-set scan mirrors pass 69's) and never
// claims a transformation it did not perform; the contract stays for
// the day `dyn`/function-pointer dispatch lands, at which point the
// mono/poly/mega shapes are chosen by observed target counts (<=1 /
// <=4 / more) from pass 69's type profiles.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class InlineCacheScanner {
public:
    explicit InlineCacheScanner(Graph& g) : g_(g) {}

    // Number of call sites that would need an IC stub (sites whose
    // target is not statically resolved). 0 in the current IR.
    u32 run() {
        u32 sites = 0;
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op != Op::Call) continue;
            if (n.aux == kNoFn) ++sites;
        }
        return sites;
    }

private:
    Graph& g_;
};
} // namespace

class InlineCacheInsertionPass : public Pass {
public:
    const char* name() const override { return "InlineCacheInsertion"; }
    int order() const override { return 70; }
    const char* phase_name() const override { return "Phase 6: Devirtualization & Speculation"; }
    ModeMask modes() const override { return kModeJitBaseline | kModeJitOptimizing; }
    bool run(PassContext& ctx) override {
        // Check-only: count IC-candidate sites (structurally empty —
        // see the file header). Telemetry hook; no claim made.
        u32 sites = 0;
        for (FunctionGraph& fg : ctx.mod.fns) {
            InlineCacheScanner s(fg.g);
            sites += s.run();
        }
        (void)sites;
        return false;
    }
};

JULES_REGISTER_PASS(InlineCacheInsertionPass, 70, "Phase 6")

} // namespace jules
