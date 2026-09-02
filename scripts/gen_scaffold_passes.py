#!/usr/bin/env python3
"""JULES pass-scaffold generator.

Single source of truth for the 89-pass catalog: generates the scaffold pass
files (the passes that are not yet implemented — every one honest about its
status), the docs/pass_status.md matrix, sources.cmake, and build.sh.
Hand-written pass files are never overwritten (existence check).

Run:  python3 scripts/gen_scaffold_passes.py [--force]
"""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "jules"))
PASSES = os.path.join(ROOT, "src/core/son/passes")

# number, ClassName, snake_name, phase, status, modes, purpose (from catalog)
CATALOG = [
    (1, "DeadNodeElimination", "dead_node_elimination", "Phase 0", "IMPLEMENTED", "all", "Remove unreachable nodes and unused pure ops."),
    (2, "IdentityCollapse", "identity_collapse", "Phase 0", "IMPLEMENTED", "all", "x+0, x*1, redundant casts."),
    (3, "ConstantFolding", "constant_folding", "Phase 0", "IMPLEMENTED", "all", "Evaluate pure ops with constant operands."),
    (4, "TypeCanonicalization", "type_canonicalization", "Phase 0", "IMPLEMENTED", "all", "Normalize widths, eliminate no-op conversions."),
    (5, "ComptimeResidueFold", "comptime_residue_fold", "Phase 0", "IMPLEMENTED", "aot", "Re-evaluate comptime expressions after specialization."),
    (6, "AssertElision", "assert_elision", "Phase 0", "VACUOUS", "all", "Remove checks proven safe (no check nodes in MVP IR)."),
    (7, "PhiSimplification", "phi_simplification", "Phase 0", "IMPLEMENTED", "all", "Collapse single-input phis, identical inputs."),
    (8, "SparseConditionalConstantPropagation", "sccp", "Phase 1", "IMPLEMENTED", "all", "Lattice const prop through control edges."),
    (9, "GlobalValueNumbering", "gvn", "Phase 1", "IMPLEMENTED", "all", "Hash-cons identical computations (dominance-checked)."),
    (10, "RedundantPhiElimination", "redundant_phi_elimination", "Phase 1", "IMPLEMENTED", "all", "Remove phis with identical/single definitions."),
    (11, "CopyPropagation", "copy_propagation", "Phase 1", "IMPLEMENTED", "all", "Replace uses of copies with originals."),
    (12, "AlgebraicSimplification", "algebraic_simplification", "Phase 1", "IMPLEMENTED", "all", "Multi-pattern algebra with side conditions."),
    (13, "Reassociation", "reassociation", "Phase 1", "IMPLEMENTED", "all", "Canonical operand order (FP untouched)."),
    (14, "SignExtensionElimination", "sign_extension_elimination", "Phase 1", "IMPLEMENTED", "all", "Remove redundant extends via width analysis."),
    (15, "NarrowingTransform", "narrowing_transform", "Phase 1", "SCAFFOLD", "all", "Demote ops to narrower types when consumers allow."),
    (16, "BitwiseOptimization", "bitwise_optimization", "Phase 1", "IMPLEMENTED", "all", "Bit-level rewrites, bitfield extracts, De Morgan."),
    (17, "SelectOptimization", "select_optimization", "Phase 1", "IMPLEMENTED", "all", "Select chains, condition inversion."),
    (18, "OverflowCheckElimination", "overflow_check_elimination", "Phase 1", "VACUOUS", "all", "Remove overflow checks via range lattices (no check nodes yet)."),
    (19, "AliasAnalysisInitialization", "alias_analysis_initialization", "Phase 2", "IMPLEMENTED", "all", "Build type-based, provenance AA structures."),
    (20, "MemoryDependenceAnalysis", "memory_dependence_analysis", "Phase 2", "IMPLEMENTED", "all", "Compute store->load dependencies."),
    (21, "RedundantLoadElimination", "redundant_load_elimination", "Phase 2", "IMPLEMENTED", "all", "Remove loads satisfied by dominating loads."),
    (22, "StoreToLoadForwarding", "store_to_load_forwarding", "Phase 2", "IMPLEMENTED", "all", "Forward store values to loads."),
    (23, "DeadStoreElimination", "dead_store_elimination", "Phase 2", "IMPLEMENTED", "all", "Remove overwritten/unread stores."),
    (24, "StoreMerging", "store_merging", "Phase 2", "SCAFFOLD", "all", "Combine adjacent narrow stores (alignment-aware)."),
    (25, "LoadHoisting", "load_hoisting", "Phase 2", "IMPLEMENTED", "all", "Hoist provably-safe loads out of loops."),
    (26, "ScalarReplacementOfAggregates", "sroa", "Phase 2", "IMPLEMENTED", "all", "Promote memory-backed locals to SSA values."),
    (27, "BitfieldLowering", "bitfield_lowering", "Phase 2", "SCAFFOLD", "all", "Lower bitfield access to mask/shift (needs bitfield types)."),
    (28, "StackSlotColoring", "stack_slot_coloring", "Phase 2", "SCAFFOLD", "all", "Merge non-overlapping stack slots (post-SROA)."),
    (29, "HeapToStackPromotion", "heap_to_stack_promotion", "Phase 2", "IMPLEMENTED", "all", "Promote NoEscape allocations to stack slots."),
    (30, "MemorySSARepair", "memory_ssa_repair", "Phase 2", "IMPLEMENTED", "all", "Restore memory chains after aggressive transforms."),
    (31, "PointsToAnalysis", "points_to_analysis", "Phase 3", "IMPLEMENTED", "all", "Flow-insensitive points-to graph."),
    (32, "EscapeAnalysis", "escape_analysis", "Phase 3", "IMPLEMENTED", "all", "NoEscape/ArgEscape/GlobalEscape classification."),
    (33, "PartialEscapeAnalysis", "partial_escape_analysis", "Phase 3", "SCAFFOLD", "jit", "Path-sensitive escape refinement (JIT)."),
    (34, "AllocationSiteProfiling", "allocation_site_profiling", "Phase 3", "SCAFFOLD", "jit", "Per-site escape behavior instrumentation (JIT)."),
    (35, "MaterializationPointInsertion", "materialization_point_insertion", "Phase 3", "SCAFFOLD", "jit", "Lazy materialization points for PEA (JIT)."),
    (36, "LockElision", "lock_elision", "Phase 3", "SCAFFOLD", "all", "Remove locks on non-escaping objects (needs sync ops)."),
    (37, "LoopDetection", "loop_detection", "Phase 4", "IMPLEMENTED", "all", "Natural loops via backedges; loop tree."),
    (38, "InductionVariableRecognition", "induction_variable_recognition", "Phase 4", "IMPLEMENTED", "all", "Basic/derived IVs, trip counts."),
    (39, "LoopClassification", "loop_classification", "Phase 4", "IMPLEMENTED", "all", "Counted/uncounted/early-exit/nested tagging."),
    (40, "LoopInvariantCodeMotion", "licm", "Phase 4", "IMPLEMENTED", "all", "Hoist invariant pure ops to preheaders."),
    (41, "LoadLICM", "load_licm", "Phase 4", "IMPLEMENTED", "all", "LICM for loads with alias proofs."),
    (42, "LoopUnrolling", "loop_unrolling", "Phase 4", "SCAFFOLD", "all", "Duplicate loop bodies (static heuristic)."),
    (43, "ProfileGuidedUnrolling", "profile_guided_unrolling", "Phase 4", "SCAFFOLD", "all", "Unroll from profiled trip counts."),
    (44, "LoopPeeling", "loop_peeling", "Phase 4", "SCAFFOLD", "all", "Extract first/last iterations."),
    (45, "LoopInterchange", "loop_interchange", "Phase 4", "SCAFFOLD", "all", "Swap nested loop order (dependence validation)."),
    (46, "LoopFusion", "loop_fusion", "Phase 4", "SCAFFOLD", "all", "Merge adjacent compatible loops."),
    (47, "LoopFission", "loop_fission", "Phase 4", "SCAFFOLD", "all", "Split loops for register pressure."),
    (48, "Predication", "predication", "Phase 4", "SCAFFOLD", "all", "Convert branches to selects (general form)."),
    (49, "IfConversion", "if_conversion", "Phase 4", "IMPLEMENTED", "all", "Effect-free if-diamonds to Select."),
    (50, "TailRecursionElimination", "tail_recursion_elimination", "Phase 4", "IMPLEMENTED", "all", "Self tail calls become entry jumps."),
    (51, "BranchProbabilityInference", "branch_probability_inference", "Phase 4", "IMPLEMENTED", "all", "Edge probabilities from heuristics."),
    (52, "HotPathStraightening", "hot_path_straightening", "Phase 4", "SIMPLIFIED", "all", "RPO layout with loop/hot fallthrough preference."),
    (53, "IdiomRecognition", "idiom_recognition", "Phase 5", "SCAFFOLD", "all", "memset/memcpy/popcount pattern detection."),
    (54, "SLPVectorizer", "slp_vectorizer", "Phase 5", "SCAFFOLD", "all", "Bottom-up adjacent scalar packing."),
    (55, "SuperwordPacker", "superword_packer", "Phase 5", "SCAFFOLD", "all", "Non-adjacent packing via packing graph (research)."),
    (56, "LoopVectorizer", "loop_vectorizer", "Phase 5", "SCAFFOLD", "all", "Top-down vectorization of counted loops."),
    (57, "OuterLoopVectorizer", "outer_loop_vectorizer", "Phase 5", "SCAFFOLD", "all", "Vectorize outer nest when inner is short."),
    (58, "InterleavedAccessRecognition", "interleaved_access_recognition", "Phase 5", "SCAFFOLD", "all", "Strided AoS patterns to shuffle-free loads."),
    (59, "ReductionRecognizer", "reduction_recognizer", "Phase 5", "SIMPLIFIED", "all", "Sum/min/max/dot patterns (analysis only)."),
    (60, "VectorLegalization", "vector_legalization", "Phase 5", "SCAFFOLD", "all", "Widen/narrow/split vectors to ISA."),
    (61, "MaskGeneration", "mask_generation", "Phase 5", "SCAFFOLD", "all", "Predicates for conditional vector execution."),
    (62, "ShuffleOptimization", "shuffle_optimization", "Phase 5", "SCAFFOLD", "all", "Minimize SLP/Superword shuffles."),
    (63, "AutoSOATransform", "auto_soa_transform", "Phase 5", "SCAFFOLD", "all", "AoS->SoA restructuring (high-risk)."),
    (64, "VectorCostModelEvaluation", "vector_cost_model_evaluation", "Phase 5", "SCAFFOLD", "all", "Profitability oracle for vectorization."),
    (65, "SIMDIntrinsicMatching", "simd_intrinsic_matching", "Phase 5", "SCAFFOLD", "all", "Map idioms to FMA/gather/... intrinsics."),
    (66, "VectorWidthSelection", "vector_width_selection", "Phase 5", "SCAFFOLD", "all", "Optimal vector width per region."),
    (67, "ClassHierarchyAnalysis", "class_hierarchy_analysis", "Phase 6", "SIMPLIFIED", "all", "Possible call targets via hierarchy (empty in MVP)."),
    (68, "StaticDevirtualization", "static_devirtualization", "Phase 6", "VACUOUS", "all", "Resolve dyn calls via CHA (no dyn dispatch in MVP)."),
    (69, "SpeculativeDevirtualization", "speculative_devirtualization", "Phase 6", "SCAFFOLD", "jit", "Profile-guided guarded direct calls (JIT)."),
    (70, "InlineCacheInsertion", "inline_cache_insertion", "Phase 6", "SCAFFOLD", "jit", "Mono/poly/mega IC stubs (JIT)."),
    (71, "GuardInsertion", "guard_insertion", "Phase 6", "SCAFFOLD", "jit", "Assumption-checking guards (JIT)."),
    (72, "GuardHoisting", "guard_hoisting", "Phase 6", "SCAFFOLD", "jit", "Move guards to cold points (JIT)."),
    (73, "GuardWeakening", "guard_weakening", "Phase 6", "SCAFFOLD", "jit", "Relax guard conditions (JIT)."),
    (74, "GuardMerging", "guard_merging", "Phase 6", "SCAFFOLD", "jit", "Combine redundant guards (JIT)."),
    (75, "AssumptionTracking", "assumption_tracking", "Phase 6", "SIMPLIFIED", "all", "Assumption registry per compiled version."),
    (76, "InliningCostModel", "inlining_cost_model", "Phase 7", "IMPLEMENTED", "all", "Per-site benefit scores."),
    (77, "AlwaysInlineEnforcement", "always_inline_enforcement", "Phase 7", "IMPLEMENTED", "all", "Force-inline annotated/trivial functions."),
    (78, "CostBasedInlining", "cost_based_inlining", "Phase 7", "IMPLEMENTED", "all", "Threshold-driven inlining with budget."),
    (79, "RecursiveInliningBounding", "recursive_inlining_bounding", "Phase 7", "IMPLEMENTED", "all", "Anti-explosion policy for recursive inlining."),
    (80, "ArgumentSpecialization", "argument_specialization", "Phase 7", "IMPLEMENTED", "all", "Constant-arg calls get priority inlining."),
    (81, "ClosureInlining", "closure_inlining", "Phase 8", "VACUOUS", "all", "Inline closure bodies + environment (no closures in MVP)."),
    (82, "LTOSummaryGeneration", "lto_summary_generation", "Phase 7", "SCAFFOLD", "aot", "Serialize cross-module info for LTO (AOT)."),
    (83, "CFGLinearization", "cfg_linearization", "Phase 8", "IMPLEMENTED", "all", "SoN -> ordered blocks with scheduled nodes."),
    (84, "InstructionSelection", "instruction_selection", "Phase 8", "IMPLEMENTED", "all", "x86-64 MIR emission (SysV)."),
    (85, "RegisterAllocation", "register_allocation", "Phase 8", "SIMPLIFIED", "all", "Spill-everywhere slots (linear-scan is the upgrade)."),
    (86, "PostRACleanup", "post_ra_cleanup", "Phase 8", "SIMPLIFIED", "all", "Fold spills, eliminate moves."),
    (87, "MachinePeephole", "machine_peephole", "Phase 8", "SIMPLIFIED", "all", "test-vs-cmp, self-move elimination."),
    (88, "MachineLICM", "machine_licm", "Phase 8", "SIMPLIFIED", "all", "Post-isel invariant hoisting (scan phase)."),
    (89, "DeoptMetadataEmission", "deopt_metadata_emission", "Phase 8", "SIMPLIFIED", "all", "Deopt manifest (JIT modes; empty guard set)."),
]

