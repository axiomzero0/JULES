// The JULES superoptimizer engine — target-agnostic search core.
//
// DESIGN CONTRACT (the reason this component exists as its own directory):
//
//   1. This file contains ZERO instruction knowledge. There is no opcode
//      name, no pattern, no "known win", no latency guess in here. Every
//      instruction fact lives in an IsaTable supplied by the target
//      (x86-64: src/targets/x86_64/x64_super_isa.cpp) as DATA: one row per
//      MIR opcode carrying its operand shapes, its variant axes (bin op /
//      condition / size / scale), its exact architectural semantics as a
//      simulate closure, its liveness effects, and its latency.
//
//   2. The search is a bounded Dijkstra over concrete machine states. Every
//      modeled location (GPR, window slot, flags) holds one value per test
//      vector, so a state IS the behavior of a candidate sequence on the
//      whole vector set. The start state is the window's live-in; the goal
//      test is exact equality with the original window's final state on the
//      live-out contract (computed by real backward liveness over the
//      label-region CFG of the function). The first goal state popped is
//      the cheapest sequence that is vector-equivalent; ties prefer shorter,
//      then enumeration order — fully deterministic.
//
//   3. Soundness structure:
//      * DEFINEDNESS RULE — a candidate instruction may only read locations
//        that are live-in to the window or written earlier in the candidate
//        program. A candidate therefore cannot depend on runtime garbage the
//        vectors did not sample. The original window is held to the same
//        rule (a window reading undefined locations is skipped, not
//        "optimized").
//      * FAULTS — a candidate that faults on any vector (divide by zero,
//        INT_MIN / -1) is discarded; if the ORIGINAL faults on any vector
//        the window is skipped entirely.
//      * CONTRACT — only locations live after the window (per CFG liveness)
//        must match; everything else is free. Dead windows reduce to the
//        empty sequence as the cost-0 goal: dead-code elimination falls out
//        of the search, it is not implemented.
//      * UNDEFINED FLAGS — flag writes the architecture leaves undefined
//        poison the flag lanes; a window whose ORIGINAL produces undefined
//        flags that are live-out is skipped.
//
//   4. HONEST LIMITS (documented, not hidden):
//      * Vector equivalence is SAMPLING, not proof: kVecSearch vectors
//        during search, a fresh kVecVerify re-check before commit, plus
//        the full regression suite as the behavioral net. The IsaTable is
//        exactly the artifact that makes an exhaustive (SMT/BMC) equivalence
//        check possible as a future verifier — the search core would not
//        change.
//      * The search is budget-bounded and incomplete BY DESIGN (pop-count
//        budgets, deterministic — never wall-clock). A window that exhausts
//        its budget keeps the original code. Missed wins are acceptable;
//        wrong code is not.
//      * Fingerprint dedup uses a 64-bit hash: a hash collision can lose a
//        win, never invent one (the goal test compares exact states).
//
//   5. Nothing here is user-visible policy: levels, budgets and telemetry
//      are knobs owned by the pass file (p92) and the environment, all with
//      single definitions and documented defaults.
#pragma once

#include "core/codegen/linear.h"

#include <array>
#include <vector>

