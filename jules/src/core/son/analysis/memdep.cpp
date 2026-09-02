// Memory dependence: reaching-def walk along the memory chain, and DSE's
// forward overwrite scan. Both are alias-aware through AliasInfo.
#include "core/son/analysis/analysis.h"

namespace jules {

namespace {
constexpr u32 kMemWalkLimit = 4096; // named walk bound (guards pathological graphs)

bool same_base(const AliasInfo& aa, NodeId a, NodeId b) {
    NodeId ba = aa.base_of(a);
    NodeId bb = aa.base_of(b);
    return ba != kNoNode && ba == bb;
}
} // namespace

std::unique_ptr<MemDep> MemDep::compute(Graph& g, AliasInfo& aa) {
    auto md = std::make_unique<MemDep>();
    md->g_ = &g;
    md->aa_ = &aa;
    return md;
}

MemDepResult MemDep::reaching_store(NodeId load, NodeId& def) const {
    visiting_.clear();
    const Node& ln = g_->node(load);
    NodeId addr = ln.in[2];
    NodeId mem = ln.in[1];
    u32 steps = 0;

    while (mem != kNoNode && g_->node(mem).op != Op::Dead && ++steps <= kMemWalkLimit) {
        const Node& m = g_->node(mem);
        switch (m.op) {
            case Op::Store:
                if (same_base(*aa_, m.in[2], addr)) {
                    def = mem;
                    return MemDepResult::Def;
                }
                mem = m.in[1];
                continue;
            case Op::Alloc:
                mem = m.in[1];
                continue; // allocation does not write
            case Op::Call:
                // Conservative: calls may read/write anything reachable
                // through their arguments; MVP treats them as barriers.
                return MemDepResult::Barrier;
            case Op::Start:
                return MemDepResult::Unknown;
            case Op::Phi: {
                // Meet over all phi inputs.
                NodeId found = kNoNode;
                bool divergent = false;
                for (u8 i = 1; i < m.n_in; ++i) {
                    NodeId d = kNoNode;
                    MemDepResult r = reaching_store_input(m.in[i], addr, d);
                    if (r == MemDepResult::Barrier) return MemDepResult::Barrier;
                    if (r == MemDepResult::Unknown) return MemDepResult::Unknown;
                    if (found == kNoNode) found = d;
                    else if (d != found) divergent = true;
                }
                if (found == kNoNode) return MemDepResult::Unknown;
                if (divergent) return MemDepResult::Barrier;
                def = found;
                return MemDepResult::Def;
            }
            default:
                return MemDepResult::Unknown;
        }
    }
    return MemDepResult::Unknown;
}

MemDepResult MemDep::reaching_store_input(NodeId mem, NodeId addr, NodeId& def) const {
    // Cycle guard: memory phis in loops reference themselves along backedges.
    if (visiting_.contains(mem)) return MemDepResult::Unknown;
    visiting_.insert(mem, true);
    MemDepResult out = reaching_store_input_impl(mem, addr, def);
    visiting_.erase(mem);
    return out;
}

MemDepResult MemDep::reaching_store_input_impl(NodeId mem, NodeId addr, NodeId& def) const {
    // Walk a single chain (helper for phi meets), bounded.
    u32 steps = 0;
    while (mem != kNoNode && g_->node(mem).op != Op::Dead && ++steps <= kMemWalkLimit) {
        const Node& m = g_->node(mem);
        switch (m.op) {
            case Op::Store:
                if (same_base(*aa_, m.in[2], addr)) {
                    def = mem;
                    return MemDepResult::Def;
                }
                mem = m.in[1];
                continue;
            case Op::Alloc:
                mem = m.in[1];
                continue;
            case Op::Call:
                return MemDepResult::Barrier;
            case Op::Start:
                return MemDepResult::Unknown;
            case Op::Phi: {
                NodeId found = kNoNode;
                bool divergent = false;
                for (u8 i = 1; i < m.n_in; ++i) {
                    NodeId d = kNoNode;
                    MemDepResult r = reaching_store_input(m.in[i], addr, d);
                    if (r != MemDepResult::Def) return r;
                    if (found == kNoNode) found = d;
                    else if (d != found) divergent = true;
                }
                if (found == kNoNode || divergent) return MemDepResult::Unknown;
                def = found;
                return MemDepResult::Def;
            }
            default:
                return MemDepResult::Unknown;
        }
    }
    return MemDepResult::Unknown;
}

bool MemDep::store_is_overwritten_before_read(NodeId s) const {
    const Node& sn = g_->node(s);
    NodeId base_addr = sn.in[2];

    // What happens to memory downstream of version `s`?
    //   Store same base  -> overwrite, path dies (good)
    //   Store other base -> continue past it
    //   Load  same base  -> read (bad)
    //   Load  other base -> continue
    //   Call             -> may read (bad)
    //   Alloc            -> continue
    //   Phi containing s -> conservative (bad)
    std::vector<NodeId> work;
    FlatMap<NodeId, bool> visited;
    auto push_users_of = [&](NodeId ver) {
        for (NodeId u : g_->uses_of(ver)) {
            const Node& un = g_->node(u);
            if (un.op == Op::Phi) {
                // a phi input in ANY slot is a merge use: it carries the
                // value across control flow and must block deadness
                for (u8 i = 1; i < un.n_in; ++i)
                    if (un.in[i] == ver) work.push_back(u);
                continue;
            }
            if ((un.op == Op::Load || un.op == Op::Store || un.op == Op::Call ||
                 un.op == Op::Alloc) &&
                un.n_in > 1 && un.in[1] == ver)
                work.push_back(u);
        }
    };
    push_users_of(s);
    u32 steps = 0;
    while (!work.empty() && ++steps <= kMemWalkLimit) {
        NodeId u = work.back();
        work.pop_back();
        if (visited.contains(u)) continue;
        visited.insert(u, true);
        const Node& un = g_->node(u);
        switch (un.op) {
            case Op::Store:
                if (same_base(*aa_, un.in[2], base_addr)) continue; // overwritten
                push_users_of(u);
                break;
            case Op::Load:
                if (same_base(*aa_, un.in[2], base_addr)) return false;
                break; // unrelated load: not part of the chain ordering we need
            case Op::Call:
                return false;
            case Op::Alloc:
                push_users_of(u);
                break;
            case Op::Phi:
                return false; // memory merges into control flow: give up conservatively
            default:
                break;
        }
    }
    return true; // no read before overwrite found
}

} // namespace jules
