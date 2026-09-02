// Linear IR + x86-64 MIR (single-backend MVP; the Target interface from the
// architecture doc is the extension point).
//
// LFunction: SoN blocks laid out in RPO (hot-path preference applied by the
// linearizer), each block's nodes topologically scheduled, phi moves
// attached to predecessor blocks. The machine passes (84-87) then lower
// this into Inst sequences with stack slots (spill-everywhere allocator in
// pass 85; linear-scan is the documented upgrade path).
#pragma once

#include "core/son/graph.h"

#include <string>
#include <vector>

namespace jules {

// ---------------------------------------------------------------------------
// Linear structure (pass 83 output)
// ---------------------------------------------------------------------------
struct LPhiCopy {
    NodeId src = kNoNode; // value node
    NodeId dst = kNoNode; // phi node (its slot)
};

struct LBlock {
    NodeId head = kNoNode;               // SoN block-head node
    std::vector<NodeId> nodes;           // scheduled non-terminator nodes
    NodeId terminator_if = kNoNode;      // If pinned here (branch end)
    NodeId terminator_return = kNoNode;  // Return pinned here
    NodeId jump = kNoNode;               // this block IS a Jump node
    std::vector<int> succs;              // block indices
    std::vector<int> preds;              // block indices
    std::vector<int> phi_copy_indices;   // copies emitted at block end
    int index = -1;
};

// ---------------------------------------------------------------------------
// x86-64 MIR (System V AMD64, AT&T syntax at serialization)
//
// Register classes: GPR scratch (rax..r11 minus rbp/rsp), GPR callee-saved
// (rbx, r12-r15 — reserved for the pass 85 allocator), XMM scratch (xmm0/1
// used by isel; xmm2-7 allocatable). Rbp/Rsp are never allocatable.
// ---------------------------------------------------------------------------
enum class R : u8 {
    Rax, Rcx, Rdx, Rsi, Rdi, R8, R9, R10, R11,
    Rbx, R12, R13, R14, R15, // callee-saved: allocator-owned
    Rbp, Rsp,
    Xmm0, Xmm1, Xmm2, Xmm3, Xmm4, Xmm5, Xmm6, Xmm7,
    Xmm8, Xmm9, Xmm10, Xmm11, Xmm12, Xmm13, // allocator: caller-saved XMM pool
    Xmm14, Xmm15,                      // isel FP const pool (loop-hoisted)
};
inline constexpr bool reg_is_xmm(R r) { return r >= R::Xmm0; }
// Callee-saved GPRs the register allocator may assign (SysV: preserved
// across calls; saved in the prologue via PushCal, restored pre-epilogue).
inline constexpr bool reg_is_callee_saved_gpr(R r) {
    return r == R::Rbx || r == R::R12 || r == R::R13 || r == R::R14 || r == R::R15;
}
// Caller-saved GPRs the allocator may use for values not live across calls
// (isel never touches r10/r11; argument registers are excluded).
inline constexpr bool reg_is_alloc_gpr(R r) {
    return r == R::R10 || r == R::R11 || reg_is_callee_saved_gpr(r);
}
inline constexpr bool reg_is_alloc_xmm(R r) {
    return r >= R::Xmm2 && r <= R::Xmm13; // xmm14-15: isel const pool
}
// The high XMM bank is the isel constant pool: never an argument register
// (SysV vector args use xmm0-7), never allocator-assigned.
inline constexpr bool reg_is_const_pool_xmm(R r) { return r >= R::Xmm8; }

enum class Cond : u8 { E, NE, L, LE, G, GE, B, BE, A, AE };

enum class IOp : u16 {
    Nop,
    Label,          // label id (unique per function)
    MovRR,          // dst = src (size)
    MovRS,          // store reg -> slot
    MovSR,          // load slot -> reg
    MovRImm,        // imm -> reg
    MovSImm,        // imm -> slot
    MovZX,          // dst = zero-extend byte reg (rax <- al)
    LoadMem,        // dst = [base]           (size)
    StoreMem,       // [base] = src           (size)
    LeaSlot,        // dst = rbp - slot*8 (stack object address)
    ArithRR,        // dst op= src   (sub encodes op)
    ArithRImm,      // dst op= imm
    ShiftImm,       // dst <<= / >>= imm
    ShiftCl,        // dst <<= / >>= %cl (variable shift)
    Cqo,            // sign-extend rax into rdx:rax (size)
    IDiv,           // signed divide rdx:rax by reg
    UDiv,           // unsigned divide rax by reg
    CmpRR,          // cmp a, b
    CmpRImm,        // cmp a, imm
    Test,           // test a, a
    Setcc,          // al = cond (after cmp)
    Cmov,           // dst = cond ? dst : src
    Jcc,            // cond jump to label
    Jmp,            // jump to label
    CallSym,        // call external symbol (malloc/printf/free)
    CallFn,         // call internal function label
    TailCallFn,     // leave; jmp function entry (pass 50 marker honored)
    XorEax,         // xor eax, eax (varargs helper)
    FpBin,          // xmm0 op= xmm1 (sub encodes fp op + size)
    FpCmp,          // ucomis xmm0, xmm1 (flags feed Setcc)
    FpNeg,          // xmm0 ^= sign mask (rodata mask by size)
    SExt32,         // movslq %eax, %rax
    LeaSym,         // lea reg, sym(%rip)
    CvtToFp,        // xmm0 = (fp) rax   (sub: signed/unsigned approx MVP)
    CvtToInt,       // rax = (int) xmm0
    FpExt,          // xmm0 = (double)(float) from 32-bit
    FpTrunc,        // xmm0 = (float)(double)
    MovFpS,         // store xmm -> slot (size 4/8)
    MovFpR,         // load slot -> xmm
    PushRbp, PopRbp,
    Ret,            // leave; ret
    FrameSub,       // subq $imm, %rsp (patched by pass 85)
    Neg,            // negq reg
    Not,            // notq reg
    MovFpFromGpr,   // movq %rax, %xmmN (bit pattern move)
    MovFpFromGpr32, // movd %eax, %xmmN
    MovFpFp,        // movsd/movss %xmmN, %xmmM (allocator reg-reg fp move)
    PushCal,        // pushq %reg — callee-saved spill area reservation (pass 85)
    RestoreCal,     // movq OFF(%rbp), %reg — callee-saved restore (pass 85)
    PopCal,         // popq %reg — frame-elided callee restore (pass 85)
    RetNaked,       // ret — no leave (frame elided)
    TailCallNaked,  // jmp fn — tail call without a frame (frame elided)
    Comment,        // emission-time annotation (MIR comments, disabled in release)
};

struct Operand {
    enum class K : u8 { None, Reg, Slot, Imm, Label, Sym } k = K::None;
    R reg = R::Rax;
    i32 slot = 0;         // virtual slot id (pre-RA) / offset applied later
    i64 imm = 0;
    int label = 0;
    const char* sym = nullptr;
};

struct Inst {
    IOp op = IOp::Nop;
    Operand a, b;          // operands per opcode
    u8 size = 8;           // operand width in bytes (1/4/8)
    Cond cond = Cond::E;   // jcc/setcc/cmov
    BinOp bin = BinOp::Add;// arith/fp op encoding
    bool sar = false;      // shift-right is arithmetic (signed operand)
    const char* note = nullptr;
};

struct StringConst {
    std::string label;
    std::string text;
};

struct LFunction {
    FnId fid = kNoFn;
    SymbolId name = kNoSymbol;
    TypeId ret = ty_void();
    std::vector<TypeId> params;
    bool always_inline = false;

