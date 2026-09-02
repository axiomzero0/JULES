// Type/base-based alias analysis (MVP): distinct non-escaping allocations
// provably do not alias; everything else is MayAlias.
#include "core/son/analysis/analysis.h"

namespace jules {

std::unique_ptr<AliasInfo> AliasInfo::compute(Graph& g) {
    auto aa = std::make_unique<AliasInfo>();
    aa->g_ = &g;

    // base_of with memoization + cycle guard.
    for (NodeId id = 0; id < g.size(); ++id) {
        if (g.node(id).op != Op::Dead) (void)aa->base_of(id);
    }

    // local allocations: value used only as load/store addresses or as the
    // memory chain (alloc node itself), never as data escaping to calls,
    // returns, stores-as-value or arithmetic.
    for (NodeId id = 0; id < g.size(); ++id) {
        const Node& n = g.node(id);
        if (n.op != Op::Alloc) continue;
        bool local = true;
        for (NodeId u : g.uses_of(id)) {
            const Node& un = g.node(u);
            switch (un.op) {
                case Op::Load:
                case Op::Store:
                    if (un.in[2] == id || un.in[1] == id) break; // addr or mem-chain use
                    local = false;
                    break;
                case Op::Call:
                    if (un.in[1] == id) break; // mem-chain use only
                    local = false;
                    break;
                default:
                    local = false;
                    break;
            }
            if (!local) break;
        }
        aa->alloc_local_.insert(id, local);
    }
    return aa;
}

NodeId AliasInfo::base_of(NodeId ptr) const {
    // iterative resolution with a cycle guard; phi inputs recurse with the
    // shared visiting set (self-referential loop phis are common after SROA)
    NodeId cur = ptr;
    u32 guard = 0;
    FlatMap<NodeId, bool> on_path;
    while (cur != kNoNode && ++guard <= g_->size()) {
        if (on_path.contains(cur)) return kNoNode; // cycle -> unknown
        if (visiting_.contains(cur)) return kNoNode;
        if (const NodeId* cached = base_cache_.find(cur)) return *cached;
        const Node& n = g_->node(cur);
        switch (n.op) {
            case Op::Alloc:
                base_cache_.insert(cur, cur);
                return cur;
            case Op::Cast:
                on_path.insert(cur, true);
                cur = n.in[1];
                continue;
            case Op::Phi: {
                // base = common base of all inputs, else unknown
                visiting_.insert(cur, true);
                NodeId common = kNoNode;
                bool first = true;
                for (u8 i = 1; i < n.n_in; ++i) {
                    NodeId b = base_of(n.in[i]);
                    if (first) { common = b; first = false; }
                    else if (b != common) { common = kNoNode; break; }
                }
                visiting_.erase(cur);
                base_cache_.insert(cur, common);
                return common;
            }
            default:
                base_cache_.insert(cur, kNoNode);
                return kNoNode;
        }
    }
    return kNoNode;
}

AliasResult AliasInfo::alias(NodeId a, NodeId b) const {
    NodeId ba = base_of(a);
    NodeId bb = base_of(b);
    if (ba == kNoNode || bb == kNoNode) return AliasResult::MayAlias;
    if (ba == bb) return AliasResult::MustAlias;
    // Two distinct local allocations cannot alias.
    bool la = false, lbb = false;
    if (const bool* v = alloc_local_.find(ba)) la = *v;
    if (const bool* v = alloc_local_.find(bb)) lbb = *v;
    if (la && lbb) return AliasResult::NoAlias;
    return AliasResult::MayAlias;
}

bool AliasInfo::is_alloc_local(NodeId alloc) const {
    if (const bool* v = alloc_local_.find(alloc)) return *v;
    return false;
}

} // namespace jules
