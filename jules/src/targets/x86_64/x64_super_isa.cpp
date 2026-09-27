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
using superopt::kNF;
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
    u64 x = static_cast<u64>(st.r[static_cast<int>(r)][v]);
    return size == 4 ? (x & 0xFFFFFFFFull) : x;
}

void wr_reg(VState& st, u32 v, R r, u8 size, u64 val) {
    if (size == 4) val &= 0xFFFFFFFFull; // 32-bit writes zero-extend
    st.r[static_cast<int>(r)][v] = static_cast<i64>(val);
}

// flag lanes: 0=ZF 1=SF 2=OF 3=CF 4=PF

void poison_flag(VState& st, u32 v, u32 lane) {
    st.f[lane][v] = static_cast<i64>(poison_lane(v, lane));
}

// ---- SIMD lane execution (the vector encodings of the closures) ---------
//
// One closure body per opcode, one ENCODING per lane width: the generic
// template below is instantiated for the SCALAR reference policy (a
// ONE-ELEMENT vector), and the SAME body inlines into the
// target-attributed forwarders (AVX2 / AVX-512) — there are not two
// semantics to keep in sync, only one source compiled three ways. The
// differential lock (JULES_SUPEROPT_SIM=scalar|avx2|avx512,
// runner-enforced on the throughput kernel) proves the encodings agree
// bit-for-bit: identical binaries, identical exploration counts.
//
// SIGNATURE DISCIPLINE (the -Werror-clean rule): a vector value NEVER
// crosses a function signature by value — not as a parameter, not as a
// return. By-value vectors are an ABI construct (their REGISTER-passing
// convention depends on the target ISA; GCC's -Wpsabi is fatal here) and
// internal helpers have no ABI at all. Every helper exchanges vectors
// through pointers/references only; with always_inline the compiler
// SROA-erases the indirection, so the machine code is identical to the
// by-value form. The scalar lane being a ONE-ELEMENT vector is what makes
// the operators uniform: `a == b`, `a < b`, `&`, `|`, `~` yield
// ALL-ONES / ZERO masks at every width (the mask discipline the policy
// prototype caught: a bare scalar `==` yields 0/1 and an `~m` blend then
// silently corrupts) — no per-width comparison helpers exist to keep in
// sync. (vector_size(8) is INTEGER-class, ABI-stable since forever.)
//
// The chunk bound: widths dividing kVecSearch (12) cover exactly the real
// lanes; the 8-wide encoding processes the padded 16 (two zmm chunks) —
// the pad lanes are never read semantically and the engine's fold masks
// them PER BLOCK, keeping them out of the fingerprint. Writers here must
// stay LANE-UNIFORM (a per-lane pad value would leak into the XOR-fold
// parity and make the fingerprint encoding-dependent); the poison table
// seeds its pads with zero for exactly this reason.

template <typename VT, u32 W> struct LaneT {
    using T = VT;
    static constexpr u32 LW = W;
    static inline __attribute__((always_inline)) void splat(u64 v, T& out) {
        for (u32 i = 0; i < W; ++i) out[i] = v;
    }
    static inline __attribute__((always_inline)) void ld(const i64* p, T& out) {
        out = *reinterpret_cast<const T*>(p);
    }
    static inline __attribute__((always_inline)) void ld(const u64* p, T& out) {
        ld(reinterpret_cast<const i64*>(p), out);
    }
    static inline __attribute__((always_inline)) void st(i64* p, const T& x) {
        *reinterpret_cast<T*>(p) = x;
    }
};
using LaneS  = LaneT<u64 __attribute__((vector_size(8))), 1>;
using LaneA2 = LaneT<u64 __attribute__((vector_size(32))), 4>;
using LaneA5 = LaneT<u64 __attribute__((vector_size(64))), 8>;

template <typename L>
constexpr u32 kLaneLIM = (superopt::kVecSearch % L::LW == 0) ? superopt::kVecSearch
                                                             : superopt::kVecPad;

// per-lane poison table (values identical to poison_lane(lane, flag)).
// ALIGNMENT CONTRACT: rows are vector-loaded (zmm claims natural 64-byte
// alignment) — the struct must be 64-byte aligned or the aligned loads
// fault (#GP).
struct alignas(64) PoisonTable {
    u64 t[kNF][superopt::kVecPad];
    static_assert(sizeof(t[0]) % 64 == 0,
                  "poison rows are vector-loaded: stride must be 64B-aligned");
    PoisonTable() {
        for (u32 f = 0; f < kNF; ++f)
            for (u32 v = 0; v < superopt::kVecPad; ++v)
                t[f][v] = v < superopt::kVecSearch ? poison_lane(v, f) : 0;
    }
};
const PoisonTable& poison_table() {
    static PoisonTable p;
    return p;
}

// window-local slot index (lane-uniform: resolved once per sim call)
static i32 slot_idx_of(const i32* slots, u32 nslots, i32 abs) {
    for (u32 k = 0; k < nslots; ++k)
        if (slots[k] == abs) return static_cast<i32>(k);
    return -1;
}

// lane-chunk helpers (sized reads/writes/flags; the shapes mirror the
// scalar reference exactly)

template <typename L>
static inline __attribute__((always_inline)) void
rd_regL(const VState& st, u32 b, R r, u8 size, typename L::T& x) {
    L::ld(&st.r[static_cast<u32>(r)][b], x);
    if (size == 4) {
        typename L::T m;
        L::splat(0xFFFFFFFFull, m);
        x = x & m;
    }
}

