// Pass 41 — LoadLICM (Phase 4)
//
// Specialized LICM for loads: different safety rules than pass 40 — a load
// may leave the loop only when no aliasing write (store to the same base,
// or any call, since calls are barriers in the MVP memory model) occurs in
// the loop, and the address is invariant. Second wave after pass 25 catches
// cases that survive GVN (e.g. reassociated addresses).
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class LoadLicm {
public:
    LoadLicm(Graph& g, LoopInfo& li, AliasInfo& aa) : g_(g), li_(li), aa_(aa) {}

    bool run() {
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (const Loop& l : li_.loops())
                this_round |= hoist_loop(l);
            changed_ |= this_round;
            if (!this_round) break;
        }
        return changed_;
    }

private:
    static constexpr u32 kMaxRounds = 4;

    bool hoist_loop(const Loop& l) {
        NodeId pre = li_.preheader(l.header);
        if (pre == kNoNode) return false;

        NodeId header_phi = kNoNode;
        for (NodeId u : g_.uses_of(l.header)) {
            const Node& un = g_.node(u);
            if (un.op == Op::Phi && un.ty == ty_mem()) { header_phi = u; break; }
        }
        if (header_phi == kNoNode) return false;
        const Node& h = g_.node(l.header);
        u8 pre_idx = 0xFF;
        for (u8 i = 0; i < h.n_in; ++i)
            if (h.in[i] == pre) pre_idx = i;
        if (pre_idx == 0xFF) return false;
        NodeId entry_mem = g_.node(header_phi).in[pre_idx + 1];

        // hazard scan: any call, or store to the same base
        bool has_call = false;
        std::vector<NodeId> stores;
        for (NodeId blk : l.blocks) {
            for (NodeId u : g_.uses_of(blk)) {
                const Node& un = g_.node(u);
                if (un.in[0] != blk) continue;
                if (un.op == Op::Call) has_call = true;
                if (un.op == Op::Store) stores.push_back(u);
            }
        }
        if (has_call) return false;

        bool changed = false;
        for (NodeId blk : l.blocks) {
            for (NodeId u : g_.uses_of(blk)) {
                Node& un = g_.node(u);
                if (un.op != Op::Load || un.in[0] != blk) continue;
                NodeId base = aa_.base_of(un.in[2]);
                if (base == kNoNode || !aa_.is_alloc_local(base)) continue;
                // invariant address
                if (defined_in_loop(un.in[2], l)) continue;
                // conflicting store?
                bool conflict = false;
                for (NodeId s : stores)
                    if (aa_.base_of(g_.node(s).in[2]) == base) { conflict = true; break; }
                if (conflict) continue;

                g_.set_input(u, 0, pre);
                g_.set_input(u, 1, entry_mem);
                changed = true;
            }
        }
        return changed;
    }

    bool defined_in_loop(NodeId n, const Loop& l) const {
        if (is_block_head(g_.node(n).op)) return li_.block_in_loop(l, n);
        return li_.block_in_loop(l, g_.node(n).in[0]);
    }

    Graph& g_;
    LoopInfo& li_;
    AliasInfo& aa_;
    bool changed_ = false;
};
} // namespace

class LoadLICMPass : public Pass {
public:
    const char* name() const override { return "LoadLICM"; }
    int order() const override { return 41; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo | AnalysisKind::AliasInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::AliasInfo | AnalysisKind::MemDep;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            LoadLicm l(fg.g, ctx.analysis.loops(fg), ctx.analysis.alias(fg));
            changed |= l.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(LoadLICMPass, 41, "Phase 4")

} // namespace jules
