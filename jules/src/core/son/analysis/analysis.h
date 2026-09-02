// Analysis infrastructure: lazily computed, cached per function, invalidated
// by pass contracts (see passes/pass.h).
#pragma once

#include "core/son/graph.h"
#include "core/son/son.h"

#include <memory>

namespace jules {

// ---- dominator tree over control blocks -------------------------------------
class DomTree {
public:
    static std::unique_ptr<DomTree> compute(Graph& g);

    const std::vector<NodeId>& rpo() const { return rpo_; }       // reachable block heads
    NodeId idom(NodeId block) const;
    bool dominates(NodeId a, NodeId b) const;                     // block heads
    const std::vector<NodeId>& preds(NodeId block) const;
    const std::vector<NodeId>& succs(NodeId block) const;
    bool reachable(NodeId block) const;
    u32 rpo_number(NodeId block) const;

private:
    NodeId intersect(NodeId a, NodeId b);

private:
    Graph* g_ = nullptr;
    std::vector<NodeId> rpo_;
    FlatMap<NodeId, NodeId> idom_;
    FlatMap<NodeId, u32> rpo_num_;
    FlatMap<NodeId, std::vector<NodeId>> preds_;
    FlatMap<NodeId, std::vector<NodeId>> succs_;
};

// ---- natural loops ------------------------------------------------------------
struct Loop {
    NodeId header = kNoNode;
    std::vector<NodeId> blocks;      // block heads in the loop (incl. header)
    NodeId parent_header = kNoNode;  // kNoNode = top level
    u32 depth = 1;
};

class LoopInfo {
public:
    static std::unique_ptr<LoopInfo> compute(Graph& g, DomTree& dom);
    const std::vector<Loop>& loops() const { return loops_; }
    bool is_header(NodeId block) const;
    const Loop* innermost_loop_of(NodeId block) const;
    // Unique non-backedge predecessor block of a loop header, or kNoNode.
    NodeId preheader(NodeId header) const;
    bool block_in_loop(const Loop& l, NodeId block) const;

private:
    Graph* g_ = nullptr;
    DomTree* dom_ = nullptr;
    std::vector<Loop> loops_;        // inner-first ordering (by body size asc)
    FlatMap<NodeId, u32> header_index_;
};

// ---- alias analysis (type/base based, MVP) --------------------------------------
enum class AliasResult : u8 { NoAlias, MayAlias, MustAlias };

class AliasInfo {
public:
    static std::unique_ptr<AliasInfo> compute(Graph& g);
    // Base object of a pointer-valued node: an Alloc id, or kNoNode=unknown.
    NodeId base_of(NodeId ptr) const;
    AliasResult alias(NodeId ptr_a, NodeId ptr_b) const;
    // An allocation whose value never leaves as data (no call args, stores,
    // returns) — memory ops on other pointers cannot touch it.
    bool is_alloc_local(NodeId alloc) const;

private:
    Graph* g_ = nullptr;
    mutable FlatMap<NodeId, NodeId> base_cache_; // memoization (const-safe)
    mutable FlatMap<NodeId, bool> visiting_;     // phi-recursion cycle guard
    FlatMap<NodeId, bool> alloc_local_;
};

// ---- memory dependence ------------------------------------------------------------
enum class MemDepResult : u8 { Def, Barrier, Unknown };

class MemDep {
public:
    static std::unique_ptr<MemDep> compute(Graph& g, AliasInfo& aa);
    // Reaching definition for a load's address base along the memory chain.
    // `def` receives the store node whose value feeds the load.
    MemDepResult reaching_store(NodeId load, NodeId& def) const;
    // Does any read/barrier of `base` happen between store `s` and its
    // overwrite (forward walk over memory users)? Used by DSE.
    bool store_is_overwritten_before_read(NodeId s) const;

private:
    Graph* g_ = nullptr;
    AliasInfo* aa_ = nullptr;
    MemDepResult reaching_store_input(NodeId mem, NodeId addr, NodeId& def) const;
    MemDepResult reaching_store_input_impl(NodeId mem, NodeId addr, NodeId& def) const;
    mutable FlatMap<NodeId, bool> visiting_; // recursion cycle guard
};

// ---- call graph --------------------------------------------------------------------
class CallGraphInfo {
public:
    static std::unique_ptr<CallGraphInfo> compute(Module& m);
    struct Site { NodeId call; FnId callee; };
    const std::vector<Site>& sites_of(FnId fn) const;
    bool calls_out(FnId fn) const;
    std::vector<FnId> callees_of(FnId fn) const;
private:
    FlatMap<FnId, std::vector<Site>> sites_;
    FlatMap<FnId, bool> calls_out_;
};

// ---- branch probability (static heuristics) --------------------------------------------
class BranchProb {
public:
    static std::unique_ptr<BranchProb> compute(Graph& g, LoopInfo& loops);
    // Probability (0..1) that If node `ifn` takes its TRUE projection.
    f64 prob_true(NodeId ifn) const;
private:
    FlatMap<NodeId, f64> ptrue_;
};

// ---- analysis manager --------------------------------------------------------------
enum class AnalysisKind : u32 {
    Dominators = 1u << 0,
    LoopInfo   = 1u << 1,
    AliasInfo  = 1u << 2,
    MemDep     = 1u << 3,
    CallGraph  = 1u << 4,
    BranchProb = 1u << 5,
};
using AnalysisMask = u32;
constexpr AnalysisMask operator|(AnalysisKind a, AnalysisKind b) {
    return static_cast<AnalysisMask>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr AnalysisMask operator|(AnalysisMask a, AnalysisKind b) {
    return a | static_cast<u32>(b);
}

class AnalysisManager {
public:
    explicit AnalysisManager(Module& m) : mod_(m) {}

    DomTree& doms(FunctionGraph& fg);
    LoopInfo& loops(FunctionGraph& fg);
    AliasInfo& alias(FunctionGraph& fg);
    MemDep& memdep(FunctionGraph& fg);
    BranchProb& branchprob(FunctionGraph& fg);
    CallGraphInfo& callgraph();

    void invalidate(AnalysisMask kinds);

private:
    FunctionGraph& fn(FunctionGraph& fg) { return fg; } // identity hook for clarity

    Module& mod_;
    // per-function caches, indexed by FnId
    std::vector<std::unique_ptr<DomTree>> dom_cache_;
    std::vector<std::unique_ptr<LoopInfo>> loop_cache_;
    std::vector<std::unique_ptr<AliasInfo>> alias_cache_;
    std::vector<std::unique_ptr<MemDep>> memdep_cache_;
    std::vector<std::unique_ptr<BranchProb>> prob_cache_;
    std::unique_ptr<CallGraphInfo> callgraph_cache_;
    size_t caches_reserved_ = 0;
};

} // namespace jules
