// The x86-64 superoptimizer ISA descriptor table — semantics as DATA.
//
// This is the ONLY file in the superoptimizer that knows instructions. One
// row per MIR IOp (the table is exhaustive over the enum, so the engine's
// conservative unknown-opcode fallback never fires):
//   * searchable rows (sim != null): exact architectural semantics over the
//     engine's vector state, operand/variant axes for enumeration, latency
//     from the target's existing cost model (DpIsel's latency classes —
//     single source, no duplicated numbers);
//   * boundary rows (sim == null): windows break on the opcode, but its
//     read/write effects still feed the function-wide liveness dataflow.
//
// Semantics ground truth = the serializer's emitted assembly:
//   * 32-bit register writes zero-extend into the 64-bit register;
//   * 32-bit SLOT writes write only the low 4 bytes (upper 4 preserved);
//   * shifts mask their count (31/63) and leave flags unchanged for 0;
//   * instructions the architecture leaves partially undefined (idiv's
//     flags, multi-bit shift OF, mul's ZF/SF/PF) write per-lane poison
//     values: a candidate may only rely on them in ways the goal test can
//     refute, and windows whose ORIGINAL produces undefined flags that are
//     live-out are skipped by the engine.
#include "superopt/superopt.h"
#include "x64_dp_isel.h"
#include "x64_super_smt.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace jules {

using superopt::Effects;
using superopt::OpDef;
using superopt::Row;
using superopt::VState;

namespace {

// ---- tiny shared helpers -------------------------------------------------

u32 bit(R r) { return 1u << static_cast<int>(r); }

// Frame registers are NOT value registers: the engine models slots
// abstractly (never as rbp-relative memory), so an instruction touching
// rbp/rsp has semantics the simulator cannot express — it is UNSEARCHABLE
// (windows break on it) and can never be a candidate operand. This is the
// lesson from the first smoke test: without this gate the search judged
// `movq %rsp, %rbp` dead (nothing in the model reads rbp) and deleted the
// frame setup.
bool reg_ok(R r) { return r != R::Rbp && r != R::Rsp; }

// rax rcx rdx rsi rdi r8 r9 r10 r11 (indices 0..8)
constexpr u32 kCallerSaved = 0x1FFu;
// rax rdi rsi rdx rcx r8 r9: rax (vararg count), SysV integer args
constexpr u32 kCallReads = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 2) |
                           (1u << 1) | (1u << 5) | (1u << 6);
constexpr u32 kAllGpr = (1u << superopt::kMaxGpr) - 1;

bool fits32(i64 v) {
    return v >= std::numeric_limits<i32>::min() && v <= std::numeric_limits<i32>::max();
}