template <typename L>
static inline __attribute__((always_inline)) void
wr_regL(VState& st, u32 b, R r, u8 size, const typename L::T& val) {
    if (size == 4) { // 32-bit writes zero-extend
        typename L::T m;
        L::splat(0xFFFFFFFFull, m);
        L::st(&st.r[static_cast<u32>(r)][b], val & m);
    } else {
        L::st(&st.r[static_cast<u32>(r)][b], val);
    }
}

template <typename L>
static inline __attribute__((always_inline)) void
wr_slotL(VState& st, u32 b, u32 k, u8 size, const typename L::T& val) {
    if (size == 4) { // 32-bit slot writes preserve the upper 4 bytes
        typename L::T hi, lo;
        L::splat(0xFFFFFFFF00000000ull, hi);
        L::splat(0xFFFFFFFFull, lo);
        typename L::T old;
        L::ld(&st.s[k][b], old);
        L::st(&st.s[k][b], (old & hi) | (val & lo));
    } else {
        L::st(&st.s[k][b], val);
    }
}

template <typename L>
static inline __attribute__((always_inline)) void
parity8L(const typename L::T& x, typename L::T& out) {
    using T = typename L::T;
    T ff, four, two, one, z;
    L::splat(0xFF, ff);
    L::splat(4, four);
    L::splat(2, two);
    L::splat(1, one);
    L::splat(0, z);
    T p = x & ff;
    p = p ^ (p >> four);
    p = p ^ (p >> two);
    p = p ^ (p >> one);
    out = (p & one) == z; // mask: even parity of the low byte
}

template <typename L>
static inline __attribute__((always_inline)) void
add_flagsL(VState& st, u32 b, const typename L::T& a, const typename L::T& bb,
           u8 size) {
    using T = typename L::T;
    T one, z, sb, m, sh;
    L::splat(1, one);
    L::splat(0, z);
    L::splat(1ull << (size * 8 - 1), sb);
    L::splat(mask(size), m);
    L::splat(size * 8 - 1, sh);
    T full = a + bb;
    T res = full & m;
    L::st(&st.f[0][b], (res == z) & one);
    L::st(&st.f[1][b], (res >> sh) & one);
    // signed overflow: same-sign operands whose result flipped sign —
    // the ~ belongs INSIDE, on (a ^ bb) only.
    T ovr = ((~(a ^ bb)) & (a ^ res)) & sb;
    L::st(&st.f[2][b], (ovr != z) & one);
    T cf;
    if (size == 4) {
        T s32;
        L::splat(32, s32);
        cf = (full >> s32) & one;
    } else {
        cf = (full < a) & one; // u64 elements: unsigned lane compare
    }
    L::st(&st.f[3][b], cf);
    T p;
    parity8L<L>(res, p);
    L::st(&st.f[4][b], p & one);
}

template <typename L>
static inline __attribute__((always_inline)) void
sub_flagsL(VState& st, u32 b, const typename L::T& a, const typename L::T& bb,
           u8 size) {
    using T = typename L::T;
    T one, z, sb, m, sh;
    L::splat(1, one);
    L::splat(0, z);
    L::splat(1ull << (size * 8 - 1), sb);
    L::splat(mask(size), m);
    L::splat(size * 8 - 1, sh);
    T res = (a - bb) & m;
    L::st(&st.f[0][b], (res == z) & one);
    L::st(&st.f[1][b], (res >> sh) & one);
    L::st(&st.f[2][b], ((((a ^ bb) & (a ^ res) & sb) != z) & one));
    L::st(&st.f[3][b], (a < bb) & one);
    T p;
    parity8L<L>(res, p);
    L::st(&st.f[4][b], p & one);
}

template <typename L>
static inline __attribute__((always_inline)) void
logic_flagsL(VState& st, u32 b, const typename L::T& res_in, u8 size) {
    using T = typename L::T;
    T one, z, m, sh;
    L::splat(1, one);
    L::splat(0, z);
    L::splat(mask(size), m);
    L::splat(size * 8 - 1, sh);
    T res = res_in & m;
    L::st(&st.f[0][b], (res == z) & one);
    L::st(&st.f[1][b], (res >> sh) & one);
    L::st(&st.f[2][b], z);
    L::st(&st.f[3][b], z);
    T p;
    parity8L<L>(res, p);
    L::st(&st.f[4][b], p & one);
}

template <typename L>
static inline __attribute__((always_inline)) void
poisonL(VState& st, u32 b, u32 fl) {
    typename L::T v;
    L::ld(&poison_table().t[fl][b], v);
    L::st(&st.f[fl][b], v);
}

// arithmetic shift right (counts in [1,63], per-lane variable at the
// shift-cl encoding): (x >>u c) | (sign-smear << (64-c)) — the explicit
// unsigned formulation compiles to vpsrlvq/vpsllvq at every width, no
// reliance on any width's signed-shift availability.
template <typename L>
static inline __attribute__((always_inline)) void
sarL(const typename L::T& x, const typename L::T& cnt, typename L::T& out) {
    using T = typename L::T;
    T z, sixty3, sixty4;
    L::splat(0, z);
    L::splat(63, sixty3);
    L::splat(64, sixty4);
    T smear = z - (x >> sixty3);
    out = (x >> cnt) | (smear << (sixty4 - cnt));
}

// SIZE-AWARE arithmetic shift: a 32-bit sar fills from the SIZE's sign
// bit (bit 31), not bit 63 — the low half must be sign-extended first
// (shift up, sar back down), then the count applies. Missing this made
// every 32-bit sar of a negative value fill zeros above bit 31.
template <typename L>
static inline __attribute__((always_inline)) void
sarseL(const typename L::T& x, const typename L::T& cnt, u8 size,
       typename L::T& out) {
    using T = typename L::T;
    if (size == 4) {
        T s32;
        L::splat(32, s32);
        T hi = x << s32;
        sarL<L>(hi, s32, out);
        sarL<L>(out, cnt, out);
    } else {
        sarL<L>(x, cnt, out);
    }
}

