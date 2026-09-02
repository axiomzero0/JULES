# 89-Pass Status Matrix

Legend: `IMPLEMENTED` — Real transform/analysis operating on the SoN graph or MIR.; `SIMPLIFIED` — Real but reduced: core mechanism present, documented reductions.; `VACUOUS` — Complete for the current IR: the constructs it targets do not exist in the MVP subset.; `SCAFFOLD` — Not yet implemented: contract, modes and telemetry in place; honest no-op.

| # | Pass | Status | Notes |
|---|------|--------|-------|
| 1 | DeadNodeElimination | IMPLEMENTED | Remove unreachable nodes and unused pure ops. |
| 2 | IdentityCollapse | IMPLEMENTED | x+0, x*1, redundant casts. |
| 3 | ConstantFolding | IMPLEMENTED | Evaluate pure ops with constant operands. |
| 4 | TypeCanonicalization | IMPLEMENTED | Normalize widths, eliminate no-op conversions. |
| 5 | ComptimeResidueFold | IMPLEMENTED | Re-evaluate comptime expressions after specialization. |
| 6 | AssertElision | VACUOUS (nothing to do in MVP) | Remove checks proven safe (no check nodes in MVP IR). |
| 7 | PhiSimplification | IMPLEMENTED | Collapse single-input phis, identical inputs. |
| 8 | SparseConditionalConstantPropagation | IMPLEMENTED | Lattice const prop through control edges; Const lattice is pre-seeded (block-independent) and unresolved branches fall back to both-executable. Fires in the post-inline cleanup re-run: locals-as-memory hides values behind Loads until forwarding/promotion (passes 21-26), so SCCP's original slot at order 8 sees only Bottom loads on MVP input. |
| 9 | GlobalValueNumbering | IMPLEMENTED | Hash-cons identical computations (dominance-checked). |
| 10 | RedundantPhiElimination | IMPLEMENTED | Remove phis with identical/single definitions. |
| 11 | CopyPropagation | IMPLEMENTED | Replace uses of copies with originals. |
| 12 | AlgebraicSimplification | IMPLEMENTED | Multi-pattern algebra with side conditions. |
| 13 | Reassociation | IMPLEMENTED | Canonical operand order (FP gated by --fp=fast). |
| 14 | SignExtensionElimination | IMPLEMENTED | Remove redundant extends via width analysis. |
| 15 | NarrowingTransform | SCAFFOLD (honest no-op) | Demote ops to narrower types when consumers allow. |
| 16 | BitwiseOptimization | IMPLEMENTED | Bit-level rewrites, bitfield extracts, De Morgan. |
| 17 | SelectOptimization | IMPLEMENTED | Select chains, condition inversion. |
| 18 | OverflowCheckElimination | VACUOUS (nothing to do in MVP) | Remove overflow checks via range lattices (no check nodes yet). |
| 19 | AliasAnalysisInitialization | IMPLEMENTED | Build type-based, provenance AA structures. |
| 20 | MemoryDependenceAnalysis | IMPLEMENTED | Compute store->load dependencies. |
| 21 | RedundantLoadElimination | IMPLEMENTED | Remove loads satisfied by dominating loads. |
| 22 | StoreToLoadForwarding | IMPLEMENTED | Forward store values to loads. |
| 23 | DeadStoreElimination | IMPLEMENTED | Remove overwritten/unread stores. |
| 24 | StoreMerging | SCAFFOLD (honest no-op) | Combine adjacent narrow stores (alignment-aware). |
| 25 | LoadHoisting | IMPLEMENTED | Hoist provably-safe loads out of loops. |
| 26 | ScalarReplacementOfAggregates | IMPLEMENTED | Promote memory-backed locals to SSA values. Promotion also treats memory-phi users as chain uses (not escapes) and ignores dead users, so loop counters/temps promote even when earlier passes killed their consumers. |
| 27 | BitfieldLowering | SCAFFOLD (honest no-op) | Lower bitfield access to mask/shift (needs bitfield types). |
| 28 | StackSlotColoring | SCAFFOLD (honest no-op) | Merge non-overlapping stack slots (post-SROA). |
| 29 | HeapToStackPromotion | IMPLEMENTED | Promote NoEscape allocations to stack slots. |
| 30 | MemorySSARepair | IMPLEMENTED | Restore memory chains after aggressive transforms. |
| 31 | PointsToAnalysis | IMPLEMENTED | Flow-insensitive points-to graph. |
| 32 | EscapeAnalysis | IMPLEMENTED | NoEscape/ArgEscape/GlobalEscape classification. |
| 33 | PartialEscapeAnalysis | SCAFFOLD (honest no-op) | Path-sensitive escape refinement (JIT). |
| 34 | AllocationSiteProfiling | SCAFFOLD (honest no-op) | Per-site escape behavior instrumentation (JIT). |
| 35 | MaterializationPointInsertion | SCAFFOLD (honest no-op) | Lazy materialization points for PEA (JIT). |
| 36 | LockElision | SCAFFOLD (honest no-op) | Remove locks on non-escaping objects (needs sync ops). |
| 37 | LoopDetection | IMPLEMENTED | Natural loops via backedges; loop tree. |
| 38 | InductionVariableRecognition | IMPLEMENTED | Basic/derived IVs, trip counts. |
| 39 | LoopClassification | IMPLEMENTED | Counted/uncounted/early-exit/nested tagging. |
| 40 | LoopInvariantCodeMotion | IMPLEMENTED | Hoist invariant pure ops to preheaders. |
| 41 | LoadLICM | IMPLEMENTED | LICM for loads with alias proofs. |
| 42 | LoopUnrolling | SCAFFOLD (honest no-op) | Duplicate loop bodies (static heuristic). |
| 43 | ProfileGuidedUnrolling | SCAFFOLD (honest no-op) | Unroll from profiled trip counts. |
| 44 | LoopPeeling | SCAFFOLD (honest no-op) | Extract first/last iterations. |
| 45 | LoopInterchange | SCAFFOLD (honest no-op) | Swap nested loop order (dependence validation). |
| 46 | LoopFusion | SCAFFOLD (honest no-op) | Merge adjacent compatible loops. |
| 47 | LoopFission | SCAFFOLD (honest no-op) | Split loops for register pressure. |
| 48 | Predication | SCAFFOLD (honest no-op) | Convert branches to selects (general form). |
| 49 | IfConversion | IMPLEMENTED | Effect-free if-diamonds to Select. |
| 50 | TailRecursionElimination | IMPLEMENTED | Self tail calls become entry jumps. |
| 51 | BranchProbabilityInference | IMPLEMENTED | Edge probabilities from heuristics. |
| 52 | HotPathStraightening | SIMPLIFIED | RPO layout with loop/hot fallthrough preference. |
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
| 83 | CFGLinearization | IMPLEMENTED | SoN -> ordered blocks with scheduled nodes. |
| 84 | InstructionSelection | IMPLEMENTED | x86-64 MIR emission (SysV). |
| 85 | RegisterAllocation | IMPLEMENTED | Level-dispatched: -O0/-Og spill-everywhere; -O1+ linear scan over live ranges derived from backwards-liveness dataflow over the emitted blocks (sound with loop backedges). Pools: callee-saved GPRs (rbx, r12-r15) for call-crossing ranges, caller-saved r10/r11 + xmm2-13 for local ranges, xmm14-15 reserved for the isel FP const pool. Spill heuristic: density-ranked, never victimizes backedge-spanning (loop-carried) ranges; a spilled value degenerates to memory operands. Frame elision (omit frame pointer) when no value needs a stack slot, with SysV call-alignment padding. |
| 86 | PostRACleanup | IMPLEMENTED | Fold store+load pairs (GP and FP) including same-register elision; slot-immediate to register-immediate forwarding. |
| 87 | MachinePeephole | IMPLEMENTED | Fused compare-and-branch (cmp/jcc from setcc/movzx/test chains, incl. post-fold short chains), loop-invariant FP constant hoisting (isel const pool materializations), mov+test folding (register and cmp-$0,[mem] forms), rax accumulator folding, copy-chain elimination through scratch registers, adjacent mov-pair elimination, dead store removal, cmp-$0 to test. |
| 88 | MachineLICM | SIMPLIFIED | Post-isel invariant hoisting (scan phase). |
| 89 | DeoptMetadataEmission | SIMPLIFIED | Deopt manifest (JIT modes; empty guard set). |