u64 splitmix64(u64 x) { // same pure function as the engine's (a hash, not
    x += 0x9E3779B97F4A7C15ull; // semantic knowledge; kept in sync by test)
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

u64 poison_lane(u32 v, u32 lane) {
    return splitmix64(0xBADA55C0DEull ^ (0x9E3779B9ull * (v + 1)) ^
                      (0x85EBCA77ull * (lane + 1)));
}

u64 mask(u8 size) { return size == 4 ? 0xFFFFFFFFull : ~0ull; }

u64 rd_reg(const VState& st, u32 v, R r, u8 size) {
    u64 x = static_cast<u64>(st.r[v][static_cast<int>(r)]);
    return size == 4 ? (x & 0xFFFFFFFFull) : x;
}

void wr_reg(VState& st, u32 v, R r, u8 size, u64 val) {
    if (size == 4) val &= 0xFFFFFFFFull; // 32-bit writes zero-extend
    st.r[v][static_cast<int>(r)] = static_cast<i64>(val);
}

u64 rd_slot(const VState& st, u32 v, const i32* slots, u32 nslots, i32 abs, u8 size) {
    for (u32 k = 0; k < nslots; ++k)
        if (slots[k] == abs) {
            u64 x = static_cast<u64>(st.s[v][k]);
            return size == 4 ? (x & 0xFFFFFFFFull) : x;
        }
    return 0;
}

void wr_slot(VState& st, u32 v, const i32* slots, u32 nslots, i32 abs, u8 size, u64 val) {
    for (u32 k = 0; k < nslots; ++k)
        if (slots[k] == abs) {
            u64 old = static_cast<u64>(st.s[v][k]);
            if (size == 4) // 32-bit slot writes preserve the upper 4 bytes
                val = (old & 0xFFFFFFFF00000000ull) | (val & 0xFFFFFFFFull);
            st.s[v][k] = static_cast<i64>(val);
            return;
        }
}

bool parity8(u64 x) { // x86 PF: 1 when the low byte has even parity
    u64 p = x & 0xFF;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    return !(p & 1);
}

// flag lanes: 0=ZF 1=SF 2=OF 3=CF 4=PF

void set_add_flags(VState& st, u32 v, u64 a, u64 b, u8 size) {
    u64 full = a + b;
    u64 res = full & mask(size);
    st.f[v][0] = res == 0 ? 1 : 0;
    st.f[v][1] = (res >> (size * 8 - 1)) & 1;
    st.f[v][2] = ((~(a ^ b)) & (a ^ res) & (1ull << (size * 8 - 1))) ? 1 : 0;
    st.f[v][3] = size == 4 ? ((full >> 32) & 1) : (full < a ? 1 : 0);
    st.f[v][4] = parity8(res) ? 1 : 0;
}

void set_sub_flags(VState& st, u32 v, u64 a, u64 b, u8 size) {
    u64 res = (a - b) & mask(size);
    st.f[v][0] = res == 0 ? 1 : 0;
    st.f[v][1] = (res >> (size * 8 - 1)) & 1;
    st.f[v][2] = ((a ^ b) & (a ^ res) & (1ull << (size * 8 - 1))) ? 1 : 0;
    st.f[v][3] = a < b ? 1 : 0;
    st.f[v][4] = parity8(res) ? 1 : 0;
}

void set_logic_flags(VState& st, u32 v, u64 res, u8 size) {
    res &= mask(size);
    st.f[v][0] = res == 0 ? 1 : 0;
    st.f[v][1] = (res >> (size * 8 - 1)) & 1;
    st.f[v][2] = 0;
    st.f[v][3] = 0;
    st.f[v][4] = parity8(res) ? 1 : 0;
}

void poison_flag(VState& st, u32 v, u32 lane) {
    st.f[v][lane] = static_cast<i64>(poison_lane(v, lane));
}

bool eval_cond(Cond c, const VState& st, u32 v) {
    bool zf = st.f[v][0] != 0, sf = st.f[v][1] != 0, of = st.f[v][2] != 0,
         cf = st.f[v][3] != 0, pf = st.f[v][4] != 0;
    switch (c) {
        case Cond::E: return zf;
        case Cond::NE: return !zf;
        case Cond::L: return sf != of;
        case Cond::LE: return zf || sf != of;
        case Cond::G: return !zf && sf == of;
        case Cond::GE: return sf == of;
        case Cond::B: return cf;
        case Cond::BE: return cf || zf;
        case Cond::A: return !cf && !zf;
        case Cond::AE: return !cf;
        case Cond::P: return pf;
        case Cond::NP: return !pf;
    }
    return false;
}

// ---- latencies (DpIsel's classes — the target's single cost source) -----

i32 lat_op(const Inst&) { return DpIsel::kCOp; }
i32 lat_ld(const Inst&) { return DpIsel::kCLd; }
i32 lat_st(const Inst&) { return DpIsel::kCSt; }
i32 lat_imm(const Inst&) { return DpIsel::kCImm; }
i32 lat_div(const Inst&) { return DpIsel::kCDiv; }
i32 lat_mulhi(const Inst&) { return DpIsel::kCMul; }
i32 lat_arith(const Inst& i) {
    return i.bin == BinOp::Mul ? DpIsel::kCMul : DpIsel::kCOp;
}

// ---- variant axes (data) ---------------------------------------------------

constexpr BinOp kArithBins[] = {BinOp::Add, BinOp::Sub,  BinOp::Mul,
                                BinOp::And, BinOp::Or,   BinOp::Xor};
constexpr bool kArithSars[] = {false, false, false, false, false, false};
constexpr BinOp kShiftBins[] = {BinOp::Shl, BinOp::Shr, BinOp::Shr};
constexpr bool kShiftSars[] = {false, false, true};
constexpr Cond kAllConds[] = {Cond::E,  Cond::NE, Cond::L,  Cond::LE,
                              Cond::G,  Cond::GE, Cond::B,  Cond::BE,
                              Cond::A,  Cond::AE, Cond::P,  Cond::NP};
constexpr u8 kSizes48[] = {4, 8};
constexpr u8 kScales[] = {1, 2, 4, 8}; // lea family: scale rides in `size`

// ---- validity predicates ---------------------------------------------------

bool v_rr(const Inst& i) {
    return i.a.k == Operand::K::Reg && i.b.k == Operand::K::Reg &&
           reg_ok(i.a.reg) && reg_ok(i.b.reg);
}
bool v_rs(const Inst& i) { // reg -> slot (a reg, b slot)
    return i.a.k == Operand::K::Reg && i.b.k == Operand::K::Slot && reg_ok(i.a.reg);
}
bool v_sr(const Inst& i) { // slot -> reg
    return i.b.k == Operand::K::Slot && i.a.k == Operand::K::Reg && reg_ok(i.a.reg);
}
bool v_rimm(const Inst& i) {
    return i.a.k == Operand::K::Reg && i.b.k == Operand::K::Imm && reg_ok(i.a.reg);
}
bool v_simm(const Inst& i) { // movq $imm32s, slot — immediate must fit imm32
    return i.a.k == Operand::K::Slot && i.b.k == Operand::K::Imm && fits32(i.b.imm);
}
bool v_arith_imm(const Inst& i) {
    return i.a.k == Operand::K::Reg && i.b.k == Operand::K::Imm && fits32(i.b.imm) &&
           reg_ok(i.a.reg);
}
bool v_shift(const Inst& i) {
    return i.a.k == Operand::K::Reg && i.b.k == Operand::K::Imm && i.b.imm >= 0 &&
           i.b.imm <= 63 && reg_ok(i.a.reg);
}
bool v_learr(const Inst& i) {
    return i.a.k == Operand::K::Reg && i.b.k == Operand::K::Reg && fits32(i.b.imm) &&
           reg_ok(i.a.reg) && reg_ok(i.b.reg);
}
bool v_lea2(const Inst& i) {
    return i.a.k == Operand::K::Reg && i.b.k == Operand::K::Reg &&
           i.b.slot >= 0 && i.b.slot < static_cast<i32>(superopt::kMaxGpr) &&
           reg_ok(i.a.reg) && reg_ok(i.b.reg) &&
           reg_ok(static_cast<R>(i.b.slot)) && fits32(i.b.imm);
}
bool v_cmp_rimm(const Inst& i) {
    return (i.a.k == Operand::K::Reg
                ? reg_ok(i.a.reg)
                : i.a.k == Operand::K::Slot) &&
           i.b.k == Operand::K::Imm && fits32(i.b.imm);
}
bool v_one(const Inst&) { return true; }

// ---- effects ---------------------------------------------------------------

void e_none(const Inst&, Effects&) {}

void e_rr(const Inst& i, Effects& e) { // NON-destructive (mov/lea): a written,
    if (i.a.k == Operand::K::Reg)     // b read; the dst is NOT read
        e.gpr_write |= bit(i.a.reg);
    if (i.b.k == Operand::K::Reg) e.gpr_read |= bit(i.b.reg);
}

// DESTRUCTIVE two-operand forms (add/sub/imul/shl/neg/not/cmov...): the
// destination holds an INPUT too. Missing the a-side read once let the
// liveness conclude `mov rdi, rbx` was dead (its only reader, `imul $5,
// rbx, rbx`, only writes rbx in the model) and the search deleted the
// parameter homing — the same reads-first lesson the pass-87 peephole
// round already paid for.
void e_destructive(const Inst& i, Effects& e) {
    if (i.a.k == Operand::K::Reg) {
        e.gpr_read |= bit(i.a.reg);
        e.gpr_write |= bit(i.a.reg);
    }
    if (i.b.k == Operand::K::Reg) e.gpr_read |= bit(i.b.reg);
}

void e_r_flags(const Inst& i, Effects& e) { // arith/shift: destructive + flags
    e_destructive(i, e);
    e.writes_flags = true;
}

// Flag TRUTH for the arith family: add/sub/logic write all five flags
// defined; imul leaves ZF/SF/PF undefined (the sim poisons those lanes) —
// the undef_flags bit is what makes the engine skip windows whose
// undefined flags are live-out (review finding 1: the sims poisoned but
// the effects claimed full definition, so the mandated guard never fired).
void e_arith_flags(const Inst& i, Effects& e) {
    e_destructive(i, e);
    e.writes_flags = true;
    e.undef_flags = i.bin == BinOp::Mul;
}

// Flag TRUTH for immediate shifts: count 0 (after the x86 mask) leaves
// flags UNAFFECTED — the instruction passes them through, so it READS the
// entry flags (review finding 2: missing read = wrong liveness kill across
// the shift, a preceding window could drop flags the original observes);
// count != 1 leaves OF undefined (sim poisons it).
void e_shiftimm_flags(const Inst& i, Effects& e) {
    e_destructive(i, e);
    e.writes_flags = true;
    const u64 cnt = static_cast<u64>(i.b.imm) & (i.size == 4 ? 31u : 63u);
    e.reads_flags = cnt == 0;      // pass-through when nothing shifts
    e.undef_flags = cnt != 1;      // OF defined only for 1-bit shifts
}

// Flag TRUTH for variable shifts: the runtime count decides — flags are
// read (count may be 0) and OF may be undefined (count may exceed 1).
void e_shiftcl_flags(const Inst& i, Effects& e) {
    e_destructive(i, e);
    e.gpr_read |= bit(R::Rcx);
    e.writes_flags = true;
    e.reads_flags = true;
    e.undef_flags = true;
}
void e_lea2(const Inst& i, Effects& e) {
    e.gpr_write |= bit(i.a.reg);
    e.gpr_read |= bit(i.b.reg) | (1u << i.b.slot);
}
void e_rs(const Inst& i, Effects& e) { // reg -> slot
    e.gpr_read |= bit(i.a.reg);
    e.slot_write = i.b.slot;
    e.slot_write_covers = i.size >= 8;
}
void e_sr(const Inst& i, Effects& e) { // slot -> reg
    e.slot_read = i.b.slot;
    e.gpr_write |= bit(i.a.reg);
}
void e_simm(const Inst& i, Effects& e) { // imm -> slot (always movq: 8 bytes)
    e.slot_write = i.a.slot;
    e.slot_write_covers = true;
}
void e_read_a(const Inst& i, Effects& e) { e.gpr_read |= bit(i.a.reg); }
void e_write_a(const Inst& i, Effects& e) { e.gpr_write |= bit(i.a.reg); }
void e_rw_a(const Inst& i, Effects& e) {
    e.gpr_read |= bit(i.a.reg);
    e.gpr_write |= bit(i.a.reg);
}
void e_div(const Inst& i, Effects& e) {
    e.gpr_read |= bit(i.a.reg) | bit(R::Rax) | bit(R::Rdx);
    e.gpr_write |= bit(R::Rax) | bit(R::Rdx);
    e.undef_flags = true;
}
void e_mulhi(const Inst& i, Effects& e) {
    e.gpr_read |= bit(i.a.reg) | bit(R::Rax);
    e.gpr_write |= bit(R::Rax) | bit(R::Rdx);
    e.undef_flags = true;
}
void e_cqo(const Inst&, Effects& e) {
    e.gpr_read |= bit(R::Rax);
    e.gpr_write |= bit(R::Rdx);
}
void e_setcc(const Inst& i, Effects& e) { // byte write preserves the upper
    e.reads_flags = true;                 // bits: a.reg is read AND written
    e_rw_a(i, e);
}
void e_cmov(const Inst& i, Effects& e) {
    e.reads_flags = true;
    e_destructive(i, e); // a is read (kept when cond fails) and written;
                         // b is read
}
void e_cmp_rr(const Inst& i, Effects& e) { // cmp READS both; writes nothing
    if (i.a.k == Operand::K::Reg) e.gpr_read |= bit(i.a.reg);
    if (i.b.k == Operand::K::Reg) e.gpr_read |= bit(i.b.reg);
    e.writes_flags = true;
}
void e_cmp_rimm(const Inst& i, Effects& e) {
    if (i.a.k == Operand::K::Reg) e.gpr_read |= bit(i.a.reg);
    else if (i.a.k == Operand::K::Slot) e.slot_read = i.a.slot;
    e.writes_flags = true;
}
void e_test(const Inst& i, Effects& e) {
    e.gpr_read |= bit(i.a.reg);
    e.writes_flags = true;
}
void e_jcc(const Inst&, Effects& e) { e.reads_flags = true; }
void e_ret(const Inst&, Effects& e) { e.gpr_read |= bit(R::Rax); }
void e_call(const Inst&, Effects& e) {
    e.gpr_read |= kCallReads;
    e.gpr_write |= kCallerSaved;
    e.undef_flags = true;
}
void e_tailcall(const Inst&, Effects& e) { e.gpr_read |= kCallReads; }
void e_leaslot(const Inst& i, Effects& e) {
    // the slot is a USE, not just scratch: its address escapes (loads and
    // stores go through it — pass 88's count_slot_refs and the RA's
    // slot_use_inst both model this). Review finding 3: with a plain reg
    // write here, a slot whose only reader is its address looked dead and
    // a window could delete its last store.
    e.gpr_write |= bit(i.a.reg);
    if (i.b.k == Operand::K::Slot) {
        e.slot_read = i.b.slot;
    }
}
void e_loadmem(const Inst& i, Effects& e) {
    e.gpr_read |= bit(i.b.reg);
    e.gpr_write |= bit(i.a.reg);
}
void e_storemem(const Inst& i, Effects& e) {
    e.gpr_read |= bit(i.a.reg);
    if (i.b.k == Operand::K::Reg) e.gpr_read |= bit(i.b.reg);
}
void e_movfp_s(const Inst& i, Effects& e) {
    e.slot_write = i.b.slot;
    e.slot_write_covers = i.size >= 8;
}
void e_movfp_r(const Inst& i, Effects& e) { e.slot_read = i.b.slot; }
void e_fpcmp(const Inst&, Effects& e) { e.undef_flags = true; }
void e_pgo_sketch(const Inst&, Effects& e) {
    // opaque text-side expansion with internal control flow: conservative
    // for liveness (reads everything, kills nothing)
    e.gpr_read |= kAllGpr;
}

// ---- simulations -----------------------------------------------------------

bool s_movrr(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v)
        wr_reg(st, v, i.a.reg, i.size, rd_reg(st, v, i.b.reg, i.size));
    return true;
}