MODES = {"all": "kModeAll", "aot": "kModeAOT", "jit": "kModeJitBaseline | kModeJitOptimizing"}

STATUS_DOC = {
    "IMPLEMENTED": "Real transform/analysis operating on the SoN graph or MIR.",
    "SIMPLIFIED": "Real but reduced: core mechanism present, documented reductions.",
    "VACUOUS": "Complete for the current IR: the constructs it targets do not exist in the MVP subset.",
    "SCAFFOLD": "Not yet implemented: contract, modes and telemetry in place; honest no-op.",
}

TEMPLATE = """// Pass {num} — {cls} ({phase})
//
// PURPOSE: {purpose}
// STATUS: SCAFFOLD — not yet implemented. This file is the declared contract
// (ordering, modes, analyses, kill switch, telemetry). It runs as a
// check-only no-op and never claims a transformation it did not perform.
// The design notes below describe the intended mechanism.
//
// DESIGN NOTES:
{notes}
#include "core/son/passes/pass_utils.h"

namespace jules {{

class {cls}Pass : public Pass {{
public:
    const char* name() const override {{ return "{cls}"; }}
    int order() const override {{ return {num}; }}
    const char* phase_name() const override {{ return "{phase}"; }}
    ModeMask modes() const override {{ return {modes}; }}
    bool run(PassContext& ctx) override {{
        // Scaffold: validate preconditions, record telemetry, change nothing.
        bool preconditions = true;
        for (FunctionGraph& fg : ctx.mod.fns) {{
            if (fg.g.live_count() == 0) preconditions = false;
        }}
        (void)preconditions; // telemetry hook
        return false;
    }}
}};

JULES_REGISTER_PASS({cls}Pass, {num}, "{phase}")

}} // namespace jules
"""

