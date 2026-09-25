// Sea-of-Nodes graph container: index-based node storage, lazy use lists,
// replace-all-uses, kill, change telemetry.
#pragma once

#include "core/son/node.h"
#include "core/support/symbols.h"

namespace jules {

class Graph {
public:
    Graph() { start_ = make(Op::Start, ty_ctrl()); }

    // ---- construction ------------------------------------------------------
    NodeId make(Op op, TypeId ty, std::initializer_list<NodeId> ins = {},
                u8 sub = 0, u32 aux = 0);
    NodeId make_arr(Op op, TypeId ty, const NodeId* ins, u8 n, u8 sub = 0, u32 aux = 0);
    void set_input(NodeId n, u8 idx, NodeId v);
    void append_input(NodeId n, NodeId v); // Region/Phi/Stop (arity-guarded)

    NodeId start() const { return start_; }
    NodeId stop() const { return stop_; }
    void set_stop(NodeId s) { stop_ = s; }

    Node& node(NodeId id) {
        assert(id < nodes_.size());
        return nodes_[id];
    }
    const Node& node(NodeId id) const {
        assert(id < nodes_.size());
        return nodes_[id];
    }
    size_t size() const { return nodes_.size(); }
    u32 live_count() const;   // non-Dead nodes

    // ---- mutation ------------------------------------------------------------
    void replace_all_uses(NodeId from, NodeId to);
    // Rewrite only uses of `from` in slot `slot` (e.g. mem-input threading).
    void replace_uses_in_slot(NodeId from, NodeId to, u8 slot);
    // Memory-version relink: rewrites slot-1 uses AND any phi input slot
    // (phi inputs >= 1 are memory versions for memory phis). Every pass that
    // removes an effect node from the chain must use THIS, or downstream
    // resolution walks will hit dangling nodes.
    void replace_uses_as_memory(NodeId from, NodeId to);
    // Kill preserves inputs (op=Dead): repair passes follow Dead->in[1].
    void kill(NodeId n);
    bool is_dead(NodeId n) const { return node(n).op == Op::Dead; }

    // ---- use lists (lazily rebuilt) -------------------------------------------
    const SmallVec<NodeId, 4>& uses_of(NodeId n);
    void mark_uses_dirty() { uses_dirty_ = true; }
    void rebuild_uses();

    // ---- telemetry -------------------------------------------------------------
    u64 changes() const { return changes_; }
    void reset_changes() { changes_ = 0; }
    void touch() { ++changes_; }

    // Structural content hash for a pure node (GVN): skips the ctrl slot.
    u64 pure_hash(NodeId n) const;
    bool pure_equals(NodeId a, NodeId b) const; // a,b pure ops, ctrl-insensitive

private:
    std::vector<Node> nodes_;
    std::vector<SmallVec<NodeId, 4>> uses_;
    NodeId start_ = kNoNode;
    NodeId stop_ = kNoNode;
    bool uses_dirty_ = true;
    u64 changes_ = 0;
};

// ---- module-level IR container ------------------------------------------------
using FnId = u32;
inline constexpr FnId kFnPrint = 0xFFFFFF01u; // builtin pseudo-targets in Call.aux
inline constexpr FnId kFnFree  = 0xFFFFFF02u;
inline constexpr FnId kFnPgoBump = 0xFFFFFF03u; // PGO counter increment (pass 43);
                                               // ival = counter index; in = {ctrl, mem}
inline constexpr FnId kFnPgoSketch = 0xFFFFFF04u; // PGO sticky-value argument
                                               // sketch (pass 91); ival = base counter
                                               // index of the [first,total,match]
                                               // triple; in = {ctrl, mem, arg}
inline constexpr FnId kNoFn    = 0xFFFFFFFFu;
inline constexpr FnId kMaxUserFn = 0xFFFFFF00u;

// extern "C" pseudo-FnIds: calls to declared-but-external functions. The
// names live in SemaModule::externs (filled at declaration) and reach the
// emitter through LinearModule::externs; externs never get a FunctionGraph
// (nothing to optimize). Every Call.aux consumer treats the whole range
// >= kExternFnBase as external (print/free/pgo builtins included).
inline constexpr u32 kExternFnBase = 0xFFFF0000u;
inline constexpr u32 kExternFnMax = 32; // MVP: plenty for libc surface
inline constexpr FnId extern_fn_id(u32 index) {
    return static_cast<FnId>(kExternFnBase + index);
}
inline constexpr bool is_extern_fn_id(FnId f) {
    return f >= kExternFnBase && f < kExternFnBase + kExternFnMax;
}
// Any call target that is not a user fn graph (externs + builtins + none).
inline constexpr bool is_external_call(FnId f) {
    return f >= static_cast<FnId>(kExternFnBase);
}

struct FunctionGraph {
    FnId fid = kNoFn;
    SymbolId name = kNoSymbol;
    Graph g;
    bool always_inline = false;
    bool no_inline = false;
    bool is_comptime = false;
    std::vector<TypeId> param_types;
    TypeId ret = ty_void();
    u32 node_estimate = 0;
};

struct Module {
    SymbolTable* syms = nullptr;
    std::vector<FunctionGraph> fns;
    const FunctionGraph* find_fn(FnId fid) const {
        if (fid >= fns.size()) return nullptr;
        return &fns[fid];
    }
    FunctionGraph* find_fn(FnId fid) {
        if (fid >= fns.size()) return nullptr;
        return &fns[fid];
    }
};

} // namespace jules
