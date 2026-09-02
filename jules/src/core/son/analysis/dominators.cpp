// Dominator computation over the SoN control subgraph (Cooper-Harvey-Kennedy).
#include "core/son/analysis/analysis.h"

namespace jules {

namespace {
// Control successors of a block head, per the IR contract:
//   If pinned to H  -> H branches to its IfTrue/IfFalse projections
//   Jump J          -> H = J.in[0] falls to J
//   Region R        -> every predecessor block head p has edge p -> R
void build_cfg(Graph& g, FlatMap<NodeId, std::vector<NodeId>>& preds,
               FlatMap<NodeId, std::vector<NodeId>>& succs) {
    auto add_edge = [&](NodeId from, NodeId to) {
        succs[from].push_back(to);
        preds[to].push_back(from);
    };
    for (NodeId id = 0; id < g.size(); ++id) {
        const Node& n = g.node(id);
        if (n.op == Op::Dead) continue;
        switch (n.op) {
            case Op::If: {
                NodeId h = n.in[0];
                for (NodeId u : g.uses_of(id)) {
                    Op uo = g.node(u).op;
                    if (uo == Op::IfTrue || uo == Op::IfFalse) add_edge(h, u);
                }
                break;
            }
            case Op::Jump:
                add_edge(n.in[0], id);
                break;
            case Op::Region:
                for (u8 i = 0; i < n.n_in; ++i) add_edge(n.in[i], id);
                break;
            default:
                break;
        }
    }
}
} // namespace

std::unique_ptr<DomTree> DomTree::compute(Graph& g) {
    auto dt = std::make_unique<DomTree>();
    dt->g_ = &g;
    build_cfg(g, dt->preds_, dt->succs_);

    // Iterative postorder DFS from Start.
    std::vector<NodeId> post;
    std::vector<NodeId> stack{g.start()};
    FlatMap<NodeId, u8> state; // 0=unseen 1=on stack 2=done
    state.insert(g.start(), 1);
    while (!stack.empty()) {
        NodeId b = stack.back();
        bool advanced = false;
        const std::vector<NodeId>* ss = dt->succs_.find(b);
        if (ss) {
            for (NodeId s : *ss) {
                u8* st = state.find(s);
                if (!st) {
                    state.insert(s, 1);
                    stack.push_back(s);
                    advanced = true;
                    break;
                }
            }
        }
        if (!advanced) {
            state.insert(b, 2);
            post.push_back(b);
            stack.pop_back();
        }
    }
    // RPO = reverse postorder
    dt->rpo_.assign(post.rbegin(), post.rend());
    for (u32 i = 0; i < dt->rpo_.size(); ++i) dt->rpo_num_.insert(dt->rpo_[i], i);

    // Cooper-Harvey-Kennedy idom computation.
    NodeId start = g.start();
    dt->idom_.insert(start, kNoNode);
    bool changed = true;
    while (changed) {
        changed = false;
        for (NodeId b : dt->rpo_) {
            if (b == start) continue;
            NodeId new_idom = kNoNode;
            const std::vector<NodeId>* ps = dt->preds_.find(b);
            if (ps) {
                for (NodeId p : *ps) {
                    if (!dt->rpo_num_.contains(p)) continue;      // unreachable pred
                    if (dt->idom_.find(p) == nullptr && p != start) continue;
                    if (new_idom == kNoNode) {
                        new_idom = p;
                    } else if (dt->idom_.contains(p) || p == start) {
                        NodeId f = dt->intersect(p, new_idom);
                        if (f != kNoNode) new_idom = f;
                    }
                }
            }
            NodeId* cur = dt->idom_.find(b);
            if (!cur || *cur != new_idom) {
                dt->idom_.insert(b, new_idom);
                changed = true;
            }
        }
    }
    return dt;
}

NodeId DomTree::intersect(NodeId a, NodeId b) {
    while (a != b) {
        while (rpo_number(a) > rpo_number(b)) a = idom(a);
        while (rpo_number(b) > rpo_number(a)) b = idom(b);
    }
    return a;
}

u32 DomTree::rpo_number(NodeId b) const {
    const u32* p = rpo_num_.find(b);
    return p ? *p : 0xFFFFFFFFu;
}

NodeId DomTree::idom(NodeId block) const {
    const NodeId* p = idom_.find(block);
    return p ? *p : kNoNode;
}

bool DomTree::dominates(NodeId a, NodeId b) const {
    if (a == b) return true;
    if (!rpo_num_.contains(a) || !rpo_num_.contains(b)) return false;
    NodeId runner = b;
    u32 guard = 0;
    while (runner != kNoNode && runner != a) {
        runner = idom(runner);
        if (++guard > 100000) return false; // named cycle guard
    }
    return runner == a;
}

const std::vector<NodeId>& DomTree::preds(NodeId block) const {
    static const std::vector<NodeId> kEmpty;
    const std::vector<NodeId>* p = preds_.find(block);
    return p ? *p : kEmpty;
}

const std::vector<NodeId>& DomTree::succs(NodeId block) const {
    static const std::vector<NodeId> kEmpty;
    const std::vector<NodeId>* p = succs_.find(block);
    return p ? *p : kEmpty;
}

bool DomTree::reachable(NodeId block) const { return rpo_num_.contains(block); }

} // namespace jules
