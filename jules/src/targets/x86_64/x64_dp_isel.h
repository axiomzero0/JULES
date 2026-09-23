// x86-64 DP-on-DAG instruction-selection planner (pass 84, the workhorse
// selector per docs/backend_architecture.md).
//
// Burton/Twig-class cover engine: a memoized min-cost dynamic program over
// each block's computation DAG. Tree-shaped chains are covered exactly
// optimally; shared DAG nodes are handled by the single-use rule below,
// which keeps the classic DAG-covering hazards (recomputed-at-every-parent,
// order dependence) out of the model. The machine pattern set is the rule
// set; the hand emitters in x64_emit.cpp remain the fallback for every node
// without a rule, so selection quality never regresses when a pattern is
// missing — the DP only claims nodes it covers strictly cheaper.
//
// The systemic win is chain folding: a single-use pure-arithmetic chain
// feeding a Load/Store address (or another Bin) is computed inline in the
// scratch registers, so intermediate values never take the store-then-load
// round trip through their frame slots. The Lea2 rules cover
// base + index*scale + disp in one instruction (the full SIB form — the one
// MIR opcode the DP adds; everything else emits through existing IOps).
//
// Cost model: latency-class units (slot load 3 / store 1, lea/arith/shl 1,
// imul 3). Ties prefer fewer instructions, then lower rule id — LLVM
// AddedComplexity-style deterministic priorities.
//
// Scratch discipline (soundness of nested chains): every form computes its
// value into the requested register using only {dst, other} where
// other = (dst == rax ? rcx : rax), and every chain operand completes before
// any leaf operand of the same form loads. A chain's internal writes are
// therefore finished before the caller relies on its own registers.
#pragma once

#include "core/codegen/linear.h"

namespace jules {

class DpIsel {
public:
    enum class Act : u8 { None, Root, Suppressed };

    // A reference from a committed form to one of its operands.
    struct OpRef {
        enum class K : u8 { Leaf, Chain, Imm };
        K k = K::Leaf;
        NodeId node = kNoNode; // Leaf load source / Chain root
        i64 imm = 0;            // Imm payload
    };

    // The DP cell: chosen way to compute one node's value into a register.
    // Committed cells double as the emitter's recipe.
    struct ValCell {
        enum class Form : u8 {
            Leaf,   // plain operand: mov from slot / const materialization
            Lea2,   // dst = base + index*scale + disp  (full SIB)
            LeaRR,  // dst = index*scale + disp         (baseless)
            ShlImm, // dst = x << k   (Mul by 2^k, or Shl)
            Arith,  // generic two-operand form (b-side may be a chain)
        };
        Form form = Form::Leaf;
        // Lea2 / LeaRR payload (8-byte chains only)
        OpRef base, idx; // Lea2; at most one may be a Chain
        NodeId absorb1 = kNoNode, absorb2 = kNoNode; // shape-absorbed nodes
        // (index term folded into the SIB scale, unwrapped const addend) —
        // suppressed at claim time when single-use
        u8 scale = 1;
        i64 disp = 0;
        // Arith / ShlImm payload
        OpRef a;      // Arith a-side (always Leaf) / ShlImm operand
        OpRef b;      // Arith b-side (Leaf, Chain, or Imm)
        bool big_imm = false; // b is an imm that needs movabs materialization
        BinOp bin = BinOp::Add;
        u8 shift = 0; // ShlImm
        u8 size = 8;
        i32 cost = 0; // committed cost of this cell (excludes materializing store)
    };

    DpIsel(LFunction& lf, FunctionGraph& fg);

    // Plan one block. Call after the short-circuit suppression set is built
    // for it — sc-consumed nodes are left alone. Planning and emission are
    // interleaved per block by the Emitter, so the per-block state (cells,
    // actions) is valid only until the next plan_block call.
    void plan_block(const LBlock& b, const FlatMap<NodeId, bool>& suppressed);

    Act act(NodeId n) const;
    const ValCell* cell(NodeId n) const; // committed cell, if any

    u32 roots() const { return roots_; } // claimed roots (DP-emitted nodes)
    u32 folds() const { return folds_; } // chain nodes consumed inline

private:
    LFunction& lf_;
    Graph& g_;
    FlatMap<NodeId, u8> act_;
    FlatMap<NodeId, ValCell> cells_;
    std::vector<i32> block_of_;
    const FlatMap<NodeId, bool>* scsup_ = nullptr;
    i32 cur_block_ = -1;
    u32 roots_ = 0, folds_ = 0;
    int depth_ = 0;

    // solver internals
    bool chainable_here(NodeId x) const;
    bool single_user_is(NodeId c, NodeId parent);
    i32 leaf_cost(NodeId x) const;
    OpRef consume(NodeId x, NodeId parent); // Chain if legal, else Leaf
    i32 opref_cost(const OpRef& r);
    bool try_commit(NodeId n);               // compute + commit best cell
    i32 fallback_mat(NodeId n) const;        // what the hand emitter pays
    void mark_consumed(NodeId n);            // Suppressed mark + walk
    void mark_children_consumed(NodeId n);   // walk only (Root keeps its own)
    void claim_root(NodeId n);
};

} // namespace jules