NOTES = {
    15: "//   * track trunc users feeding narrow consumers; rewrite op width\n//   * requires the narrow-type legality check against all uses",
    18: "//   * needs Check nodes (arithmetic overflow) in the IR first\n//   * then: range lattice per IV, elide in-range checks",
    24: "//   * find stores to consecutive addresses of the same base\n//   * verify alignment, merge into a single wide store",
    27: "//   * requires bitfield types (language milestone 2)\n//   * lowers field access to (x >> off) & mask sequences before SROA",
    28: "//   * compute live ranges of frame slots post-SROA\n//   * graph-color non-overlapping ranges onto shared slots",
    33: "//   * path-sensitive escape states with materialization at branch joins\n//   * consumes allocation-site profiles (pass 34)",
    34: "//   * instrument alloc sites with counters + escape flags\n//   * profile format shared with pass 69's type profiles",
    35: "//   * on non-escaping paths, keep the object virtual\n//   * emit materialization recipes into deopt metadata (pass 89)",
    36: "//   * requires synchronization ops in the IR (monitorenter/exit)\n//   * elide when receiver proven non-escaping",
    42: "//   * duplicate the loop body subgraph k times, rewiring header phis\n//   * guarded by loop classification (pass 39) + cost model",
    43: "//   * same shape as 42 but trip-count distribution from profiles",
    44: "//   * clone the first iteration out of the loop; peel-phi alignment",
    45: "//   * requires dependence vectors between nests (needs arrays)",
    46: "//   * adjacent loops with identical bounds and no aliasing writes",
    47: "//   * split by def-use clusters; guided by register pressure",
    48: "//   * general branch-to-select in loops (if-conversion is the diamond case)",
    53: "//   * recognize zero-fill loops -> memset; copy loops -> memcpy\n//   * popcount via CL once patterns land",
    54: "//   * pack isomorphic scalar ops in adjacent statements bottom-up\n//   * cost model gate, then vector ops in the IR",
    55: "//   * packing graph over independent scalars (not just adjacent)\n//   * research pass: Superword (See et al.) style",
    56: "//   * counted loop + reduction + contiguous access -> vector body\n//   * legality: no cross-iteration deps except the reduction",
    57: "//   * when inner nest is short, vectorize the outer instead",
    58: "//   * stride-2/4 AoS loads -> wide load + deinterleave shuffles",
    59: "//   * detect phi = phi op x inside loops; tag sum/min/max/dot\n//   * feeds the (future) horizontal reduction ops",
    60: "//   * map packed widths to target ISA (128/256/512-bit)\n//   * split/concat legalizations",
    61: "//   * active-mask per lane; mask registers post-legality",
    62: "//   * cancel/merge shuffle pairs; move shuffles to cold edges",
    63: "//   * whole-program AoS->SoA restructure; needs struct types\n//   * profitability from access-pattern profiles",
    64: "//   * cost tables per target; reject unprofitable packs",
    65: "//   * match FMA/gather/permute idioms to intrinsics post-vectorization",
    66: "//   * region-level width choice from target + register pressure",
    69: "//   * type profile per call site; guard + direct call when hot\n//   * deopt on violation lands in the fallback variant",
    70: "//   * emit IC stub shapes (mono/poly/mega) at call sites",
    71: "//   * emit Check nodes feeding pass 89's manifest",
    72: "//   * LICM over guard nodes once they exist in the IR",
    73: "//   * equality guard -> range guard when ranges suffice",
    74: "//   * same-value guards within a dominator subtree merge",
    82: "//   * serialize fn sizes/const-args/inline hints for link-time inlining",
}