namespace jules::superopt {

// ---- fixed engine capacities (compile-time; policies live in Opts) ------
constexpr u32 kMaxGpr = 15;     // rax..r15 (rbp/rsp are never window operands)
constexpr u32 kMaxSlot = 6;     // slots a window may touch
constexpr u32 kNF = 5;         // ZF SF OF CF PF
constexpr u32 kVecSearch = 12;  // test vectors per batch (search + verify
                                 // lanes; see the honesty note in .h §4)

// One concrete machine state: per-vector values for every modeled location.
// Flag lanes may hold POISON values (undefined-flag writes) which simply
// never compare equal to anything sane.
struct VState {
    i64 r[kVecSearch][kMaxGpr];
    i64 s[kVecSearch][kMaxSlot];
    i64 f[kVecSearch][kNF];
};

// Liveness/read-write effects of one concrete Inst. Produced by the ISA
// table; consumed by the liveness dataflow and the definedness rule.
// Slot ids are the function's ABSOLUTE slot ids (liveness is function-wide).
struct Effects {
    u32 gpr_read = 0;            // bit i = GPR index i
    u32 gpr_write = 0;
    bool reads_flags = false;
    bool writes_flags = false;    // defined write (kills flag liveness)
    bool undef_flags = false;    // undefined write (also kills; poisons sim)
    i32 slot_read = -1;          // -1 = none
    i32 slot_write = -1;
    bool slot_write_covers = false; // write is >= 8 bytes (kills slot liveness)
};

// Operand descriptor: one enumerable operand of an opcode. `pool` picks the
// candidate set ('R' register pool, 'S' slot pool, 'I' immediate pool,
// 'B' register-or-slot); `field` says WHERE the bound value lands —
// generic MIR structure, not opcode knowledge:
//   0 = operand a (whole: kind comes from the pool)
//   1 = operand b (whole: kind comes from the pool)
//   2 = b.slot = index-register enum (Lea2's SIB index; pool 'R')
//   3 = b.imm = displacement (lea family; pool 'I'; b.k untouched)
struct OpDef {
    char pool = '-';
    u8 field = 0;
};

// One ISA descriptor row: EVERYTHING the engine knows about one opcode.
//   sim != nullptr : the opcode is searchable (candidate alphabet)
//   sim == nullptr : boundary — windows break on it, but its Effects are
//                    still required for function-wide liveness.
struct Row {
    IOp op = IOp::Nop;
    i32 (*latency)(const Inst&) = nullptr; // cost units, target cost model

    OpDef ops[4];               // enumerable operands (see OpDef)
    u32 nops = 0;

    // Variant axes (data, supplied by the target; empty axis = fixed).
    const BinOp* bins = nullptr;
    u32 nbins = 0;
    const bool* sars = nullptr;   // parallel to bins (shift rows)
    const Cond* conds = nullptr;
    u32 nconds = 0;
    const u8* sizes = nullptr;    // operand-width variants; for the lea
    u32 nsizes = 0;               // family this is the SIB scale (1/2/4/8),
                                  // because it rides in Inst.size

    // Shape legality for a CONSTRUCTED candidate (imm range, shift count,
    // operand kinds this opcode actually accepts).
    bool (*valid)(const Inst&) = nullptr;

    // Exact architectural semantics over the vector state (window-local
    // slot remap: slots[k] = absolute id of state slot k). Returns false
    // when the instruction faults.
    bool (*sim)(VState&, const i32* slots, u32 nslots, const Inst&) = nullptr;

