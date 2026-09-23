// Target-neutral register allocator core — the shared backend algorithm.
//
// Architecture (per the backend split): the register allocator is written
// ONCE against this interface; each machine target contributes only its
// instruction selection plus a register-file description (banks, class
// facts, callee-savedness). Nothing in this translation unit includes a
// target header — x86-64, and any future architecture, feed it problems
// through RaProblem and consume assignments from RaSolution.
//
// The allocator is hybrid, two phases:
//
//   Phase 1 — exact min-cost spill/split selection (network flow).
//     Which live ranges keep registers and which stay in memory is decided
//     GLOBALLY by a min-cost circulation on the segment chain: every live
//     range is an arc, every program point a capacity, and the optimum is
//     the minimum-weight spill set for the register count available at each
//     point (weights: loop-depth-amplified use costs, near-zero for
//     rematerializable values). The construction is exact: the constraint
//     system is a consecutive-ones (interval) program, hence totally
//     unimodular, hence the integral flow optimum IS the integral optimum.
//     Because the selected set has pointwise overlap at most K, it is
//     guaranteed colorable — assignment in Phase 2 never needs to invent a
//     new spill.
//
//   Phase 2 — assignment + iterated coalescing (IRC-grade repair).
//     Bank-aware greedy interval assignment (callee-saved banks for
//     call-crossing ranges, caller-first otherwise, -Os flips the
//     preference), then iterated conservative move coalescing at
//     sub-interval (generation) precision: move-related ranges whose exact
//     live sub-intervals never overlap are unified onto one register,
//     deleting the moves between them. Coalescing is only allowed when
//     every neighbor of the moved range already conflicts with the
//     destination (or occupies a disjoint interval), so a merge can never
//     make the assignment uncolorable.
//
// Soundness contract with the target: the target pre-filters which vregs
// are candidates (address-taken, class-conflicted, dead, or
// no-eligible-bank ranges are excluded), provides exact live sub-intervals
// ("gens") and the hull, and applies the resulting assignment. A vreg the
// core does not promote simply keeps its memory operands — spilling
// degenerates to the target's memory model, never to undefined behavior.
#pragma once

#include "core/support/common.h"

#include <string>
#include <vector>

namespace jules::ralloc {

// ---------------------------------------------------------------------------
// Problem description (target -> core)
// ---------------------------------------------------------------------------

// One live sub-interval of a vreg, as [start, end] positions in the
// target's program order. Positions are opaque to the core (the target
// uses its own instruction indices); only ordering matters.
struct RaGen {
    u32 start = 0;
    u32 end = 0;
};

struct RaVReg {
    i32 id = -1;             // target-side id (frame slot number, ...); -1 = none
    u8 cls = 0;              // index into RaProblem::classes
    u32 weight = 0;          // spill cost: sum of loop-depth-weighted uses;
                             // the flow minimizes total spilled weight
    bool crosses_call = false; // eligible only for callee-saved banks
    bool loop_carried = false; // live across a backedge (weight already
                               // amplified by the target; informational
                               // here)
    // hull range: [first, last] activity span. `starts_at_def` = the first
    // activity DEFINES the value (a two-operand handover may share a
    // register with a range ending exactly at `first`).
    u32 first = 0;
    u32 last = 0;
    bool starts_at_def = false;
    std::vector<RaGen> gens;  // exact disjoint sub-intervals (coalescing
                             // precision); may be empty (= the hull)
};

// A coalescing candidate: two vregs connected by a move the target wants
// deleted. Attempted in order; each unification must pass the conservative
// generation-disjointness test.
struct RaMove {
    i32 a = -1;
    i32 b = -1;
};

// A physical register bank: a set of opaque register ids sharing
// save/restore behavior. Preference order is the bank order within a
// class. A vreg is eligible for bank B iff !crosses_call || B.callee_saved.
struct RaBank {
    std::vector<u16> regs;
    bool callee_saved = false;
    const char* name = "";
};

struct RaClass {
    const char* name = "";
    std::vector<RaBank> banks;
    // Total/callee-saved capacities (derived; filled by ralloc_hybrid).
    u32 total_regs() const;
    u32 callee_saved_regs() const;
};

struct RaProblem {
    std::vector<RaVReg> vregs;
    std::vector<RaMove> moves;
    std::vector<RaClass> classes;
    bool prefer_callee_saved = false; // -Os: flip bank preference for
                                      // non-crossing ranges
};

// ---------------------------------------------------------------------------
// Solution (core -> target)
// ---------------------------------------------------------------------------

struct RaAssign {
    i32 id = -1;        // echo of RaVReg::id
    u16 reg = 0;        // physical register (valid iff promoted)
    bool promoted = false;
};

struct RaSolution {
    std::vector<RaAssign> vregs;  // same order as problem vregs
    u32 promoted_count = 0;
    u32 spilled_count = 0;         // candidate vregs left in memory
    u32 coalesced_moves = 0;      // successful unifications
};

// Run the hybrid allocator. Deterministic; O(V log V + E log V) for the
// flow phases plus coalescing rounds.
RaSolution ralloc_hybrid(const RaProblem& p);

// ---------------------------------------------------------------------------
// Phase 1 internals (exposed for the offline validator)
// ---------------------------------------------------------------------------

// Occupancy span of a vreg on the segment chain: occupies half-open
// [start, end). A range whose first activity is a USE occupies the
// segment BEFORE its first position (the value is live at entry); a range
// starting at a DEF does not (the handover rule — a two-operand op reads
// the old value and writes the new one in a single instruction).
struct RaSpan {
    u32 start = 0;
    u32 end = 0;
    i32 vreg = -1;
    u32 weight = 0;
};

// Exact max-weight selection of spans with per-segment capacities
// (min-cost circulation on the consecutive-ones network; integral
// optimum). Returns the selected vreg ids. `caps` is indexed by segment
// id (span coordinates must lie inside [0, caps.size())).
std::vector<i32> ralloc_flow_select(const std::vector<RaSpan>& spans,
                                    const std::vector<u32>& caps);

} // namespace jules::ralloc
