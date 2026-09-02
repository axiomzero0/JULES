// Pass 30 — MemorySSARepair (Phase 2: Memory Optimization)
//
// Correctness-critical maintenance: after aggressive memory transforms,
// memory-chain links may point at killed nodes (stores bypassed in a
// different order than their users were rewritten). This pass walks every
// memory-version slot and follows Dead links to the nearest live ancestor,
// then rebuilds use lists. Cheap, idempotent, keeps the verifier green.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class MemSsaRepair {
public:
    explicit MemSsaRepair(Graph& g) : g_(g) {}

    bool run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op == Op::Dead) continue;
            switch (n.op) {
                case Op::Phi:
                    for (u8 i = 1; i < n.n_in; ++i) repair_slot(id, i);
                    break;
                case Op::Load:
                case Op::Store:
                case Op::Call:
                case Op::Alloc:
                case Op::Return:
                    if (n.n_in > 1) repair_slot(id, 1);
                    break;
                default:
                    break;
            }
        }
        if (changed_) g_.rebuild_uses();
        return changed_;
    }

private:
    void repair_slot(NodeId id, u8 slot) {
        Node& n = g_.node(id);
        NodeId cur = n.in[slot];
        u32 guard = 0;
        while (cur != kNoNode && g_.node(cur).op == Op::Dead && ++guard <= kMaxFollow) {
            const Node& dead = g_.node(cur);
            cur = dead.n_in > 1 ? dead.in[1] : kNoNode;
        }
        if (cur != kNoNode && cur != n.in[slot] && g_.node(cur).op != Op::Dead) {
            n.in[slot] = cur;
            g_.mark_uses_dirty();
            changed_ = true;
        }
    }

    static constexpr u32 kMaxFollow = 64; // named bound for chain following
    Graph& g_;
    bool changed_ = false;
};
} // namespace

class MemorySSARepairPass : public Pass {
public:
    const char* name() const override { return "MemorySSARepair"; }
    int order() const override { return 30; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    bool parallelizable() const override { return true; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            MemSsaRepair r(fg.g);
            changed |= r.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(MemorySSARepairPass, 30, "Phase 2")

} // namespace jules
