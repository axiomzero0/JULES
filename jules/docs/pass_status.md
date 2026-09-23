# 91-Pass Status Matrix

Audit (2025-09 session): all catalog passes are registered
(`--list-passes`: 91 rows), scheduled by the catalog-order pass manager
with mode/level/kill-switch gating, and emit telemetry (`--stats`
per-pass `changes` / node deltas; `--only`/`--disable` isolate any pass).
Pass-activity is regression-locked: tools/test_runner.sh asserts the pass
actually transformed on programs written to exercise it (GVN, SCCP, SROA,
inlining, TCO, LICM, unrolling, predication, LOOP VECTORIZATION, SLP).
Reproduce the audit: `./build/julesc --list-passes`,
`./build/julesc --stats <file>`, `./tools/test_runner.sh`.

This session (the hybrid register allocator round, 2026-09-23 third
round) replaced the linear scan with the two-phase hybrid per the backend
split: the register allocation ALGORITHM is now written once, target-
neutral, in src/core/codegen/ralloc.{h,cpp} + ralloc_flow.cpp; each
architecture contributes only instruction selection plus a register-file
description (core/codegen/target.{h,cpp} registry; targets/x86_64/
x64_target.{h,cpp} is the first; driver `--target=` validates against it).
Phase 1 is an EXACT min-cost spill selection by network flow: the
consecutive-ones segment program (one capacity inequality per program
point) is totally unimodular, so its differenced min-cost circulation
(slack arcs = idle registers, span arcs = busy registers at -weight,
supplies = capacity deltas, successive-shortest-path augmentation) has an
INTEGRAL optimum that IS the optimal spill set — validated against
exhaustive brute force on 20,003 random + adversarial instances
(scripts/ra_flow_selftest.cpp: 0 mismatches), including the greedy-swap
trap (two light chained ranges beating one heavy overlapping range). Two
staged flow problems per class (call-crossing ranges under the callee
bank capacity, then non-crossing under the residual), a greedy repair,
bank-aware interval assignment with the same handover-touch semantics,
and iterated conservative coalescing at generation precision (the
George-Appel IRC repair on a pre-solved coloring — merges can never
uncolor the assignment). Weights: uses x 10 x 2^loop-depth, x16 for
loop-carried values. The first cut had TWO weight-model defects that the
mandel benchmark caught as an 11% regression and a controlled asm A/B
isolated: (a) loop depth from LINEAR backedge containment interleaves on
rotated layouts (inner latch emitted after an outer latch) and
undercounted nesting; (b) the loop-carried flag was the old hull
approximation `first < bot && last > top` — which marks EVERY in-body
temp as loop-carried, so the x16 multiplier stopped discriminating and
INVERTED the spill ranking (the intra-iteration mag^2 temp outscored the
loop-carried recurrence input zx0; the flow — optimally, per its inputs —
spilled the wrong one, and the spilled reload sat on the recurrence's
critical path while the old code's round trip had slack behind the
escape branch). Both now computed exactly at BLOCK level: loop-carried =
live_out at a backedge latch (the liveness fixpoint already produced it),
depth = natural-loop membership count (backward reachability from the
latch stopping at the header). mandel restored to 1.08x; 8-kernel geomean
0.92x (O2) / 0.90x (O3); suite 343/343. Machine-level: FpCmp A-side
operand folding is naturally blocked by the spilled reload — the compare
keeps the const-to-scratch copy + ucomisd-through-scratch shape that
measured 11% faster on this Xeon than the direct const-pool-register
form (isolated by hand-patched asm variants; kept, not fought).

This session (the PE range round, 2026-09-23 second round) widened the
assumption lattice of the PE/deopt family from single constants to
RANGES — the Const -> Range -> Dynamic widening of the partial-deop
design. The argument sketch grew from [first, total, match] to
[first, total, match, min, max] (x64: two more conditional counter
updates per sketch site); a not-sticky 64-bit integer parameter whose
observed hull fits the level budget now gets a RANGE rung: guard
(p >= lo) and (p <= hi) — two nested signed compares — with the param
KEPT as a runtime argument in the variant; interval knowledge folds
every branch no value in [lo, hi] can take (bounds checks, saturating
clamps) while the computation stays live. The fold engine's lattice
gained the Range rung with overflow-checked hull transfers and
endpoint-based compare folding, plus a SOUND WIDENING CAP: a loop-
carried phi's hull grows without fixpoint below the span cap, and a
truncated drain would leave stale Const conditions on its users (the
t24 miscompile class found by the suite); hulls may grow at most
twice before collapsing to Bottom — the classic SCCP jump. Because a
real profile's hull covers every observed value, the range-deopt path
is regression-locked by an ADVERSARIAL PROFILE (t42:
scripts/t42_shrink_hull.py shrinks the recorded hulls below reality;
the use build must transfer the out-of-hull calls down the ladder
with byte-identical output). Suite 343/343; 8-kernel geomean vs
gcc -O3 unchanged (0.93x O2 / 0.91x O3, all outputs verified).

This session (the recursion round, 2026-09-22 evening) took the 8-kernel
geomean vs gcc -O3 from 1.34x to **0.93x (O2) / 0.90x (O3)** — the suite
is now faster than gcc -O3 overall. Four changes (details in
bench/README.md): (1) p50 SPINE-TAIL BASE WIDENING — when f(k) folds to
constants for k in [0..K] and the recursion is structurally descending
(g2 = x - c, c >= 1) with a constant-bounded base (d <= K+1), the
accumulator loop exits at w <= K through the folded table; every
f(k <= K) subtree collapses to a constant (fib: 2.09x -> 0.17x, the
executed-call count drops ~75x; the widened head stays one signed
compare and negative arguments keep the original B(w) default — the
structural gates exist because naive widening miscompiles ascending
spines and bases above the table; locked by t39_widenbase). (2) p56
VECTOR-LOOP UNROLL x2 (perf levels): two packs per iteration, integer
Add reductions split into two accumulators merged lane-wise at the exit
(vecsum 1.57x -> 1.11x). (3) p87 gained CFG REGISTER LIVENESS (labels +
jump edges, u32 bitmasks; reads over-approximated, writes only where
certain — sound for "is D read later" consumers) and with it OPERAND-
COPY ELIMINATION: a promoted movaps feeding a two-operand SSE op is
deleted with the op redirected to the copy's source. (4) p61 CLAMP-TO-
ZERO MIN/MAX: Select(Gt(t,Z), t, Z) -> one Max(Z, t) emitted as
`maxpd dst=t src=Z` — bitwise-exact including NaN and -0.0 lanes
(hardware-verified); the Ge/Le forms are REJECTED (select yields -0.0
where the instruction yields +0.0). vecmask: 1.22x -> 1.05x (the same
movups/maxpd/movups sequence gcc emits). Suite 313/313.

This session (the vector-benchmark round, 2026-09-22) made the suite
measure the SIMD family: two new kernels (vecsum i64 reduction, vecmask
f64 masked blend), each verified byte-identical vs gcc before timing.
First honest numbers: vecsum 9.60x / vecmask 2.45x vs gcc -O3. FOUR
fixes followed (details in bench/README.md): (1) RA packed-chain fusion
— the accumulator fuse + FP operand fold only knew FpBin/FpNeg, never
VecBin*/VecLogical/VecCmp*/VecBcast (same xmm0-dst contract), leaving
four movaps copies around every paddq; (2) p61 degenerate blends —
Select(m,t,ZERO) is bitwise And(m,t) (one packed op instead of three;
the zero broadcast disappears); (3) p56's vector trip count nv=bound/
lanes was pinned at the loop HEAD — an idiv every vector iteration,
the top of the 9.6x gap — now pinned at the entry edge; (4) TWO
miscompile-class RA bugs exposed by (3): the pair-fold and the
single-use home-store forwarding delete a value's slot round-trip and
leave it in the UNTRACKED isel scratch (rax) across a loop body, where
address chains write rax unconditionally — both now carry backedge-
straddle guards (store outside a loop + reload/consumer at/after its
head keeps the round-trip; t24/t32/t38 segfaulted at -O2+ before).
After: vecsum 1.57x / vecmask 1.22x vs gcc -O3 (6.1x / 2.1x speedups
on the kernels themselves); 8-kernel geomean 1.34x; suite 304/304.

