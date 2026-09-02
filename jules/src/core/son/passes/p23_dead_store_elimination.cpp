// Pass 23 — DeadStoreElimination (Phase 2: Memory Optimization)
//
// Two supported proofs of deadness:
//   * trailing stores to local allocations that no remaining load reads
//   * stores overwritten before any read (MemDep forward overwrite scan)
// Killed stores are bypassed in the memory chain (their mem users relinked
// to the store's own mem input), which is what keeps the chain well-formed.
// Requires AA + liveness, hence separate from DCE.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class DeadStoreEliminator {
public:
    DeadStoreEliminator(Graph& g, AliasInfo& aa, MemDep& md) : g_(g), aa_(aa), md_(md) {}

    bool run() {
        // fixpoint: removing a trailing store can expose the previous one
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (NodeId id = 0; id < g_.size(); ++id) {
                Node& n = g_.node(id);
                if (n.op != Op::Store) continue;
                if (is_dead(id)) {
                    bypass(id);
                    this_round = true;
                }
            }
            changed_ |= this_round;
            if (!this_round) break;
        }
        return changed_;
    }

private:
    static constexpr u32 kMaxRounds = 8;

    bool is_dead(NodeId s) {
        const Node& n = g_.node(s);
        NodeId base = aa_.base_of(n.in[2]);
        if (base == kNoNode) return false;

        // Case 1: local allocation with no remaining loads.
        if (aa_.is_alloc_local(base)) {
            bool any_load = false;
            for (NodeId u : g_.uses_of(base)) {
                const Node& un = g_.node(u);
                if (un.op == Op::Load && un.in[2] == base) { any_load = true; break; }
            }
            if (!any_load) return true;
        }

        // Case 2: overwritten before any read (alias-aware forward scan).
        if (md_.store_is_overwritten_before_read(s)) return true;
        return false;
    }

    void bypass(NodeId s) {
        // relink memory users (incl. phi input slots) past the dead store
        g_.replace_uses_as_memory(s, g_.node(s).in[1]);
        g_.kill(s);
    }

    Graph& g_;
    AliasInfo& aa_;
    MemDep& md_;
    bool changed_ = false;
};
} // namespace

class DeadStoreEliminationPass : public Pass {
public:
    const char* name() const override { return "DeadStoreElimination"; }
    int order() const override { return 23; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    AnalysisMask required() const override {
        return AnalysisKind::AliasInfo | AnalysisKind::MemDep;
    }
    AnalysisMask invalidated() const override {
        return static_cast<AnalysisMask>(AnalysisKind::MemDep | AnalysisKind::AliasInfo);
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            DeadStoreEliminator e(fg.g, ctx.analysis.alias(fg), ctx.analysis.memdep(fg));
            changed |= e.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(DeadStoreEliminationPass, 23, "Phase 2")

} // namespace jules
