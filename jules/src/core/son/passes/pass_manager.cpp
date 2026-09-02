// Pass scheduler: ordering by catalog number, mode gating, kill switches,
// analysis invalidation, per-pass stats, post-inline cleanup re-run, and a
// computed parallel-group plan (execution is sequential in this MVP; groups
// are logged as scheduling metadata).
#include "core/son/passes/pass.h"
#include "core/son/son.h"

#include <chrono>
#include <cstdio>

namespace jules {

PassRegistry& PassRegistry::instance() {
    static PassRegistry reg;
    return reg;
}

void PassRegistry::add(int order, Pass* (*factory)()) {
    for (const Entry& e : entries_)
        if (e.order == order) return; // deterministic: first registrant wins
    entries_.push_back(Entry{order, factory});
}

std::vector<Pass*> PassRegistry::create_all() const {
    std::vector<Entry> sorted = entries_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Entry& a, const Entry& b) { return a.order < b.order; });
    std::vector<Pass*> out;
    out.reserve(sorted.size());
    for (const Entry& e : sorted) out.push_back(e.factory());
    return out;
}

bool PassManager::should_skip(Pass& p, const char*& reason) {
    const char* n = p.name();
    if (!ctx_.opts.only.empty()) {
        bool found = false;
        for (const std::string& s : ctx_.opts.only)
            if (s == n) { found = true; break; }
        if (!found && p.stage() == Stage::Son) { reason = "disabled"; return true; }
    }
    if (ctx_.opts.disabled.contains(n)) { reason = "disabled"; return true; }

    ModeMask want = 0;
    switch (ctx_.opts.mode) {
        case CompileMode::AOT:           want = kModeAOT; break;
        case CompileMode::JitBaseline:   want = kModeJitBaseline; break;
        case CompileMode::JitOptimizing: want = kModeJitOptimizing; break;
    }
    if ((p.modes() & want) == 0) { reason = "mode"; return true; }

    // Level gating (spec §7 availability matrix). Required lowering passes
    // are On at every level; everything else follows the matrix / phase
    // defaults. LTO visibility: pass 82's summaries only make sense with
    // more-than-none visibility (single-module compiler defaults to Full).
    if (!level_runs(p.order(), ctx_.opts.level)) { reason = "level"; return true; }
    if (p.order() == 82 && ctx_.opts.lto == LtoMode::None) {
        reason = "level"; return true;
    }

    if (p.stage() == Stage::Linear && ctx_.lin == nullptr) { reason = "stage"; return true; }
    return false;
}

bool PassManager::run_one(Pass& p, PassStats& st) {
    for (FunctionGraph& fg : ctx_.mod.fns) fg.g.reset_changes();
    u32 nodes_before = 0;
    for (FunctionGraph& fg : ctx_.mod.fns) nodes_before += fg.g.live_count();

    auto t0 = std::chrono::steady_clock::now();
    bool changed = p.run(ctx_);
    auto t1 = std::chrono::steady_clock::now();

    u32 nodes_after = 0;
    for (FunctionGraph& fg : ctx_.mod.fns) nodes_after += fg.g.live_count();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    st.ran = true;
    st.changes = changed ? 1 : 0;
    for (FunctionGraph& fg : ctx_.mod.fns) st.changes += fg.g.changes();
    st.nodes_before = nodes_before;
    st.nodes_after = nodes_after;
    st.ms = ms;

    ctx_.analysis.invalidate(p.invalidated());

    if (ctx_.opts.verify_each) {
        for (FunctionGraph& fg : ctx_.mod.fns) {
            if (!verify_graph(fg.g, ctx_.syms, ctx_.diag)) {
                ctx_.diag.error(SourcePos{},
                                std::string("graph verification failed after pass '") +
                                p.name() + "' in function '" +
                                std::string(ctx_.syms.name(fg.name)) + "'");
                return false;
            }
        }
    }
    if (ctx_.opts.emit_ir && p.stage() == Stage::Son) {
        std::vector<SymbolId> fn_syms;
        fn_syms.reserve(ctx_.mod.fns.size());
        for (const FunctionGraph& fg : ctx_.mod.fns) fn_syms.push_back(fg.name);
        for (FunctionGraph& fg : ctx_.mod.fns) {
            std::fprintf(stdout, "; ---- after pass %d (%s) fn %s ----\n", p.order(),
                         p.name(), ctx_.syms.name(fg.name).data());
            std::fputs(dump_graph_text(fg.g, ctx_.syms, &fn_syms).c_str(), stdout);
        }
    }
    return true;
}