bool s_movsr(VState& st, const i32* slots, u32 nslots, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v)
        wr_reg(st, v, i.a.reg, i.size, rd_slot(st, v, slots, nslots, i.b.slot, i.size));
    return true;
}

bool s_movrs(VState& st, const i32* slots, u32 nslots, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v)
        wr_slot(st, v, slots, nslots, i.b.slot, i.size, rd_reg(st, v, i.a.reg, i.size));
    return true;
}

bool s_movrimm(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v)
        wr_reg(st, v, i.a.reg, 8, static_cast<u64>(i.b.imm));
    return true;
}

bool s_movsimm(VState& st, const i32* slots, u32 nslots, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v)
        wr_slot(st, v, slots, nslots, i.a.slot, 8, static_cast<u64>(i.b.imm));
    return true;
}

bool s_arith_rr(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = rd_reg(st, v, i.a.reg, i.size);
        u64 b = rd_reg(st, v, i.b.reg, i.size);
        u64 m = mask(i.size);
        switch (i.bin) {
            case BinOp::Add: {
                u64 r = a + b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_add_flags(st, v, a, b, i.size);
                break;
            }
            case BinOp::Sub: {
                u64 r = a - b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_sub_flags(st, v, a, b, i.size);
                break;
            }
            case BinOp::Mul: { // imul r, rm: signed low half; CF/OF = overflow
                i64 sa = i.size == 4 ? static_cast<i64>(static_cast<i32>(static_cast<u32>(a)))
                                    : static_cast<i64>(a);
                i64 sbb = i.size == 4 ? static_cast<i64>(static_cast<i32>(static_cast<u32>(b)))
                                     : static_cast<i64>(b);
                __int128 full = static_cast<__int128>(sa) * static_cast<__int128>(sbb);
                u64 r = static_cast<u64>(static_cast<i64>(full)) & m;
                wr_reg(st, v, i.a.reg, i.size, r);
                i64 sext = i.size == 4
                              ? static_cast<i64>(static_cast<i32>(static_cast<u32>(r)))
                              : static_cast<i64>(r);
                u32 ovf = static_cast<__int128>(sext) != full ? 1 : 0;
                st.f[v][2] = ovf;
                st.f[v][3] = ovf;
                poison_flag(st, v, 0);
                poison_flag(st, v, 1);
                poison_flag(st, v, 4);
                break;
            }
            case BinOp::And: {
                u64 r = a & b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_logic_flags(st, v, r, i.size);
                break;
            }
            case BinOp::Or: {
                u64 r = a | b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_logic_flags(st, v, r, i.size);
                break;
            }
            case BinOp::Xor: {
                u64 r = a ^ b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_logic_flags(st, v, r, i.size);
                break;
            }
            default: return false; // not an encodable arith bin
        }
    }
    return true;
}

