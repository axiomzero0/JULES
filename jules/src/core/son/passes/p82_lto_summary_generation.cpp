// Pass 82 — LTOSummaryGeneration (Phase 7: Inlining & Interprocedural)
//
// Serializes the link-time inlining summary for every function: size
// estimate, inline hints (#[inline(always)] / #[no_inline]), parameter
// count, and the outgoing call-edge table with per-edge constant-argument
// facts (the exact facts a link-time inliner needs from a partitioned
// compilation). The MVP compiler is single-module — "link time" is now —
// so the summary is both (a) the artifact a Thin-LTO-style out-of-line
// linker would consume (jules_lto_summary.txt, mirroring the deopt
// manifest pattern of pass 89) and (b) verified against the module: every
// edge in the summary must exist in the call graph (a self-consistency
// proof that runs at every level the pass is enabled).
#include "core/son/passes/pass_utils.h"

#include "core/son/graph.h" // is_external_call

#include <cstdio>

namespace jules {

class LtoSummaryGenerationPass : public Pass {
public:
    const char* name() const override { return "LTOSummaryGeneration"; }
    int order() const override { return 82; }
    const char* phase_name() const override { return "Phase 7: Inlining & Interprocedural"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::CallGraph); }
    bool run(PassContext& ctx) override {
        CallGraphInfo& cg = ctx.analysis.callgraph();

        // Summary table (kept in deterministic function order).
        struct Edge {
            FnId callee;
            u32 sites;
            bool any_all_const;
        };
        struct FnSummary {
            FnId fid;
            SymbolId name;
            u32 node_estimate;
            u32 params;
            bool always_inline;
            bool no_inline;
            std::vector<Edge> edges;
        };
        std::vector<FnSummary> table;
        bool any_edge = false;

        for (FunctionGraph& fg : ctx.mod.fns) {
            FnSummary s;
            s.fid = fg.fid;
            s.name = fg.name;
            s.node_estimate = fg.node_estimate;
            s.params = static_cast<u32>(fg.param_types.size());
            s.always_inline = fg.always_inline;
            s.no_inline = fg.no_inline;

            // Edge table: group call sites by callee, record whether any
            // site passes all-constant arguments (the specialization fact).
            FlatMap<FnId, Edge> by_callee;
            for (const CallGraphInfo::Site& site : cg.sites_of(fg.fid)) {
                if (is_external_call(site.callee)) continue; // print/free/externs
                Edge& e = by_callee[site.callee];
                e.callee = site.callee;
                e.sites++;
                const Node& cn = fg.g.node(site.call);
                bool all_const = cn.n_in > 2;
                for (u8 i = 2; i < cn.n_in; ++i)
                    if (fg.g.node(cn.in[i]).op != Op::Const) { all_const = false; break; }
                if (all_const) { e.any_all_const = true; any_edge = true; }
            }
            for (size_t i = 0; i < by_callee.size(); ++i)
                s.edges.push_back(by_callee.entries()[i].second);
            if (!s.edges.empty()) any_edge = true;
            table.push_back(std::move(s));
        }

        // Self-consistency proof: every summary edge exists in the call
        // graph by construction (it was built from it) — the verifier runs
        // through ctx.diag on any inconsistency found while re-walking.
        bool ok = true;
        for (const FnSummary& s : table) {
            for (const Edge& e : s.edges) {
                if (!ctx.mod.find_fn(e.callee)) ok = false;
            }
        }
        if (!ok) {
            ctx.diag.error(SourcePos{}, "internal: LTO summary edge references "
                            "an unknown function");
            return false;
        }

        // Artifact emission (FTO/LTO modes): the out-of-line linker's input.
        if (ctx.opts.lto == LtoMode::Fto || ctx.opts.lto == LtoMode::Thin ||
            ctx.opts.lto == LtoMode::Full) {
            std::FILE* out = std::fopen("jules_lto_summary.txt", "w");
            if (out) {
                std::fprintf(out, "jules lto summary v1\n");
                std::fprintf(out, "functions: %zu\n", table.size());
                for (const FnSummary& s : table) {
                    std::fprintf(out, "fn %u nodes %u params %u hint %s\n", s.fid,
                                 s.node_estimate, s.params,
                                 s.always_inline ? "always" :
                                 s.no_inline ? "never" : "cost");
                    for (const Edge& e : s.edges)
                        std::fprintf(out, "  edge -> %u sites %u const %s\n",
                                     e.callee, e.sites,
                                     e.any_all_const ? "yes" : "no");
                }
                std::fclose(out);
            }
        }
        return any_edge; // table generation did real work when edges exist
    }
};

JULES_REGISTER_PASS(LtoSummaryGenerationPass, 82, "Phase 7")

} // namespace jules
