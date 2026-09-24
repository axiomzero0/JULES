// x86-64 ILP instruction-selection tier (pass 84, the "sniper" — the second
// tier of the two-tier selector design in docs/backend_architecture.md).
//
// The DP (x64_dp_isel) is the workhorse: per-root memoized, single-use
// gated, therefore exact over its own decision space but blind to decisions
// that only pay JOINTLY. This tier is the exact solver for hot regions
// (inner-loop bodies under an instruction budget) — it runs AFTER the DP
// cover (the feasible seed) and only ever commits assignments that improve
// the shared objective:
//
//     objective = latency cost + lambda * peak live values
//
// The constraint families (the plan's list):
//   * data dependency: chain consumption follows single-use in-block pure
//     chains exactly as the DP gates them;
//   * register structure: Magic forms use the rdx:rax pair and are
//     root-only (never chain operands) — the covering model encodes that
//     by never offering Magic nodes as absorb sources;
//   * mutual-exclusion coverage: a shared node either materializes once
//     for all its readers or dies only when EVERY reader consumes it
//     inline — the multi-use index-term "death groups".
//
// Decisions (v1 — the ones the current rule set actually has slack in):
//   1. DEATH GROUPS: a multi-use Mul(i,2^k)/Shl(i,k) whose every use is an
//      address Add already claimed as a Lea2/LeaRR root — the claim
//      extracts i directly into the SIB scale, so the multiplier's store is
//      dead code today. When every reader is such a claim, the node is
//      suppressed: strictly better in cost AND pressure (the DP cannot see
//      this — it is an all-users covering constraint, NP-hard in general).
//   2. ABSORB-FLIPS: an absorbed single-use chain costs the region one
//      materialization less (+4 latency units) but keeps the chain's leaf
//      values live until the consuming root's position. Under the pressure
//      term (lambda), a flip to "materialize at its own position" wins when
//      the leaf-live-range extension crosses enough other definitions.
//      The lambda default is calibrated small (conservative: the RA-round
//      lesson — an exact optimizer amplifies objective-model errors).
//
// Region qualification: natural-loop depth >= 1, block < kBudget SoN nodes,
// at least one decision candidate. Scheduling freedom (emission-order
// choice for independent roots, with the fixed-register pair conflicts it
// exposes) is the documented next increment — coverage stays decoupled
// from scheduling per the LLVM-comparison decision.
//
// Telemetry: JULES_ILP_STATS=1 (regions/groups/flips per function);
// JULES_ILP_TRACE=1 (per-region decision log). JULES_ILP_ISEL=0 disables
// the tier (the DP cover stands, exactly as before this tier existed);
// JULES_ILP_LAMBDA overrides the pressure weight for bench studies.
#pragma once

#include "core/codegen/linear.h"

namespace jules {

class DpIsel;

class IlpIsel {
public:
    IlpIsel(LFunction& lf, FunctionGraph& fg);

    // After DpIsel::plan_block for this block: qualify the region, solve,
    // commit through the DP's action/cell state. No-op (and cheap) when the
    // block does not qualify.
    void refine_block(const LBlock& b, const FlatMap<NodeId, bool>& suppressed,
                      DpIsel& dp);

    u32 regions() const { return regions_; } // qualifying regions solved
    u32 groups() const { return groups_; }    // death groups committed
    u32 flips() const { return flips_; }      // absorb->materialize flips

    static constexpr size_t kBudget = 50; // SoN nodes per the plan

private:
    LFunction& lf_;
    Graph& g_;
    std::vector<i32> loop_depth_; // per LBlock index (natural-loop nesting)
    std::vector<i32> block_of_;   // node -> block index
    u32 regions_ = 0;            // qualifying regions solved
    u32 groups_ = 0;              // death groups committed
    u32 flips_ = 0;               // absorb->materialize flips

    void compute_loops();

    bool enabled() const;
};

} // namespace jules