bool PassManager::run() {
    std::vector<Pass*> all = PassRegistry::instance().create_all();

    // ---- parallel-group plan (metadata; execution stays sequential in MVP) ----
    {
        std::vector<std::pair<bool, std::vector<const char*>>> groups;
        for (Pass* p : all) {
            if (groups.empty() || groups.back().first != p->parallelizable())
                groups.push_back({p->parallelizable(), {}});
            groups.back().second.push_back(p->name());
        }
        if (ctx_.opts.mode == CompileMode::JitOptimizing) {
            std::fprintf(stdout, "; scheduler: %zu pass(es), %zu parallel group(s) computed\n",
                         all.size(), groups.size());
        }
    }

    for (Pass* p : all) {
        PassStats st;
        st.name = p->name();
        st.order = p->order();

        const char* reason = nullptr;
        if (should_skip(*p, reason)) {
            st.skip_reason = reason;
            stats_.push_back(st);
            continue;
        }
        if (!run_one(*p, st)) {
            stats_.push_back(st);
            delete p;
            return false;
        }
        stats_.push_back(st);

        // Post-inline cleanup: re-run the core cleanup/cse set after the
        // inlining phase exposes new folding opportunities (scheduler-level
        // decision; passes are allowed to repeat). Rounds are budget presets
        // (spec §8 fixpoint iterations): O0/Og skip, O1=1, O2=2, O3=3;
        // rounds stop early when a fixpoint is reached. SROA/DSE run BEFORE
        // the folding/cse passes so promoted values are visible to GVN. SCCP
        // (8) is included: load forwarding/promotion in the main run (passes
        // 21-26) exposes SSA constants only AFTER SCCP's original slot, so
        // the post-inline re-run is where its control-conditional lattice
        // gets real input — the same reason production pipelines (LLVM
        // IPSCCP, Graal) re-run conditional propagation after inlining.
        if (ctx_.opts.post_inline_cleanup && p->order() == 82 && p->stage() == Stage::Son) {
            u32 rounds = level_budgets(ctx_.opts.level).cleanup_rounds;
            static const int kCleanupOrders[] = {26, 30, 23, 1, 2, 3, 7, 8, 9};
            for (u32 r = 0; r < rounds; ++r) {
                std::vector<Pass*> again = PassRegistry::instance().create_all();
                FlatMap<int, Pass*> by_order;
                for (Pass* q : again)
                    if (q->stage() == Stage::Son) by_order.insert(q->order(), q);
                bool any_change = false;
                for (int order : kCleanupOrders) {
                    Pass** q = by_order.find(order);
                    if (!q || !*q) continue;
                    PassStats st2;
                    st2.name = (*q)->name();
                    st2.order = order;
                    if (!run_one(**q, st2)) {
                        stats_.push_back(st2);
                        break;
                    }
                    if (st2.changes > 0) any_change = true;
                    stats_.push_back(st2);
                }
                for (Pass* q : again) delete q;
                if (!any_change) break; // fixpoint reached early
            }
        }
    }

    for (Pass* p : all) delete p;
    return !ctx_.diag.has_errors();
}

void render_pass_stats(std::FILE* out, const std::vector<PassStats>& stats) {
    std::fprintf(out, "%-4s %-44s %-9s %-12s %-9s %-8s\n", "#", "pass", "ran",
                 "changes", "nodes", "ms");
    for (const PassStats& st : stats) {
        if (!st.ran) {
            std::fprintf(out, "%-4d %-44s skipped(%s)\n", st.order, st.name, st.skip_reason);
            continue;
        }
        std::fprintf(out, "%-4d %-44s %-9s %-12llu %-9u %-8.2f\n", st.order, st.name,
                     "yes", static_cast<unsigned long long>(st.changes),
                     st.nodes_after, st.ms);
    }
}

} // namespace jules
