// Call graph over the module + branch probability heuristics + the analysis
// manager (lazy compute / contract invalidation).
#include "core/son/analysis/analysis.h"

namespace jules {

// ---- call graph ---------------------------------------------------------------
std::unique_ptr<CallGraphInfo> CallGraphInfo::compute(Module& m) {
    auto cg = std::make_unique<CallGraphInfo>();
    for (FunctionGraph& fg : m.fns) {
        auto& sites = cg->sites_[fg.fid];
        bool calls_out = false;
        for (NodeId id = 0; id < fg.g.size(); ++id) {
            const Node& n = fg.g.node(id);
            if (n.op != Op::Call) continue;
            // data slots start at 2
            sites.push_back(CallGraphInfo::Site{id, n.aux});
            if (n.aux != kFnPrint && n.aux != kFnFree) calls_out = true;
        }
        cg->calls_out_.insert(fg.fid, calls_out);
    }
    return cg;
}

const std::vector<CallGraphInfo::Site>& CallGraphInfo::sites_of(FnId fn) const {
    static const std::vector<Site> kEmpty;
    const auto* p = sites_.find(fn);
    return p ? *p : kEmpty;
}

bool CallGraphInfo::calls_out(FnId fn) const {
    if (const bool* p = calls_out_.find(fn)) return *p;
    return false;
}

std::vector<FnId> CallGraphInfo::callees_of(FnId fn) const {
    std::vector<FnId> out;
    for (const Site& s : sites_of(fn))
        if (s.callee != kFnPrint && s.callee != kFnFree)
            out.push_back(s.callee);
    return out;
}

// ---- branch probability ----------------------------------------------------------
std::unique_ptr<BranchProb> BranchProb::compute(Graph& g, LoopInfo& loops) {
    auto bp = std::make_unique<BranchProb>();
    constexpr f64 kBackedgeProb = 0.90; // loop body taken (named constant)
    constexpr f64 kDefaultProb  = 0.50;

    for (NodeId id = 0; id < g.size(); ++id) {
        const Node& n = g.node(id);
        if (n.op != Op::If) continue;
        f64 p = kDefaultProb;
        // True projection is the loop backedge? -> hot.
        for (NodeId u : g.uses_of(id)) {
            if (g.node(u).op != Op::IfTrue) continue;
            for (NodeId uu : g.uses_of(u)) {
                // IfTrue used as region pred; if that region is a loop header
                // and the edge is a backedge, the true side is hot.
                const Node& r = g.node(uu);
                if (r.op == Op::Region && loops.is_header(uu)) {
                    // backedge if IfTrue's block is inside the loop
                    for (const Loop& l : loops.loops()) {
                        if (l.header == uu) {
                            for (NodeId blk : l.blocks)
                                if (blk == u) p = kBackedgeProb;
                        }
                    }
                }
            }
            // If projection falls into a Return block -> likely exit path
        }
        bp->ptrue_.insert(id, p);
    }
    return bp;
}

f64 BranchProb::prob_true(NodeId ifn) const {
    if (const f64* p = ptrue_.find(ifn)) return *p;
    return 0.5;
}

// ---- analysis manager -------------------------------------------------------------
DomTree& AnalysisManager::doms(FunctionGraph& fg) {
    if (dom_cache_.size() < mod_.fns.size()) dom_cache_.resize(mod_.fns.size());
    auto& slot = dom_cache_[fg.fid];
    if (!slot) slot = DomTree::compute(fg.g);
    return *slot;
}

LoopInfo& AnalysisManager::loops(FunctionGraph& fg) {
    if (loop_cache_.size() < mod_.fns.size()) loop_cache_.resize(mod_.fns.size());
    auto& slot = loop_cache_[fg.fid];
    if (!slot) slot = LoopInfo::compute(fg.g, doms(fg));
    return *slot;
}

AliasInfo& AnalysisManager::alias(FunctionGraph& fg) {
    if (alias_cache_.size() < mod_.fns.size()) alias_cache_.resize(mod_.fns.size());
    auto& slot = alias_cache_[fg.fid];
    if (!slot) slot = AliasInfo::compute(fg.g);
    return *slot;
}

MemDep& AnalysisManager::memdep(FunctionGraph& fg) {
    if (memdep_cache_.size() < mod_.fns.size()) memdep_cache_.resize(mod_.fns.size());
    auto& slot = memdep_cache_[fg.fid];
    if (!slot) slot = MemDep::compute(fg.g, alias(fg));
    return *slot;
}

BranchProb& AnalysisManager::branchprob(FunctionGraph& fg) {
    if (prob_cache_.size() < mod_.fns.size()) prob_cache_.resize(mod_.fns.size());
    auto& slot = prob_cache_[fg.fid];
    if (!slot) slot = BranchProb::compute(fg.g, loops(fg));
    return *slot;
}

CallGraphInfo& AnalysisManager::callgraph() {
    if (!callgraph_cache_) callgraph_cache_ = CallGraphInfo::compute(mod_);
    return *callgraph_cache_;
}

void AnalysisManager::invalidate(AnalysisMask kinds) {
    if (kinds & static_cast<u32>(AnalysisKind::Dominators)) dom_cache_.clear();
    if (kinds & static_cast<u32>(AnalysisKind::LoopInfo)) loop_cache_.clear();
    if (kinds & static_cast<u32>(AnalysisKind::AliasInfo)) alias_cache_.clear();
    if (kinds & static_cast<u32>(AnalysisKind::MemDep)) memdep_cache_.clear();
    if (kinds & static_cast<u32>(AnalysisKind::BranchProb)) prob_cache_.clear();
    if (kinds & static_cast<u32>(AnalysisKind::CallGraph)) callgraph_cache_.reset();
}

} // namespace jules
