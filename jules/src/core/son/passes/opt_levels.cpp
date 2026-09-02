// Optimization levels: implementation of the availability matrix and budgets.
// See opt_levels.h for the design contract.
#include "core/son/passes/opt_levels.h"

#include <string>

namespace jules {

const char* opt_level_name(OptLevel l) {
    switch (l) {
        case OptLevel::O0: return "-O0";
        case OptLevel::Og: return "-Og";
        case OptLevel::O1: return "-O1";
        case OptLevel::O2: return "-O2";
        case OptLevel::O3: return "-O3";
        case OptLevel::Os: return "-Os";
        case OptLevel::Oz: return "-Oz";
    }
    return "-O2";
}

bool parse_opt_level(const std::string& s, OptLevel& out) {
    // Accept both the flag form "-O2" and the bare form "O2" / "2".
    std::string t = s;
    if (!t.empty() && t[0] == '-') t = t.substr(1);
    if (t.size() >= 2 && (t[0] == 'O' || t[0] == 'o')) {
        std::string r = t.substr(1);
        if (r == "0") { out = OptLevel::O0; return true; }
        if (r == "g" || r == "G") { out = OptLevel::Og; return true; }
        if (r == "1") { out = OptLevel::O1; return true; }
        if (r == "2") { out = OptLevel::O2; return true; }
        if (r == "3") { out = OptLevel::O3; return true; }
        if (r == "s" || r == "S") { out = OptLevel::Os; return true; }
        if (r == "z" || r == "Z") { out = OptLevel::Oz; return true; }
    }
    if (t == "0") { out = OptLevel::O0; return true; }
    if (t == "1") { out = OptLevel::O1; return true; }
    if (t == "2") { out = OptLevel::O2; return true; }
    if (t == "3") { out = OptLevel::O3; return true; }
    return false;
}

// ---- availability matrix ---------------------------------------------------
//
// Encoded per catalog order. Row identifiers reference the spec's §7 tables;
// passes the spec does not list inherit their phase's default row. The tier
// letters map: Off = ✗, Limited = ~, On = ✓, Aggressive = ✓✓.
//
// Distinct rows used by the spec (7 columns: O0 Og O1 O2 O3 Os Oz):
//   A: Off Off Off  On Aggr On  Limited   (SCCP, GVN — "~" at Og/O1, "~" Oz)
//   B: Off On  On   On On  On  On         (basic scalar cleanup)
//   C: Off Off Lim  On Aggr On  Lim       (memory/loop heavy opts)
//   D: Off Off Off  On On  Lim Off        (code-growing transforms)
//   E: Off Lim On   On On  On  Lim        (O1-grade scalar opts)
//   R: On  On  On   On On  On  On         (required lowering, always on)

namespace {

constexpr int O = static_cast<int>(OptLevel::O0);
constexpr int G = static_cast<int>(OptLevel::Og);
constexpr int N1 = static_cast<int>(OptLevel::O1);
constexpr int N2 = static_cast<int>(OptLevel::O2);
constexpr int N3 = static_cast<int>(OptLevel::O3);
constexpr int S = static_cast<int>(OptLevel::Os);
constexpr int Z = static_cast<int>(OptLevel::Oz);

enum T : u8 { X = 0, L = 1, Y = 2, A = 3 }; // Off, Limited, On, Aggressive

struct Row {
    u8 t[kOptLevelCount];
};

// Row prototypes from the spec tables.
constexpr Row kRowScalar1{{X, Y, Y, Y, Y, Y, Y}};   // dead-code/identity/fold class
constexpr Row kRowScalarA{{X, L, L, Y, A, Y, L}};   // SCCP, GVN
constexpr Row kRowScalarE{{X, X, L, Y, Y, Y, L}};   // reassoc-class
constexpr Row kRowMemC{{X, X, L, Y, A, Y, L}};      // alias/dse/sroa-class
constexpr Row kRowMemLite{{X, X, L, Y, Y, Y, Y}};   // store merging, slot coloring
constexpr Row kRowEscapeC{{X, X, L, Y, A, Y, L}};   // points-to/escape
constexpr Row kRowLoopDetect{{X, L, Y, Y, Y, Y, L}}; // loop detection family
constexpr Row kRowLoopC{{X, X, L, Y, A, Y, L}}      // LICM class
;
constexpr Row kRowLoopD{{X, X, L, Y, Y, L, X}};     // unrolling-class (size-growing)
constexpr Row kRowLoopE{{X, X, L, Y, Y, Y, L}};     // peeling/fusion
constexpr Row kRowVec{{X, X, X, Y, A, L, X}};       // vectorizer family
constexpr Row kRowVecLite{{X, L, L, Y, Y, Y, L}};   // idiom recognition, simd match
constexpr Row kRowDevirt{{X, L, Y, Y, Y, Y, L}};    // static devirt family
constexpr Row kRowSpec{{X, X, X, L, Y, L, X}};      // speculative family
constexpr Row kRowInlineC{{X, L, L, Y, A, L, X}};   // cost-based inlining
constexpr Row kRowInlineLite{{X, L, Y, Y, Y, Y, L}}; // closure/recursive bounding
constexpr Row kRowMach{{X, L, Y, Y, Y, Y, L}};      // post-RA cleanup
constexpr Row kRowPeep{{X, L, Y, Y, Y, Y, L}};      // machine peephole
constexpr Row kRowRequired{{Y, Y, Y, Y, Y, Y, Y}};  // required lowering
constexpr Row kRowAlways{{L, Y, Y, Y, Y, Y, Y}};    // always-inline (semantic)

// One entry per catalog order 1..89; nullptr => phase default (kRowScalar1
// capped by level: nothing but required lowering runs at -O0).
const Row* kMatrix[90] = {};

void init_matrix() {
    struct Spec {
        int order;
        const Row* row;
    };
    static const Spec kSpec[] = {
        // Phase 0: frontend residue cleanup
        {1, &kRowScalar1},   // DeadNodeElimination (~ at O0)
        {2, &kRowRequired},  // IdentityCollapse (✗ O0 handled below)
        {3, &kRowRequired},  // ConstantFolding
        {4, &kRowRequired},  // TypeCanonicalization: required lowering
        {5, &kRowRequired},  // ComptimeResidueFold: required lowering
        {6, nullptr},        // AssertElision: ✗✗~✓✓~✗ (vacuous in MVP)
        {7, &kRowRequired},  // PhiSimplification: required
        // Phase 1: value and scalar optimization
        {8, &kRowScalarA},   // SCCP
        {9, &kRowScalarA},   // GVN
        {10, &kRowScalar1},  // RedundantPhiElimination
        {11, &kRowScalar1},  // CopyPropagation
        {12, &kRowScalar1},  // AlgebraicSimplification (✗ at O0, ~ Oz via row)
        {13, &kRowScalarE},  // Reassociation (FP gated by --fp)
        {14, &kRowScalarE},  // SignExtensionElimination
        {15, &kRowScalarE},  // NarrowingTransform
        {16, &kRowScalar1},  // BitwiseOptimization
        {17, nullptr},       // SelectOptimization: ✗~✓✓✓✓~
        {18, nullptr},       // OverflowCheckElimination (vacuous)
        // Phase 2: memory optimization
        {19, &kRowMemC},     // AliasAnalysisInitialization
        {20, &kRowMemC},     // MemoryDependenceAnalysis
        {21, &kRowMemC},     // RedundantLoadElimination
        {22, &kRowMemC},     // StoreToLoadForwarding
        {23, &kRowMemC},     // DeadStoreElimination
        {24, &kRowMemLite},  // StoreMerging
        {25, &kRowLoopD},    // LoadHoisting
        {26, &kRowMemC},     // SROA
        {27, &kRowRequired}, // BitfieldLowering: required lowering
        {28, &kRowMemLite},  // StackSlotColoring
        {29, &kRowMemC},     // HeapToStackPromotion
        {30, &kRowRequired}, // MemorySSARepair: required after memory transforms
        // Phase 3: escape and allocation analysis
        {31, &kRowEscapeC},  // PointsToAnalysis
        {32, &kRowEscapeC},  // EscapeAnalysis
        {33, &kRowSpec},     // PartialEscapeAnalysis (scaffold)
        {34, nullptr},       // AllocationSiteProfiling: JIT/profile only
        {35, nullptr},       // MaterializationPointInsertion: with PEA
        {36, nullptr},       // LockElision: if proven
        // Phase 4: loop analysis and transforms
        {37, &kRowLoopDetect}, // LoopDetection
        {38, &kRowLoopDetect}, // InductionVariableRecognition
        {39, &kRowLoopDetect}, // LoopClassification
        {40, &kRowLoopC},      // LICM
        {41, &kRowLoopC},      // LoadLICM
        {42, &kRowLoopD},      // LoopUnrolling
        {43, nullptr},         // ProfileGuidedUnrolling: with PGO
        {44, &kRowLoopE},      // LoopPeeling
        {45, nullptr},         // LoopInterchange: ✗✗✗~✓~✗
        {46, &kRowLoopE},      // LoopFusion
        {47, nullptr},         // LoopFission: ✗✗✗~✓✗✗
        {48, &kRowLoopD},      // Predication
        {49, &kRowLoopD},      // IfConversion
        {50, &kRowScalar1},    // TailRecursionElimination (✗ O0 via row)
        {51, &kRowLoopDetect}, // BranchProbabilityInference
        {52, &kRowLoopC},      // HotPathStraightening
        // Phase 5: vectorization and superword parallelism
        {53, &kRowVecLite},   // IdiomRecognition
        {54, &kRowVec},       // SLPVectorizer
        {55, &kRowVec},       // SuperwordPacker
        {56, &kRowVec},       // LoopVectorizer
        {57, &kRowVec},       // OuterLoopVectorizer
        {58, &kRowVecLite},   // InterleavedAccessRecognition
        {59, &kRowVecLite},   // ReductionRecognizer
        {60, &kRowRequired},  // VectorLegalization: if vectorizing
        {61, &kRowRequired},  // MaskGeneration: if vectorizing
        {62, &kRowVec},       // ShuffleOptimization
        {63, nullptr},        // AutoSOATransform: opt-in even at -O3
        {64, &kRowRequired},  // VectorCostModelEvaluation: if vectorizing
        {65, &kRowVecLite},   // SIMDIntrinsicMatching
        {66, &kRowVec},       // VectorWidthSelection
        // Phase 6: devirtualization and speculation
        {67, &kRowDevirt},    // ClassHierarchyAnalysis
        {68, &kRowDevirt},    // StaticDevirtualization
        {69, &kRowSpec},      // SpeculativeDevirtualization
        {70, nullptr},        // InlineCacheInsertion: JIT only
        {71, &kRowSpec},      // GuardInsertion: with speculation
        {72, &kRowSpec},      // GuardHoisting
        {73, &kRowSpec},      // GuardWeakening
        {74, &kRowSpec},      // GuardMerging
        {75, &kRowSpec},      // AssumptionTracking
        // Phase 7: inlining and interprocedural
        {76, &kRowRequired},  // InliningCostModel: required if inlining
        {77, &kRowAlways},    // AlwaysInlineEnforcement (source-mandated)
        {78, &kRowInlineC},   // CostBasedInlining
        {79, &kRowInlineLite},// RecursiveInliningBounding
        {80, &kRowInlineC},   // ArgumentSpecialization
        {81, &kRowInlineLite},// ClosureInlining
        {82, &kRowRequired},  // LTOSummaryGeneration: visibility modes
        // Phase 8: lowering and machine optimization
        {83, &kRowRequired},  // CFGLinearization: always
        {84, &kRowRequired},  // InstructionSelection: always
        {85, nullptr},       // RegisterAllocation: dispatches by level below
        {86, &kRowMach},      // PostRACleanup
        {87, &kRowPeep},      // MachinePeephole
        {88, &kRowLoopC},     // MachineLICM (scaffold)
        {89, nullptr},        // DeoptMetadataEmission: if speculation/JIT
    };
    static bool done = false;
    if (done) return;
    done = true;
    for (const Spec& s : kSpec)
        if (s.order >= 1 && s.order < 90) kMatrix[s.order] = s.row;
}

} // namespace

Avail pass_avail(int order, OptLevel lvl) {
    init_matrix();
    int li = static_cast<int>(lvl);
    const Row* row = nullptr;
    if (order >= 1 && order < 90) row = kMatrix[order];

    // Pass 85 is the register allocator: levels map to allocator grades
    // (spec: O0/Og simple, O1 linear scan, O2/O3 budgeted, Os/Oz linear).
    if (order == 85) {
        switch (lvl) {
            case OptLevel::O0:
            case OptLevel::Og: return Avail::Limited; // spill-everywhere (simple)
            default: return Avail::On;                // linear scan
        }
    }

    if (!row) {
        // Phase default: unlisted passes follow the scalar-cleanup class,
        // except nothing but required lowering runs at -O0.
        if (lvl == OptLevel::O0) return Avail::Off;
        if (lvl == OptLevel::Oz) return Avail::Limited;
        return Avail::On;
    }

    // Two spec-ordered exceptions the row prototypes cannot express:
    // IdentityCollapse/ConstantFolding are ✗ at -O0 (they are optimizations,
    // not lowering); DeadNodeElimination is ~ at -O0 (required cleanup).
    if (lvl == OptLevel::O0) {
        if (order == 2 || order == 3) return Avail::Off;
    }

    switch (row->t[li]) {
        case X: return Avail::Off;
        case L: return Avail::Limited;
        case A: return Avail::Aggressive;
        default: break;
    }
    return Avail::On;
}

// ---- budgets (spec §8) --------------------------------------------------------

LevelBudgets level_budgets(OptLevel lvl) {
    LevelBudgets b{};
    switch (lvl) {
        case OptLevel::O0:
        case OptLevel::Og:
            b.inline_threshold = 8;   // tiny: source-mandated sites only
            b.inline_budget = 24;
            b.cleanup_rounds = 0;
            b.unroll_factor = 1;
            b.size_biased = false;
            b.ra_registers = false;   // simple allocator (spill-everywhere)
            break;
        case OptLevel::O1:
            b.inline_threshold = 16;
            b.inline_budget = 50;     // kO1_InlineBudget
            b.cleanup_rounds = 1;
            b.unroll_factor = 1;      // no unrolling except trivial
            b.size_biased = false;
            b.ra_registers = true;    // linear scan
            break;
        case OptLevel::O2:
            b.inline_threshold = 24;
            b.inline_budget = 225;    // kO2_InlineBudget
            b.cleanup_rounds = 2;
            b.unroll_factor = 4;      // kO2_MaxUnrollFactor
            b.size_biased = false;
            b.ra_registers = true;    // linear scan + coalescing
            break;
        case OptLevel::O3:
            b.inline_threshold = 48;
            b.inline_budget = 600;    // kO3_InlineBudget
            b.cleanup_rounds = 3;     // kO3_FixpointIterations
            b.unroll_factor = 8;      // kO3_MaxUnrollFactor
            b.size_biased = false;
            b.ra_registers = true;
            break;
        case OptLevel::Os:
        case OptLevel::Oz:
            b.inline_threshold = lvl == OptLevel::Os ? 16 : 8;
            b.inline_budget = lvl == OptLevel::Os ? 100 : 40;
            b.cleanup_rounds = 1;
            b.unroll_factor = 1;      // minimal unrolling (size-first)
            b.size_biased = true;
            b.ra_registers = true;    // linear scan (size-biased reg choice)
            break;
    }
    return b;
}

OptLevel cap_level_for_jit(JitBudget b, OptLevel requested) {
    switch (b) {
        case JitBudget::Fast: // roughly -O1 to light -O2 with a strict budget
            return requested < OptLevel::O1 ? requested : OptLevel::O1;
        case JitBudget::Balanced: // roughly -O2 with profile specialization
            return requested < OptLevel::O2 ? requested : OptLevel::O2;
        case JitBudget::Peak:
            return requested;
    }
    return requested;
}

} // namespace jules
