// JULES Sea-of-Nodes IR: node taxonomy.
//
// Design (Graal/C2-inspired, index-based, no pointers):
//   * Every node is an index into Graph's node vector (NodeId). Serializable,
//     cache-friendly, stable under mutation.
//   * ALL value nodes are "pinned": in[0] is their control input (a block
//     head). Pure ops can be repinned by LICM/GVN with dominance checks.
//   * Memory is an explicit chain: effectful nodes (Store/Call/Alloc) consume
//     a memory version and produce a new one. A node's "memory version" IS the
//     node itself. MemoryPhis at Regions merge versions. Loads read a version.
//   * Phi inputs are aligned with their Region's predecessors (in[0]=Region).
//   * Maximum input arity is MAX_IN (MVP: bounded by design; diagnostics fire
//     if user code exceeds it).
#pragma once

#include "core/son/types.h"
#include "core/support/common.h"

namespace jules {

using NodeId = u32;
inline constexpr NodeId kNoNode = 0xFFFFFFFFu;

enum class Op : u8 {
    // ---- control ----
    Start,      // entry: initial control + initial memory; params anchor here
    Region,     // control merge; in = predecessor block heads
    If,         // branch; in = {ctrl, cond}; pinned to the branching block
    IfTrue,     // projection; in = {If}
    IfFalse,    // projection; in = {If}
    Jump,       // unconditional control edge; in = {ctrl}; block head
    Return,     // in = {ctrl, mem, value?}
    Stop,       // in = {Return, ...}; DCE root
    // ---- pure data ----
    Const,      // in = {ctrl}; payload ival/fval, ty carries type
    Param,      // in = {Start}; aux = parameter index
    Phi,        // in = {Region, v0..vN}; ty = value type or Mem
    Bin,        // in = {ctrl, a, b}; sub = BinOp
    Cmp,        // in = {ctrl, a, b}; sub = CmpOp; ty = I1
    Un,         // in = {ctrl, x}; sub = UnOp
    Cast,       // in = {ctrl, x}; sub = CastKind; ty = target type
    Select,     // in = {ctrl, cond, t, f}
    // ---- memory ----
    Load,       // in = {ctrl, mem, addr}; result = loaded value
    Store,      // in = {ctrl, mem, addr, value}; result = new memory version
    Alloc,      // in = {ctrl, mem, size}; result = pointer (also a mem version)
    Call,       // in = {ctrl, mem, args...}; aux = FnId; result = return value
    // ---- tombstone ----
    Dead,       // killed node; no inputs; skipped by every pass
};

enum class BinOp : u8 { Add, Sub, Mul, Div, Mod, And, Or, Xor, Shl, Shr };
enum class CmpOp : u8 { Eq, Ne, Lt, Le, Gt, Ge };
enum class UnOp  : u8 { Neg, Not, BNot };
enum class CastOp : u8 {
    ZExt, SExt, Trunc, SiToFp, FpToSi, FpExt, FpTrunc, Ptr,
    // Vector-only casts (passes 54-66; never appear in user source):
    Broadcast, // scalar -> vector, every lane = the scalar
    Extract,   // vector -> scalar lane; aux = lane index
};

// Node flags (bit positions; keep under 8 bits).
enum : u8 {
    kFlagNone          = 0,
    kFlagStackPromoted = 1u << 0, // Alloc: heap->stack promoted (pass 29)
    kFlagTailCall      = 1u << 1, // Call: marked as tail call (pass 50)
    kFlagGuardSite     = 1u << 2, // future: deopt guard location
    kFlagProfile       = 1u << 3, // Store/Call: profile counter op (PGO);
                                   // immune to DSE/mem-chain pruning
    kFlagVecEpilogue   = 1u << 4, // Store: vectorizer remainder-loop store
};

constexpr u8 kMaxInputs = 16; // node input arity (MVP limit, diagnosed upstream)

struct Node {
    Op op = Op::Dead;
    u8 n_in = 0;
    u8 sub = 0;        // BinOp / CmpOp / UnOp / CastOp
    u8 flags = 0;
    TypeId ty = ty_none();
    u32 aux = 0;       // Param index / Call FnId
    i64 ival = 0;      // Const payload (ints/bool)
    f64 fval = 0;      // Const payload (floats)
    NodeId in[kMaxInputs] = {kNoNode, kNoNode, kNoNode, kNoNode, kNoNode, kNoNode,
                             kNoNode, kNoNode, kNoNode, kNoNode, kNoNode, kNoNode};
};

// ---- op predicates ---------------------------------------------------------
inline bool is_block_head(Op o) {
    return o == Op::Start || o == Op::Region || o == Op::IfTrue || o == Op::IfFalse || o == Op::Jump;
}
inline bool is_control_op(Op o) {
    return is_block_head(o) || o == Op::If || o == Op::Return || o == Op::Stop;
}
// Writes/allocates memory (changes the memory version):
inline bool is_effect_op(Op o) { return o == Op::Store || o == Op::Call || o == Op::Alloc; }
// A node usable as a memory version input:
inline bool is_mem_version(Op o, TypeId ty) {
    switch (o) {
        case Op::Start: case Op::Store: case Op::Call: case Op::Alloc: return true;
        case Op::Phi: return ty == ty_mem();
        default: return false;
    }
}
// Side-effect-free value ops (safe for GVN/LICM/reordering given inputs):
inline bool is_pure_op(Op o) {
    switch (o) {
        case Op::Const: case Op::Param: case Op::Bin: case Op::Cmp:
        case Op::Un: case Op::Cast: case Op::Select:
            return true;
        default: return false;
    }
}
inline bool is_value_op(Op o) {
    return is_pure_op(o) || o == Op::Phi || o == Op::Load || o == Op::Call || o == Op::Alloc;
}

const char* op_name(Op o);
const char* bin_name(BinOp b);
const char* cmp_name(CmpOp c);
const char* un_name(UnOp u);
const char* cast_name(CastOp c);

} // namespace jules