// FULL-WIDTH nonzero mask by arithmetic: all-ones when x != 0, else 0.
// (Out-ref form — vector values never cross signatures by value.)
template <typename L>
static inline __attribute__((always_inline)) void
nzmask(const typename L::T& x, typename L::T& out) {
    typename L::T z, s63;
    L::splat(0, z);
    L::splat(63, s63);
    out = z - ((x | (z - x)) >> s63);
}

// condition evaluation over 0/1-or-poison flag vectors (truth = nonzero,
// exactly the scalar eval_cond's rule; poison flags read truthy)
template <typename L>
static inline __attribute__((always_inline)) void
condL(VState& st, u32 b, Cond c, typename L::T& out) {
    using T = typename L::T;
    T z;
    L::splat(0, z);
    T zf, sf, of, cf, pf;
    L::ld(&st.f[0][b], zf);
    L::ld(&st.f[1][b], sf);
    L::ld(&st.f[2][b], of);
    L::ld(&st.f[3][b], cf);
    L::ld(&st.f[4][b], pf);
    // TRUTH MASKS BY ARITHMETIC, FULL-WIDTH: nonzero(x) is
    // 0 - ((x | -x) >> 63) — 0 when x==0, ALL-ONES otherwise (any nonzero
    // poison smears, the "poison reads truthy" rule). The FIRST arithmetic
    // version forgot the negation and produced 0/1 — the ~m blends in
    // cmov then corrupted every bit except bit 0. Conditions follow the
    // reference's BOOL algebra exactly (eval_cond converts flags to bools
    // BEFORE comparing: sf != of is TRUTH-xor, not a raw-value compare).
    // Arithmetic masks also keep comparison trees out of the bitwise
    // algebra: GCC 14.2 ICEs in do_store_flag when compare results feed
    // |/^ combines stored to a variable-indexed struct slot (t_setcc).
    T zz, ss, oo, cc, pp;
    nzmask<L>(zf, zz);
    nzmask<L>(sf, ss);
    nzmask<L>(of, oo);
    nzmask<L>(cf, cc);
    nzmask<L>(pf, pp);
    switch (c) {
        case Cond::E: out = zz; return;
        case Cond::NE: out = ~zz; return;
        case Cond::L: out = ss ^ oo; return;
        case Cond::LE: out = zz | (ss ^ oo); return;
        case Cond::G: out = ~zz & ~(ss ^ oo); return;
        case Cond::GE: out = ~(ss ^ oo); return;
        case Cond::B: out = cc; return;
        case Cond::BE: out = cc | zz; return;
        case Cond::A: out = ~cc & ~zz; return;
        case Cond::AE: out = ~cc; return;
        case Cond::P: out = pp; return;
        case Cond::NP: out = ~pp; return;
    }
    out = z;
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
//
// The closure bodies: one generic template per opcode (always_inline — the
// lane policy picks the encoding; vector values live ONLY in locals, see
// the signature discipline above), plus the SCALAR-ONLY family where the
// semantics need 128-bit lanes (idiv/udiv/mulhi) or the full product
// (imul's overflow bit): those stay scalar at every tier, registered
// directly — honest, and rare in the search alphabet.

// 128-bit-lane semantics: imul's overflow check needs the full 64x64->128
// product; no SIMD lane form exists. Shared by the rr and rimm rows.
static bool arith_mul_scalar(VState& st, const Inst& i, bool b_imm) {
    const u64 m = mask(i.size);
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = rd_reg(st, v, i.a.reg, i.size);
        u64 b = b_imm ? static_cast<u64>(i.b.imm)
                      : rd_reg(st, v, i.b.reg, i.size);
        if (i.size == 4) b &= m;
        i64 sa = i.size == 4
                     ? static_cast<i64>(static_cast<i32>(static_cast<u32>(a)))
                     : static_cast<i64>(a);
        i64 sbb = i.size == 4
                      ? static_cast<i64>(static_cast<i32>(static_cast<u32>(b)))
                      : static_cast<i64>(b);
        __int128 full = static_cast<__int128>(sa) * static_cast<__int128>(sbb);
        u64 r = static_cast<u64>(static_cast<i64>(full)) & m;
        wr_reg(st, v, i.a.reg, i.size, r);
        i64 sext = i.size == 4
                       ? static_cast<i64>(static_cast<i32>(static_cast<u32>(r)))
                       : static_cast<i64>(r);
        u32 ovf = static_cast<__int128>(sext) != full ? 1 : 0;
        st.f[2][v] = ovf;
        st.f[3][v] = ovf;
        poison_flag(st, v, 0);
        poison_flag(st, v, 1);
        poison_flag(st, v, 4);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_movrr(VState& st, const i32*,
                                                   u32, const Inst& i) {
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T x;
        rd_regL<L>(st, b, i.b.reg, i.size, x);
        wr_regL<L>(st, b, i.a.reg, i.size, x);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_movsr(VState& st, const i32* slots,
                                                   u32 nslots, const Inst& i) {
    const i32 k = slot_idx_of(slots, nslots, i.b.slot);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T z;
        L::splat(0, z);
        typename L::T x = z; // absent slot reads as zero
        if (k >= 0) {
            L::ld(&st.s[k][b], x);
            if (i.size == 4) {
                typename L::T m;
                L::splat(0xFFFFFFFFull, m);
                x = x & m;
            }
        }
        wr_regL<L>(st, b, i.a.reg, i.size, x);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_movrs(VState& st, const i32* slots,
                                                   u32 nslots, const Inst& i) {
    const i32 k = slot_idx_of(slots, nslots, i.b.slot);
    if (k < 0) return true; // absent slot: the write no-ops
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T x;
        rd_regL<L>(st, b, i.a.reg, i.size, x);
        wr_slotL<L>(st, b, static_cast<u32>(k), i.size, x);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_movrimm(VState& st, const i32*,
                                                     u32, const Inst& i) {
    typename L::T v;
    L::splat(static_cast<u64>(i.b.imm), v);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW)
        wr_regL<L>(st, b, i.a.reg, 8, v);
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_movsimm(VState& st, const i32* slots,
                                                     u32 nslots, const Inst& i) {
    const i32 k = slot_idx_of(slots, nslots, i.a.slot);
    if (k < 0) return true;
    typename L::T v;
    L::splat(static_cast<u64>(i.b.imm), v);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW)
        L::st(&st.s[k][b], v); // 8-byte write: covers
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_arith_rr(VState& st, const i32*,
                                                      u32, const Inst& i) {
    if (i.bin == BinOp::Mul)
        return arith_mul_scalar(st, i, false);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T a, bb;
        rd_regL<L>(st, b, i.a.reg, i.size, a);
        rd_regL<L>(st, b, i.b.reg, i.size, bb);
        switch (i.bin) {
            case BinOp::Add: {
                typename L::T r = a + bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                add_flagsL<L>(st, b, a, bb, i.size);
                break;
            }
            case BinOp::Sub: {
                typename L::T r = a - bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                sub_flagsL<L>(st, b, a, bb, i.size);
                break;
            }
            case BinOp::And: {
                typename L::T r = a & bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                logic_flagsL<L>(st, b, r, i.size);
                break;
            }
            case BinOp::Or: {
                typename L::T r = a | bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                logic_flagsL<L>(st, b, r, i.size);
                break;
            }
            case BinOp::Xor: {
                typename L::T r = a ^ bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                logic_flagsL<L>(st, b, r, i.size);
                break;
            }
            default: return false; // not an encodable arith bin
        }
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_arith_rimm(VState& st, const i32*,
                                                        u32, const Inst& i) {
    if (i.bin == BinOp::Mul)
        return arith_mul_scalar(st, i, true);
    // sign-extended imm32; 32-bit ops use the low 32 bits of it
    typename L::T bb;
    L::splat(i.size == 4 ? (static_cast<u64>(i.b.imm) & 0xFFFFFFFFull)
                         : static_cast<u64>(i.b.imm),
             bb);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T a;
        rd_regL<L>(st, b, i.a.reg, i.size, a);
        switch (i.bin) {
            case BinOp::Add: {
                typename L::T r = a + bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                add_flagsL<L>(st, b, a, bb, i.size);
                break;
            }
            case BinOp::Sub: {
                typename L::T r = a - bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                sub_flagsL<L>(st, b, a, bb, i.size);
                break;
            }
            case BinOp::And: {
                typename L::T r = a & bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                logic_flagsL<L>(st, b, r, i.size);
                break;
            }
            case BinOp::Or: {
                typename L::T r = a | bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                logic_flagsL<L>(st, b, r, i.size);
                break;
            }
            case BinOp::Xor: {
                typename L::T r = a ^ bb;
                wr_regL<L>(st, b, i.a.reg, i.size, r);
                logic_flagsL<L>(st, b, r, i.size);
                break;
            }
            default: return false;
        }
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_shiftimm(VState& st, const i32*,
                                                      u32, const Inst& i) {
    const u64 cnt = static_cast<u64>(i.b.imm) & (i.size == 4 ? 31u : 63u);
    const u32 width = i.size * 8;
    if (cnt == 0) { // nothing shifts: register re-written, flags unchanged
        for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
            typename L::T a;
            rd_regL<L>(st, b, i.a.reg, i.size, a);
            wr_regL<L>(st, b, i.a.reg, i.size, a);
        }
        return true;
    }
    typename L::T m, sb, cv, one, z, cm1;
    L::splat(mask(i.size), m);
    L::splat(1ull << (width - 1), sb);
    L::splat(cnt, cv);
    L::splat(1, one);
    L::splat(0, z);
    L::splat(cnt - 1, cm1);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T a;
        rd_regL<L>(st, b, i.a.reg, i.size, a);
        typename L::T res, cf, of;
        if (i.bin == BinOp::Shl) {
            typename L::T wo;
            L::splat(width - cnt, wo);
            res = (a << cv) & m;
            cf = (a >> wo) & one;
        } else if (i.sar) {
            typename L::T sard;
            sarseL<L>(a, cv, i.size, sard);
            res = sard & m;
            cf = (a >> cm1) & one;
        } else {
            res = (a & m) >> cv;
            cf = (a >> cm1) & one;
        }
        wr_regL<L>(st, b, i.a.reg, i.size, res);
        L::st(&st.f[0][b], (res == z) & one);
        L::st(&st.f[1][b], ((res & sb) != z) & one);
        L::st(&st.f[3][b], cf);
        typename L::T p;
        parity8L<L>(res, p);
        L::st(&st.f[4][b], p & one);
        if (cnt == 1) { // OF defined only for 1-bit shifts
            if (i.bin == BinOp::Shl)
                of = (((res & sb) != z) & one) ^ cf;
            else if (i.sar)
                of = z;
            else
                of = ((a & sb) != z) & one;
            L::st(&st.f[2][b], of);
        } else {
            poisonL<L>(st, b, 2);
        }
    }
    return true;
}

// Variable-count shifts: the count is PER-LANE (rcx), so every count-
// dependent decision becomes a select — including the count==0 lanes
// (register re-written, flags pass through untouched).
//
// UB DISCIPLINE: the count-derived shift amounts (w64 - c for shl's CF,
// c - 1 for the others) are computed from c1 (the count OR 1) rather than
// c, because a 0-count lane would otherwise shift by 64/-1 — undefined in
// the C++ vector operations on every width. On count==0 lanes those CF
// values are garbage-by-construction and are unconditionally overwritten
// by the old flags through the zq select below.
template <typename L>
inline __attribute__((always_inline)) bool t_shiftcl(VState& st, const i32*,
                                                     u32, const Inst& i) {
    const u32 width = i.size * 8;
    const u32 cmask = i.size == 4 ? 31u : 63u;
    typename L::T m, sb, one, z, w64, w1, cmv, c1v;
    L::splat(mask(i.size), m);
    L::splat(1ull << (width - 1), sb);
    L::splat(1, one);
    L::splat(0, z);
    L::splat(width, w64);
    L::splat(width - 1, w1);
    L::splat(cmask, cmv);
    L::splat(1, c1v);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T a, cx;
        rd_regL<L>(st, b, i.a.reg, i.size, a);
        L::ld(&st.r[static_cast<u32>(R::Rcx)][b], cx);
        typename L::T c = cx & cmv;
        typename L::T zq = (c == z);           // count==0 mask
        typename L::T c1 = (zq & one) | (~zq & c); // count, or 1 where 0
        typename L::T res, cf, of;
        if (i.bin == BinOp::Shl) {
            res = (a << c) & m;
            cf = (a >> (w64 - c1)) & one;
        } else if (i.sar) {
            typename L::T sard;
            sarseL<L>(a, c1, i.size, sard);
            res = sard & m;
            cf = (a >> (c1 - c1v)) & one;
        } else {
            res = (a & m) >> c;
            cf = (a >> (c1 - c1v)) & one;
        }
        // count==0 lanes keep the old register and the old flags
        typename L::T zf0, sf0, of0, cf0, pf0;
        L::ld(&st.f[0][b], zf0);
        L::ld(&st.f[1][b], sf0);
        L::ld(&st.f[2][b], of0);
        L::ld(&st.f[3][b], cf0);
        L::ld(&st.f[4][b], pf0);
        res = (zq & a) | (~zq & res);
        if (i.bin == BinOp::Shl)
            of = (((res & sb) != z) & one) ^ cf;
        else if (i.sar)
            of = z;
        else
            of = ((a & sb) != z) & one;
        // OF is defined only for count==1 lanes; poison otherwise
        typename L::T one_m = (c == one);
        typename L::T po;
        L::ld(&poison_table().t[2][b], po);
        of = (one_m & of) | (~one_m & po);
        wr_regL<L>(st, b, i.a.reg, i.size, res);
        typename L::T p;
        parity8L<L>(res, p);
        L::st(&st.f[0][b], (zq & zf0) | (~zq & ((res == z) & one)));
        L::st(&st.f[1][b], (zq & sf0) | (~zq & ((res >> w1) & one)));
        L::st(&st.f[2][b], (zq & of0) | (~zq & of));
        L::st(&st.f[3][b], (zq & cf0) | (~zq & cf));
        L::st(&st.f[4][b], (zq & pf0) | (~zq & (p & one)));
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_neg(VState& st, const i32*,
                                                 u32, const Inst& i) {
    const u32 width = i.size * 8;
    typename L::T m, one, z, sb, sh;
    L::splat(mask(i.size), m);
    L::splat(1, one);
    L::splat(0, z);
    L::splat(1ull << (width - 1), sb);
    L::splat(width - 1, sh);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T a;
        rd_regL<L>(st, b, i.a.reg, i.size, a);
        typename L::T res = (z - a) & m;
        wr_regL<L>(st, b, i.a.reg, i.size, res);
        L::st(&st.f[0][b], (res == z) & one);
        L::st(&st.f[1][b], (res >> sh) & one);
        L::st(&st.f[2][b], (a == sb) & one);
        L::st(&st.f[3][b], (a != z) & one);
        typename L::T p;
        parity8L<L>(res, p);
        L::st(&st.f[4][b], p & one);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_not(VState& st, const i32*,
                                                 u32, const Inst& i) {
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T a;
        rd_regL<L>(st, b, i.a.reg, i.size, a);
        wr_regL<L>(st, b, i.a.reg, i.size, ~a); // no flags
    }
    return true;
}

// the lea family's scale axis rides in Inst.size as {1,2,4,8} — a power
// of two, so the multiply is a uniform shift
static u32 scale_shift(u8 scale) {
    return scale == 8 ? 3u : scale == 4 ? 2u : scale == 2 ? 1u : 0u;
}

template <typename L>
inline __attribute__((always_inline)) bool t_learr(VState& st, const i32*,
                                                   u32, const Inst& i) {
    const u32 sh = scale_shift(i.size);
    typename L::T disp, shv;
    L::splat(static_cast<u64>(i.b.imm), disp);
    L::splat(sh, shv);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T x;
        rd_regL<L>(st, b, i.b.reg, 8, x);
        wr_regL<L>(st, b, i.a.reg, 8, (x << shv) + disp);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_lea2(VState& st, const i32*,
                                                  u32, const Inst& i) {
    const R idx = static_cast<R>(i.b.slot);
    const u32 sh = scale_shift(i.size);
    typename L::T disp, shv;
    L::splat(static_cast<u64>(i.b.imm), disp);
    L::splat(sh, shv);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T base, ix;
        rd_regL<L>(st, b, i.b.reg, 8, base);
        rd_regL<L>(st, b, idx, 8, ix);
        wr_regL<L>(st, b, i.a.reg, 8, (base + (ix << shv)) + disp);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_cqo(VState& st, const i32*,
                                                 u32, const Inst&) {
    typename L::T z, sixty3;
    L::splat(0, z);
    L::splat(63, sixty3);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T x;
        L::ld(&st.r[static_cast<u32>(R::Rax)][b], x);
        // rdx = rax <s 0 ? -1 : 0 — the sign smear
        L::st(&st.r[static_cast<u32>(R::Rdx)][b], z - (x >> sixty3));
    }
    return true;
}

// IDiv / UDiv / MulHi: 128-bit lane semantics — SCALAR at every tier.
bool s_idiv(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        i64 d = static_cast<i64>(rd_reg(st, v, i.a.reg, 8));
        u64 lo = static_cast<u64>(st.r[static_cast<int>(R::Rax)][v]);
        u64 hi = static_cast<u64>(st.r[static_cast<int>(R::Rdx)][v]);
        __int128 n = (static_cast<__int128>(static_cast<i64>(hi)) << 64) |
                     static_cast<__uint128_t>(lo);
        if (d == 0) return false;
        __int128 q = n / d;
        if (q > std::numeric_limits<i64>::max() || q < std::numeric_limits<i64>::min())
            return false;
        __int128 r = n % d;
        st.r[static_cast<int>(R::Rax)][v] = static_cast<i64>(q);
        st.r[static_cast<int>(R::Rdx)][v] = static_cast<i64>(r);
        for (u32 f = 0; f < kNF; ++f) poison_flag(st, v, f);
    }
    return true;
}

bool s_udiv(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 d = rd_reg(st, v, i.a.reg, 8);
        unsigned __int128 n =
            (static_cast<unsigned __int128>(static_cast<u64>(st.r[static_cast<int>(R::Rdx)][v])) << 64) |
            static_cast<u64>(st.r[static_cast<int>(R::Rax)][v]);
        if (d == 0) return false;
        unsigned __int128 q = n / d;
        if (q > std::numeric_limits<u64>::max()) return false;
        st.r[static_cast<int>(R::Rax)][v] = static_cast<i64>(static_cast<u64>(q));
        st.r[static_cast<int>(R::Rdx)][v] =
            static_cast<i64>(static_cast<u64>(n % d));
        for (u32 f = 0; f < kNF; ++f) poison_flag(st, v, f);
    }
    return true;
}

bool s_mulhi(VState& st, const i32*, u32, const Inst& i) {
    for (u32 v = 0; v < superopt::kVecSearch; ++v) {
        u64 a = static_cast<u64>(st.r[static_cast<int>(R::Rax)][v]);
        u64 b = rd_reg(st, v, i.a.reg, 8);
        unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
        u64 hi = static_cast<u64>(p >> 64);
        u64 lo = static_cast<u64>(p);
        st.r[static_cast<int>(R::Rax)][v] = static_cast<i64>(lo);
        st.r[static_cast<int>(R::Rdx)][v] = static_cast<i64>(hi);
        st.f[2][v] = hi != 0 ? 1 : 0; // CF=OF: high half nonzero
        st.f[3][v] = hi != 0 ? 1 : 0;
        poison_flag(st, v, 0);
        poison_flag(st, v, 1);
        poison_flag(st, v, 4);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_cmprr(VState& st, const i32*,
                                                   u32, const Inst& i) {
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T a, bb;
        rd_regL<L>(st, b, i.a.reg, i.size, a);
        rd_regL<L>(st, b, i.b.reg, i.size, bb);
        sub_flagsL<L>(st, b, a, bb, i.size);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_cmprimm(VState& st, const i32* slots,
                                                     u32 nslots, const Inst& i) {
    const i32 k = i.a.k == Operand::K::Slot
                      ? slot_idx_of(slots, nslots, i.a.slot)
                      : -1;
    typename L::T bimm;
    L::splat(i.size == 4 ? (static_cast<u64>(i.b.imm) & 0xFFFFFFFFull)
                         : static_cast<u64>(i.b.imm),
             bimm);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T a;
        if (i.a.k == Operand::K::Slot) {
            if (k >= 0) {
                L::ld(&st.s[k][b], a);
                if (i.size == 4) {
                    typename L::T m;
                    L::splat(0xFFFFFFFFull, m);
                    a = a & m;
                }
            } else {
                L::splat(0, a); // absent slot reads as zero (the reference
                                 // rd_slot contract), never the register
            }
        } else {
            rd_regL<L>(st, b, i.a.reg, i.size, a);
        }
        sub_flagsL<L>(st, b, a, bimm, i.size);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_test(VState& st, const i32*,
                                                  u32, const Inst& i) {
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T a;
        rd_regL<L>(st, b, i.a.reg, i.size, a);
        logic_flagsL<L>(st, b, a, i.size);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_setcc(VState& st, const i32*,
                                                   u32, const Inst& i) {
    typename L::T ff;
    L::splat(~0xFFull, ff);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T old, m, one;
        L::ld(&st.r[static_cast<u32>(i.a.reg)][b], old);
        L::splat(1, one);
        condL<L>(st, b, i.cond, m);
        L::st(&st.r[static_cast<u32>(i.a.reg)][b],
              (old & ff) | (m & one)); // byte write
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_cmov(VState& st, const i32*,
                                                  u32, const Inst& i) {
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T m, a_full, bb;
        condL<L>(st, b, i.cond, m);
        L::ld(&st.r[static_cast<u32>(i.a.reg)][b], a_full); // full 64: the
        // destination is NOT written when the condition fails — a
        // size-masked read-back would truncate the register on false lanes
        rd_regL<L>(st, b, i.b.reg, i.size, bb);              // sized source
        // true: dest = zero/sign-correct sized src; false: dest unchanged
        L::st(&st.r[static_cast<u32>(i.a.reg)][b],
              (m & bb) | (~m & a_full));
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_movzx(VState& st, const i32*,
                                                    u32, const Inst& i) {
    // movzbq %al, dst: the DESTINATION IS REPLACED by the zero-extended
    // byte (not merged — the byte-merge belongs to setcc, whose write is
    // partial; this one is a full 8-byte register write).
    typename L::T mask8;
    L::splat(0xFF, mask8);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T rax;
        L::ld(&st.r[static_cast<u32>(R::Rax)][b], rax);
        L::st(&st.r[static_cast<u32>(i.a.reg)][b], rax & mask8);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_sext32(VState& st, const i32*,
                                                     u32, const Inst&) {
    typename L::T s32;
    L::splat(32, s32);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        typename L::T x, t, out;
        L::ld(&st.r[static_cast<u32>(R::Rax)][b], x);
        t = x << s32;
        sarL<L>(t, s32, out);
        L::st(&st.r[static_cast<u32>(R::Rax)][b], out);
    }
    return true;
}

template <typename L>
inline __attribute__((always_inline)) bool t_xoreax(VState& st, const i32*,
                                                    u32, const Inst&) {
    typename L::T z, one;
    L::splat(0, z);
    L::splat(1, one);
    for (u32 b = 0; b < kLaneLIM<L>; b += L::LW) {
        L::st(&st.r[static_cast<u32>(R::Rax)][b], z);
        L::st(&st.f[0][b], one);
        L::st(&st.f[1][b], z);
        L::st(&st.f[2][b], z);
        L::st(&st.f[3][b], z);
        L::st(&st.f[4][b], one); // even parity of zero
    }
    return true;
}

// ---- the SIMD forwarders ---------------------------------------------------
// The target-attributed entry points: the always-inline generic bodies
// compile under the forwarder's ISA context (default features are a
// subset, so GCC inlines them — verified by zmm/ymm in the disassembly).
// Registered only when the runtime CPU supports the width.

#define JULES_SO_SIM_FWD(name)                                                \
    __attribute__((target("avx2")))                                           \
    bool name##_a2(VState & st, const i32 * slots, u32 nslots,                 \
                   const Inst & i) {                                          \
        return name<LaneA2>(st, slots, nslots, i);                             \
    }                                                                          \
    __attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))             \
    bool name##_a5(VState & st, const i32 * slots, u32 nslots,                 \
                   const Inst & i) {                                          \
        return name<LaneA5>(st, slots, nslots, i);                             \
    }

JULES_SO_SIM_FWD(t_movrr)
JULES_SO_SIM_FWD(t_movsr)
JULES_SO_SIM_FWD(t_movrs)
JULES_SO_SIM_FWD(t_movrimm)
JULES_SO_SIM_FWD(t_movsimm)
JULES_SO_SIM_FWD(t_arith_rr)
JULES_SO_SIM_FWD(t_arith_rimm)
JULES_SO_SIM_FWD(t_shiftimm)
JULES_SO_SIM_FWD(t_shiftcl)
JULES_SO_SIM_FWD(t_neg)
JULES_SO_SIM_FWD(t_not)
JULES_SO_SIM_FWD(t_learr)
JULES_SO_SIM_FWD(t_lea2)
JULES_SO_SIM_FWD(t_cqo)
JULES_SO_SIM_FWD(t_cmprr)
JULES_SO_SIM_FWD(t_cmprimm)
JULES_SO_SIM_FWD(t_test)
JULES_SO_SIM_FWD(t_setcc)
JULES_SO_SIM_FWD(t_cmov)
JULES_SO_SIM_FWD(t_movzx)
JULES_SO_SIM_FWD(t_sext32)
JULES_SO_SIM_FWD(t_xoreax)

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
        //
        // SIM picks the lane ENCODING for the vectorizable rows: the
        // scalar reference instantiation, or the target-attributed
        // forwarders when the runtime CPU (or the JULES_SUPEROPT_SIM
        // override) selects a wider tier. The div/mulhi family and imul's
        // Mul variant stay scalar at every tier (128-bit lanes).
        using SimFn = bool (*)(VState&, const i32*, u32, const Inst&);
        const superopt::LaneMode lm = superopt::lane_mode();
        auto SIM = [lm](SimFn s, SimFn a2, SimFn a5) -> SimFn {
            switch (lm) {
                case superopt::LaneMode::Avx512: return a5;
                case superopt::LaneMode::Avx2: return a2;
                default: return s;
            }
        };
        add(IOp::MovRR, lat_op, SIM(t_movrr<LaneS>, t_movrr_a2, t_movrr_a5),
            e_rr, v_rr, {{'R',0},{'R',1}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::MovSR, lat_ld, SIM(t_movsr<LaneS>, t_movsr_a2, t_movsr_a5),
            e_sr, v_sr, {{'R',0},{'S',1}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::MovRS, lat_st, SIM(t_movrs<LaneS>, t_movrs_a2, t_movrs_a5),
            e_rs, v_rs, {{'R',0},{'S',1}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::MovRImm, lat_imm, SIM(t_movrimm<LaneS>, t_movrimm_a2, t_movrimm_a5),
            e_write_a, v_rimm, {{'R',0},{'I',1}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::MovSImm, lat_st, SIM(t_movsimm<LaneS>, t_movsimm_a2, t_movsimm_a5),
            e_simm, v_simm, {{'S',0},{'I',1}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::ArithRR, lat_arith,
            SIM(t_arith_rr<LaneS>, t_arith_rr_a2, t_arith_rr_a5),
            e_arith_flags, v_rr, {{'R',0},{'R',1}},
            kArithBins, 6, kArithSars, nullptr, 0, kSizes48, 2);
        add(IOp::ArithRImm, lat_arith,
            SIM(t_arith_rimm<LaneS>, t_arith_rimm_a2, t_arith_rimm_a5),
            e_arith_flags, v_arith_imm,
            {{'R',0},{'I',1}}, kArithBins, 6, kArithSars, nullptr, 0, kSizes48, 2);
        add(IOp::ShiftImm, lat_op,
            SIM(t_shiftimm<LaneS>, t_shiftimm_a2, t_shiftimm_a5),
            e_shiftimm_flags, v_shift, {{'R',0},{'I',1}},
            kShiftBins, 3, kShiftSars, nullptr, 0, kSizes48, 2);
        add(IOp::ShiftCl, lat_op,
            SIM(t_shiftcl<LaneS>, t_shiftcl_a2, t_shiftcl_a5),
            e_shiftcl_flags, v_reg_a, {{'R',0}},
            kShiftBins, 3, kShiftSars, nullptr, 0, kSizes48, 2);
        add(IOp::Neg, lat_op, SIM(t_neg<LaneS>, t_neg_a2, t_neg_a5),
            e_r_flags, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::Not, lat_op, SIM(t_not<LaneS>, t_not_a2, t_not_a5),
            e_destructive, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::LeaRR, lat_op, SIM(t_learr<LaneS>, t_learr_a2, t_learr_a5),
            e_rr, v_learr, {{'R',0},{'R',1},{'I',3}},
            nullptr, 0, nullptr, nullptr, 0, kScales, 4);
        add(IOp::Lea2, lat_op, SIM(t_lea2<LaneS>, t_lea2_a2, t_lea2_a5),
            e_lea2, v_lea2,
            {{'R',0},{'R',1},{'R',2},{'I',3}},
            nullptr, 0, nullptr, nullptr, 0, kScales, 4);
        add(IOp::Cqo, lat_op, SIM(t_cqo<LaneS>, t_cqo_a2, t_cqo_a5),
            e_cqo, v_one, {},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::IDiv, lat_div, s_idiv, e_div, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::UDiv, lat_div, s_udiv, e_div, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::MulHi, lat_mulhi, s_mulhi, e_mulhi, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::CmpRR, lat_op, SIM(t_cmprr<LaneS>, t_cmprr_a2, t_cmprr_a5),
            e_cmp_rr, v_rr, {{'R',0},{'R',1}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::CmpRImm, lat_op,
            SIM(t_cmprimm<LaneS>, t_cmprimm_a2, t_cmprimm_a5),
            e_cmp_rimm, v_cmp_rimm,
            {{'B',0},{'I',1}}, nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::Test, lat_op, SIM(t_test<LaneS>, t_test_a2, t_test_a5),
            e_test, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, kSizes48, 2);
        add(IOp::Setcc, lat_op, SIM(t_setcc<LaneS>, t_setcc_a2, t_setcc_a5),
            e_setcc, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, kAllConds, 12, nullptr, 0);
        add(IOp::Cmov, lat_op, SIM(t_cmov<LaneS>, t_cmov_a2, t_cmov_a5),
            e_cmov, v_rr, {{'R',0},{'R',1}},
            nullptr, 0, nullptr, kAllConds, 12, kSizes48, 2);
        add(IOp::MovZX, lat_op, SIM(t_movzx<LaneS>, t_movzx_a2, t_movzx_a5),
            e_movzx, v_reg_a, {{'R',0}},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::SExt32, lat_op, SIM(t_sext32<LaneS>, t_sext32_a2, t_sext32_a5),
            e_rax_rw, v_one, {},
            nullptr, 0, nullptr, nullptr, 0, nullptr, 0);
        add(IOp::XorEax, lat_op, SIM(t_xoreax<LaneS>, t_xoreax_a2, t_xoreax_a5),
            e_xoreax, v_one, {},
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
        const char* lm = "?";
        switch (superopt::lane_mode()) {
            case superopt::LaneMode::Scalar: lm = "scalar"; break;
            case superopt::LaneMode::Avx2: lm = "avx2"; break;
            case superopt::LaneMode::Avx512: lm = "avx512"; break;
        }
        if (sec > 0.0)
            std::fprintf(stderr,
                         "[superopt] search-core throughput (sim=%s): %.3fs | "
                         "pops=%llu (%.2fM/s) constructions=%llu (%.2fM/s) "
                         "simulations=%llu (%.2fM/s)\n",
                         lm, sec, static_cast<unsigned long long>(tp_pops),
                         static_cast<double>(tp_pops) / sec / 1e6,
                         static_cast<unsigned long long>(tp_con),
                         static_cast<double>(tp_con) / sec / 1e6,
                         static_cast<unsigned long long>(tp_sim),
                         static_cast<double>(tp_sim) / sec / 1e6);
    }
    return any;
}

} // namespace jules
