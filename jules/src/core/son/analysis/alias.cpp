// Type/base-based alias analysis (MVP): distinct non-escaping allocations
// provably do not alias; everything else is MayAlias.
#include <functional>

#include "core/son/analysis/analysis.h"

namespace jules {

std::unique_ptr<AliasInfo> AliasInfo::compute(Graph& g) {
    auto aa = std::make_unique<AliasInfo>();
    aa->g_ = &g;

    // base_of with memoization + cycle guard.
    for (NodeId id = 0; id < g.size(); ++id) {
        if (g.node(id).op != Op::Dead) (void)aa->base_of(id);
    }

    // local allocations: the pointer VALUE flows only to load/store
    // ADDRESSES — directly, or through the address-arithmetic grammar
    // (ptrcast casts, base+offset adds: the shapes match_addr recognizes) —
    // to the memory chain (allocs/loads consume it as a version), or to
    // free(); never to calls, returns, stores-as-value, phis or data
    // arithmetic. Offsets past the allocation are source-level UB, so
    // distinct live allocations stay NoAlias (malloc semantics).
    for (NodeId id = 0; id < g.size(); ++id) {
        const Node& n = g.node(id);
        if (n.op != Op::Alloc) continue;
        FlatMap<NodeId, bool> visiting;
        // all uses of `id` (and, through transparent address nodes, of its
        // derived values) must be address/chain/free forms
        std::function<bool(NodeId)> uses_addr_only = [&](NodeId v) -> bool {
            for (NodeId u : g.uses_of(v)) {
                if (g.is_dead(u)) continue;
                const Node& un = g.node(u);
                switch (un.op) {
                    case Op::Load:
                    case Op::Store:
                        if (un.in[2] == v || un.in[1] == v) break; // addr / mem chain
                        return false;
                        break;
                    case Op::Alloc:
                        if (un.in[1] == v) break; // chained allocation (mem)
                        return false;
                        break;
                    case Op::Phi: {
                        // memory-state phi (loop header / region merge): the
                        // alloc's VERSION flows as the loop's initial memory
                        // — a chain use, not a pointer escape. Value phis of
                        // the pointer itself stay escapes (conservative).
                        if (un.ty != ty_mem()) return false;
                        bool is_input = false;
                        for (u8 i = 1; i < un.n_in; ++i)
                            if (un.in[i] == v) is_input = true;
                        if (!is_input) return false;
                        break;
                    }
                    case Op::Call: {
                        if (un.in[1] == v) break; // mem-chain use only
                        // free(ptr): lifetime end, not an escape — pass 29's
                        // own escape classifier applies the same rule.
                        if (un.aux == kFnFree) {
                            bool freed_here = false;
                            for (u8 i = 2; i < un.n_in; ++i)
                                if (un.in[i] == v) freed_here = true;
                            if (freed_here) break;
                        }
                        return false;
                    }
                    case Op::Cast: {
                        // address arithmetic: ptrcast of the derived value;
                        // the result must itself be address-only
                        CastOp cs = static_cast<CastOp>(un.sub);
                        if (un.in[1] != v) return false; // pin slot only
                        if (cs != CastOp::Ptr) return false;
                        if (visiting.contains(u)) continue; // cycle guard
                        visiting.insert(u, true);
                        bool ok = uses_addr_only(u);
                        visiting.erase(u);
                        if (!ok) return false;
                        break;
                    }
                    case Op::Bin: {
                        // base + offset: the derived pointer-ish value stays
                        // address-only (the offset operand's own uses are
                        // not the alloc's business)
                        if (static_cast<BinOp>(un.sub) != BinOp::Add) return false;
                        if (un.in[1] != v && un.in[2] != v) return false;
                        if (visiting.contains(u)) continue;
                        visiting.insert(u, true);
                        bool ok = uses_addr_only(u);
                        visiting.erase(u);
                        if (!ok) return false;
                        break;
                    }
                    default:
                        return false; // phi / return / value use: escapes
                }
            }
            return true;
        };
        bool local = uses_addr_only(id);
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
    // Two distinct allocation sites never alias (malloc semantics: live
    // allocations from distinct sites hold disjoint memory; stale access
    // after free is source-level UB — LLVM's malloc-based NoAlias). This
    // holds even when a pointer ESCAPES into a call: escape matters for
    // "who can read this memory" (DSE's never-read reasoning and load
    // hoisting consult is_alloc_local, which stays escape-conservative),
    // not for "can two distinct sites overlap".
    if (g_->node(ba).op == Op::Alloc && g_->node(bb).op == Op::Alloc)
        return AliasResult::NoAlias;
    return AliasResult::MayAlias;
}

bool AliasInfo::is_alloc_local(NodeId alloc) const {
    if (const bool* v = alloc_local_.find(alloc)) return *v;
    return false;
}

} // namespace jules
