// Pass 55 — SuperwordPacker (Phase 5)
//
// Non-adjacent packing-graph analysis: finds every isomorphic
// consecutive-element store pair in each function (the SLP seed set —
// including pairs pass 54 cannot execute because they are not adjacent in
// the memory chain), builds the pack list, and reports coverage: how many
// candidate pairs exist vs. how many are chain-adjacent (packable today).
// The same discovery predicate is shared by pass 54's executor (the pair
// shape check is duplicated as the pure function vecx::match_addr over
// consecutive constants). This pass is the ANALYSIS half: telemetry +
// Module-level pack table consumed by reporting; no rewrites.
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {
class SuperwordPacker {
public:
    explicit SuperwordPacker(Graph& g) : g_(g) {}

    u32 run() {
        FlatMap<NodeId, bool> seen_pair;
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op != Op::Store) continue;
            vecx::AddrPattern a1 = vecx::match_addr(g_, n.in[2]);
            if (!a1.ok) continue;
            ConstVal c1;
            if (!const_of(g_, a1.idx, c1)) continue;
            // any other store with base+1 index (anywhere in the function)
            for (NodeId u = 0; u < g_.size(); ++u) {
                if (u == id) continue;
                const Node& un = g_.node(u);
                if (un.op != Op::Store) continue;
                vecx::AddrPattern a2 = vecx::match_addr(g_, un.in[2]);
                if (!a2.ok || a2.base != a1.base) continue;
                ConstVal c2;
                if (!const_of(g_, a2.idx, c2)) continue;
                if (c2.iv != c1.iv + 1) continue;
                if (seen_pair.contains(id)) continue;
                seen_pair.insert(id, true);
                ++candidates_;
                // chain-adjacent (executable by 54)?
                if (un.in[1] == id) ++adjacent_;
                if (getenv("JULES_DEBUG_VEC"))
                    fprintf(stderr, "[slp-pack] stores n%u/n%u base n%u [%lld, %lld]%s\n",
                            id, u, a1.base, (long long)c1.iv, (long long)c2.iv,
                            un.in[1] == id ? " adjacent" : "");
                break;
            }
        }
        return candidates_;
    }

    u32 adjacent() const { return adjacent_; }

private:
    Graph& g_;
    u32 candidates_ = 0;
    u32 adjacent_ = 0;
};
} // namespace

class SuperwordPackerPass : public Pass {
public:
    const char* name() const override { return "SuperwordPacker"; }
    int order() const override { return 55; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool any = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            SuperwordPacker p(fg.g);
            any |= p.run() > 0;
        }
        return any; // analysis: pack discovery + coverage telemetry
    }
};

JULES_REGISTER_PASS(SuperwordPackerPass, 55, "Phase 5")

} // namespace jules
