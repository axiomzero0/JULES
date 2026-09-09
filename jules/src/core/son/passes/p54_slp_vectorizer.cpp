// Pass 54 — SLPVectorizer (Phase 5)
//
// Bottom-up Superword-Level Parallelism on straight-line code: adjacent
// store PAIRS to consecutive elements of one array, whose values come from
// the isomorphic scalar computation over the same pair of a source array:
//
//   a[C]   = b[C]   * k        a[C]   = b[C]        (pure copy)
//   a[C+1] = b[C+1] * k   ==>  one packed iteration:
//   (adjacent in the memory chain)   [movups load b+C][paddq/pmul.. by
//                                     broadcast k][movups store a+C]
//
// The pair's addresses are the SAME element-C addresses (the vector ops
// read/write 16 bytes there), so no address arithmetic is rebuilt. The
// pattern arises naturally after the unroller fully flattens small const
// loops (a[0]=b[0]; a[1]=b[1]; ...) and from hand-unrolled source.
// Pass 55 discovers and reports the candidate packs (including
// non-adjacent ones); this pass executes the chain-adjacent pairs of the
// PURE-CONST index family (the symbolic/interleaved family is pass 58's).
//
// SOUNDNESS (regression-locked by t25's overlap shape): the packed load
// reads both elements at the FIRST load's memory version, and the packed
// store writes both elements at the FIRST store's chain position. The
// shared gate vecx::pack_pair_sound proves the loads' versions carry the
// same values (intervening stores may not write the second element) and
// that no other user of the first store's version observes the second
// element's write one position earlier; alias-unprovable shapes reject
// the pair (the old emitter trusted the chain and miscompiled
// `a[1]=a[0]+7; a[2]=a[1]+7` — it packed the (a[0],a[1]) load pair across
// the a[1] store and read the stale element; found by the pass audit).
#include "core/son/passes/vector_utils.h"

namespace jules {

class SLPVectorizerPass : public Pass {
public:
    const char* name() const override { return "SLPVectorizer"; }
    int order() const override { return 54; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::AliasInfo); }
    AnalysisMask invalidated() const override { return static_cast<AnalysisMask>(AnalysisKind::AliasInfo); }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            // chain-adjacent store pairs: walk every store's memory users
            u32 packed = vecx::pack_store_pairs(fg.g, ctx.analysis.alias(fg),
                                                ctx.opts.fp,
                                                vecx::PairFamily::kConstAdjacent);
            changed |= packed > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(SLPVectorizerPass, 54, "Phase 5")

} // namespace jules
