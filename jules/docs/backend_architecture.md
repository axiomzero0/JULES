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
  |       (x64_dp_isel.cpp DP-on-DAG cover layered over the x64_emit.cpp
  |        hand emitters, which stay the per-node fallback; opt-in ILP
  |        sniper is the evolution path)
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

## Instruction selection (the DP workhorse + the ILP sniper are live)

The pass-84 selector is now the planned two-tier structure's workhorse:
a **DP-on-DAG cover** (Burton/Twig-class) lives in
`src/targets/x86_64/x64_dp_isel.{h,cpp}`, layered over the hand emitters
(`x64_emit.cpp`), which remain the fallback for every node without a
claimed rule — selection quality never regresses when a pattern is
missing, because the DP only claims nodes it covers strictly cheaper
(latency-class cost units: slot load 3 / store 1, lea/arith/shl 1,
imul 3; ties break on fewer instructions, then lower rule id).

Per block the planner runs two passes: Pass A claims Load/Store address
chains (always a win — the address arithmetic never takes the
store-then-load round trip through frame slots; address ptrcasts are
unwrapped as machine-level identities), Pass B claims Bin nodes the
rule set covers strictly cheaper than the hand emitter. Claimed roots
are emitted from committed cells; chain nodes consumed inline are
suppressed; everything else flows to the hand emitters unchanged.

The rule set: full-SIB `Lea2` (base + index*scale + disp — the one MIR
opcode the DP adds; the index register rides in `b.slot`, consumers are
op-gated), baseless `LeaRR`, `ShlImm` (Mul by 2^k), mul-by-3/5/9 as
`lea x + x*scale`, and generic Arith with an inline b-side chain.
Scratch discipline: every form computes into {dst, other} with at most
one nested chain operand, so nested chains cannot clobber each other.

Telemetry (JULES_DP_STATS=1): per-function claimed roots and folded
chain nodes. On the 2D-indexing stress test: 24 full-SIB leas where the
previous pipeline had 0, and the old compiler's 914-line asm drops to
695 lines for the same computation.

**The bugs the selector's tests flushed out** (kept here as the lessons —
the second round of the same pattern: new optimizer tier + stress shapes
the old tests didn't have):

*Round 1 (the DP round):* writing the selector's test program — fill,
RMW loop, 2D sum — produced a wrong answer at -O2 that predated the DP
entirely. Root cause: pass 42's body cloner could not copy a body
containing an inner loop. The clone order deadlocked on the
header-Region/latch cycle, the safety-append emitted blocks in an order
where the Region's backedge `remap()` fell through to the ORIGINAL node,
and the cloned inner loops silently lost their backedges — the bodies
died, leaving empty cmp/jge guard shells, so the outer loop's unrolled
copies summed only one row in four (3066 vs 12282, regression-locked by
t_nested_unroll). Fixed in `loop_transforms.cpp`: body_order is
cycle-aware (a Region's backedge preds — reachable from the Region
itself — do not block placement),
remap misses on in-body nodes are deferred and patched after each
copy's sweep, an ordering gate refuses to transform bodies that cannot
be linearized (before any mutation), and pass 42 unrolls innermost
loops only (outer unrolling duplicates whole inner loops — bloat with
no ILP below the inner loop's own unroll).

*Round 2 (the ILP/magic round — five more, all fixed at the mechanism,
regression-locked by t_magic_div / t_ilp_isel):*

1. **Inliner, straight-line callees** (pre-existing): a callee whose
   body pins directly to `start` (no Regions) was cloned onto a
   synthetic `Jump` node — a block the linearizer must place AFTER the
   caller's block — while consumers could stay AT the caller's block:
   use-before-def across blocks (an inlined `return x / d` helper
   printed garbage). Fix: straight-line callees pin to the call site's
   own region; no new control at all.
2. **Inliner, the `before` closure crossed phi backedges** (pre-existing):
   the backward closure over the call's inputs followed the loop
   memory-phi's BACKEDGE input, classifying post-call consumers as
   "before the call" and stranding them at the original block (same
   symptom, control-flow-callee variant). Fix: Phi value inputs are
   not followed — phis are block-owned, their entry-edge suppliers
   live above the loop, their backedge suppliers are same-block
   post-call nodes.
3. **Branch-fusion's copy-chain scan checked writes before reads**:
   destructive two-operand ops (`xor rax, rcx`) read AND write their
   destination, so the scan classified them as "redefinitions" and the
   fold deleted a load the destructive consumer still read. Fix:
   reads-first classification (with the fixed-register implicit reads
   of IDiv/UDiv/MulHi/Cqo/SExt32 and partial-register reads of
   MovZX/AL included).
4. **The RA's GP operand fold killed multi-use scratch movs**: folding
   [mov A<-B] into the first consumer deleted the mov while later
   consumers still referenced A. Fix: a multi-use guard — the mov's
   destination must have no other reader before its next plain
   redefinition (control boundaries end the window conservatively).
5. **The vectorizer's reduction-phi backedge at packs==1** (pre-existing,
   -Os/-Oz only): `append_input(vaccA, two_acc ? updA : updB)` appended
   updB — never set when the size-biased levels keep one pack and one
   accumulator — so the vector phi's backedge was kNoNode and the graph
   was corrupt from pass 56 onward (a downstream assert, not a
   miscompile). Fix: the 1-acc backedge is acc_next, the chain's last
   update.

