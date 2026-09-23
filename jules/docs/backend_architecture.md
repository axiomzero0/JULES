# Backend Architecture: Shared Allocation Core, Per-Architecture Selection

The backend is split so that every algorithm that does not depend on
machine encoding is written once, and each machine-code architecture
contributes only the parts that do.

```
SoN (target-independent IR)
  |  pass 83 CFG linearization
  v
Linear MIR  ---------------------------- target-neutral
  |  pass 84 instruction selection       <-- PER ARCHITECTURE
  |       (x64_emit.cpp today; the DP-on-DAGs selector + opt-in ILP
  |        sniper below is the evolution path)
  v
Post-isel MIR (every value in a frame slot; scratch-register isel
contract: operands load into rax/rcx/xmm0/xmm1, results store back)
  |  pass 85 register allocation:
  |      x64_ra.cpp builds a RaProblem (live ranges, exact generation
  |      sub-intervals, loop facts, bank facts from the machine target
  |      description) and applies the RaSolution (operand rewriting,
  |      callee-saved prologue, frame layout, slot coloring)
  |        |
  |        v
  |    core/codegen/ralloc.{h,cpp} + ralloc_flow.cpp   <-- WRITTEN ONCE,
  |    the hybrid allocator core                          shared by every
  |                                                      architecture
  v
passes 86-88 machine cleanup / LICM / rotation
  |
  v
elf_writer / serializer                  (per-architecture encoding)
```

## The contract

`src/core/codegen/ralloc.h` defines the target-neutral problem/solution
types (`RaProblem`, `RaVReg`, `RaBank`, `RaClass`, `RaMove`, `RaSolution`)
and the entry point `ralloc_hybrid()`. `src/core/codegen/target.h` defines
`MachineTarget` — the register-file description (classes, banks,
callee-savedness) — plus the registry the driver's `--target=` flag
validates against. A new architecture implements:

1. instruction selection into its MIR (the pass-84 seam),
2. a `MachineTarget` subclass with its register facts,
3. an RA input builder + solution applier (the x64_ra.cpp role).

The allocator core never sees an x86 header.

## Phase 1: exact spill selection (network flow)

Which live ranges keep registers and which are spilled is decided
globally. Each candidate range contributes an occupancy span on the
program-point segment chain; the constraint "at most K ranges overlap at
any point" is a consecutive-ones (interval) program — totally unimodular,
so its differenced min-cost circulation relaxation has an integral
optimum that is the true optimum. The flow network:

- nodes: compressed segment boundaries (span endpoints + capacity
  changes; interior slack flow is constant between them),
- slack arcs `(p+1 -> p)` with capacity `cap[p]`: a register sitting idle,
- span arcs `(end_v -> start_v)` with capacity 1 and cost `-weight_v`:
  a register holding value v,
- supplies: `cap[q-1] - cap[q]` at boundary nodes (capacity deltas),

solved by successive shortest augmenting paths (SPFA; the initial
network is acyclic right-to-left, so shortest paths exist and no negative
cycle appears along the SSP sequence). A span is selected iff its arc
carries flow. Because the selection is integral and respects capacity
pointwise, the selected set is guaranteed colorable — Phase 2 never needs
to invent a spill. Bank eligibility (call-crossing ranges need
callee-saved banks) is handled by staging: crossing ranges flow under
the callee-bank capacity, non-crossing under the residual `K - x_p`, a
greedy repair adds rejected crossing ranges back where both banks have
room.

Validation: `scripts/ra_flow_selftest.cpp` compares the flow against
exhaustive subset enumeration on 20,003 random and adversarial instances
(0 mismatches), including the case greedy-by-weight loses: two light
chained ranges that reuse one register beating one heavy overlapping
range.

## Phase 2: assignment + iterated coalescing

Bank-aware interval assignment by left endpoint with the handover-touch
rule (a range ending exactly where a def-starting range begins frees the
register — the two-operand read-modify-write shape). Then iterated
conservative move coalescing at generation (sub-interval) precision: a
move pair unifies when their exact live sub-intervals are disjoint or
touch only at handover boundaries, the bank survives the move, and no
third range on the destination register overlaps the mover's hull.
This is the George-Appel IRC coalescing repair applied on top of a
pre-solved coloring — a merge can never make the assignment uncolorable,
which is what the simplify/freeze stack buys in textbook IRC.

## Weights (the cost model)

Spill cost = sum over uses of `10 x 2^loop_depth(use)`, multiplied by 16
when the value is loop-carried (live across a backedge), computed exactly
at block level:

- loop-carried: `in live_out` of some backedge latch block (the block
  liveness fixpoint already produces this),
- loop depth: natural-loop membership count (backward reachability from
  the latch stopping at the header).

The first implementation used linear-stream approximations for both and
inverted the spill ranking on mandelbrot (details in the pass-85 session
note): every in-body temp was marked loop-carried, so the x16
amplification stopped discriminating, and the intra-iteration `mag^2`
temp outscored the loop-carried recurrence input `zx0`. The flow spilled
"optimally" per its inputs — the wrong value — and the spilled reload
sat on the recurrence's critical path while the old code's round trip
hid behind the escape branch's latency slack. Lesson: an exact optimizer
amplifies weight-model errors; a heuristic smears them.

## Instruction selection (the next tier)

The current selector is a one-node-at-a-time template expander. The
evolution path, per the design:

1. **DP on DAGs (the workhorse)**: per-basic-block tree/DAG cover by
   dynamic programming over machine patterns (Burton/Twig-class), O(N)
   per block, optimal local covers, native handling of complex
   addressing modes; runs at every -O level.
2. **Custom ILP (the sniper)**: for profiled-hot or high-pressure
   regions only (inner loops below an instruction budget), a compact
   ILP over pattern choice + scheduling + bank assignment with
   dependency, register-constraint, and mutual-exclusion structure —
   millisecond solves on small regions, global vision where it pays.

The RA contract above is the prerequisite: both tiers emit into the same
frame-slot + scratch-register MIR, so the allocator core does not change
when the selectors do.
