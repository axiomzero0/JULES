// Pass 57 — OuterLoopVectorizer (Phase 5)
//
// "Vectorize the outer nest when the inner is short": a counted INNER
// loop with a small compile-time trip count (2..8) that runs inside an
// enclosing (outer) loop is FLATTENED — fully unrolled into the outer
// body with the same body-chain cloner pass 42 uses — so the outer
// iteration now carries `trip` adjacent, isomorphic statements. The
// post-inline cleanup re-runs the SLP vectorizer (54), which packs those
// statements into packed vector ops: the OUTER loop's body executes with
// SIMD lanes even though the outer trip count is dynamic.
//
// Why flatten the inner and not the outer: the inner trip is constant
// (known lane count = inner trip), while the outer trip is what the
// vectorizer can loop over. Pass 42 deliberately skips small inner
// loops (trip < 6, and its trade is innermost-ILP unrolling, not
// vectorization shaping); this pass is the outer-loop-shaped consumer
// of exactly those.
//
// After the T-copy clone the inner loop's guard executes exactly twice
// (entry check true, exit check false: the IV steps T*k per unrolled
// iteration and T*k covers the whole range) — the loop runs ONCE and its
// body is the straight-line chain. The residual guard pair is the
// documented cost of reusing the cloner instead of full loop
// elimination (roadmap).
//
// Safety: match_counted enforces no-early-exits, no packed ops, and the
// deopt-guard family ownership (loops containing a kFlagGuardSite If
// stay scalar — the cloner's merge re-threading would sever the
// fallback rung).
#include "core/son/passes/loop_transforms.h"

#include <algorithm>

namespace jules {

class OuterLoopVectorizerPass : public Pass {
public:
    const char* name() const override { return "OuterLoopVectorizer"; }
    int order() const override { return 57; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo |
               AnalysisKind::MemDep;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            for (int round = 0; round < 6; ++round) {
                auto li = LoopInfo::compute(fg.g, ctx.analysis.doms(fg));
                bool this_round = false;
                for (const Loop& l : li->loops()) {
                    // candidate: a counted loop with a small const trip
                    loopx::CountedLoop cl;
                    if (!loopx::match_counted(fg.g, *li, ctx.analysis.doms(fg), l, cl))
                        continue;
                    if (cl.trip < 2 || cl.trip > 8) continue;
                    if (cl.body_nodes == 0) continue;
                    if (cl.body_nodes * cl.trip > 60) continue; // size guard

                    // it must be an INNER loop of some enclosing loop —
                    // flattening a top-level small loop is pass 42's
                    // trade, not outer vectorization
                    bool has_outer = false;
                    for (const Loop& o : li->loops()) {
                        if (o.header == l.header) continue;
                        if (std::find(o.blocks.begin(), o.blocks.end(), l.header) !=
                            o.blocks.end()) {
                            has_outer = true;
                            break;
                        }
                    }
                    if (!has_outer) continue;

                    // fully unroll: T-1 extra copies -> the body chain
                    // executes T original iterations per (single) loop
                    // entry; the SLP re-run in the cleanup sweep packs
                    // the adjacent copies.
                    if (loopx::clone_body_chain(fg.g, cl,
                                                static_cast<u32>(cl.trip) - 1,
                                                false) == 0)
                        continue;
                    this_round = true;
                    break; // structure changed: recompute loop info
                }
                changed |= this_round;
                if (!this_round) break;
            }
        }
        return changed;
    }
};

JULES_REGISTER_PASS(OuterLoopVectorizerPass, 57, "Phase 5")

} // namespace jules
