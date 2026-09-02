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
// ---------------------------------------------------------------------------
enum class R : u8 {
    Rax, Rcx, Rdx, Rsi, Rdi, R8, R9, R10, R11, Rbp, Rsp,
    Xmm0, Xmm1, Xmm2, Xmm3, Xmm4, Xmm5, Xmm6, Xmm7,
};
inline constexpr bool reg_is_xmm(R r) { return r >= R::Xmm0; }

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
bool x64_post_ra_cleanup(LFunction& lf);
bool x64_machine_peephole(LFunction& lf);

} // namespace jules