bool s_arith_rimm(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = rd_reg(st, v, i.a.reg, i.size);
        u64 b = static_cast<u64>(i.b.imm); // sign-extended imm32
        u64 m = mask(i.size);
        if (i.size == 4) b &= m;
        switch (i.bin) {
            case BinOp::Add: {
                u64 r = a + b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_add_flags(st, v, a, b, i.size);
                break;
            }
            case BinOp::Sub: {
                u64 r = a - b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_sub_flags(st, v, a, b, i.size);
                break;
            }
            case BinOp::Mul: { // imul $imm, r, r: signed low half
                i64 sa = i.size == 4 ? static_cast<i64>(static_cast<i32>(static_cast<u32>(a)))
                                    : static_cast<i64>(a);
                i64 sbb = i.size == 4 ? static_cast<i64>(static_cast<i32>(static_cast<u32>(b)))
                                     : static_cast<i64>(b);
                __int128 full = static_cast<__int128>(sa) * static_cast<__int128>(sbb);
                u64 r = static_cast<u64>(static_cast<i64>(full)) & m;
                wr_reg(st, v, i.a.reg, i.size, r);
                i64 sext = i.size == 4
                              ? static_cast<i64>(static_cast<i32>(static_cast<u32>(r)))
                              : static_cast<i64>(r);
                u32 ovf = static_cast<__int128>(sext) != full ? 1 : 0;
                st.f[v][2] = ovf;
                st.f[v][3] = ovf;
                poison_flag(st, v, 0);
                poison_flag(st, v, 1);
                poison_flag(st, v, 4);
                break;
            }
            case BinOp::And: {
                u64 r = a & b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_logic_flags(st, v, r, i.size);
                break;
            }
            case BinOp::Or: {
                u64 r = a | b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_logic_flags(st, v, r, i.size);
                break;
            }
            case BinOp::Xor: {
                u64 r = a ^ b;
                wr_reg(st, v, i.a.reg, i.size, r);
                set_logic_flags(st, v, r, i.size);
                break;
            }
            default: return false;
        }
    }
    return true;
}