STATUS_LABEL = {
    "IMPLEMENTED": "IMPLEMENTED",
    "SIMPLIFIED": "SIMPLIFIED",
    "VACUOUS": "VACUOUS (nothing to do in MVP)",
    "SCAFFOLD": "SCAFFOLD (honest no-op)",
}


def main():
    force = "--force" in sys.argv
    made = []
    for num, cls, snake, phase, status, modes, purpose in CATALOG:
        if status != "SCAFFOLD":
            continue  # hand-written file already exists
        path = os.path.join(PASSES, "p%02d_%s.cpp" % (num, snake))
        if os.path.exists(path) and not force:
            continue
        text = TEMPLATE.format(num=num, cls=cls, phase=phase, purpose=purpose,
                               modes=MODES[modes], notes=NOTES.get(num, "//   * (design notes to be added with the implementation)"))
        with open(path, "w") as f:
            f.write(text)
        made.append(os.path.basename(path))
    print("generated %d scaffold pass files" % len(made))

    # ---- pass_status.md -----------------------------------------------------------
    rows = ["| # | Pass | Status | Notes |", "|---|------|--------|-------|"]
    for num, cls, snake, phase, status, modes, purpose in CATALOG:
        file_exists = os.path.exists(os.path.join(PASSES, "p%02d_%s.cpp" % (num, snake)))
        rows.append("| %d | %s | %s | %s |" % (
            num, cls, STATUS_LABEL[status],
            purpose + ("" if file_exists else " **FILE MISSING**")))
    body = "# 89-Pass Status Matrix\n\n"
    body += "Legend: " + "; ".join("`%s` — %s" % (k, v) for k, v in STATUS_DOC.items()) + "\n\n"
    body += "\n".join(rows) + "\n"
    with open(os.path.join(ROOT, "docs/pass_status.md"), "w") as f:
        f.write(body)
    print("wrote docs/pass_status.md")

    # ---- sources.cmake ---------------------------------------------------------------
    srcs = []
    for root, _, files in os.walk(os.path.join(ROOT, "src")):
        for fn in sorted(files):
            if fn.endswith(".cpp"):
                srcs.append(os.path.relpath(os.path.join(root, fn), ROOT))
    srcs.sort()
    cmake = "# Generated by scripts/gen_scaffold_passes.py — deterministic source list.\nset(JULES_SOURCES\n"
    for s in srcs:
        cmake += "    %s\n" % s
    cmake += ")\n"
    with open(os.path.join(ROOT, "sources.cmake"), "w") as f:
        f.write(cmake)
    print("wrote sources.cmake (%d sources)" % len(srcs))

    total = len([f for f in os.listdir(PASSES)
                 if f.startswith("p") and f.endswith(".cpp") and f != "pass_manager.cpp"])
    print("total pass files present: %d (target 89)" % total)
    if total != 89:
        print("ERROR: pass file count mismatch", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