This session (the registration-honesty audit, 2026-09-18) verified the
matrix from the CODE side and fixed what it found. REGISTRATION: every
catalog pass registers exactly once (JULES_REGISTER_PASS, orders 1-89,
89 files, all compiled per sources.cmake; `--list-passes` = 89 rows;
static-init order safe via the function-local registry singleton,
first-registrant-wins dedup by order). The checked-in binary was STALE
(sept-04 build of pre-indexing sources: it rejected `p[i]`, so 110/301
suite checks failed); rebuilt from current source, the full suite passes
301/301 — the matrix's claims are all reproducible on a fresh build.
KILL-SWITCH BYPASS (real honesty gap, now fixed): the post-inline cleanup
re-run invoked run_one directly, skipping should_skip — `--disable Name`
was silently ignored there, so a disabled pass RAN AGAIN in the cleanup
sweep (repro: `--disable ScalarReplacementOfAggregates` on t04 → main
run skipped, cleanup ran with changes=17; 14 passes affected: orders 26
30 23 24 1 2 3 7 8 9 44 42 54 58). The re-run now applies the same
should_skip gates, and assert_pass_disabled regression-locks SROA, GVN,
and StoreMerging staying dead under --disable (suite: 304/304).
STATUS-LABEL DRIFT (5 rows relabeled): p27/p36/p63/p69/p70 said VACUOUS
("constructs absent") but their files self-identify as SCAFFOLD ("not
built") — both no-ops today, but the code header's label is the honest
one, and the doc now uses it. p28 stays IMPLEMENTED but annotated
(delegated): the pass file is a declared no-op, the feature lives in
p85's finalize() slot coloring (x64_ra.cpp, lf.ra_colored). TELEMETRY:
p51 BranchProbabilityInference returned true whenever If nodes existed
(analysis-only pass claiming transformation; --stats showed changes=1 on
every branchy program) — now returns false, the same contract as p19/p20.
The audit tooling lives on: scripts/audit_honesty_matrix.py re-derives
this section's claimed-vs-detected table from docs/pass_status.md +
src/core/son/passes/*.cpp (claimed-vs-code cross-check, one row per pass).

This session (pass-reality audit) also hardened the pipeline underneath
the new vector passes — real bugs found by the new tests, all fixed with
regressions: (1) p83 scheduled loads AFTER effects sharing their memory
version (loads read freed memory; t23), (2) SCCP's pred-trim compacted a
region's predecessor list before realigning its phis (misaligned phis;
t24), (3) the unroller's cloner held `const Node&` across vector growth
(use-after-free; ASAN), (4) the unroller's copy-seed chain remapped clone
ids that the next copy's original-keyed map could not see (4x-unrolled
bodies duplicated their third lane; t25-style print loops), (5) the isel
FP const pool served stale XMM constants across sibling regions that
never executed the materialization (dominator-checked reuse now), (6)
int constants reaching XMM consumers were treated as FP (broadcast of
zero), (7) SROA typed pointer-slot value phis as the pointee — the type
lie collapsed at phi folds. The language grew `alloc(T, n)` + `p[i]`
indexing so the vectorization family has real memory traffic to pack
(t23-t25).

This session (the audit-deepening round) converted two more scaffolds
into real machinery — p58 interleaved access recognition (stride-2
element pairs with a shared SYMBOLIC index pack into one shuffle-free
128-bit access) and p47 loop fission (counted loops carrying two
independent reduction clusters split per cluster; the split halves are
the single-reduction shape p56 requires, so dynamic-trip two-reduction
loops vectorize after the split — fission is the enabler) — and the
audit trail behind them fixed FIVE latent bugs, all regression-locked:
(1) BUG-19: SLP packed the (a[0],a[1]) load pair across the a[1] store
in `a[1]=a[0]+7; a[2]=a[1]+7` and read the stale lane (the doc claimed
a disjointness gate the code never had; t25's overlap() locks it, the
shared vecx::pack_pair_sound gate now proves load-version equivalence
and early-write visibility for both 54 and 58); (2) BUG-20: a call
chained on an alloc while ALSO passing it as an argument masked the
argument escape as a chain use — SROA then promoted a freed heap object
and killed the alloc under the live free call (t05 abort); (3) BUG-21:
DSE's "no remaining loads" scan only saw slot-shaped direct base loads,
so array stores that computed-address loads still read were killed
(t23); (4) BUG-22: the LINEARIZER dropped fallthrough successor edges
for blocks whose recorded terminator had no live projections (an empty
IfFalse block whose only user is the next block's Jump head; an
inlined-leftover If) — the register allocator's liveness dataflow lost
the path, values defined before a loop and read after it were not live
around the backedge, and the loop body redefined their registers
(always-inlined copy loops over loop-filled arrays segfaulted on the
stale base pointer); (5) BUG-23: the loop vectorizer broadcast the
loop-VARYING IV phi as a stored value (`a[i] = i` filled every element
with the same stale merged value — the header's "iv-derived stored
values are rejected" contract now actually holds for the raw-phi form).
The alias analysis grew the machinery the pair/vector passes needed:
the address-arithmetic grammar (ptrcast casts, base+offset adds) and
memory-phi inputs are recognized as non-escaping uses, free() is
lifetime-end rather than escape (p29's own classifier agrees), and
distinct allocation sites are NoAlias unconditionally (malloc
semantics; escapes matter for is_alloc_local's read-visibility
consumers, not for site disjointness). 54/58 run in the post-inline
cleanup sweep (inlining turns param-based MayAlias pair candidates
into distinct local allocations); pure_equiv compares the duplicated
symbolic index parts the first-sweep schedule leaves behind.
Fusion's old guard Cmp is now killed with its If (--verify hygiene,
t32).

This session (the pass-reality continuation round) converted five more
scaffolds into real machinery — p24 store merging, p45 loop interchange,
p46 loop fusion, p53 idiom recognition, p65 SIMD intrinsic matching —
and, building the test for p65, found that IF-CONVERSION (p49) had
never fired on any program (its arm-cleanliness check rejected the merge
region itself as "nested control" for the IfTrue-in-slot-0 shape every
if/else produces; also arm values defined before the branch — e.g. a
loaded array element — were wrongly required to be pure-movable).
Fixing it exposed three more latent isel bugs, all regression-locked by
t29: (a) integer/FP selects branched on stale flags instead of testing
the materialized bool (t15's diamond always took the true arm), (b)
scalar f32 stores printed `movl %xmm` (un-assemblable; first scalar-f32
store test in the suite), (c) FP relational compares were NaN-wrong
(ucomis+B/BE treats unordered as less; now the swapped-operand A/AE
forms, with parity-qualified eq/ne via setcc pairs). Min/max operand
order is load-bearing for NaN (src operand returned on unordered) and
is never commuted.

This session (the PGO round) converted the last high-value scaffold —
p43 profile-guided unrolling — into the full PROFILE-GUIDED pipeline
(instrument -> run -> jules.prof -> use), the project's first
feedback-driven optimization loop. INSTRUMENT builds give every
counted-shape loop two counters (entry: pinned at the loop's entry
predecessor; header: at the guard's body projection) as
Call{kFnPgoBump} effects chained into the memory phi (no version
forks; self-latching phis are read, never rewired — rewiring would
cycle); the isel lowers each to one `incq jules_pgo_counters+idx*8(%rip)`,
and an .init_array constructor registers an atexit dump that writes the
raw counters to jules.prof at process exit. Instrumented functions are
marked no-inline (the inliner's clone is demand-driven from the
Return — dangling bumps would be dropped and counts would under-report;
the same policy pass 56 applies to vectorized functions; trip counts
are inlining-invariant, and counter indices are assigned at p43's slot
BEFORE the inliner runs, so instrument and use builds derive the
identical enumeration). USE builds read the profile in the driver,
pick the factor from the profiled average trip (largest power of two
<= min(avg, level budget); loops that never ran, or average < 2, are
skipped), and split dynamic-trip loops into the GUARDED-EPILOGUE form:
the F-unrolled main loop guarded by the LAST COPY's test
(`iv + (F-1)*step <rel> bound` — the unrolled body only runs when all
F scalar iterations would have; correctness NEVER depends on the
profile, only the factor does) plus a fresh scalar epilogue entered
from the main exit; post-loop readers retarget to the epilogue's
phis/exit. Writing the Le-relation test the new matcher needed found
BUG-24: the unroller's trip formula for `<=` counted loops was off by
two iterations (span = bound-init-1 instead of bound-init+step) —
every const-trip `while i <= n` loop that took the exact-unroll path
executed its body two iterations past the bound (sumle(9) returned 66,
not 45; the suite only ever tested `<` loops, and t04's shape happened
to fall into the peel path, whose re-match fails on the non-const
post-peel entry — masked, not fixed). Regression-locked by t37
(Le at positive/zero/negative bounds, step-3 variant, all levels).
Round-trip locked by t36: the instrument binary's output must equal
the plain build's, the profile must be non-empty, the use build must
report ProfileGuidedUnrolling activity, the use binary must match, and
a malformed profile must warn and degrade to no-profile mode.

This session (the mask round) converted p61 MaskGeneration — the last
scaffold in the vectorization family — into the MASKED-VECTORIZATION
pipeline: conditional packed values over per-lane active masks. The
chain runs end to end from SOURCE: an in-loop `if v > 0 { x = v } else
{ x = 0 }` diamond is SROA-promoted, if-converted (49) to a Select,
PACKED by the loop vectorizer (56 — new body classification for
Cmp/Select: the Cmp becomes a lane-mask compare with the result typed
as the same vector type, all-ones/all-zeros bits; every Cmp user must
be a body Select or the loop skips), lowered by THIS pass (61) into
Or(And(m,t), AndNot(m,f)) — the canonical SSE2 masked blend
(pand/pandn/por; blendv is SSE4.1) — and emitted as one cmppd/cmpps
ordered predicate (f64/f32 lanes) or a pcmpeqd/pcmpgtd composition
(i32 lanes; all-ones flips built in the dead operand register). The
soundness argument is bitwise end-to-end: ordered compares + a
lane-arithmetic-free blend reproduce the scalar NaN semantics exactly,
so strict-FP masked f64 loops vectorize (reductions keep their
--fp=fast gate, unchanged). i64-element masked loops skip honestly
(v2i64 has no compare below SSE4.2). The isel keeps a safety-net blend
emission so --disable 61 stays correct, and lowering at the SoN level
exposes the blend to GVN/DCE/peepholes. Writing the test the pass
needed found BUG-25, the 7th latent miscompile of the audit and the
2nd PRE-EXISTING in the committed baseline: the machine peephole's
cmp-$0 -> test fold preserved the compare's 32-bit size, but the Test
serializer always printed testq — a negative i32 (zero-extended by
its 32-bit load) tested positive as a 64-bit signed value, so `if
v > 0` went TRUE for v = -10 at every -O1+ build (t38's masked i32
loop sums returned the unmasked totals; no test program had combined
an i32 array, a `> 0` guard, and a select consumer before). The fix is
one line of width-respecting test emission; regression-locked by
t38 at every level. t38 also locks the honest skip: masked_i64's
loop never vectorizes and its result stays exact.

Legend: `IMPLEMENTED` — Real transform/analysis operating on the SoN graph or MIR.; `SIMPLIFIED` — Real but reduced: core mechanism present, documented reductions.; `VACUOUS` — Complete for the current IR: the constructs it targets do not exist in the MVP subset.; `SCAFFOLD` — Not yet implemented: contract, modes and telemetry in place; honest no-op.

Status roll-up: 68 IMPLEMENTED (1 delegated: 28), 4 SIMPLIFIED, 4 VACUOUS,
15 SCAFFOLD.

| # | Pass | Status | Notes |
|---|------|--------|-------|
| 1 | DeadNodeElimination | IMPLEMENTED | Remove unreachable nodes and unused pure ops. |
| 2 | IdentityCollapse | IMPLEMENTED | x+0, x*1, redundant casts. |
| 3 | ConstantFolding | IMPLEMENTED | Evaluate pure ops with constant operands. |
| 4 | TypeCanonicalization | IMPLEMENTED | Normalize widths, eliminate no-op conversions. |
| 5 | ComptimeResidueFold | IMPLEMENTED | Re-evaluate comptime expressions after specialization. |
| 6 | AssertElision | VACUOUS (nothing to do in MVP) | Remove checks proven safe (no check nodes in MVP IR). |
| 7 | PhiSimplification | IMPLEMENTED | Collapse single-input phis, identical inputs. |
| 8 | SparseConditionalConstantPropagation | IMPLEMENTED | Lattice const prop through control edges; Const lattice is pre-seeded (block-independent) and unresolved branches fall back to both-executable. Re-run-safe: an If whose dead projection a previous sweep already killed still marks its SURVIVING projection executable (bailing out starves downstream blocks and the unreachable-pred trim then decapitates reachable code — observed at real -O3 with inlined copies, fixed with regression t17). Fires in the post-inline cleanup re-run. |
| 9 | GlobalValueNumbering | IMPLEMENTED | Hash-cons identical computations (dominance-checked). |
| 10 | RedundantPhiElimination | IMPLEMENTED | Remove phis with identical/single definitions. |
| 11 | CopyPropagation | IMPLEMENTED | Replace uses of copies with originals. |
| 12 | AlgebraicSimplification | IMPLEMENTED | Multi-pattern algebra with side conditions. |
| 13 | Reassociation | IMPLEMENTED | Canonical operand order (FP gated by --fp=fast). |
| 14 | SignExtensionElimination | IMPLEMENTED | Remove redundant extends via width analysis. |
| 15 | NarrowingTransform | SCAFFOLD (honest no-op) | Demote ops to narrower types when consumers allow. Blocked on isel: the x64 backend emits per-width encodings keyed on the node type; retyping live IV arithmetic (i64->i32) mid-graph needs matching i32 arithmetic support in emit + RA operand folding. No 32-bit speed gain on x86-64 anyway (size-only win, -Os/-Oz). |
| 16 | BitwiseOptimization | IMPLEMENTED | Bit-level rewrites, bitfield extracts, De Morgan. |
| 17 | SelectOptimization | IMPLEMENTED | Select chains, condition inversion. |
| 18 | OverflowCheckElimination | VACUOUS (nothing to do in MVP) | Remove overflow checks via range lattices (no check nodes yet). |
| 19 | AliasAnalysisInitialization | IMPLEMENTED | Base/provenance AA: base_of resolves casts and phis to allocation sites; distinct ALLOCATION SITES are NoAlias unconditionally (malloc semantics — LLVM's malloc-based rule; escape matters for is_alloc_local's read-visibility consumers, not for site disjointness); the local-allocation escape walk understands the address-arithmetic grammar (ptrcast casts, base+offset adds), memory-phi inputs (loop headers threading the entry version), chained allocations, and free() as lifetime-end rather than escape (pass 29's own classifier agrees). Everything unresolved is MayAlias. |
| 20 | MemoryDependenceAnalysis | IMPLEMENTED | Compute store->load dependencies. |
| 21 | RedundantLoadElimination | IMPLEMENTED | Remove loads satisfied by dominating loads. |
| 22 | StoreToLoadForwarding | IMPLEMENTED | Forward store values to loads. |
| 23 | DeadStoreElimination | IMPLEMENTED | Remove overwritten/unread stores. Case 1 (local alloc, never read) scans for loads that MAY read the base through the full address grammar (base_of of every live load) — the slot-shape-only direct scan killed stores that array-indexed loads still read (found by the pass audit on t23). Case 2 is the alias-aware overwritten-before-read forward scan. |
| 24 | StoreMerging | IMPLEMENTED | Adjacent-in-chain 4-byte const stores to the same base with consecutive const indices merge into one 8-byte store of the combined bit pattern — gated on the pattern being reproducible from a sign-extended imm32 (`movq $imm, m64` has no imm64 form; the (0,0) zero-init pair and (c,0) shapes are the profitable cases, one instruction instead of two). Soundness: chain adjacency (no intervening effect) plus a scan that rejects any load of either element consuming the first store's version (it must observe the pre-store value). A load of either element consuming the first store's version is detected and rejects the merge. The isel emits the merged constant directly as an immediate store (movq $imm / movl $imm — constants never round-trip through a register). Runs in the main pipeline and the cleanup re-run (added to the sweep so post-unroll store pairs from pass 42 merge too). SLP (54) owns load/store-isomorphic pairs; this pass owns the constant pairs SLP cannot pack. Fires on t30. |
| 25 | LoadHoisting | IMPLEMENTED | Hoist provably-safe loads out of loops. |
| 26 | ScalarReplacementOfAggregates | IMPLEMENTED | Promote memory-backed locals to SSA values. Promotion also treats memory-phi users as chain uses (not escapes) and ignores dead users, so loop counters/temps promote even when earlier passes killed their consumers. |
| 27 | BitfieldLowering | SCAFFOLD (honest no-op) | The language has no bitfield types AND the mask/shift lowering mechanism is not built — the file self-identifies as scaffold. Relabeled from VACUOUS by the 2026-09-18 audit: "nothing to lower" was true today but overstated the pass's completeness; the code header's own label is the honest one. Contract stays for the day the type lattice grows. |
| 28 | StackSlotColoring | IMPLEMENTED (delegated) | The pass file itself is a declared no-op; the FEATURE is machine-level coloring in pass 85's finalize(): memory-resident slots whose exact live ranges (the same generation sub-intervals the register assignment used) are disjoint share one rbp offset; address-taken slots never share (distinct allocations keep distinct addresses); the frame size and offsets are derived from the color count. Telemetry: lf.ra_colored. (2026-09-18 audit: status annotated "delegated" — previously the bare IMPLEMENTED contradicted the file's own scaffold header.) |
| 29 | HeapToStackPromotion | IMPLEMENTED | Promote NoEscape allocations to stack slots (free sites become removable no-ops). A call chained on the alloc while ALSO passing it as an argument is an escape — the chain-use check masks the arg use if it short-circuits (the same masking fix as SROA's Call case). |
| 30 | MemorySSARepair | IMPLEMENTED | Restore memory chains after aggressive transforms. |
| 31 | PointsToAnalysis | IMPLEMENTED | Flow-insensitive points-to graph. |
| 32 | EscapeAnalysis | IMPLEMENTED | NoEscape/ArgEscape/GlobalEscape classification. |
| 33 | PartialEscapeAnalysis | SCAFFOLD (honest no-op) | Path-sensitive escape refinement (JIT). |
| 34 | AllocationSiteProfiling | SCAFFOLD (honest no-op) | Per-site escape behavior instrumentation (JIT). |
| 35 | MaterializationPointInsertion | SCAFFOLD (honest no-op) | Lazy materialization points for PEA (JIT). |
| 36 | LockElision | SCAFFOLD (honest no-op) | No sync/lock ops exist in the MVP language or IR — and the non-escaping-receiver elision mechanism is not built either (file self-identifies as scaffold; relabeled from VACUOUS by the 2026-09-18 audit). |
| 37 | LoopDetection | IMPLEMENTED | Natural loops via backedges; loop tree. |
| 38 | InductionVariableRecognition | IMPLEMENTED | Basic/derived IVs, trip counts. |
| 39 | LoopClassification | IMPLEMENTED | Counted/uncounted/early-exit/nested tagging. |
| 40 | LoopInvariantCodeMotion | IMPLEMENTED | Hoist invariant pure ops to preheaders. |
| 41 | LoadLICM | IMPLEMENTED | LICM for loads with alias proofs. |
| 42 | LoopUnrolling | IMPLEMENTED | Body-duplication unrolling for counted loops with compile-time-constant trip counts (shared cloner in passes/loop_transforms.cpp): match IV phi + Add(phi,+k) + const bound + no early exits; factor from the level budget (O2:4, O3:8); remainder peeled first when T%F!=0; F-1 copies chained behind the body (per-copy seeds remap every header phi to the previous copy's update so copy m sees iteration base+m); the header latch and phi backedges retarget to the last copy; IV steps F*k per unrolled iteration. Trip formula: Lt span = bound-init; Le span = bound-init+step (the inclusive bound — BUG-24, fixed with t37). Runs in the main pipeline and again in the post-inline cleanup (inlining propagates const bounds). Dynamic-trip loops are pass 43's (the guarded-epilogue form). Fires on t18_unroll/t37. | 2026-09-23 FIX: the body cloner could not copy a body containing an INNER LOOP — body_order deadlocked on the header-Region/latch cycle, the safety-append emitted blocks so backedge remaps fell through to ORIGINAL nodes, cloned inner loops lost their backedges (bodies died, empty cmp/jge shells remained) and nested-loop programs silently computed on a quarter of the iterations (t_nested_unroll: 3066 vs 12282; found via t_dp_lea2's RMW test). loop_transforms.cpp rewritten: cycle-aware body_order (a Region's backedge preds — reachable from the Region — do not block placement), DEFERRED BACKEDGE PATCHING (in-body remap misses are placeholders patched after each copy's sweep), an ordering gate that refuses non-linearizable bodies BEFORE any mutation, and pass 42 is INNERMOST-ONLY (outer unroll duplicates whole inner loops: bloat, no ILP below the inner loop's own unroll).
| 43 | ProfileGuidedUnrolling | IMPLEMENTED | The PGO pipeline's consumer. INSTRUMENT (`--pgo=instrument`): every counted-shape loop (const OR dynamic bound, canonicalized Lt/Le — the builder emits the reversed `Cmp gt(bound, iv)` form) gets two Call{kFnPgoBump} counter effects (entry pred + guard body projection, the rotation-stable pins; memory-phi-chained with a self-latch read to avoid cycles) lowering to one incq against a .bss table; an .init_array constructor registers an atexit fwrite of magic+count+counters to jules.prof. Counter pairs indexed in deterministic (fn, loop) order BEFORE mutation (identical enumeration in both modes); instrumented fns marked no-inline (the inliner's Return-demand clone would drop dangling bumps — under-counting); const-trip unroll/peel duplicates bumps but totals stay = trip; every other loop transform rejects call-carrying loops, so trip structure is preserved. USE (`--pgo=use=<f>`): factor = largest pow2 <= min(profiled avg trip H/E, level budget); E=0/H=0/avg<2 loops skipped. The transform: guarded-epilogue split of DYNAMIC-trip loops — F-unrolled main loop with the LAST-COPY guard `iv+(F-1)*step <rel> bound` (sound for every runtime trip; the profile only picks F) + a fresh scalar epilogue (p47-style fresh loop: Region/guard/projections, phis taking the main's phis as entries, body deep-cloned with phi remap, post-loop control+value users retargeted). Single-block bodies only (v1 family); countdown loops out of scope. The epilogue is the counted shape the VECTORIZER can take. Fires on t36 (changes=3, 6 counters, round-trip: instrument output == plain output == use output; bad-profile warning + degrade). |
| 44 | LoopPeeling | IMPLEMENTED | Remainder peeling at the loop ENTRY: r = T mod F peeled copies chain from the entry predecessor (each executes unconditionally — T is an exact constant, so those iterations always ran), every header phi's entry input becomes the last peeled copy's value (the IV starts at init + r*k), and the residual loop trips T-r, divisible by F so pass 42 unrolls it exactly. Fires on t18_unroll (sumsq(101)). |
| 45 | LoopInterchange | IMPLEMENTED | Perfect-nest swap for strided walks: `while i<W { while j<H { s += a[j*W+i] } }` becomes the j-outer/i-inner form — unit inner stride, the `j*W` term invariant in the inner body (hoisted by LICM/machine LICM), one IV update per swapped role. Gates: read-only nest (loads + pure ops), trivial self-thread phis collapsed first (SROA's per-version threads — including the memory phis, which are self-latching in effect-free nests), carried pairs in the standard (X_o = Phi(ext, X_i)) shape with accumulating updates restricted to integer commutative reductions whose feed never reads the accumulator (FP gated on --fp=fast: the fold order changes), and an address analysis that extracts how each load's index depends on each IV (Unit/Wide/Absent — coefficients may be runtime values like the width param): every address must use the OUTER iv with unit coefficient and at least one must scale the INNER iv wide. Mechanics: control edges re-thread (entry into the new outer; the old outer's body projection becomes the new inner's body entry), carried phi pairs ROTATE inputs, the IV updates move to their new loops' blocks (i++ into the inner body, j++ onto the outer latch path), post-nest readers re-target to the new outer phis. Guards keep testing their own IVs. Fires on t33 (column-major sum; the already-stride-1 row-major twin is correctly left alone). |
| 46 | LoopFusion | IMPLEMENTED | Adjacent loops over the SAME iteration sequence (same IV entry value and constant step, same guard relation against the same bound node — GVN unifies constants, param bounds work, back-to-back entry: L2's entry pred is L1's exit projection) merge into one loop: one header/guard/IV update instead of two, a body SLP/LICM/the vectorizer can see whole. Safety: base-level write/read independence both directions via alias analysis (writes of one never touch what the other accesses), no calls, carried values available before L1's entry (anything derived from L1's RESULTS rejects — the dependent a[i]=a[i]*2 then sum(a) pair stays two loops), and pure nodes LICM parked between the loops (the gap block) are hoisted ahead of the fused loop when their inputs allow. Mechanics: iv2 RAUs to iv1; L2's header phis split into trivial self-latch threads (collapse), the memory phi (inside-B2 users rethreaded to B1's last effect, post-loop users to the fused loop's phi), and real carried phis rebuilt at header1; the guard's exit projection RAUs to L1's; B2's first block contents join B1's last block; the fused backedge comes from B2's last block. Fires on t32 (two disjoint sum loops fuse; the dependent pair does not). |
| 47 | LoopFission | IMPLEMENTED | Split a counted loop carrying TWO INDEPENDENT reduction clusters into two loops, one per cluster — the inverse of pass 46's fusion, applied where fusion would not: the clusters share the iteration sequence and nothing else, so per-loop register pressure halves and each split half is the single-reduction shape the LOOP VECTORIZER (56) requires (a two-reduction loop is rejected there with `second-loop-carried-phi`; dynamic-trip two-reduction loops vectorize after the split — fission is the enabler, the same interaction production compilers get from loop distribution). FIRST FAMILY (documented scope): READ-ONLY counted loops — the memory phi self-latches when one exists (no store in the body), and when the phi has collapsed away entirely (loads read a preceding region's version) the body scan proves no store/call directly; a single body block, no early exits, exactly two carried value phis, each a FLAT reduction whose feed is a subtree of loads over ONE cluster array plus pure ops; the clusters' load bases pairwise NoAlias; feeds never reference the other accumulator (the `y = y + x` dependent pair stays one loop) and every body load feeds one of the two reductions. Const-trip two-reduction loops are taken by the UNROLLER (42) first — the dynamic-trip family is fission's complementary territory. Mechanics: the second cluster gets a fresh loop after the first's exit projection — new header Region/guard Cmp/If (same bound node and relation), fresh IV phi (same init/step, cloned Add), optional self-latching memory phi, and a cloned cluster subtree (loads re-pinned, IV references remapped, loop-external leaves reused); post-loop readers of the moved accumulator read the new phi; the post-loop code pinned to the exit projection re-pins after the new loop's exit (nodes the clone reuses stay before it, pre-validated); the old phi and its original subtree die with the next DCE sweep. Fires on t35 (both halves vectorize: paddq/pxor lanes). |
| 48 | Predication | IMPLEMENTED | Short-circuit condition flattening: `a && b` / `a || b` lower as full control flow (If -> projections -> Region -> value phi); when the RHS slice is pure (no loads/stores/calls; phis allowed as dominating loop-header leaves), the merge dissolves into Bin(And|Or, c1, c2) pinned in the pre-branch block — one branch instead of two per iteration, the escape-check shape every latency-bound loop wants. Nested short-circuits flatten iteratively (inner first). The short-circuit constant is identified by VALUE (earlier passes re-pin Consts to Start); projection contents (including that constant) are re-pinned to the branch block before the projections die. Fires on mandel's inner guard and t19_predication. |
| 49 | IfConversion | IMPLEMENTED | Effect-free if-diamonds to Select (one Cmov / one min-max instead of two branches). The merge region itself is exempt from the arm-cleanliness scan (this pass silently matched NOTHING for the project's whole life until the pass-reality audit: every if/else's IfTrue sits in region slot 0, and the region was misread as "nested control in the arm"); arm values only need to be AVAILABLE before the branch — defined-in-arm values must be pure so they hoist, but pre-branch values may be any non-control node (a loaded array element selecting against a constant is the standard clamp shape). The trivial memory phi forwards; the merge dissolves by re-pinning R's contents to the branching block. Feeds p17 and p65 (select-over-compare -> Min/Max). Regression-locked by t07/t15/t29. |
| 50 | TailRecursionElimination | IMPLEMENTED | Four transforms in the tail-call family. (1) TAIL-CALL-TO-LOOP: a direct self tail call `return f(A..)` becomes a REAL loop — header Region, memory phi, per-param value phis (RAUW), slot-aware memory rethread (`replace_uses_as_memory` on the entry memory so past iterations' callee effects stay visible to later iterations AND the base return), and a re-pin fixpoint that moves entry-pinned value/control nodes into the header when they (transitively) read loop-carried values. The old kFlagTailCall jump-to-entry paid a full frame teardown+rebuild per spine iteration (~19 instructions of churn on tak's 39.5M tail calls — p85 popped the frame, p88 rotated, then the entry re-pushed everything); the loop form pays phi-update movs and one branch, and feeds the normal loop machinery (SCCP/GVN on the phis, RA phi coalescing, p88 rotation + fallthrough layout). The jump form survives as the fallback for shapes the loopify rejects (entry-merge Regions, degenerate no-base-case, non-sole-user calls, arity mismatch). (2) RECURSIVE SELF-INLINING (-O3, bounded): small self-recursive functions (<=120 live nodes, <=4 inner self-call sites, not accumulator-transformed) get a one-level expansion — the body is deep-cloned as an unregistered inline source, its returns merged to one via Region+phis (the inliner requires single-return), and the clone is inlined at every INNER self-call site. The clone's internal calls keep targeting the ORIGINAL function, so every physical call executes one explicit level plus the inlined level — halving the dynamic call count (tak: 118M -> 57M calls; gcc's own -O3 recursive-inlining mechanism). The tail-position call is left for (1). Expanded functions are marked no-inline. (3) Self tail calls that survive (1) become entry jumps (kFlagTailCall honored by the emitter). (4) RECURSION UNROLLING via accumulator introduction: `f(x) = if C(x) B(x) else f(g1(x)) + f(g2(x))` (integer +, both calls to self, pure C/B/g1/g2) rewrites to a phi-threaded `acc` loop — one recursive call per level instead of two (the transform GCC applies under -foptimize-sibling-calls; same divergence behavior). Transformed functions marked no-inline (the inliner's repin closure does not support calls feeding loop-phi backedges). (5) SPINE-TAIL BASE WIDENING (2026-09-22, the recursion round): when the base region partially evaluates — f(k) constant-folds for every k in [0..K] (a mini-evaluator over the pure C/B/g1/g2 subtrees, depth 24, budget 8192, K <= 10) — and the recursion is STRUCTURALLY descending (g2 = x - c, c >= 1) with a constant-bounded base (C = x <s d with 0 <= d <= K+1, or x <=s d with -1 <= d <= K, single-parameter functions only), the loop exits at `w <= K` through the folded table TBL[w] + acc instead of running the spine down to C. The soundness identity: the loop's remaining contribution from spine value w IS f(w) (unroll the recursion definition; the call terms line up with the per-step calls one-to-one, and the fold's success proves it bottoms in constants). Every f(k <= K) subtree collapses to a constant — fib's executed-call count drops ~75x (fib 2.09x -> 0.17x vs gcc -O3; gcc walks every tree node, the table prunes them). The widened head is a single signed `w <= K` compare (same shape as the original base check); negative arguments take the TBL default arm B(w) (C-true there by the structural gate — the gates exist because naive widening MISCOMPILES ascending spines, base bounds above the table, and multi-parameter shapes; all rejected shapes keep the plain accumulator form and t39_widenbase locks the table-edge/negative/rejected cases). Fires on t08/t20/t22/t39 (changes=17 on t39). |
| 51 | BranchProbabilityInference | IMPLEMENTED | Edge probabilities from heuristics (loop backedges 0.90, others 0.50) computed and cached in the analysis manager's BranchProb; the linearizer consumes it via pass 52. Analysis-only: reports no transformation (2026-09-18 audit fix — previously returned true whenever If nodes existed, inflating the --stats changes column; now matches the p19/p20 contract). |
| 52 | HotPathStraightening | SIMPLIFIED | RPO layout with loop/hot fallthrough preference. The branch-count half of straightening (loop rotation: one taken branch per iteration instead of two) is implemented at machine level in pass 88, where the rotated shape is expressible; this pass keeps the block-order half. |
| 53 | IdiomRecognition | IMPLEMENTED | Library-idiom loop shapes lowered to their optimal forms. First family: constant-trip FILL and COPY loops small enough that the loop machinery costs more than the body (the unroller refuses trips < 6; the vectorizer keeps guard + remainder): `while i<T { a[i]=c }` becomes T immediate stores at the loop entry; `while i<T { a[i]=b[i] }` becomes T loads + T stores (SLP then packs the pairs into movups — the inlined-memcpy shape; NoAlias bases required). The expansion threads the store chain off the entry memory, RAUs the memory phi to the final store (post-loop readers see the fully-written array), RAUs the IV phi to init+T*step (exact — counted loop, no early exits), and pins the guard's condition to the constant that always exits; the dead shell is reclaimed by the cleanup SCCP/DCE sweep. Fill values must be loop-invariant AND available before the entry (dominance-checked: use-before-def rejected). Fires on t31. |
| 54 | SLPVectorizer | IMPLEMENTED | Straight-line adjacent store pairs -> one packed iteration: two Stores consecutive in the memory chain, same base, CONST consecutive element indices (the symbolic/interleaved family is pass 58's), values = the corresponding adjacent loads (copy) or ONE isomorphic Bin over those loads with the same extra operand (scale). Emits packed Load + (optional packed Bin with a Broadcast) + packed Store at the pair's first address (the scalar address node is reused — the vector op spans the same 16 bytes). Constant-folded addresses (Add(x,0)/Add(x,$imm) forms) are recovered via the known element size. SOUNDNESS (regression-locked by t25's overlap()): the shared gate vecx::pack_pair_sound proves the loads' memory versions carry the same values (intervening stores may not write lane 1's element — the packed load reads at the FIRST load's version) and that no other user of the first store's version observes the second element's write one chain position early; alias-unprovable shapes reject (the old emitter trusted the chain and miscompiled `a[1]=a[0]+7; a[2]=a[1]+7`). 54/58 run in the main sweep AND the post-inline cleanup (inlining turns param-based MayAlias pair candidates into distinct local allocations: NoAlias); vectorized functions stay outlined (the inliner predates packed control shapes). Fires on t25_slp (inlined bodies pack: movups pairs). |
| 55 | SuperwordPacker | IMPLEMENTED | The pack-graph ANALYSIS half of the SLP pair: discovers every isomorphic consecutive-element store pair in a function (including pairs 54 cannot execute — not adjacent in the memory chain), reports candidate count vs chain-adjacent count (coverage telemetry; JULES_DEBUG_VEC traces each pack). Shares the address/discovery predicate family with 54's executor; production compilers do the same split (packing-graph analysis vs vector code emission). |
| 56 | LoopVectorizer | IMPLEMENTED | Top-down vectorization of counted loops (SSE2 128-bit: 2x8B / 4x4B lanes). Structure: [preheader] guard(bound >= VF) -> vector loop k = 0..nv (nv = bound / VF, one packed iteration computes VF scalar iterations: packed Load at base + k*16, packed Bins with Broadcast invariants, packed Store, reduction phi as a v2 phi) -> merge feeding the ORIGINAL loop as the scalar remainder (iv entry = Phi(0, k*VF); reduction entry = Phi(init, horizontal lane extract tree)). Integer reductions are exact; FP reductions gated on --fp=fast (lane tree reassociates); element-wise FP ops are lane-exact (strict-safe). Match requirements: init 0, step 1, i32/i64 iv, single body block, unit-stride same-base addresses, no calls/branches in body, bases loop-invariant; pass-through phis (SROA pointer locals) accepted as invariant. Stored values must be mapped loads, mapped pure ops, or broadcastable LOOP-INVARIANT leaves — the raw IV phi as a stored value (`a[i] = i`) is elem-typed but loop-varying and is rejected (broadcasting it read a stale merged value into every lane; regression-locked by t35's dependent companion). Cost model + width selection consulted via vecx:: at decision time. NEW this session (the pass-61 mask path): body Cmp+Select pairs pack — a Cmp whose every user is a body Select becomes a packed lane-mask compare (result = the same vector type, all-ones/all-zeros bits; i64 elements skip: no v2i64 compare on SSE2) and the Select becomes Select(vmask, tvec, fvec), lowered by pass 61 into the and/andn/or blend; the packed compare's relation is bitwise-exact so strict-FP masked f64 loops vectorize (only the reduction lane tree keeps its --fp=fast gate). Scalar unroll/peel skip packed loops (match_counted rejects vector-typed phis). The vector trip count nv = bound/lanes is pinned AT THE LOOP ENTRY, not the header (2026-09-22: a header pin re-executed the idiv every vector iteration, ~30 cycles per 2-element step — the bulk of a 9.6x benchmark gap). UNROLL x2 at perf levels (2026-09-22 recursion round): two packs per iteration — k counts 32-byte pairs, nv = bound/(VF*2), an odd pack count tails into the scalar remainder exactly (floor-floor division identity); pack B's byte offset is the pair offset +16 (the imm-lea-foldable form). Integer Add reductions split into TWO accumulators (independent chains — the shape gcc emits — merged lane-wise at the exit: exact for integers); FP and Min/Max/Mul reductions keep ONE accumulator with the two pack updates chained (preserves the existing reassociation semantics; the reduction-reading pure ops map to the pack's own accumulator view). Size-biased levels (-Os/-Oz) keep one pack per iteration. Fires on t24 (changes=6: sum, two-array sum, f64 axpy element-wise, const fill), on p47's split halves (t35), and on the masked loops of t38. |
| 57 | OuterLoopVectorizer | SCAFFOLD (honest no-op) | Vectorize outer nest when inner is short. |
| 58 | InterleavedAccessRecognition | IMPLEMENTED | Interleaved (stride-2) element pairs with a SHARED SYMBOLIC index — a[2i], a[2i+1], the shape an AoS struct-of-two walk compiles to (with no structs in the language, stride-2 indexing IS the interleaved access) — pack into one SHUFFLE-FREE 128-bit access: the pair addresses 16 CONTIGUOUS bytes at the first element, so one packed load + (optional packed bin/broadcast) + one packed store (movups) covers exactly them; no deinterleave shuffles are needed, which is what distinguishes this family from a stride-2 GATHER (non-adjacent lane pairs cannot pack without shuffles; SSE2 has no cheap lane insert/extract and those shapes are rejected). The shared symbolic part m may be any node — Mul(i,2), the deinterleave Add(Mul(i,2),1) forms, or a computed index g(i): the pair proof only needs element adjacency (value-equal m via vecx::pure_equiv — the first-sweep schedule leaves the per-occurrence `2*i` muls duplicated until GVN's cleanup re-run, which Phase 5 does not re-run). Shares the pair executor and soundness gate with 54 (vecx::pack_store_pairs / pack_pair_sound): same pin for the four nodes, load-version equivalence for lane 1 (intervening stores may not write it), no other user of the first store's version reading the element the packed store writes early; alias-unprovable shapes reject. In-loop pairs pass the gate when bases are NoAlias (distinct local allocations — the inlined/main-array case) or MustAlias with element-disjoint lanes (in-place same-array walks); MayAlias param bases reject (an offset overlap would make the packed load read the pre-store lane). The mixed const-dest/symbolic-src pairs stay with 54/58 respectively (family split by the dest pair's index kind). Fires on t34 (deinterleave + in-place + the param negative). |
| 59 | ReductionRecognizer | IMPLEMENTED | Loop-reduction analysis (acc = phi(init, acc op x)) shared with pass 56's transform (vecx::match_reduction is the single matcher both use — catalog order puts the recognizer after the emitters, mirroring LLVM's ReductionAnalyzer split). Reports every reduction in the module with op/type telemetry; the vectorizer additionally canonicalizes nested integer Add updates ((s+a)+b -> s+(a+b)) so level-dependent reassociation order cannot hide the reduction shape (t24's sum_two at -O3). |
| 60 | VectorLegalization | IMPLEMENTED | Post-vectorizer safety net: verifies every packed node has an SSE2-legal form (Bin op legal for the lane type, Extract lane < lane count, Broadcast operand scalar) and kills illegal shapes (DCE reclaims the loop); removes dead Broadcasts whose consumers died after vectorization. Legal packed set: f64/f32 add/sub/mul/div/min/max; i64/i32 add/sub; logicals (and/or/xor/andnot) on every non-bool lane kind (pand/por/pxor/pandn are bitwise — lane typing irrelevant; fp lanes gain them only through pass 61's mask blends). Packed Cmp validates operands==result type and the lane kind SSE2 can compare (v4i32/v4f32/v2f64 yes; v2i64 killed — pcmpgtq is SSE4.2); packed Select validates the mask's lane count and t/f types. |
| 61 | MaskGeneration | IMPLEMENTED | The active-lane MASK machinery for conditional packed values — the piece that lets the loop vectorizer take if-converted bodies. Chain: pass 49 converts an effect-free in-loop diamond (`if v > 0 { x = v } else { x = 0 }`, SROA-promoted) into Select(Cmp(v,0), v, 0); pass 56's body classifier now admits the pair — the Cmp packs into a per-lane all-ones/all-zeros mask in the SAME vector type (the exact bit shape x86 compare results produce; every Cmp user must be a body Select, else the loop skips), the Select packs into Select(vmask, tvec, fvec); THIS pass lowers every packed Select into the explicit blend Or(And(m,t), AndNot(m,f)) — SSE2's canonical masked select (pand/pandn/por; no blendv below SSE4.1). BinOp::AndNot is the new vector-only IR op for pandn (~a & b). Machine side: VecCmpF64/F32 emit one cmppd/cmpps with the ORDERED predicates (eq=0 ne=4 lt=1 le=2 gt=6 ge=5 — NaN-exact, bit-identical to the scalar ucomis semantics), VecCmpI32 composes pcmpeqd/pcmpgtd with an all-ones flip built in the dead operand register (Eq/Gt 1 inst; Ne/Le 3; Ge/Lt 3 + move). SOUNDNESS is bitwise end-to-end: the compare is ordered, the blend never does lane arithmetic, so strict-FP masked loops (f64) vectorize with NaN semantics exact; only reductions stay --fp=fast-gated (lane-tree reassociation, unchanged). i64-element masked loops SKIP honestly (pcmpgtq is SSE4.2, pcmpeqq SSE4.1 — no v2i64 compare on the baseline; the scalar loop covers them, t38's masked_i64). The isel keeps a safety-net emission for packed Selects (same and/andn/or triple inline) so `--disable 61` stays correct; lowering at the SoN level (not in the isel) exposes the blend to GVN/DCE/peepholes as ordinary packed Bins. Degenerate zero arms collapse bitwise (2026-09-22): Select(m,t,ZERO) -> And(m,t), Select(m,ZERO,f) -> AndNot(m,f) — one packed op instead of three for the clamp-to-zero / masked-accumulate family. CLAMP-TO-ZERO MIN/MAX (2026-09-22 recursion round): Select(Gt(t,Z), t, Z) and Select(Lt(t,Z), t, Z) — where Z is the SAME +0.0 both the zero arm and the compare's constant broadcast — rewrite to a single Max(Z, t) / Min(t, Z), emitted as `maxpd dst=t src=Z` / `minpd dst=t src=Z` (vecmask: cmpnlepd+pand -> one maxpd, the exact instruction sequence gcc emits for the same source). Bitwise-exact on EVERY lane including NaN and -0.0 (hardware-verified: unordered and equal-zero cases return the SRC = +0.0, which is precisely the select's false arm); the Ge/Le relations are DELIBERATELY REJECTED — the select yields the raw -0.0 where the instruction yields +0.0 (the mirrored-compare forms Gt(Z,t)/Lt(Z,t) normalize through the swap). Found by writing t38: BUG-25, the 7th latent miscompile of the audit — the machine peephole's cmp-$0 -> test fold kept the compare's 32-bit SIZE but the Test serializer always emitted testq, so a negative i32 (zero-extended by its 32-bit load) tested POSITIVE as a 64-bit value: `if v > 0` went true for v = -10 on every committed -O1+ build. Fixed (test respects size); masked i32 loops now execute `testl %r8d, %r8d; setg`. Fires on t38 (i32 masked store+reduction loops via pcmpgtd masks, f64 masked loop via cmppd $6 — and the clamp forms via the maxpd path). |
| 62 | ShuffleOptimization | IMPLEMENTED | Lane-traffic minimization on the SoN analog of shuffles: Extract(Broadcast(x), k) -> x (every lane IS x — the identity pair arises around reduction exits and broadcast operands; collapsing removes a punpcklqdq + extraction per site). Dead extracts fall to DCE. |
| 63 | AutoSOATransform | SCAFFOLD (honest no-op) | AoS-to-SoA restructuring needs struct/record types the MVP language lacks — and the whole-program restructuring mechanism is not built (file self-identifies as scaffold; relabeled from VACUOUS by the 2026-09-18 audit). Contract stays for the day the type lattice grows. |
| 64 | VectorCostModelEvaluation | IMPLEMENTED | The profitability oracle: vecx::vector_cost_ok (packed-form availability, trip < VF, reductions too short, size-biased dynamic-trip rejection) is consulted at decision time by 54/56 — the shared-function split production compilers use. The pass re-evaluates every packed loop after the fact, recovering trip counts that only became constant after inlining, and records verdicts + telemetry for the reporting pipeline. |
| 65 | SIMDIntrinsicMatching | IMPLEMENTED | Source idioms mapped to single SSE2 instructions the generic Bin lowering cannot find. First family: FP min/max — `if a < b { m = a } else { m = b }` (and the 7 other relational/arm permutations) after IfConversion is a Select over a Cmp of the same operands; the matcher rewrites it to one Min/Max Bin node, emitted as one `minsd`/`maxsd` (scalar; `minpd`/`maxpd` when a later vectorization sweep packs it). NaN-exactness is load-bearing: x86 min/max return the SRC (AT&T first) operand on unordered, so each matched shape maps to the canonical IR form (Min(a,b) = Select(Lt(a,b),a,b), Max(a,b) = Select(Lt(a,b),b,a)) whose NaN arm is bit-identical to the source select; machine passes never commute Min/Max operands. Integer selects stay on the Cmov path (no SSE integer min/max below SSE4.1). min(x,x)/max(x,x) collapse in p2; consts fold through eval_bin_const. The old FP select lowering was two branches + two loads — one instruction now. Fires on t29 (clamp loop: 2 instructions per element, zero branches). |
| 66 | VectorWidthSelection | IMPLEMENTED | Width policy for the fixed SSE2 baseline (128-bit, the universal x86-64 contract): 16 / lane bytes (2x f64/i64, 4x i32/f32 — no -march flags in the spec). Validates every packed type's lane count x lane bytes == 16, kills illegal widths (the guard for future 256-bit AVX work), and reports the width decisions (JULES_DEBUG_VEC). |
| 67 | ClassHierarchyAnalysis | SIMPLIFIED | Possible call targets via hierarchy (empty in MVP). |
| 68 | StaticDevirtualization | VACUOUS (nothing to do in MVP) | Resolve dyn calls via CHA (no dyn dispatch in MVP). |
| 69 | SpeculativeDevirtualization | SCAFFOLD (honest no-op) | Profile-guided guarded direct calls need dynamic dispatch sites the MVP language lacks — and the guard/deopt machinery is not built (file self-identifies as scaffold; relabeled from VACUOUS by the 2026-09-18 audit: the JIT twin's contract remains documented). |
| 70 | InlineCacheInsertion | SCAFFOLD (honest no-op) | Mono/poly/mega inline caches memoize polymorphic call-site targets; the MVP language has no virtual/indirect calls — and the IC-stub emission mechanism is not built (file self-identifies as scaffold; relabeled from VACUOUS by the 2026-09-18 audit). Contract stays for the day dispatch lands. |
| 71 | GuardInsertion | SCAFFOLD (honest no-op) | Assumption-checking guards (JIT). |
| 72 | GuardHoisting | SCAFFOLD (honest no-op) | Move guards to cold points (JIT). |
| 73 | GuardWeakening | SCAFFOLD (honest no-op) | Relax guard conditions (JIT). |
| 74 | GuardMerging | SCAFFOLD (honest no-op) | Combine redundant guards (JIT). |
| 75 | AssumptionTracking | SIMPLIFIED | Assumption registry per compiled version. |
| 76 | InliningCostModel | IMPLEMENTED | Per-site benefit scores. |
| 77 | AlwaysInlineEnforcement | IMPLEMENTED | Force-inline annotated/trivial functions. |
| 78 | CostBasedInlining | IMPLEMENTED | Threshold-driven inlining with budget. |
| 79 | RecursiveInliningBounding | IMPLEMENTED | Anti-explosion policy for recursive inlining. |
| 80 | ArgumentSpecialization | IMPLEMENTED | Constant-arg calls get priority inlining. |
| 81 | ClosureInlining | VACUOUS (nothing to do in MVP) | Inline closure bodies + environment (no closures in MVP). |
| 82 | LTOSummaryGeneration | SCAFFOLD (honest no-op) | Serialize cross-module info for LTO (AOT). |
| 83 | CFGLinearization | IMPLEMENTED | SoN -> ordered blocks with scheduled nodes. Successor attachment records the LAYOUT FALLTHROUGH edge for any block whose control-derived successor list came out empty (an empty IfFalse block whose only user is the next block's Jump head; a leftover If whose projections died with inlining) — the emitter falls through in exactly those cases, and the RA's liveness dataflow needs the edge or loop-exit readers look dead around the backedge (the loop body then clobbered their registers; always-inlined copy loops over loop-filled arrays segfaulted on the stale base pointer). Also module-level DEAD-FUNCTION ELIMINATION: after inlining, functions unreachable from main in a closed module (no shared-library exports) are dropped from the linear module — inlined-away callees no longer double every benchmark binary (reachability = fixpoint over live Call FnIds from main). |
| 84 | InstructionSelection | IMPLEMENTED | x86-64 MIR emission (SysV). DP-ON-DAG COVER (2026-09-23, x64_dp_isel.{h,cpp}): per-block memoized min-cost cover layered over the hand emitters — Pass A claims Load/Store address chains (single-use, in-block, through machine-identity ptrcasts; the address arithmetic never takes the slot round trip), Pass B claims Bin nodes the rule set covers strictly cheaper than the hand emitter (latency-class costs, no-tie policy). Rules: full-SIB Lea2 (base + idx*scale + disp — new opcode; the index rides in b.slot, op-gated consumers), baseless LeaRR, ShlImm (Mul by 2^k), mul-by-3/5/9 -> lea x+x*scale, generic Arith with inline b-side chains; scratch discipline {dst, other} keeps nested chains sound; shape-absorbed nodes (index terms folded into the SIB scale) are suppressed at claim time. Telemetry JULES_DP_STATS=1. On the 2D-indexing stress: 24 full-SIB leas vs 0 before, 914 -> 695 asm lines. Fused short-circuit branches: a bool And/Or chain over single-use compares feeding an If lowers as compare+branch pairs straight off the flags — zero setcc/movzx/and round trips (the materialized form remains for mixed nesting, multiply-used values, integer bitwise & — type-gated on i1). Compare emission: swapped-operand form folds to `cmp reg, $imm` (no per-iteration constant re-materialization); non-imm32-encodable i64 constants fall back to register compares (cmpq has no imm64 form — the old emitter produced un-assemblable output; fixed with t21). Reg-reg FP moves emit full-width movapd/movaps (merge-encoded movsd is not move-eliminable and adds a cycle per FP dep-chain copy). |
| 85 | RegisterAllocation | IMPLEMENTED | Level-dispatched: -O0/-Og spill-everywhere; -O1+ the hybrid two-phase allocator (2026-09-23 third round: target-neutral core in src/core/codegen/ralloc.*, driven from x64_ra.cpp through a RaProblem/RaSolution contract; per-architecture facts come from the machine target description registry). PHASE 1 (exact): which ranges keep registers is a min-cost network flow over the segment chain — the consecutive-ones program is totally unimodular, so the integral flow optimum is the true optimum spill set (validated vs exhaustive search, 20,003 instances, 0 mismatches; scripts/ra_flow_selftest.cpp); staged per class: call-crossing ranges under the callee bank, then non-crossing under the residual capacity K - x_p, then a greedy repair for rejected crossing ranges. PHASE 2: bank-aware interval assignment (callee-saved banks for call-crossing; caller-first otherwise, -Os flips; r10/r11 plus write-scanned clean argument registers rdx/rsi/rdi/r8/r9; xmm2..N where N adapts to the isel FP const pool watermark) with the handover-touch expiry, then ITERATED CONSERVATIVE COALESCING at generation precision (IRC-grade move unification on a pre-solved coloring). Weights: uses x 10 x 2^loop-depth x (16 if loop-carried), with loop facts computed EXACTLY at block level (loop-carried = live_out at a backedge latch; depth = natural-loop membership — the old linear-hull approximation inverted the mandel spill ranking and cost 11%; see the session note). This session: operand folds run to fixpoint THROUGH killed instructions (two setup movs per consumer exposed the A-side only after the B-side died); snapshot stores [store s_mid<-acc] and FP const-materialization pairs are legal chain elements/gaps so pure-register recurrences fuse; self-update chains (a = a*c + d through the phi slot) fuse in place; single-use single-def home-store forwarding kills [movsd H<-S][consumer reads H] round trips (window scan blocks on ANY reference to H or the scratch S — setcc/movzx write AL/RAX and once slipped through); stack slot coloring (pass 28) shares rbp offsets; CmpRImm A-side folding drops the loop-guard setup mov. Exact liveness per slot via generation sub-intervals (multi-def phi slots split at redefinitions); expiry allows def-start handovers. Accumulator-chain fusion: rax/xmm0-threaded [load][op]+[store] chains rewrite into the result's register ([mov Z, X][op Z, src]), in place when phi-cycle coalescing (hint retarget validated on generation-disjoint exact liveness) lands the pair on one register — loop updates emit `addq $1, %r15` / `addsd %xmm4, %xmm3` directly. Phi copies between coalesced slots vanish. Soundness gates: result slot must not feed its own chain, no operand may share the result register post-coalescing, in-place only for phi-successor or non-loop-carried sources. Frame elision with SysV alignment; callee-save pushes deduplicated per physical register. |
| 86 | PostRACleanup | IMPLEMENTED | Fold store+load pairs (GP and FP) including same-register elision; slot-immediate to register-immediate forwarding. Same-register pairs also fold ACROSS the fused-guard gap ([store s,X][loads/compares/branches][load X,s] -> delete both): every gap instruction is read-only or loads a different register, so X stays live across the branches — kills the x*x+y*y spill round-trips in rotated short-circuit guards. |
| 87 | MachinePeephole | IMPLEMENTED | Fused compare-and-branch (cmp/jcc from setcc/movzx/test chains, incl. post-fold short chains and cross-block promoted booleans), loop-invariant FP constant hoisting, mov+test folding (register and cmp-$0,[mem] forms), rax accumulator folding, copy-chain elimination through scratch registers, adjacent mov-pair elimination, dead store removal, cmp-$0 to test, linear dead code after unconditional transfers (TCO zombie epilogues), LEA FORMATION ([mov R2,R1][add/sub $k,R2] -> [lea R2,k(R1)] and [mov R2,R1][shl $k<=3,R2] -> [lea R2,(,R1,2^k)]), PRODUCER DESTINATION RETARGET ([lea/mov-imm -> S][mov D,S] -> [producer -> D]: recursive call args land in their arg register in one hop — gcc's `lea -1(%rbx),%rdi` shape; the deadness scan treats calls as a redefinition of rax, the return register; ArithRImm producers are EXCLUDED — they read their dst and retargeting computed D+k instead of S+k, the 13th lifetime miscompile, caught by t01@-Og and regression-locked there), and COMMUTATIVE PHI-BACKEDGE FUSION ([op X,Y][mov Y,X] with X dead on every reachable path -> [op Y,X]: the accumulator update writes its phi home directly — `addq %rbx,%rax; movq %rax,%rbx` becomes `addq %rax,%rbx`, one instruction per loop iteration on fib; the deadness walk follows jmp targets and requires BOTH jcc paths dead, budget-bounded).  Also 2026-09-22 (recursion round): CFG REGISTER LIVENESS + OPERAND-COPY ELIMINATION — a register-liveness analysis over the emitted CFG (labels + jump edges -> blocks, backward fixpoint over u32 reg bitmasks, per-instruction live-after; reads over-approximated and writes only-where-certain so consumers asking 'is D read later' stay sound; computed once at entry since peephole rewrites only remove or redirect reads) — and on top of it [MovFpFp/MovRR D <- S][two-operand op b=D] -> move deleted, the op's operand redirected to S, for VecBin*/FpBin/FpCmp/ArithRR/CmpRR consumers whose other operand is not S. Found on vecsum's unrolled loop: the pair-fold's promoted movaps fed paddq through a copy the loop paid once per pack; the old linear dead-scan aborted at every backedge (why the liveness analysis exists). |
| 88 | MachineLICM | IMPLEMENTED | Machine loop transforms, post-RA: (1) LOOP ROTATION for BOTH guard polarities — Form A [head: cond; jcc body][exit][body; jmp head] and Form B [head: cond; jcc EXIT][body...][jmp head] (the inlined/unrolled polarity: guard jumps out, body is fallthrough) rotate to [entry: jmp check][body...][check: cond; jcc'(inv) body] with the latch deleted; multi-jcc heads (fused short-circuit guards: [cmp][jcc exit][cmp][jcc body] in one block) rotate with the LAST jcc as the backedge and the earlier guards jumping into the moved exit segment. (2) Invariant FP-constant materialization hoisting out of backedge regions; after rotation the hoist lands before the entry shim so both fallthrough and jump entries execute it once. (3) LOOP-ENTRY FALLTHROUGH LAYOUT: the rotated [entry][shim: jmp CHECK][body][CHECK: cond; jcc body][base: jmp EPI][EPI: ...ret] shape pays one taken jump per physical entry (158M on tak — every recursive call) plus one per leaf exit (118M). The [CHECK..EPI] cluster (chained through [jmp L][L:] adjacency so the epilogue travels with it) moves to immediately after the entry code, the shim is deleted (entry falls into the guard), the body gets an explicit latch jmp, and [jmp L][Label L] pairs elide to fallthrough: the LEAF path (guard not taken -> base -> epilogue -> ret) becomes ZERO taken jumps. Label-based jumps make any contiguous range movable; each cut preserves its fallthrough edge (entry->CHECK via the deleted shim, body->CHECK via the appended latch, cluster end terminates). Fixpoint handles nested loops (inner shims resolve after outer moves). Sound when the cluster carries a guard jcc (loop checks only) and ends at a full terminator. |
| 89 | DeoptMetadataEmission | SIMPLIFIED | Deopt manifest (JIT modes; empty guard set). |
| 90 | PartialEvaluation | IMPLEMENTED | STATIC specialization (2026-09-23, PE family — `src/core/son/passes/pe/`). For every direct call with a MIXED argument list (some provable constants post-SCCP, some dynamic), clone the callee under the constant bindings, fold the specialized graph, retarget the call with the bound arguments dropped. Identical binding sets across call sites share ONE variant via structural-key dedup — the code-size discipline per-site inlining lacks (two `c==1` sites = one variant, t40). The core is a reusable PE engine: BINDING-TIME ANALYSIS (abstract interpretation over a Const/Top/Bottom lattice — how many value/branch nodes go compile-time under the bindings) as the benefit gate (variant rejected when the binding buys nothing); a FOLD ENGINE ({reachability, constant propagation, dead-branch pruning, phi collapse} to fixpoint — the specialized graph gets a late-stage peephole, e.g. `c==1` variants lose both taken branches and the residual loop simplifies); TERMINATION BY CONSTRUCTION: single generation (variants are never themselves specialized), per-function variant caps and per-variant node-growth budgets tied to -O level (O2: 2 variants/fn, 1.25x growth; O3: 4, 1.5x; Os: 1, 1.125x; O0/O1/Oz: off) and a global node budget; variants whose fold overflows a budget are rejected whole. Calls re-targeting use the exact inliner exit-rewire discipline (value vs memory slot split). Honesty: BTA treats effectful nodes (Load/Call/Alloc) as conservatively dynamic — memory-state specialization (alias-disjoint load folding) is the documented next frontier, the usual bottleneck for PE on SoN IR. Runs after the inlining family (78-82: surviving call sites are exactly those the inliner rejected); off in instrument mode (a variant would clone pass-43 loop-counter bumps and double-count trips). |
| 91 | PartialDeoptimization | IMPLEMENTED | The guarded version LADDER (2026-09-23, PE family) — partial deop as a first-class citizen, driven by PGO argument sketches. INSTRUMENT build: one sticky-value sketch per (function, integer parameter) at the function entry — counters [first, total, match, min, max]: `first` = first observed argument, `total` = invocation count, `match` = count equal to `first`, and [min, max] = the observed signed hull; the bumps ride the entry memory chain so inlining copies them per call site and counts aggregate (x64: a PgoSketch text expansion — seed-on-first + increment + conditional match/min/max updates). USE build, Const rungs: a parameter is hot when match/total >= 95% at total >= 64; every call whose hot-parameter argument is DYNAMIC gets `guard p==V { deeper rung } else { this rung's variant }`, descending at most 2 rungs, generic floor — failure transfers DOWN ONE RUNG, never to a cold path: V12 fails p2 -> V1; V1 fails p1 -> generic. USE build, RANGE rungs (2026-09-23 second round — the Const -> Range -> Dynamic widening of the spec): not-sticky 64-bit int parameters whose observed hull fits the level budget (O2: span <= 256, O3: 4096, Os: 16; narrower int widths are excluded — zero-extended sketch patterns are not the source domain's signed order) get `guard (p >= lo) and (p <= hi) { deeper } else { this rung }` as two nested signed compares — the IR's Cmp is signed-only and nested guards reuse the exact control shapes the const path exercises. The Range-bound param STAYS a runtime parameter in the variant (only Const bindings drop args); the interval knowledge folds every branch that no value in [lo, hi] can take — bounds checks and saturating clamps die while the computation stays live (t42: scale's `c < 0` and `c > 200` guards both fold to false under [3,9]). The fold engine's lattice gained the Range rung (Top -> Const -> Range -> Bottom): hull transfers for Add/Sub/Mul-by-const/Neg with overflow-checked arithmetic (__builtin_*_overflow), endpoint-based compare folding (Lt/Le/Gt/Ge true/false only when the WHOLE hull sits on one side), phi meets as hull unions, Select with a range condition meets both arms. SOUND WIDENING CAP: a loop-carried phi's hull grows by 1 per drain visit and has no fixpoint below the span cap — the step limit would truncate the drain mid-widening and leave STALE Const conditions on its users (the observed t24 miscompile class: `i < 7` still Const(true) from the [0,1] era pruned the live exit); a Range hull may grow at most TWICE before collapsing to Bottom — the classic SCCP jump to overdefined. Stable meets (phi of independent branch values, range-seeded params) never grow and keep their hull. Every guard sits at a call-boundary checkpoint BEFORE any specialized effect executes — no OSR, no compensation, no side-effect replay; exactly one rung runs, and each rung is the whole function. CORRECTNESS NEVER DEPENDS ON THE PROFILE: a wrong hot value just fails the guard and runs the generic body (t41 forces the const guard: 199 of 200 loop calls see mode==1, call 200 passes mode==3). Because a range hull from a real profile covers every observed value, the range-deopt path is forced in the suite by an ADVERSARIAL PROFILE (scripts/t42_shrink_hull.py shrinks the recorded [min, max] below reality: c [3,9]->[4,8], x [0,199]->[1,198]); the use build then transfers c==3/c==9/x==199 DOWN the ladder and the output must stay byte-identical — locked by t42's PGO round + adversarial round. Emission rebuilds control around the call site (guard Ifs -> true/false arms -> per-level Region+phis -> merge; dependent nodes repinned to the merge so they cannot execute before the guard), and the ladder region is marked family-owned so the unrolling/peeling/vectorizing cloners refuse to re-thread it. Locked by t40/t41/t42 (343/343) including disable-flag holds for both passes and full PGO instrument->run->use round-trips with forced deopts (t41 const guard, t42 range guards via the adversarial profile). Honesty: 2 rungs max, function-entry checkpoints only — loop-header deop, OSR and mid-region guards are future work. |
