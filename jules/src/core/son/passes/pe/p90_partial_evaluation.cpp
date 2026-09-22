// Pass 90 — PartialEvaluation (PE family; see pe/pe.h)
//
// STATIC specialization: for every direct call whose argument list contains
// provable constants (post-SCCP), clone the callee with those parameters
// bound, fold the specialized graph (dead branches on the binding prune
// away), and retarget the call to the variant with the bound arguments
// dropped. Identical binding sets across call sites share ONE variant
// (structural dedup) — the code-size discipline inline-per-site lacks.
//
// Interplay:
//   * Runs after the inlining phase (78-82): the call sites that survive
//     are the ones the inliner rejected (too big / no budget) — exactly
//     where a shared variant beats per-site cloning. All-constant calls
//     were already inlined by pass 80.
//   * The post-inline cleanup sweep runs after this pass (scheduler: after
//     the last SoN pass) and folds the PE output back into GVN/SCCP.
//   * Off under --pgo=instrument: a variant would CLONE pass-43's counter
//     bumps and double-count loop trips in the next profile round.
//
// Honesty: memory-state specialization (alias-disjoint loads) is not
// attempted — BTA treats all effectful nodes as dynamic. Single generation:
// variants are never themselves specialized (termination by construction).
#include "core/son/passes/pe/pe.h"

namespace jules {

class PartialEvaluationPass : public Pass {
public:
    const char* name() const override { return "PartialEvaluation"; }
    int order() const override { return 90; }
    const char* phase_name() const override { return "Phase 7: Inlining & Interprocedural"; }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo |
               AnalysisKind::AliasInfo | AnalysisKind::MemDep | AnalysisKind::CallGraph;
    }
    bool run(PassContext& ctx) override {
        if (ctx.opts.pgo == PgoMode::Instrument) return false; // see header
        PeBudgets b = pe_budgets(ctx.opts.level);
        if (b.max_variants_per_fn == 0) return false;

        bool changed = false;
        // Single generation: scan only the functions that existed at
        // pipeline entry (variants appended below are skipped). NOTE:
        // pe_make_variant appends to mod.fns, which may reallocate the
        // vector — every graph reference is RE-ACQUIRED after a call to
        // it, and the call node is snapshotted by value beforehand.
        u32 fn_count = static_cast<u32>(ctx.mod.fns.size());
        for (u32 fi = 0; fi < fn_count; ++fi) {
            for (NodeId id = 0; id < ctx.mod.fns[fi].g.size(); ++id) {
                Node nc = ctx.mod.fns[fi].g.node(id); // value snapshot
                if (nc.op != Op::Call || nc.n_in <= 2) continue;
                FnId target = nc.aux;
                if (target == kFnPrint || target == kFnFree ||
                    target == kFnPgoBump || target == kFnPgoSketch) {
                    continue;
                }
                if (target >= fn_count) continue; // not an original user fn
                {
                    const FunctionGraph* callee = ctx.mod.find_fn(target);
                    if (!callee || callee->param_types.empty()) continue;
                    if (callee->fid == ctx.mod.fns[fi].fid) continue; // self-recursion
                }

                // collect constant arguments (binding candidates)
                std::vector<PeAssumption> bindings;
                bool any_dynamic = false;
                bool arity_ok = true;
                for (u8 i = 2; i < nc.n_in; ++i) {
                    u32 pidx = static_cast<u32>(i - 2);
                    const FunctionGraph* callee = ctx.mod.find_fn(target);
                    if (pidx >= callee->param_types.size()) { arity_ok = false; break; }
                    ConstVal v;
                    if (const_of(ctx.mod.fns[fi].g, nc.in[i], v)) {
                        PeAssumption a;
                        a.param = static_cast<u8>(pidx);
                        a.value = v;
                        a.value.ty = callee->param_types[pidx];
                        bindings.push_back(a);
                    } else {
                        any_dynamic = true;
                    }
                }
                if (!arity_ok || bindings.empty()) continue;
                if (!any_dynamic) continue; // all-const: pass 80's family

                bool made = false;
                FnId variant = pe_make_variant(ctx.mod, ctx.syms, target, bindings, b,
                                               &made);
                if (variant == kNoFn || !made) continue;

                // Re-acquire the caller (mod.fns may have moved) and rebuild
                // the call: same pin + memory, dynamic args only, target =
                // variant (bound params are dropped from its signature).
                Graph& g = ctx.mod.fns[fi].g;
                if (id >= g.size() || g.node(id).op != Op::Call) continue; // defensive
                std::vector<NodeId> keep;
                for (u8 i = 2; i < nc.n_in; ++i) {
                    u32 pidx = static_cast<u32>(i - 2);
                    bool bound = false;
                    for (const PeAssumption& a : bindings)
                        if (a.param == pidx) { bound = true; break; }
                    if (!bound) keep.push_back(nc.in[i]);
                }
                NodeId ins[kMaxInputs];
                ins[0] = nc.in[0];
                ins[1] = nc.in[1];
                u8 k = 2;
                for (NodeId a : keep) {
                    if (k >= kMaxInputs) break;
                    ins[k++] = a;
                }
                NodeId call = g.make_arr(Op::Call, nc.ty, ins, k, 0, variant);
                g.replace_all_uses(id, call);
                g.kill(id);
                changed = true;
            }
        }
        return changed;
    }
};

JULES_REGISTER_PASS(PartialEvaluationPass, 90, "Phase 7")

} // namespace jules
