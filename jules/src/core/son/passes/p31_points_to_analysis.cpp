// Pass 31 — PointsToAnalysis (Phase 3: Escape & Allocation Analysis)
//
// Flow-insensitive points-to graph: each Alloc gets the singleton {itself};
// pointer-typed params and loads of unknown provenance get the unknown set.
// Exposes base sets for escape analysis (pass 32) — that is the analysis
// the manager's AliasInfo also consumes (base_of).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class PointsTo {
public:
    explicit PointsTo(Graph& g) : g_(g) {}

    bool run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op == Op::Dead) continue;
            if (!ty_is_ptr(n.ty)) continue;
            NodeId base = base_of(id);
            if (base != kNoNode && !sets_.contains(id)) {
                sets_.insert(id, base);
                changed_ = true; // telemetry: new points-to fact recorded
            }
        }
        return changed_;
    }

    // Iterative base resolution: alloc / cast / phi-of-same-base. The phi
    // branch recurses with a visiting set (self-referential loop phis).
    NodeId base_of(NodeId ptr) {
        NodeId cur = ptr;
        u32 guard = 0;
        while (cur != kNoNode && ++guard <= kMaxChain) {
            const Node& n = g_.node(cur);
            switch (n.op) {
                case Op::Alloc:
                    return cur;
                case Op::Cast:
                    cur = n.in[1];
                    continue;
                case Op::Phi: {
                    if (visiting_.contains(cur)) return kNoNode; // cycle
                    visiting_.insert(cur, true);
                    NodeId common = kNoNode;
                    bool cycle = false;
                    for (u8 i = 1; i < n.n_in; ++i) {
                        NodeId b = base_of(n.in[i]);
                        if (i == 1) common = b;
                        else if (b != common) { cycle = true; break; }
                    }
                    visiting_.erase(cur);
                    return cycle ? kNoNode : common;
                }
                default:
                    return kNoNode;
            }
        }
        return kNoNode;
    }

private:
    static constexpr u32 kMaxChain = 64;
    Graph& g_;
    FlatMap<NodeId, NodeId> sets_; // ptr node -> allocation base
    FlatMap<NodeId, bool> visiting_; // phi recursion guard
    bool changed_ = false;
};
} // namespace

class PointsToAnalysisPass : public Pass {
public:
    const char* name() const override { return "PointsToAnalysis"; }
    int order() const override { return 31; }
    const char* phase_name() const override { return "Phase 3: Escape & Allocation Analysis"; }
    AnalysisMask invalidated() const override { return 0; } // pure analysis
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            PointsTo p(fg.g);
            changed |= p.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(PointsToAnalysisPass, 31, "Phase 3")

} // namespace jules