bool s_shiftimm(VState& st, const i32*, u32, const Inst& i) {
    u64 cnt = static_cast<u64>(i.b.imm);
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = rd_reg(st, v, i.a.reg, i.size);
        u64 m = mask(i.size);
        u64 sb = 1ull << (i.size * 8 - 1);
        u64 c = cnt & (i.size == 4 ? 31u : 63u);
        if (c == 0) {
            wr_reg(st, v, i.a.reg, i.size, a); // flags unchanged
            continue;
        }
        u64 res, cf;
        if (i.bin == BinOp::Shl) {
            res = (a << c) & m;
            cf = (a >> (i.size * 8 - c)) & 1; // last bit shifted out
        } else if (i.sar) {
            i64 sa = i.size == 4 ? static_cast<i64>(static_cast<i32>(static_cast<u32>(a)))
                                : static_cast<i64>(a);
            res = static_cast<u64>(sa >> c) & m;
            cf = (a >> (c - 1)) & 1;
        } else {
            res = (a & m) >> c;
            cf = (a >> (c - 1)) & 1;
        }
        wr_reg(st, v, i.a.reg, i.size, res);
        st.f[v][0] = res == 0 ? 1 : 0;
        st.f[v][1] = (res & sb) ? 1 : 0;
        st.f[v][3] = cf;
        st.f[v][4] = parity8(res) ? 1 : 0;
        if (c == 1) {
            if (i.bin == BinOp::Shl) st.f[v][2] = ((res & sb) ? 1 : 0) ^ cf;
            else if (i.sar) st.f[v][2] = 0;
            else st.f[v][2] = (a & sb) ? 1 : 0;
        } else {
            poison_flag(st, v, 2); // OF defined only for 1-bit shifts
        }
    }
    return true;
}

bool s_shiftcl(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = rd_reg(st, v, i.a.reg, i.size);
        u64 m = mask(i.size);
        u64 sb = 1ull << (i.size * 8 - 1);
        u64 c = static_cast<u64>(st.r[v][static_cast<int>(R::Rcx)]) &
                (i.size == 4 ? 31u : 63u);
        if (c == 0) {
            wr_reg(st, v, i.a.reg, i.size, a);
            continue;
        }
        u64 res, cf;
        if (i.bin == BinOp::Shl) {
            res = (a << c) & m;
            cf = (a >> (i.size * 8 - c)) & 1;
        } else if (i.sar) {
            i64 sa = i.size == 4 ? static_cast<i64>(static_cast<i32>(static_cast<u32>(a)))
                                : static_cast<i64>(a);
            res = static_cast<u64>(sa >> c) & m;
            cf = (a >> (c - 1)) & 1;
        } else {
            res = (a & m) >> c;
            cf = (a >> (c - 1)) & 1;
        }
        wr_reg(st, v, i.a.reg, i.size, res);
        st.f[v][0] = res == 0 ? 1 : 0;
        st.f[v][1] = (res & sb) ? 1 : 0;
        st.f[v][3] = cf;
        st.f[v][4] = parity8(res) ? 1 : 0;
        if (c == 1) {
            if (i.bin == BinOp::Shl) st.f[v][2] = ((res & sb) ? 1 : 0) ^ cf;
            else if (i.sar) st.f[v][2] = 0;
            else st.f[v][2] = (a & sb) ? 1 : 0;
        } else {
            poison_flag(st, v, 2);
        }
    }
    return true;
}

bool s_neg(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = rd_reg(st, v, i.a.reg, i.size);
        u64 m = mask(i.size);
        u64 res = (0 - a) & m;
        wr_reg(st, v, i.a.reg, i.size, res);
        st.f[v][0] = res == 0 ? 1 : 0;
        st.f[v][1] = (res >> (i.size * 8 - 1)) & 1;
        st.f[v][2] = (a == (1ull << (i.size * 8 - 1))) ? 1 : 0;
        st.f[v][3] = a != 0 ? 1 : 0;
        st.f[v][4] = parity8(res) ? 1 : 0;
    }
    return true;
}

bool s_not(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = rd_reg(st, v, i.a.reg, i.size);
        wr_reg(st, v, i.a.reg, i.size, ~a); // no flags
    }
    return true;
}

bool s_learr(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v)
        wr_reg(st, v, i.a.reg, 8,
               static_cast<u64>(rd_reg(st, v, i.b.reg, 8)) * i.size +
                   static_cast<u64>(i.b.imm));
    return true;
}

bool s_lea2(VState& st, const i32*, u32, const Inst& i) {
    R idx = static_cast<R>(i.b.slot);
    for (u32 v = 0; v < superopt::kVecSearch; ++v)
        wr_reg(st, v, i.a.reg, 8,
               static_cast<u64>(rd_reg(st, v, i.b.reg, 8)) +
                   static_cast<u64>(rd_reg(st, v, idx, 8)) * i.size +
                   static_cast<u64>(i.b.imm));
    return true;
}

bool s_cqo(VState& st, const i32*, u32, const Inst&) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v)
        st.r[v][static_cast<int>(R::Rdx)] = st.r[v][static_cast<int>(R::Rax)] < 0 ? -1 : 0;
    return true;
}

