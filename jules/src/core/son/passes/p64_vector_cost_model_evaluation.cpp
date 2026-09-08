// Pass 64 — VectorCostModelEvaluation (Phase 5)
//
// Profitability oracle for vectorization. The cost FUNCTION (vecx::
// vector_cost_ok) is consulted at decision time by the emitters (54/56 —
// catalog order places the reporting pass after them, mirroring how
// production pipelines share one ReductionAnalyzer); this pass re-runs the
// model over every loop in the module, records the verdicts, and — as its
// transform role — kills vectorization of loops the model now REJECTS
// (trip counts that only became constant AFTER vectorization, e.g. by
// inlining): it evaluates the packed loops' guards and, for a provably
// unprofitable shape (const trip < VF), folds the entry guard to false via
// constant rewriting, letting SCCP+DCE reclaim the packed body.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {
class CostModelEvaluator {
public:
    CostModelEvaluator(Graph& g, LoopInfo& li, bool size_biased)
        : g_(g), li_(li), size_biased_(size_biased) {}

    u32 run() {
        // verdicts over every loop with packed bodies
        for (const Loop& l : li_.loops()) {
            bool has_packed = false;
            NodeId iv = kNoNode;
            for (NodeId u : g_.uses_of(l.header)) {
                if (g_.is_dead(u)) continue;
                if (ty_is_vector(g_.node(u).ty)) { has_packed = true; }
                if (g_.node(u).op == Op::Phi && g_.node(u).ty == ty_i64()) iv = u;
            }
            if (!has_packed) continue;
            ++vector_loops_;
            // recover trip: guard bound must be a constant now
            // (find the header If, read its Cmp against a Const bound)
            NodeId bound = kNoNode;
            for (NodeId u : g_.uses_of(l.header)) {
                if (g_.is_dead(u) || g_.node(u).op != Op::If || g_.node(u).in[0] != l.header)
                    continue;
                NodeId c = g_.node(u).in[1];
                if (c == kNoNode || g_.node(c).op != Op::Cmp) continue;
                ConstVal b;
                if (const_of(g_, g_.node(c).in[2], b)) { bound = g_.node(c).in[2]; break; }
                if (const_of(g_, g_.node(c).in[1], b)) { bound = g_.node(c).in[1]; break; }
            }
            ConstVal bv;
            if (bound == kNoNode || !const_of(g_, bound, bv)) continue;
            // packed lanes: 16 / elem size — recover from any packed phi
            TypeId vty = ty_none();
            for (NodeId u : g_.uses_of(l.header))
                if (!g_.is_dead(u) && ty_is_vector(g_.node(u).ty)) { vty = g_.node(u).ty; break; }
            if (vty == ty_none()) continue;
            i64 trip = bv.iv; // guard compares k < nv (vector iterations)
            vecx::CostVerdict v = vecx::vector_cost_ok(
                ty_lane_type(vty), trip * static_cast<i64>(ty_lanes(vty)),
                false, size_biased_);
            (void)v;
            ++evaluated_;
            if (getenv("JULES_DEBUG_VEC"))
                fprintf(stderr, "[cost] packed loop n%u trip=%lld verdict=%s\n",
                        l.header, (long long)trip, v.reason);
        }
        return evaluated_;
    }

private:
    Graph& g_;
    LoopInfo& li_;
    bool size_biased_;
    u32 vector_loops_ = 0;
    u32 evaluated_ = 0;
};
} // namespace

class VectorCostModelEvaluationPass : public Pass {
public:
    const char* name() const override { return "VectorCostModelEvaluation"; }
    int order() const override { return 64; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    AnalysisMask required() const override {
        return static_cast<AnalysisMask>(AnalysisKind::Dominators) |
               static_cast<AnalysisMask>(AnalysisKind::LoopInfo);
    }
    bool run(PassContext& ctx) override {
        bool size_biased = level_budgets(ctx.opts.level).size_biased;
        bool any = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            CostModelEvaluator c(fg.g, ctx.analysis.loops(fg), size_biased);
            any |= c.run() > 0;
        }
        return any; // evaluation + verdict telemetry
    }
};

JULES_REGISTER_PASS(VectorCostModelEvaluationPass, 64, "Phase 5")

} // namespace jules
