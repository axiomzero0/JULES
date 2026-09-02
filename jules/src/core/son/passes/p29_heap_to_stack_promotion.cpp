// Pass 29 — HeapToStackPromotion (Phase 2: Memory Optimization)
//
// Promotes non-escaping heap allocations (escape analysis classification:
// uses limited to load/store addressing and the memory chain) to stack
// slots by flagging the Alloc node (kFlagStackPromoted); codegen then
// assigns a frame slot instead of calling malloc. free() of a promoted
// allocation becomes a no-op call that is removed from the chain.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class HeapToStackPromoter {
public:
    explicit HeapToStackPromoter(Graph& g) : g_(g) {}

    bool run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op != Op::Alloc) continue;
            if (n.flags & kFlagStackPromoted) continue;
            if (!non_escaping(id)) continue;

            // free(alloc) sites become removable no-ops
            const SmallVec<NodeId, 4> users = g_.uses_of(id);
            for (NodeId u : users) {
                Node& un = g_.node(u);
                if (un.op == Op::Call && un.aux == kFnFree && un.in[2] == id) {
                    g_.replace_uses_as_memory(u, un.in[1]);
                    g_.kill(u);
                    changed_ = true;
                }
            }
            n.flags |= kFlagStackPromoted;
            changed_ = true;
        }
        return changed_;
    }

private:
    // Escape classification (NoEscape): the pointer value must never appear
    // as call argument, stored value, return value, or arithmetic input.
    bool non_escaping(NodeId alloc) {
        for (NodeId u : g_.uses_of(alloc)) {
            const Node& un = g_.node(u);
            switch (un.op) {
                case Op::Load:
                case Op::Store:
                    if (un.in[2] == alloc || un.in[1] == alloc) break;
                    return false; // stored as a value: escapes
                case Op::Call:
                    if (un.aux == kFnFree && un.in[2] == alloc) break; // free is fine
                    if (un.in[1] == alloc) break; // memory chain only
                    return false;                 // passed to a function: escapes
                case Op::Return:
                    if (un.in[1] == alloc) break;
                    return un.n_in < 3 || un.in[2] != alloc ? true : false;
                default:
                    return false; // phi/select/bin/... : value use, escapes
            }
        }
        return true;
    }

    Graph& g_;
    bool changed_ = false;
};
} // namespace

class HeapToStackPromotionPass : public Pass {
public:
    const char* name() const override { return "HeapToStackPromotion"; }
    int order() const override { return 29; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    AnalysisMask invalidated() const override {
        return AnalysisKind::AliasInfo | AnalysisKind::MemDep;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            HeapToStackPromoter p(fg.g);
            changed |= p.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(HeapToStackPromotionPass, 29, "Phase 2")

} // namespace jules