bool s_idiv(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        i64 d = static_cast<i64>(rd_reg(st, v, i.a.reg, 8));
        u64 lo = static_cast<u64>(st.r[v][static_cast<int>(R::Rax)]);
        u64 hi = static_cast<u64>(st.r[v][static_cast<int>(R::Rdx)]);
        __int128 n = (static_cast<__int128>(static_cast<i64>(hi)) << 64) |
                     static_cast<__uint128_t>(lo);
        if (d == 0) return false;
        __int128 q = n / d;
        if (q > std::numeric_limits<i64>::max() || q < std::numeric_limits<i64>::min())
            return false;
        __int128 r = n % d;
        st.r[v][static_cast<int>(R::Rax)] = static_cast<i64>(q);
        st.r[v][static_cast<int>(R::Rdx)] = static_cast<i64>(r);
        for (u32 f = 0; f < superopt::kNF; ++f) poison_flag(st, v, f);
    }
    return true;
}

bool s_udiv(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 d = rd_reg(st, v, i.a.reg, 8);
        unsigned __int128 n =
            (static_cast<unsigned __int128>(static_cast<u64>(st.r[v][static_cast<int>(R::Rdx)])) << 64) |
            static_cast<u64>(st.r[v][static_cast<int>(R::Rax)]);
        if (d == 0) return false;
        unsigned __int128 q = n / d;
        if (q > std::numeric_limits<u64>::max()) return false;
        st.r[v][static_cast<int>(R::Rax)] = static_cast<i64>(static_cast<u64>(q));
        st.r[v][static_cast<int>(R::Rdx)] =
            static_cast<i64>(static_cast<u64>(n % d));
        for (u32 f = 0; f < superopt::kNF; ++f) poison_flag(st, v, f);
    }
    return true;
}

bool s_mulhi(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = static_cast<u64>(st.r[v][static_cast<int>(R::Rax)]);
        u64 b = rd_reg(st, v, i.a.reg, 8);
        unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
        u64 hi = static_cast<u64>(p >> 64);
        u64 lo = static_cast<u64>(p);
        st.r[v][static_cast<int>(R::Rax)] = static_cast<i64>(lo);
        st.r[v][static_cast<int>(R::Rdx)] = static_cast<i64>(hi);
        st.f[v][2] = hi != 0 ? 1 : 0; // CF=OF: high half nonzero
        st.f[v][3] = hi != 0 ? 1 : 0;
        poison_flag(st, v, 0);
        poison_flag(st, v, 1);
        poison_flag(st, v, 4);
    }
    return true;
}

bool s_cmprr(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = rd_reg(st, v, i.a.reg, i.size);
        u64 b = rd_reg(st, v, i.b.reg, i.size);
        set_sub_flags(st, v, a, b, i.size);
    }
    return true;
}

bool s_cmprimm(VState& st, const i32* slots, u32 nslots, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = i.a.k == Operand::K::Slot
                    ? rd_slot(st, v, slots, nslots, i.a.slot, i.size)
                    : rd_reg(st, v, i.a.reg, i.size);
        u64 b = static_cast<u64>(i.b.imm);
        if (i.size == 4) b &= 0xFFFFFFFFull;
        set_sub_flags(st, v, a, b, i.size);
    }
    return true;
}

bool s_test(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = rd_reg(st, v, i.a.reg, i.size);
        set_logic_flags(st, v, a & a, i.size);
    }
    return true;
}

bool s_setcc(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 old = static_cast<u64>(st.r[v][static_cast<int>(i.a.reg)]);
        u64 val = eval_cond(i.cond, st, v) ? 1 : 0;
        st.r[v][static_cast<int>(i.a.reg)] = static_cast<i64>((old & ~0xFFull) | val);
    }
    return true;
}

bool s_cmov(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        if (eval_cond(i.cond, st, v))
            wr_reg(st, v, i.a.reg, i.size, rd_reg(st, v, i.b.reg, i.size));
    }
    return true;
}

bool s_movzx(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 al = static_cast<u64>(st.r[v][static_cast<int>(R::Rax)]) & 0xFF;
        st.r[v][static_cast<int>(i.a.reg)] = static_cast<i64>(al);
    }
    return true;
}

bool s_sext32(VState& st, const i32*, u32, const Inst&) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 rax = static_cast<u64>(st.r[v][static_cast<int>(R::Rax)]);
        st.r[v][static_cast<int>(R::Rax)] =
            static_cast<i64>(static_cast<i32>(static_cast<u32>(rax & 0xFFFFFFFF)));
    }
    return true;
}

bool s_xoreax(VState& st, const i32*, u32, const Inst&) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        st.r[v][static_cast<int>(R::Rax)] = 0;
        st.f[v][0] = 1;
        st.f[v][1] = 0;
        st.f[v][2] = 0;
        st.f[v][3] = 0;
        st.f[v][4] = 1; // even parity of zero
    }
    return true;
}

void e_rax_rw(const Inst&, Effects& e) {
    e.gpr_read |= bit(R::Rax);
    e.gpr_write |= bit(R::Rax);
}
void e_rax_read(const Inst&, Effects& e) { e.gpr_read |= bit(R::Rax); }
void e_rax_write(const Inst&, Effects& e) { e.gpr_write |= bit(R::Rax); }
void e_xoreax(const Inst&, Effects& e) {
    e.gpr_write |= bit(R::Rax);
    e.writes_flags = true;
}
void e_movzx(const Inst& i, Effects& e) {
    e.gpr_read |= bit(R::Rax); // reads %al
    e.gpr_write |= bit(i.a.reg);
}
bool v_reg_a(const Inst& i) {
    return i.a.k == Operand::K::Reg && reg_ok(i.a.reg);
}

} // namespace

// ---- the table ----------------------------------------------------------------