    std::vector<LBlock> blocks;
    std::vector<LPhiCopy> phi_copies;

    // machine side (filled by passes 84-87)
    std::vector<Inst> code;
    FlatMap<NodeId, i32> slot_of;       // value node -> virtual slot
    i32 slot_count = 0;
    std::vector<i32> slot_offset;       // pass 85: concrete rbp offsets
    FlatMap<i32, R> slot_reg;           // pass 85: slot -> assigned register
    u32 ra_promoted = 0;                // pass 85 telemetry: promoted slots
    u32 ra_spilled = 0;                 // pass 85 telemetry: memory slots
    i32 frame_size = 0;
    std::vector<StringConst> strings;   // printf formats owned by this fn
    int label_counter = 0;
    bool is_main = false;
};

struct LinearModule {
    std::vector<LFunction> fns;
    std::vector<StringConst> module_strings; // .rodata entries
    // Deopt manifest entries (pass 89)
    struct GuardSite {
        FnId fn;
        int block;
        const char* kind;
    };
    std::vector<GuardSite> guard_sites;
};

// Serialization: MIR -> AT&T assembly text (whole module).
std::string serialize_module_asm(const LinearModule& lin, SymbolTable& syms);

// Machine pass entry points (invoked by pass files 84-87).
bool x64_select_instructions(LFunction& lf, FunctionGraph& fg, SymbolTable& syms);
bool x64_allocate_frame(LFunction& lf);
// Pass 85 allocator: linear scan over slot live ranges (see x64_ra.cpp).
//   level < O1  -> spill-everywhere (correctness-first, spec "simple")
//   level >= O1 -> linear scan with callee-saved/caller-saved/XMM pools
bool x64_allocate_registers(LFunction& lf, const Graph* g, bool use_registers,
                            bool aggressive, bool size_biased);
bool x64_post_ra_cleanup(LFunction& lf);
bool x64_machine_peephole(LFunction& lf);
// Pass 87 helper: fused compare-and-branch + accumulator folds.
bool x64_branch_fusion(LFunction& lf);

} // namespace jules