    // Read/write effects for liveness (absolute slot ids from the Inst).
    void (*effects)(const Inst&, Effects&) = nullptr;
};

typedef std::vector<Row> IsaTable;

// ---- Tier-4 final verification (the Z3 layer) ------------------------------
//
// The verification LADDER (strongest tier runs last, only on survivors):
//   Tier 1 — in-search vector equivalence (the goal test itself): every
//            candidate is refuted or kept for ~300ns on the batch-0 lanes.
//   Tier 2 — fresh-batch re-verification (the commit gate): 7x12 fresh
//            lanes re-check the winner before it may commit.
//   Tier 3 — the concrete cross-probe: when self-test is enabled, the
//            Tier-4 encoder re-evaluates the ORIGINAL window on the batch-0
//            seed values and must agree with the simulator bit-for-bit
//            (the differential lock binding the two semantic sources).
//   Tier 4 — SMT equivalence proof (Z3): the original window and the
//            candidate are encoded symbolically over the live-in contract;
//            Z3 proves equivalence on ALL inputs (not a sample). Refuted
//            candidates are rejected even though sampling passed them.
//
// SEMANTIC STRENGTHENING over sampling (deliberate, documented):
//   * faults are exact: a candidate that does not fault wherever the
//     original faults (or vice versa, on ANY input) is refuted;
//   * undefined flags are UNCONSTRAINED symbols, not the simulator's
//     deterministic per-lane poison — a candidate may not exploit
//     poison-value coincidences the sampled lanes cannot refute;
//   * entry flags are symbolic and SHARED between both sides (the runtime
//     value is the same for both; the proof covers every valuation);
//   * non-live-in locations (U_R/U_S) are single symbols SHARED by both
//     sides and unconstrained — the definedness rule makes both sides
//     write-before-read on them, so the sharing is sound, and the
//     cross-probe surfaces any violation (an unbound symbol leaking into
//     a contract term yields a model-dependent value and aborts).
//
// A Tier-4 verdict of Unknown (solver absent/timeout/unknown) degrades to
// the Tier-2 sampling verdict unless JULES_SUPEROPT_SMT=2 (proof required).

enum class Verdict { Proven, Refuted, Unknown };

struct Tier4Query {
    const LFunction* lf = nullptr; // the function (original window: [w0,w1))
    u32 w0 = 0, w1 = 0;
    const Inst* cand = nullptr;   // the replacement (may be null/empty = erase)
    u32 ncand = 0;
    const VState* seed = nullptr;       // batch-0 seed (probe values: lane 0)
    const VState* orig_final = nullptr; // original's batch-0 final state
    u32 livein_gpr = 0;   // bit i = GPR i is a symbolic input
    u32 livein_slots = 0; // bit k = window slot k is a symbolic input
    u32 contract_gpr = 0;   // bit i = GPR i must match at exit
    u32 contract_slots = 0; // bit k = window slot k must match at exit
    bool contract_flags = false;
    i32 slots[kMaxSlot] = {0}; // window slot k -> absolute slot id
    u32 nslots = 0;
};

// ---- budgets (single definitions; env-overridable at the pass layer) ----
struct Opts {
    // Pop budgets are DRAIN-sized: after the construction caps fire, a pop
    // is just simulate + fingerprint + goal check (microseconds), and the
    // goal for a depth-2 win sits behind every cost-1 state in the queue.
    u32 pops_per_window = 768;  // Dijkstra pops before giving up
    u32 pops_per_fn = 4096;     // total pop budget per function
    u32 candidates_per_window = 2048; // constructed candidates per window
    u32 candidates_per_pop = 800;  // construction budget per queue pop
    u32 states_per_window = 2048; // distinct fingerprints per window
    u32 max_window = 16;        // instructions in an optimizable window
    u32 max_growth = 2;         // candidate length may exceed original by this
    u32 reg_pool_cap = 8;       // live registers a window may involve
    u32 imm_pool_cap = 6;       // immediate candidates (window's own
                               // constants + {0} first, then corners)
    u32 slot_pool_cap = kMaxSlot;
    u32 max_slots_fn = 512;     // function slot count ceiling (liveness)
    u32 max_regions_fn = 4096;  // function region count ceiling

    // Tier-4 hooks (target-supplied; null = the tier is absent and the
    // ladder stops at Tier-2 sampling). tier4_selftest receives
    // cand == nullptr and runs the Tier-3 cross-probe on the original.
    Verdict (*tier4)(const Tier4Query&) = nullptr;
    bool (*tier4_selftest)(const Tier4Query&) = nullptr;
    // Proof-required policy: an Unknown verdict (solver absent/timeout)
    // rejects the commit instead of degrading to the sampling verdict.
    bool tier4_required = false;
};

struct Report {
    u32 windows = 0;       // windows attempted (qualified + searched)
    u32 improved = 0;      // windows replaced by a cheaper equivalent
    u32 erased = 0;        // windows reduced to the empty sequence
    u32 skipped_dead_def = 0; // windows skipped (reads undefined registers/slots)
    u32 skipped_orig_fault = 0; // windows skipped (original faults on a lane)
    u32 skipped_flag_in = 0;  // windows skipped (reads entry flags — unmodellable)
    u32 skipped_undef_flags = 0; // windows skipped (undefined flags live-out)
    u32 budget_exhausted = 0;  // windows that ran out of pops/candidates/states
    u64 pops = 0;          // search nodes expanded
    u64 constructions = 0; // candidate bindings constructed (the throughput
                            // unit: every operand/variant binding the search
                            // materialized, gated or not)
    u64 simulations = 0;    // candidates that reached the simulator (passed
                            // the validity/dom gates) — the work unit
    u32 tier4_proven = 0;   // commits certified by the SMT equivalence proof
    u32 tier4_refuted = 0;  // tier-2-passing winners REJECTED by the proof
    u32 tier4_unknown = 0;  // solver absent/timeout/unknown (degraded)
    u32 smt_selfchecks = 0; // Tier-3 cross-probes executed
    i64 attempted_cost = 0;  // latency units summed over ALL attempted windows
    i64 committed_before = 0; // originals of the windows actually replaced
    i64 committed_after = 0; // replacements' latency units
};

// Optimize every qualifying window of one function. Never leaves the
// function in a state that is not vector-equivalent on its live-outs.
Report run(LFunction& lf, const IsaTable& isa, const Opts& o);

} // namespace jules::superopt