const superopt::IsaTable& x64_superopt_isa() {
    static const superopt::IsaTable t = [] {
        superopt::IsaTable tb;
        using superopt::kVecSearch;
        auto add = [&tb](IOp op, i32 (*lat)(const Inst&),
                         bool (*sim)(VState&, const i32*, u32, const Inst&),
                         void (*eff)(const Inst&, Effects&),
                         bool (*valid)(const Inst&),
                         std::initializer_list<OpDef> ods, const BinOp* bins,
                         u32 nbins, const bool* sars, const Cond* conds,
                         u32 nconds, const u8* sizes, u32 nsizes) {
            Row r;
            r.op = op;
            r.latency = lat;
            r.sim = sim;
            r.effects = eff;
            r.valid = valid;
            u32 k = 0;
            for (const OpDef& d : ods)
                if (k < 4) r.ops[k++] = d;
            r.nops = k;
            r.bins = bins;
            r.nbins = nbins;
            r.sars = sars;
            r.conds = conds;
            r.nconds = nconds;
            r.sizes = sizes;
            r.nsizes = nsizes;
            tb.push_back(r);
        };

        // ---- searchable rows (candidate alphabet) ------------------------
        // (op, latency, semantics, effects, validity, operands,
        //  bin variants, condition variants, size/scale variants)
        add(IOp::MovRR, lat_op, s_movrr, e_rr, v_rr, {{'R',0},{'R',1}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::MovSR, lat_ld, s_movsr, e_sr, v_sr, {{'R',0},{'S',1}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::MovRS, lat_st, s_movrs, e_rs, v_rs, {{'R',0},{'S',1}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::MovRImm, lat_imm, s_movrimm, e_write_a, v_rimm, {{'R',0},{'I',1}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::MovSImm, lat_st, s_movsimm, e_simm, v_simm, {{'S',0},{'I',1}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::ArithRR, lat_arith, s_arith_rr, e_arith_flags, v_rr, {{'R',0},{'R',1}},
            kArithBins, 6, kArithSars, nullptr, 0, kSizes48, 2);
        add(IOp::ArithRImm, lat_arith, s_arith_rimm, e_arith_flags, v_arith_imm,
            {{'R',0},{'I',1}}, kArithBins, 6, kArithSars, nullptr, 0, kSizes48, 2);
        add(IOp::ShiftImm, lat_op, s_shiftimm, e_shiftimm_flags, v_shift, {{'R',0},{'I',1}},
            kShiftBins, 3, kShiftSars, nullptr, 0, kSizes48, 2);
        add(IOp::ShiftCl, lat_op, s_shiftcl, e_shiftcl_flags, v_reg_a, {{'R',0}},
            kShiftBins, 3, kShiftSars, nullptr, 0, kSizes48, 2);
        add(IOp::Neg, lat_op, s_neg, e_r_flags, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::Not, lat_op, s_not, e_destructive, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::LeaRR, lat_op, s_learr, e_rr, v_learr, {{'R',0},{'R',1},{'I',3}},
            nullptr, 0, nullptr, nullptr, 0, kScales, 4);
        add(IOp::Lea2, lat_op, s_lea2, e_lea2, v_lea2,
            {{'R',0},{'R',1},{'R',2},{'I',3}},
            nullptr, 0, nullptr, nullptr, 0, kScales, 4);
        add(IOp::Cqo, lat_op, s_cqo, e_cqo, v_one, {},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::IDiv, lat_div, s_idiv, e_div, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::UDiv, lat_div, s_udiv, e_div, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::MulHi, lat_mulhi, s_mulhi, e_mulhi, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::CmpRR, lat_op, s_cmprr, e_cmp_rr, v_rr, {{'R',0},{'R',1}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::CmpRImm, lat_op, s_cmprimm, e_cmp_rimm, v_cmp_rimm,
            {{'B',0},{'I',1}}, nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::Test, lat_op, s_test, e_test, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::Setcc, lat_op, s_setcc, e_setcc, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, kAllConds, 12, nullptr, 0);
        add(IOp::Cmov, lat_op, s_cmov, e_cmov, v_rr, {{'R',0},{'R',1}},
            nullptr, 0, nullptr, kAllConds, 12, kSizes48, 2);
        add(IOp::MovZX, lat_op, s_movzx, e_movzx, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::SExt32, lat_op, s_sext32, e_rax_rw, v_one, {},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::XorEax, lat_op, s_xoreax, e_xoreax, v_one, {},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);

        // ---- boundary rows (windows break; effects feed liveness) -------
        auto bnd = [&tb](IOp op, void (*eff)(const Inst&, Effects&)) {
            Row r;
            r.op = op;
            r.latency = lat_op;
            r.effects = eff;
            tb.push_back(r);
        };
        bnd(IOp::Nop, e_none);
        bnd(IOp::Label, e_none);
        bnd(IOp::Comment, e_none);
        bnd(IOp::LoadMem, e_loadmem);
        bnd(IOp::StoreMem, e_storemem);
        bnd(IOp::LeaSlot, e_leaslot);
        bnd(IOp::LeaSym, e_write_a);
        bnd(IOp::Jcc, e_jcc);
        bnd(IOp::Jmp, e_none);
        bnd(IOp::CallSym, e_call);
        bnd(IOp::CallFn, e_call);
        bnd(IOp::TailCallFn, e_tailcall);
        bnd(IOp::TailCallNaked, e_tailcall);
        bnd(IOp::FpBin, e_none);
        bnd(IOp::FpCmp, e_fpcmp);
        bnd(IOp::FpNeg, e_none);
        bnd(IOp::CvtToFp, e_rax_read);
        bnd(IOp::CvtToInt, e_rax_write);
        bnd(IOp::FpExt, e_none);
        bnd(IOp::FpTrunc, e_none);
        bnd(IOp::MovFpS, e_movfp_s);
        bnd(IOp::MovFpR, e_movfp_r);
        bnd(IOp::MovFpFromGpr, e_rax_read);
        bnd(IOp::MovFpFromGpr32, e_rax_read);
        bnd(IOp::MovFpFp, e_none);
        bnd(IOp::FpZero, e_none);
        bnd(IOp::PushRbp, e_none);
        bnd(IOp::PopRbp, e_none);
        bnd(IOp::Ret, e_ret);
        bnd(IOp::FrameSub, e_none);
        bnd(IOp::PushCal, e_read_a);
        bnd(IOp::RestoreCal, e_write_a);
        bnd(IOp::PopCal, e_write_a);
        bnd(IOp::RetNaked, e_ret);
        bnd(IOp::VecBinF64, e_none);
        bnd(IOp::VecBinI64, e_none);
        bnd(IOp::VecBinI32, e_none);
        bnd(IOp::VecBinF32, e_none);
        bnd(IOp::VecLogical, e_none);
        bnd(IOp::VecCmpI32, e_none);
        bnd(IOp::VecCmpF32, e_none);
        bnd(IOp::VecCmpF64, e_none);
        bnd(IOp::VecExtract, e_none);
        bnd(IOp::VecBcast, e_none);
        bnd(IOp::PgoInc, e_none);
        bnd(IOp::PgoSketch, e_pgo_sketch);
        return tb;
    }();
    return t;
}

// ---- module entry (pass 92) ---------------------------------------------------

namespace {

bool env_flag(const char* name, bool def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    return *v != '0';
}

u32 env_u32(const char* name, u32 def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    long x = std::atol(v);
    return x >= 0 ? static_cast<u32>(x) : def;
}

} // namespace

bool x64_superopt_module(LinearModule& lin) {
    if (!env_flag("JULES_SUPEROPT", true)) return false;
    const superopt::IsaTable& isa = x64_superopt_isa();
    superopt::Opts o;
    o.pops_per_window = env_u32("JULES_SUPEROPT_POPS", o.pops_per_window);
    o.pops_per_fn = env_u32("JULES_SUPEROPT_FN_POPS", o.pops_per_fn);
    o.candidates_per_pop = env_u32("JULES_SUPEROPT_CANDS", o.candidates_per_pop);
    o.candidates_per_window = env_u32("JULES_SUPEROPT_WIN_CANDS",
                                      o.candidates_per_window);
    o.states_per_window = env_u32("JULES_SUPEROPT_STATES", o.states_per_window);
    // Tier-4 policy (JULES_SUPEROPT_SMT): 0 = off; 1 = best-effort when the
    // solver is found (default); 2 = proof required (Unknown rejects).
    // The cross-probe (JULES_SUPEROPT_SMT_SELFTEST=1) runs the differential
    // lock on every attempted window and aborts on disagreement.
    int smt_mode = 1;
    if (const char* v = std::getenv("JULES_SUPEROPT_SMT"))
        if (*v) smt_mode = std::atoi(v);
    if (smt_mode > 0 && x64_super_smt_available()) {
        o.tier4 = &x64_super_tier4;
        o.tier4_required = smt_mode >= 2;
        if (env_flag("JULES_SUPEROPT_SMT_SELFTEST", false))
            o.tier4_selftest = &x64_super_smt_selftest;
    }
    bool any = false;
    const bool stats = env_flag("JULES_SUPEROPT_STATS", false);
    const bool tput = env_flag("JULES_SUPEROPT_TPUT", false);
    // Throughput telemetry uses wall clock for the RATES ONLY — search
    // behavior, budgets and results are still pop-count driven and
    // fully deterministic; this clock never gates anything.
    std::chrono::steady_clock::time_point t0, t1;
    u64 tp_pops = 0, tp_con = 0, tp_sim = 0;
    if (tput) t0 = std::chrono::steady_clock::now();
    for (LFunction& lf : lin.fns) {
        superopt::Report r = superopt::run(lf, isa, o);
        if (tput) {
            tp_pops += r.pops;
            tp_con += r.constructions;
            tp_sim += r.simulations;
        }
        if (r.improved || r.erased) any = true;
        if (stats && (r.windows || r.skipped_dead_def || r.skipped_flag_in ||
                    r.skipped_orig_fault))
            std::fprintf(stderr,
                         "[superopt] fn %d: windows=%u improved=%u erased=%u "
                         "pops=%llu saved=%lld units (attempted=%lld, dead_def=%u "
                         "flag_in=%u undef=%u fault=%u exhaust=%u, "
                         "t4: proven=%u refuted=%u unknown=%u probe=%u)\n",
                         static_cast<int>(lf.fid), r.windows, r.improved, r.erased,
                         static_cast<unsigned long long>(r.pops),
                         static_cast<long long>(r.committed_before - r.committed_after),
                         static_cast<long long>(r.attempted_cost), r.skipped_dead_def,
                         r.skipped_flag_in, r.skipped_undef_flags,
                         r.skipped_orig_fault, r.budget_exhausted,
                         r.tier4_proven, r.tier4_refuted, r.tier4_unknown,
                         r.smt_selfchecks);
    }
    if (tput) {
        t1 = std::chrono::steady_clock::now();
        double sec =
            std::chrono::duration<double>(t1 - t0).count();
        if (sec > 0.0)
            std::fprintf(stderr,
                         "[superopt] search-core throughput: %.3fs | "
                         "pops=%llu (%.2fM/s) constructions=%llu (%.2fM/s) "
                         "simulations=%llu (%.2fM/s)\n",
                         sec, static_cast<unsigned long long>(tp_pops),
                         static_cast<double>(tp_pops) / sec / 1e6,
                         static_cast<unsigned long long>(tp_con),
                         static_cast<double>(tp_con) / sec / 1e6,
                         static_cast<unsigned long long>(tp_sim),
                         static_cast<double>(tp_sim) / sec / 1e6);
    }
    return any;
}

} // namespace jules
