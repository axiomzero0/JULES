// Pass 25 — LoadHoisting (Phase 2: Memory Optimization)
//
// Hoists loads out of loops when:
//   * the address is loop-invariant and points at a local (non-escaping)
//     allocation
//   * no store to that base and no call occurs inside the loop
// Distinct from LICM: only loads move, and the memory input is rewired to
// the version entering the loop (the header phi's preheader input).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class LoadHoister {
public:
    LoadHoister(Graph& g, LoopInfo& loops, AliasInfo& aa, DomTree& dom)
        : g_(g), loops_(loops), aa_(aa), dom_(dom) {}

    bool run() {
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (const Loop& l : loops_.loops())
                this_round |= hoist_loop(l);
            changed_ |= this_round;
            if (!this_round) break;
        }
        return changed_;
    }

private:
    static constexpr u32 kMaxRounds = 4;

    bool hoist_loop(const Loop& l) {
        NodeId pre = loops_.preheader(l.header);
        if (pre == kNoNode) return false;

        // loop hazards: any store to a local base, or ANY call
        bool has_call = false;
        for (NodeId blk : l.blocks) {
            for (NodeId u : g_.uses_of(blk)) {
                const Node& un = g_.node(u);
                if (un.in[0] != blk) continue;
                if (un.op == Op::Call) { has_call = true; break; }
            }
            if (has_call) break;
        }
        if (has_call) return false;

        NodeId header_phi = kNoNode;
        for (NodeId u : g_.uses_of(l.header)) {
            const Node& un = g_.node(u);
            if (un.op == Op::Phi && un.ty == ty_mem()) { header_phi = u; break; }
        }
        if (header_phi == kNoNode) return false;

        // which pred index is the preheader?
        const Node& h = g_.node(l.header);
        u8 pre_idx = 0xFF;
        for (u8 i = 0; i < h.n_in; ++i)
            if (h.in[i] == pre) pre_idx = i;
        if (pre_idx == 0xFF) return false;
        NodeId entry_mem = g_.node(header_phi).in[pre_idx + 1];

        bool changed = false;
        for (NodeId blk : l.blocks) {
            for (NodeId u : g_.uses_of(blk)) {
                Node& un = g_.node(u);
                if (un.op != Op::Load || un.in[0] != blk) continue;
                NodeId base = aa_.base_of(un.in[2]);
                if (base == kNoNode || !aa_.is_alloc_local(base)) continue;
                // address must be loop-invariant (defined outside)
                if (in_loop(un.in[2], l)) continue;
                // no store to this base inside the loop
                bool store_conflict = false;
                for (NodeId b2 : l.blocks) {
                    for (NodeId w : g_.uses_of(b2)) {
                        const Node& wn = g_.node(w);
                        if (wn.op == Op::Store && wn.in[0] == b2 &&
                            aa_.base_of(wn.in[2]) == base) {
                            store_conflict = true;
                            break;
                        }
                    }
                    if (store_conflict) break;
                }
                if (store_conflict) continue;

                g_.set_input(u, 0, pre);      // repin to preheader
                g_.set_input(u, 1, entry_mem); // read the entering version
                changed = true;
            }
        }
        return changed;
    }

    bool in_loop(NodeId n, const Loop& l) const {
        if (!is_block_head(g_.node(n).op)) {
            // data node: its definition block must be outside
            NodeId blk = g_.node(n).in[0];
            return loops_.block_in_loop(l, blk);
        }
        return loops_.block_in_loop(l, n);
    }

    Graph& g_;
    LoopInfo& loops_;
    AliasInfo& aa_;
    DomTree& dom_;
    bool changed_ = false;
};
} // namespace

class LoadHoistingPass : public Pass {
public:
    const char* name() const override { return "LoadHoisting"; }
    int order() const override { return 25; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo | AnalysisKind::AliasInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::MemDep | AnalysisKind::AliasInfo;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            LoadHoister h(fg.g, ctx.analysis.loops(fg), ctx.analysis.alias(fg),
                          ctx.analysis.doms(fg));
            changed |= h.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(LoadHoistingPass, 25, "Phase 2")

} // namespace jules
