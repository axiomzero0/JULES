# 89-Pass Status Matrix

Audit (2025-09 session): all 89 catalog passes are registered
(`--list-passes`: 89 rows), scheduled by the catalog-order pass manager
with mode/level/kill-switch gating, and emit telemetry (`--stats`
per-pass `changes` / node deltas; `--only`/`--disable` isolate any pass).
Pass-activity is regression-locked: tools/test_runner.sh asserts the pass
actually transformed on programs written to exercise it (GVN, SCCP, SROA,
inlining, TCO, LICM, unrolling, predication). Reproduce the audit:
`./build/julesc --list-passes`, `./build/julesc --stats <file>`,
`./tools/test_runner.sh`.

Legend: `IMPLEMENTED` — Real transform/analysis operating on the SoN graph or MIR.; `SIMPLIFIED` — Real but reduced: core mechanism present, documented reductions.; `VACUOUS` — Complete for the current IR: the constructs it targets do not exist in the MVP subset.; `SCAFFOLD` — Not yet implemented: contract, modes and telemetry in place; honest no-op.

Status roll-up: 49 IMPLEMENTED, 5 SIMPLIFIED, 6 VACUOUS, 29 SCAFFOLD.

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
| 19 | AliasAnalysisInitialization | IMPLEMENTED | Build type-based, provenance AA structures. |
| 20 | MemoryDependenceAnalysis | IMPLEMENTED | Compute store->load dependencies. |
| 21 | RedundantLoadElimination | IMPLEMENTED | Remove loads satisfied by dominating loads. |
| 22 | StoreToLoadForwarding | IMPLEMENTED | Forward store values to loads. |
| 23 | DeadStoreElimination | IMPLEMENTED | Remove overwritten/unread stores. |
| 24 | StoreMerging | SCAFFOLD (honest no-op) | Combine adjacent narrow stores (alignment-aware). The SoN emits one Store per assignment; adjacent-field writes only arise from struct-like usage this MVP language does not have (SROA promotes the single-field cells first). |
| 25 | LoadHoisting | IMPLEMENTED | Hoist provably-safe loads out of loops. |
| 26 | ScalarReplacementOfAggregates | IMPLEMENTED | Promote memory-backed locals to SSA values. Promotion also treats memory-phi users as chain uses (not escapes) and ignores dead users, so loop counters/temps promote even when earlier passes killed their consumers. |
| 27 | BitfieldLowering | VACUOUS (nothing to lower) | The language has no bitfield types: the IR cannot express a bitfield access, so there is nothing to lower. Contract stays for the day the type lattice grows. |
| 28 | StackSlotColoring | IMPLEMENTED | Machine-level coloring in pass 85's finalize(): memory-resident slots whose exact live ranges (the same generation sub-intervals the register assignment used) are disjoint share one rbp offset; address-taken slots never share (distinct allocations keep distinct addresses); the frame size and offsets are derived from the color count. Telemetry: lf.ra_colored. |
| 29 | HeapToStackPromotion | IMPLEMENTED | Promote NoEscape allocations to stack slots. |
| 30 | MemorySSARepair | IMPLEMENTED | Restore memory chains after aggressive transforms. |
| 31 | PointsToAnalysis | IMPLEMENTED | Flow-insensitive points-to graph. |
| 32 | EscapeAnalysis | IMPLEMENTED | NoEscape/ArgEscape/GlobalEscape classification. |
| 33 | PartialEscapeAnalysis | SCAFFOLD (honest no-op) | Path-sensitive escape refinement (JIT). |
| 34 | AllocationSiteProfiling | SCAFFOLD (honest no-op) | Per-site escape behavior instrumentation (JIT). |
| 35 | MaterializationPointInsertion | SCAFFOLD (honest no-op) | Lazy materialization points for PEA (JIT). |
| 36 | LockElision | VACUOUS (nothing to elide) | No sync/lock ops exist in the MVP language or IR; there is no lock to elide. |
| 37 | LoopDetection | IMPLEMENTED | Natural loops via backedges; loop tree. |
| 38 | InductionVariableRecognition | IMPLEMENTED | Basic/derived IVs, trip counts. |
| 39 | LoopClassification | IMPLEMENTED | Counted/uncounted/early-exit/nested tagging. |
| 40 | LoopInvariantCodeMotion | IMPLEMENTED | Hoist invariant pure ops to preheaders. |
| 41 | LoadLICM | IMPLEMENTED | LICM for loads with alias proofs. |
| 42 | LoopUnrolling | IMPLEMENTED | Body-duplication unrolling for counted loops with compile-time-constant trip counts (shared cloner in passes/loop_transforms.cpp): match IV phi + Add(phi,+k) + const bound + no early exits; factor from the level budget (O2:4, O3:8); remainder peeled first when T%F!=0; F-1 copies chained behind the body (per-copy seeds remap every header phi to the previous copy's update so copy m sees iteration base+m); the header latch and phi backedges retarget to the last copy; IV steps F*k per unrolled iteration. Runs in the main pipeline and again in the post-inline cleanup (inlining propagates const bounds). Dynamic-trip guarded epilogue unrolling = documented roadmap. Fires on t18_unroll. |
| 43 | ProfileGuidedUnrolling | SCAFFOLD (honest no-op) | Unroll from profiled trip counts. Needs the PGO profile format + counters (not in this MVP); pass 42 already consumes the static-const trip counts. |
| 44 | LoopPeeling | IMPLEMENTED | Remainder peeling at the loop ENTRY: r = T mod F peeled copies chain from the entry predecessor (each executes unconditionally — T is an exact constant, so those iterations always ran), every header phi's entry input becomes the last peeled copy's value (the IV starts at init + r*k), and the residual loop trips T-r, divisible by F so pass 42 unrolls it exactly. Fires on t18_unroll (sumsq(101)). |
| 45 | LoopInterchange | SCAFFOLD (honest no-op) | Swap nested loop order (dependence validation). |
| 46 | LoopFusion | SCAFFOLD (honest no-op) | Merge adjacent compatible loops. |
| 47 | LoopFission | SCAFFOLD (honest no-op) | Split loops for register pressure. |
| 48 | Predication | IMPLEMENTED | Short-circuit condition flattening: `a && b` / `a || b` lower as full control flow (If -> projections -> Region -> value phi); when the RHS slice is pure (no loads/stores/calls; phis allowed as dominating loop-header leaves), the merge dissolves into Bin(And|Or, c1, c2) pinned in the pre-branch block — one branch instead of two per iteration, the escape-check shape every latency-bound loop wants. Nested short-circuits flatten iteratively (inner first). The short-circuit constant is identified by VALUE (earlier passes re-pin Consts to Start); projection contents (including that constant) are re-pinned to the branch block before the projections die. Fires on mandel's inner guard and t19_predication. |
| 49 | IfConversion | IMPLEMENTED | Effect-free if-diamonds to Select. |
| 50 | TailRecursionElimination | IMPLEMENTED | Two transforms in the tail-call family. (1) Self tail calls become entry jumps (kFlagTailCall honored by the emitter). (2) RECURSION UNROLLING via accumulator introduction: `f(x) = if C(x) B(x) else f(g1(x)) + f(g2(x))` (integer +, both calls to self, C/B/g1/g2 pure, no other effects or returns) rewrites to a loop phi-threaded `acc` — one recursive call per level instead of two, halving the dynamic call count of fib-shaped recursion (the transform GCC applies under -foptimize-sibling-calls). Same divergence behavior (the loop follows the original right-spine g2-chain). Transformed functions are marked no-inline (the inliner's repin closure does not support a self-call feeding a loop phi backedge — see t20's inliner fix history). Fires on t20_accumulator (changes=9 incl. graph surgery). |
| 51 | BranchProbabilityInference | IMPLEMENTED | Edge probabilities from heuristics. |
| 52 | HotPathStraightening | SIMPLIFIED | RPO layout with loop/hot fallthrough preference. The branch-count half of straightening (loop rotation: one taken branch per iteration instead of two) is implemented at machine level in pass 88, where the rotated shape is expressible; this pass keeps the block-order half. |
| 53 | IdiomRecognition | SCAFFOLD (honest no-op) | memset/memcpy/popcount pattern detection. |
| 54 | SLPVectorizer | SCAFFOLD (honest no-op) | Bottom-up adjacent scalar packing. |
| 55 | SuperwordPacker | SCAFFOLD (honest no-op) | Non-adjacent packing via packing graph (research). |
| 56 | LoopVectorizer | SCAFFOLD (honest no-op) | Top-down vectorization of counted loops. |
| 57 | OuterLoopVectorizer | SCAFFOLD (honest no-op) | Vectorize outer nest when inner is short. |
| 58 | InterleavedAccessRecognition | SCAFFOLD (honest no-op) | Strided AoS patterns to shuffle-free loads. |
| 59 | ReductionRecognizer | SIMPLIFIED | Sum/min/max/dot patterns (analysis only). |
| 60 | VectorLegalization | SCAFFOLD (honest no-op) | Widen/narrow/split vectors to ISA. |
| 61 | MaskGeneration | SCAFFOLD (honest no-op) | Predicates for conditional vector execution. |
| 62 | ShuffleOptimization | SCAFFOLD (honest no-op) | Minimize SLP/Superword shuffles. |
| 63 | AutoSOATransform | SCAFFOLD (honest no-op) | AoS->SoA restructuring (high-risk). |
| 64 | VectorCostModelEvaluation | SCAFFOLD (honest no-op) | Profitability oracle for vectorization. |
| 65 | SIMDIntrinsicMatching | SCAFFOLD (honest no-op) | Map idioms to FMA/gather/... intrinsics. |
| 66 | VectorWidthSelection | SCAFFOLD (honest no-op) | Optimal vector width per region. |
| 67 | ClassHierarchyAnalysis | SIMPLIFIED | Possible call targets via hierarchy (empty in MVP). |
| 68 | StaticDevirtualization | VACUOUS (nothing to do in MVP) | Resolve dyn calls via CHA (no dyn dispatch in MVP). |
| 69 | SpeculativeDevirtualization | SCAFFOLD (honest no-op) | Profile-guided guarded direct calls (JIT). |
| 70 | InlineCacheInsertion | SCAFFOLD (honest no-op) | Mono/poly/mega IC stubs (JIT). |
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
| 83 | CFGLinearization | IMPLEMENTED | SoN -> ordered blocks with scheduled nodes. Also module-level DEAD-FUNCTION ELIMINATION: after inlining, functions unreachable from main in a closed module (no shared-library exports) are dropped from the linear module — inlined-away callees no longer double every benchmark binary (reachability = fixpoint over live Call FnIds from main). |
| 84 | InstructionSelection | IMPLEMENTED | x86-64 MIR emission (SysV). Fused short-circuit branches: a bool And/Or chain over single-use compares feeding an If lowers as compare+branch pairs straight off the flags — zero setcc/movzx/and round trips (the materialized form remains for mixed nesting, multiply-used values, integer bitwise & — type-gated on i1). Compare emission: swapped-operand form folds to `cmp reg, $imm` (no per-iteration constant re-materialization); non-imm32-encodable i64 constants fall back to register compares (cmpq has no imm64 form — the old emitter produced un-assemblable output; fixed with t21). Reg-reg FP moves emit full-width movapd/movaps (merge-encoded movsd is not move-eliminable and adds a cycle per FP dep-chain copy). |
| 85 | RegisterAllocation | IMPLEMENTED | Level-dispatched: -O0/-Og spill-everywhere; -O1+ linear scan over live ranges from backwards-liveness dataflow over the emitted blocks (sound with loop backedges). Pools: callee-saved GPRs (rbx, r12-r15) for call-crossing ranges; caller-saved r10/r11 plus every isel-untouched argument register (rdx/rsi/rdi/r8/r9 — write-scanned, call-free functions only) for local ranges; xmm2..N where N adapts to the isel FP const pool watermark (the pool grows down from xmm15 for loop constants). Spill heuristic: density-ranked, never victimizes backedge-spanning (loop-carried) ranges. This session: operand folds run to fixpoint THROUGH killed instructions (two setup movs per consumer exposed the A-side only after the B-side died); snapshot stores [store s_mid<-acc] and FP const-materialization pairs are legal chain elements/gaps so pure-register recurrences fuse; self-update chains (a = a*c + d through the phi slot) fuse in place; single-use single-def home-store forwarding kills [movsd H<-S][consumer reads H] round trips (window scan blocks on ANY reference to H or the scratch S — setcc/movzx write AL/RAX and once slipped through); stack slot coloring (pass 28) shares rbp offsets; CmpRImm A-side folding drops the loop-guard setup mov. Exact liveness per slot via generation sub-intervals (multi-def phi slots split at redefinitions); expiry allows def-start handovers. Accumulator-chain fusion: rax/xmm0-threaded [load][op]+[store] chains rewrite into the result's register ([mov Z, X][op Z, src]), in place when phi-cycle coalescing (hint retarget validated on generation-disjoint exact liveness) lands the pair on one register — loop updates emit `addq $1, %r15` / `addsd %xmm4, %xmm3` directly. Phi copies between coalesced slots vanish. Soundness gates: result slot must not feed its own chain, no operand may share the result register post-coalescing, in-place only for phi-successor or non-loop-carried sources. Frame elision with SysV alignment; callee-save pushes deduplicated per physical register. |
| 86 | PostRACleanup | IMPLEMENTED | Fold store+load pairs (GP and FP) including same-register elision; slot-immediate to register-immediate forwarding. Same-register pairs also fold ACROSS the fused-guard gap ([store s,X][loads/compares/branches][load X,s] -> delete both): every gap instruction is read-only or loads a different register, so X stays live across the branches — kills the x*x+y*y spill round-trips in rotated short-circuit guards. |
| 87 | MachinePeephole | IMPLEMENTED | Fused compare-and-branch (cmp/jcc from setcc/movzx/test chains, incl. post-fold short chains and cross-block promoted booleans), loop-invariant FP constant hoisting, mov+test folding (register and cmp-$0,[mem] forms), rax accumulator folding, copy-chain elimination through scratch registers, adjacent mov-pair elimination, dead store removal, cmp-$0 to test, linear dead code after unconditional transfers (TCO zombie epilogues), LEA FORMATION ([mov R2,R1][add/sub $k,R2] -> [lea R2,k(R1)] and [mov R2,R1][shl $k<=3,R2] -> [lea R2,(,R1,2^k)] — IV updates and address arithmetic in one instruction, the shape every production compiler's counters take). |
| 88 | MachineLICM | IMPLEMENTED | Machine loop transforms, post-RA: (1) LOOP ROTATION for BOTH guard polarities — Form A [head: cond; jcc body][exit][body; jmp head] and Form B [head: cond; jcc EXIT][body...][jmp head] (the inlined/unrolled polarity: guard jumps out, body is fallthrough) rotate to [entry: jmp check][body...][check: cond; jcc'(inv) body] with the latch deleted; multi-jcc heads (fused short-circuit guards: [cmp][jcc exit][cmp][jcc body] in one block) rotate with the LAST jcc as the backedge and the earlier guards jumping into the moved exit segment. (2) Invariant FP-constant materialization hoisting out of backedge regions; after rotation the hoist lands before the entry shim so both fallthrough and jump entries execute it once. |
| 89 | DeoptMetadataEmission | SIMPLIFIED | Deopt manifest (JIT modes; empty guard set). |
