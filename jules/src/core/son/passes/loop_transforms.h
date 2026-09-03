// Shared machinery for loop-body duplication transforms (passes 42/44:
// unrolling, peeling). A SoN body cloner + a counted-loop matcher.
//
// The cloner duplicates the body blocks of a natural loop k times as a
// chain, remapping per-copy seeds (each header phi maps to the previous
// copy's update value, so copy m sees the IV value of dynamic iteration
// base+m). Control wiring:
//     guard-body-projection P -> body_0 ... latch J_0
//   becomes
//     P -> body_0 ... J_0 -> E_1 -> body_1 ... J_1 -> ... -> J_{k-1} -> header
// The header's latch predecessor and every phi's backedge input point at
// the LAST copy; each E_m is a fresh Jump block heading copy m's first
// block contents.
#pragma once

#include "core/son/passes/pass_utils.h"

#include <vector>

namespace jules {
namespace loopx {

struct CountedLoop {
    NodeId header = kNoNode;     // Region (2 preds: entry, latch)
    u8 entry_slot = 0, latch_slot = 1;
    NodeId guard_if = kNoNode;   // If pinned at header
    NodeId body_proj = kNoNode;  // guard projection entering the body
    NodeId exit_proj = kNoNode;  // guard projection leaving the loop
    NodeId iv_phi = kNoNode;     // step phi at the header
    i64 iv_step = 1;             // positive constant step
    i64 trip = -1;               // exact dynamic iteration count (const)
    std::vector<NodeId> blocks;  // body block heads (RPO order, no header)
    u32 body_nodes = 0;          // non-control nodes pinned in the body
    std::vector<NodeId> phis;    // ALL header phis (incl. mem)
};

// Recognize `for (i = C0; i < C1 (| <=); i += Ck) body` with no early
// exits: the guard If is pinned at the header, its condition compares the
// IV phi against a constant bound, the IV's backedge input is Add(phi, k),
// and no If inside the body branches out of the loop. Returns false when
// any shape assumption fails.
bool match_counted(Graph& g, LoopInfo& li, DomTree& dom, const Loop& l,
                   CountedLoop& out);

// Duplicate the body `extra` times as a trailing chain (extra >= 1).
// After the call the loop's header latch and phi backedges reference the
// last copy. Returns the number of cloned nodes (0 = failure).
u32 clone_body_chain(Graph& g, const CountedLoop& cl, u32 extra, bool at_entry);

} // namespace loopx
} // namespace jules
