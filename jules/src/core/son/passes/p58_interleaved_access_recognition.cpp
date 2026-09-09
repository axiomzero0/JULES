// Pass 58 — InterleavedAccessRecognition (Phase 5)
//
// Interleaved (stride-2) access pairs -> one SHUFFLE-FREE packed access.
// The element-adjacent pair with a SYMBOLIC shared index — a[2i], a[2i+1]
// (the struct-of-two walk an AoS layout compiles to; with no structs in
// the language, stride-2 indexing IS the interleaved shape) — addresses
// 16 CONTIGUOUS bytes at the first element's address:
//
//   a[2i]   = b[2i]   + k
//   a[2i+1] = b[2i+1] + k   ==>  [movups load b+16i][paddq by broadcast k]
//   (adjacent in the memory chain)  [movups store a+16i]
//
// No deinterleave shuffles are needed: the pair is naturally contiguous,
// which is what distinguishes this from a stride-2 GATHER (pairs whose
// elements are NOT adjacent cannot pack without shuffles; SSE2 has no
// cheap lane insert/extract and the family rejects them). The shared
// symbolic part m may be any node — Mul(i,2), Add(Mul(i,2),1)-style
// deinterleaves, or a computed index g(i): the pair proof only needs
// element adjacency (same m, constant difference 1).
//
// Shares the pair executor and the soundness gate with pass 54
// (vecx::pack_store_pairs / vecx::pack_pair_sound): same pin for the four
// nodes, load-version equivalence for lane 1, no other user of the first
// store's version reading the element the packed store writes early.
// In-loop pairs pass the gate when the bases are NoAlias (distinct local
// allocations — the inlined/main-array case) or MustAlias with disjoint
// elements (in-place same-array walks); MayAlias param bases reject
// (an offset overlap would make the packed load read the pre-store lane).
#include "core/son/passes/vector_utils.h"

namespace jules {

class InterleavedAccessRecognitionPass : public Pass {
public:
    const char* name() const override { return "InterleavedAccessRecognition"; }
    int order() const override { return 58; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::AliasInfo); }
    AnalysisMask invalidated() const override { return static_cast<AnalysisMask>(AnalysisKind::AliasInfo); }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            u32 packed = vecx::pack_store_pairs(fg.g, ctx.analysis.alias(fg),
                                                ctx.opts.fp,
                                                vecx::PairFamily::kInterleaved);
            changed |= packed > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(InterleavedAccessRecognitionPass, 58, "Phase 5")

} // namespace jules
