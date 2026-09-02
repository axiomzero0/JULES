// Natural loop detection: backedge u->h (h dominates u), reverse reachability.
#include "core/son/analysis/analysis.h"

namespace jules {

std::unique_ptr<LoopInfo> LoopInfo::compute(Graph& g, DomTree& dom) {
    auto li = std::make_unique<LoopInfo>();
    li->g_ = &g;
    li->dom_ = &dom;

    // back edges: succ u -> h where dominates(h, u)
    std::vector<std::pair<NodeId, NodeId>> backedges;
    for (NodeId b : dom.rpo()) {
        for (NodeId s : dom.succs(b)) {
            if (dom.dominates(s, b)) backedges.emplace_back(b, s); // u=b, h=s
        }
    }

    // Merge natural loops that share a header (multi-backedge loops).
    FlatMap<NodeId, std::vector<std::vector<NodeId>>> by_header;
    for (auto& [u, h] : backedges) {
        std::vector<NodeId> body;
        if (dom.dominates(h, u) && u != h) {
            // reverse reachability from u, stopping at h
            std::vector<NodeId> work{u};
            FlatMap<NodeId, bool> seen;
            seen.insert(u, true);
            while (!work.empty()) {
                NodeId n = work.back();
                work.pop_back();
                if (n == h) continue;
                body.push_back(n);
                for (NodeId p : dom.preds(n)) {
                    if (!seen.contains(p)) {
                        seen.insert(p, true);
                        work.push_back(p);
                    }
                }
            }
        } else {
            body = {u}; // self-loop
        }
        by_header[h].push_back(std::move(body));
    }

    for (const auto& [h, bodies] : by_header.entries()) {
        Loop l;
        l.header = h;
        FlatMap<NodeId, bool> in;
        in.insert(h, true);
        l.blocks.push_back(h);
        for (const std::vector<NodeId>& b : bodies) {
            for (NodeId n : b) {
                if (!in.contains(n)) {
                    in.insert(n, true);
                    l.blocks.push_back(n);
                }
            }
        }
        li->loops_.push_back(std::move(l));
    }

    // Sort by body size ascending -> inner loops first.
    std::sort(li->loops_.begin(), li->loops_.end(),
              [](const Loop& a, const Loop& b) { return a.blocks.size() < b.blocks.size(); });

    // Nesting: parent = smallest strictly-containing loop.
    for (size_t i = 0; i < li->loops_.size(); ++i) {
        Loop& inner = li->loops_[i];
        size_t parent_idx = SIZE_MAX;
        for (size_t j = 0; j < li->loops_.size(); ++j) {
            if (i == j) continue;
            const Loop& outer = li->loops_[j];
            if (outer.blocks.size() <= inner.blocks.size()) continue;
            bool all_in = true;
            for (NodeId b : inner.blocks)
                if (!li->block_in_loop(outer, b) && b != outer.header) { all_in = false; break; }
            if (!all_in) continue;
            if (parent_idx == SIZE_MAX || outer.blocks.size() < li->loops_[parent_idx].blocks.size())
                parent_idx = j;
        }
        if (parent_idx != SIZE_MAX) inner.parent_header = li->loops_[parent_idx].header;
    }

    for (size_t i = 0; i < li->loops_.size(); ++i) {
        Loop& l = li->loops_[i];
        l.depth = 1;
        NodeId p = l.parent_header;
        u32 guard = 0;
        while (p != kNoNode && ++guard <= li->loops_.size()) {
            ++l.depth;
            const Loop* pl = nullptr;
            for (const Loop& cand : li->loops_)
                if (cand.header == p) { pl = &cand; break; }
            if (!pl) break;
            p = pl->parent_header;
        }
        li->header_index_.insert(l.header, static_cast<u32>(i));
    }
    return li;
}

bool LoopInfo::is_header(NodeId block) const { return header_index_.contains(block); }

const Loop* LoopInfo::innermost_loop_of(NodeId block) const {
    const Loop* best = nullptr;
    for (const Loop& l : loops_) {
        if (block_in_loop(l, block)) {
            if (!best || l.blocks.size() < best->blocks.size()) best = &l;
        }
    }
    return best;
}

NodeId LoopInfo::preheader(NodeId header) const {
    NodeId found = kNoNode;
    u32 outside = 0;
    for (NodeId p : dom_->preds(header)) {
        bool in_loop = false;
        for (const Loop& l : loops_) {
            if (l.header == header && block_in_loop(l, p)) { in_loop = true; break; }
        }
        if (!in_loop) {
            ++outside;
            found = p;
        }
    }
    return outside == 1 ? found : kNoNode;
}

bool LoopInfo::block_in_loop(const Loop& l, NodeId block) const {
    for (NodeId b : l.blocks)
        if (b == block) return true;
    return false;
}

} // namespace jules