The meta-lesson from both rounds stands: the pattern that finds these is
"new optimizer tier + stress shapes the old tests didn't have". Two of
round 2's finds (1, 2) were found by simply CALLING a tiny division
helper from a loop — the shape every real program has and the suite
somehow didn't. The MIR dump hooks added this round (JULES_MIR_DUMP:
post-isel / post-promotion / post-gp-fold / post-fuse / p86-entry /
p86-exit / pre-peephole) are what made the fold bugs bisectable in
minutes instead of hours; keep them.

1. **DP on DAGs (the workhorse)**: shipped (above).
2. **The ILP sniper**: SHIPPED (`src/targets/x86_64/x64_ilp_isel.{h,cpp}`, runs
   after the DP cover per block). Qualifying regions: natural-loop depth
   >= 1 (dominator-computed from the LBlock CFG), block < 50 SoN nodes,
   at least one joint decision. The DP cover is the feasible seed; the
   solver only commits strictly-improving assignments, so regions it
   declines are exactly the DP's answer (JULES_ILP_ISEL=0 restores the
   pre-tier behavior; JULES_ILP_STATS/TRACE for telemetry).

   Decision families (v1):
   * **DEATH GROUPS** — the flagship. A multi-use index term
     (Mul(i,2^k)/Shl(i,k)) whose users are all in-block address Adds:
     per-root, the DP reads the mul's materialized slot (the Arith form
     ties Lea2 on cost and wins the rank tie), so the mul materializes
     for everyone — but if EVERY reader takes the Lea2 form (raw index
     into the SIB scale), the mul is dead. That is an all-readers
     covering constraint, invisible per-root: the exact Bruno–Sethi
     freedom the workhorse cannot see. Commit = rewrite each reader's
     cell to Lea2 + suppress the mul (joint cost gate in the DP's own
     latency units; strictly better or no commit). On the two-array
     shape (`r[i] + s[i]` in a loop) the body's scale multiply and its
     round trip disappear: `leaq 0(%rcx,%rax,8), %rcx` where the DP
     kept `leaq 0(,%rbx,8); mov; add`.
   * **ABSORB-FLIPS** — the pressure term: an absorbed single-use chain
     saves one materialization (+4 latency units) but keeps its leaf
     values live until the consuming root's position; under
     objective = latency + lambda*peak-live (lambda = 1, calibrated
     conservatively per the RA-round lesson), a flip to "materialize at
     its own position" wins when the leaf-live extension crosses enough
     other definitions. Exact subset enumeration (<= 2^10 flips, the DP
     seed is the empty set), deterministic tie-breaks.

   The constraint families per the plan: data dependency (chain
   consumption follows the DP's single-use in-block gates), register
   structure (Magic forms use the rdx:rax pair and are root-only —
   never absorb sources), mutual-exclusion coverage (a shared node
   materializes once for all readers or dies only when EVERY reader
   consumes it inline). Emission-order scheduling for independent
   roots — with the fixed-register pair conflicts it would expose —
   stays decoupled from coverage per the LLVM-comparison decision and
   is the documented next increment.

3. **The division family** (this round's rule payload): Div/Mod by a
   non-pow2 CONSTANT is selected as a Granlund–Montgomery magic-number
   sequence (both the fitting form and the increment form for M >= 2^64;
   signed dividends go through an abs-wrapper so one u64 mulhi path
   serves everything; Mod recomputes r = x - q*d). The math is verified
   by jules/scripts/verify_magic_math.py (boundary sweeps at every multiple
   of d, 2^k*d, randomized — 200 unsigned + signed divisors). The high
   half comes from the MulHi MIR opcode (rdx:rax fixed pair, the same
   register shape as IDiv — wired through every RA/peephole mask). Cost
   model: idiv's latency class is 30 units (vs imul 3), which is what
   makes the claims decisive: `x / 7` goes from cqo+idiv (~30 cycles)
   to a 6-instruction mulhi chain (~8). Pow2 divisors are IR-level
   (pass 12): shifts/masks with the signed round-toward-zero bias
   forms — target-neutral, so every future architecture inherits
   them; the magic rules are the x86 per-arch layer.

   The sequences are fold-proof by construction: the mulhi high half
   STAYS in rdx (mul's implicit destination) — no value rides a scratch
   register across two consumers, so the RA/peephole operand folds
   (which model per-consumer scratch movs) cannot break them; the GP
   operand fold's new multi-use guard is the belt to these suspenders.

The RA contract above is the prerequisite: both tiers emit into the same
frame-slot + scratch-register MIR, so the allocator core does not change
when the selectors do.


## Pass 92 — The superoptimizer (the search tier)

`src/superopt/` (engine, target-agnostic) + `src/targets/x86_64/x64_super_isa.cpp`
(ISA descriptor table). The third machine tier, -O3 only, running after 88
on the final MIR. Where the DP selector (84) covers patterns the rule set
names and the ILP tier refines hot regions within a fixed decision space,
the search tier answers the question neither can: **is there ANY cheaper
instruction sequence — from the ISA table's alphabet, built from the
window's own operands and constants — that is equivalent on the window's
live-out contract?**

The division of knowledge is the point:

- The engine knows search mechanics only: regions, liveness, definedness,
  budgets, vector lanes, a priority queue. Zero opcodes, zero patterns,
  zero latencies.
- The ISA table is the single source of instruction semantics, as data:
  one row per IOp with exact per-lane `simulate` closures (32-bit
  zero-extension, partial slot writes, shift-count masking, undefined-flag
  poison), liveness `effects` (destructive destinations read; count-0
  shifts pass flags through; LeaSlot's slot is a USE), and latencies
  referenced from DpIsel's classes. A second target is a new table, not a
  new engine.

Findings from the founding rounds (all fixed at the mechanism, all
regression-locked by the suite): the state-dedup-keeps-first-program bug
(lazy Dijkstra), the operand-bind wipe that zeroed every lea displacement
and forced every Lea2 index to rax, the frame-register escape (`movq %rsp,
%rbp` judged dead because the model's slots don't read rbp — rbp/rsp are
now unsearchable by validity), the destructive-read liveness hole (the
same reads-first lesson as the pass-87 round, paid again), the
call-adjacent window's 8-register pool explosion (candidate registers =
the original's read+write set), and the never-fires-condition vector hole
(constant-sweep lanes). The permanent lesson is institutionalized in
`docs/agent_review_rule.md`: every change ships with an independent agent
review; the first one caught two more latent soundness holes (imul/shift
flag-truth, LeaSlot escape) after self-review and the suite were green.

Honest limits (README has the full list): equivalence is sampled (12
search + 84 commit lanes — the table is the substrate for a future
SMT/BMC exhaustive verifier), the search is budget-incomplete by design
(deterministic pop/candidate/state caps; missed wins acceptable, wrong
code not), and the goal-first pop ordering trades minimality for
reachability (the winner is strictly better than the original, not
necessarily minimal).
