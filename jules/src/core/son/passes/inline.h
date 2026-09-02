// Inlining infrastructure shared by passes 77/78/80 (not itself a pass).
//
// Graph-level inlining of a direct call:
//   * callee must have exactly one Return
//   * callee nodes are cloned into the caller with:
//       Start (ctrl pin)  -> fresh Jump block J_entry pinned at the call site
//       Start (memory)    -> the caller memory version feeding the call
//       Param i           -> the call's argument node
//   * the cloned Return dissolves into a Jump J_exit; its value/mem replace
//     the call's users; caller nodes pinned after the call (i.e. not part of
//     the call's memory ancestry) are repinned to J_exit so control flows
//     call-site -> callee body -> resume point.
#pragma once

#include "core/son/passes/pass_utils.h"

namespace jules {

// Returns true if the call was inlined (callee dissolved into caller).
bool inline_call(FunctionGraph& caller, NodeId call, FunctionGraph& callee);

// Policy hooks used by the inlining passes (pass 79 owns the bounds).
bool inline_recursion_ok(FnId caller, FnId callee);
u32 inline_budget_default();

} // namespace jules
