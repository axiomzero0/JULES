// The x86-64 Tier-4 SMT equivalence encoder — the Z3 verification layer.
//
// ROLE (see superopt.h for the ladder): encode the original window and the
// candidate replacement as two symbolic programs over the SAME inputs
// (live-in GPRs, live-in window slots, entry flags shared), and ask Z3
// whether the live-out contract can differ:
//
//     assert NOT [ fault_O == fault_C  AND  (fault  OR  contract equal) ]
//     check-sat  ->  unsat  ==  PROVEN equivalent on ALL inputs
//                 ->  sat    ==  REFUTED (a counterexample exists)
//
// This is strictly stronger than the sampled-lane gates: unsampled fault
// inputs and undefined-flag (poison) coincidences are both covered. The
// simulator's deterministic per-lane poison becomes an UNCONSTRAINED symbol
// here — a candidate may not rely on what undefined flags happened to hold.
//
// SECOND SEMANTIC SOURCE, LOCKED BY DIFFERENTIAL TEST: these emitters are a
// re-statement of x64_super_isa.cpp's sim closures as SMT-LIB2. The lock is
// the Tier-3 cross-probe (JULES_SUPEROPT_SMT_SELFTEST=1): for every window,
// the encoder re-evaluates the ORIGINAL on the batch-0 concrete inputs and
// must agree with the simulator bit-for-bit on the contract, else abort.
// Run the suite once with the flag on after any change to either file.
//
// NO instruction knowledge exists above this file and the ISA table — the
// engine knows nothing about SMT, Z3, or x86 semantics.
#include "superopt/superopt.h"
#include "x64_super_smt.h"

#include <unistd.h> // access, mkstemp, fdopen, popen, pclose, unlink

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

namespace jules {

using superopt::kMaxGpr;
using superopt::kMaxSlot;
using superopt::kNF;
using superopt::Tier4Query;
using superopt::Verdict;

namespace {

// ---- constants -----------------------------------------------------------

const std::string kZero64 = "#x0000000000000000";
const std::string kOne64 = "#x0000000000000001";
const std::string kAllOnes = "#xffffffffffffffff";
const std::string kMask32 = "#x00000000ffffffff";
const std::string kMaskHi32 = "#xffffffff00000000";
const std::string kMaskNotLo8 = "#xffffffffffffff00";

std::string hex64(u64 v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "#x%016llx",
                  static_cast<unsigned long long>(v));
    return std::string(buf);
}

// The simulator's poison (x64_super_isa.cpp, kept in sync for the probe).
u64 sm_splitmix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
u64 sm_poison_lane(u32 v, u32 lane) {
    return sm_splitmix64(0xBADA55C0DEull ^ (0x9E3779B9ull * (v + 1)) ^
                         (0x85EBCA77ull * (lane + 1)));
}

// ---- one symbolic program (one "side" of the equivalence) ---------------

struct Side {
    std::string tag;   // "o_" / "c_" — every local term name starts with it
    std::string decls;  // declare-funs local to this side (poison symbols)
    std::string defs;   // define-funs (the program)
    std::string r[kMaxGpr]; // current term per GPR
    std::string s[kMaxSlot]; // current term per window slot
    std::string f[kNF];  // ZF SF OF CF PF (bv64 terms)
    std::string fault = "false"; // Bool expression (OR of fault conditions)
    u32 step = 0;
    std::vector<std::pair<std::string, u32>> poisons; // (symbol, flag idx)
    // (is_idiv, divisor term, dividend-128 term) per division — for the
    // selftest's fault-liveness injections
    std::vector<std::tuple<bool, std::string, std::string>> divs;
};

std::string bv1_of(const std::string& t, u32 bit) {
    // bit `bit` of t as a 64-bit 0/1 term
    return "(ite (= ((_ extract " + std::to_string(bit) + " " +
           std::to_string(bit) + ") " + t + ") #b1) " + kOne64 + " " + kZero64 +
           ")";
}

std::string bool_of(const std::string& t, u32 bit) {
    return "(= ((_ extract " + std::to_string(bit) + " " +
           std::to_string(bit) + ") " + t + ") #b1)";
}

std::string ite64(const std::string& c, const std::string& a,
                  const std::string& b) {
    return "(ite " + c + " " + a + " " + b + ")";
}

// parity of the low byte, as bv64 0/1 (x86 PF: 1 = even parity)
std::string parity64(const std::string& res) {
    std::string p = "((_ extract 0 0) " + res + ")";
    for (u32 b = 1; b < 8; ++b)
        p = "(bvxor " + p + " ((_ extract " + std::to_string(b) + " " +
            std::to_string(b) + ") " + res + "))";
    return "(ite (= " + p + " #b0) " + kOne64 + " " + kZero64 + ")";
}

// condition booleans over the side's flag terms ("set" == nonzero)
std::string cond_bool(Cond c, const Side& S) {
    std::string zf = "(not (= " + S.f[0] + " " + kZero64 + "))";
    std::string sf = "(not (= " + S.f[1] + " " + kZero64 + "))";
    std::string of = "(not (= " + S.f[2] + " " + kZero64 + "))";
    std::string cf = "(not (= " + S.f[3] + " " + kZero64 + "))";
    std::string pf = "(not (= " + S.f[4] + " " + kZero64 + "))";
    switch (c) {
        case Cond::E: return zf;
        case Cond::NE: return "(not " + zf + ")";
        case Cond::L: return "(xor " + sf + " " + of + ")";
        case Cond::LE: return "(or " + zf + " (xor " + sf + " " + of + "))";
        case Cond::G: return "(and (not " + zf + ") (= " + sf + " " + of + "))";
        case Cond::GE: return "(= " + sf + " " + of + ")";
        case Cond::B: return cf;
        case Cond::BE: return "(or " + cf + " " + zf + ")";
        case Cond::A: return "(and (not " + cf + ") (not " + zf + "))";
        case Cond::AE: return "(not " + cf + ")";
        case Cond::P: return pf;
        case Cond::NP: return "(not " + pf + ")";
    }
    return "false";
}

// ---- state term management -----------------------------------------------

void def_reg(Side& S, u32 g, const std::string& expr) {
    const std::string name = S.tag + "R" + std::to_string(g) + "!" +
                             std::to_string(S.step);
    S.defs += "(define-fun " + name + " () (_ BitVec 64) " + expr + ")\n";
    S.r[g] = name;
}

void def_slot(Side& S, u32 k, const std::string& expr) {
    const std::string name = S.tag + "S" + std::to_string(k) + "!" +
                             std::to_string(S.step);
    S.defs += "(define-fun " + name + " () (_ BitVec 64) " + expr + ")\n";
    S.s[k] = name;
}

void def_flag(Side& S, u32 fi, const std::string& expr) {
    const std::string name = S.tag + "F" + std::to_string(fi) + "!" +
                             std::to_string(S.step);
    S.defs += "(define-fun " + name + " () (_ BitVec 64) " + expr + ")\n";
    S.f[fi] = name;
}

void def_poison_flag(Side& S, u32 fi) {
    const std::string name = S.tag + "U" + std::to_string(fi) + "!" +
                             std::to_string(S.step);
    S.decls += "(declare-fun " + name + " () (_ BitVec 64))\n";
    S.poisons.push_back({name, fi});
    S.f[fi] = name;
}

// read/write with the x86 width discipline (mirrors rd_reg/wr_reg)

std::string rd_reg(const Side& S, const Operand& op, u8 size) {
    const std::string& t = S.r[static_cast<u32>(op.reg)];
    return size == 4 ? "(bvand " + t + " " + kMask32 + ")" : t;
}

std::string wr_reg_expr(const std::string& val, u8 size) {
    return size == 4 ? "(bvand " + val + " " + kMask32 + ")" : val;
}

std::string wr_slot_expr(const std::string& old, const std::string& val,
                         u8 size) {
    if (size == 4)
        return "(bvor (bvand " + old + " " + kMaskHi32 + ") (bvand " + val +
               " " + kMask32 + "))";
    return val;
}

// ---- flag family builders (mirror set_add/sub/logic_flags) ---------------

void emit_add_flags(Side& S, const std::string& a, const std::string& b,
                    const std::string& res, u8 size) {
    const u32 sb = size == 4 ? 31 : 63;
    const std::string full = "(bvadd " + a + " " + b + ")";
    def_flag(S, 0, ite64("(= " + res + " " + kZero64 + ")", kOne64, kZero64));
    def_flag(S, 1, bv1_of(res, sb));
    def_flag(S, 2, ite64(bool_of("(bvand (bvnot (bvxor " + a + " " + b +
                                     ")) (bvxor " + a + " " + res + "))",
                                 sb),
                         kOne64, kZero64));
    if (size == 4)
        def_flag(S, 3, bv1_of(full, 32));
    else
        def_flag(S, 3, ite64("(bvult " + full + " " + a + ")", kOne64, kZero64));
    def_flag(S, 4, parity64(res));
}

void emit_sub_flags(Side& S, const std::string& a, const std::string& b,
                    const std::string& res, u8 size) {
    const u32 sb = size == 4 ? 31 : 63;
    def_flag(S, 0, ite64("(= " + res + " " + kZero64 + ")", kOne64, kZero64));
    def_flag(S, 1, bv1_of(res, sb));
    def_flag(S, 2, ite64(bool_of("(bvand (bvxor " + a + " " + b + ") (bvxor " +
                                     a + " " + res + "))",
                                 sb),
                         kOne64, kZero64));
    def_flag(S, 3, ite64("(bvult " + a + " " + b + ")", kOne64, kZero64));
    def_flag(S, 4, parity64(res));
}

void emit_logic_flags(Side& S, const std::string& res, u8 size) {
    const u32 sb = size == 4 ? 31 : 63;
    def_flag(S, 0, ite64("(= " + res + " " + kZero64 + ")", kOne64, kZero64));
    def_flag(S, 1, bv1_of(res, sb));
    def_flag(S, 2, kZero64);
    def_flag(S, 3, kZero64);
    def_flag(S, 4, parity64(res));
}

// ---- the instruction emitter (all 25 searchable rows) --------------------
//
// Mirrors x64_super_isa.cpp's sim closures exactly; the Tier-3 cross-probe
// is the lock that keeps them honest. Returns false on an op with no
// emitter (defensive — windows only contain searchable rows).

bool emit_inst(Side& S, const Inst& i, const i32* slots, u32 nslots) {
    const u32 sb = i.size == 4 ? 31 : 63;
    const std::string m32 = kMask32;
    const std::string sbc = i.size == 4 ? "#x0000000080000000" : "#x8000000000000000";
    ++S.step;

    switch (i.op) {
        case IOp::MovRR:
            def_reg(S, static_cast<u32>(i.a.reg),
                     wr_reg_expr(rd_reg(S, i.b, i.size), i.size));
            return true;
        case IOp::MovSR: {
            u32 k = 0;
            while (k < nslots && slots[k] != i.b.slot) ++k;
            if (k >= nslots) return false;
            const std::string val =
                i.size == 4 ? "(bvand " + S.s[k] + " " + m32 + ")" : S.s[k];
            def_reg(S, static_cast<u32>(i.a.reg), wr_reg_expr(val, i.size));
            return true;
        }
        case IOp::MovRS: {
            u32 k = 0;
            while (k < nslots && slots[k] != i.b.slot) ++k;
            if (k >= nslots) return false;
            def_slot(S, k, wr_slot_expr(S.s[k], rd_reg(S, i.a, i.size), i.size));
            return true;
        }
        case IOp::MovRImm:
            def_reg(S, static_cast<u32>(i.a.reg), hex64(static_cast<u64>(i.b.imm)));
            return true;
        case IOp::MovSImm: {
            u32 k = 0;
            while (k < nslots && slots[k] != i.a.slot) ++k;
            if (k >= nslots) return false;
            def_slot(S, k, hex64(static_cast<u64>(i.b.imm)));
            return true;
        }
        case IOp::ArithRR:
        case IOp::ArithRImm: {
            const std::string a = rd_reg(S, i.a, i.size);
            const std::string b =
                i.op == IOp::ArithRR
                    ? rd_reg(S, i.b, i.size)
                    : (i.size == 4
                           ? "(bvand " + hex64(static_cast<u64>(i.b.imm)) + " " + m32 + ")"
                           : hex64(static_cast<u64>(i.b.imm)));
            switch (i.bin) {
                case BinOp::Add: {
                    const std::string res = "(bvand (bvadd " + a + " " + b + ") " +
                                            (i.size == 4 ? m32 : kAllOnes) + ")";
                    def_reg(S, static_cast<u32>(i.a.reg), wr_reg_expr(res, i.size));
                    emit_add_flags(S, a, b, res, i.size);
                    return true;
                }
                case BinOp::Sub: {
                    const std::string res = "(bvand (bvsub " + a + " " + b + ") " +
                                            (i.size == 4 ? m32 : kAllOnes) + ")";
                    def_reg(S, static_cast<u32>(i.a.reg), wr_reg_expr(res, i.size));
                    emit_sub_flags(S, a, b, res, i.size);
                    return true;
                }
                case BinOp::Mul: {
                    // imul r, rm: signed low half; CF=OF=overflow;
                    // ZF/SF/PF undefined
                    const std::string sa = i.size == 4
                        ? "((_ sign_extend 32) ((_ extract 31 0) " + a + "))"
                        : a;
                    const std::string sbv = i.size == 4
                        ? "((_ sign_extend 32) ((_ extract 31 0) " + b + "))"
                        : b;
                    const std::string p128 = "(bvmul ((_ sign_extend 64) " + sa +
                                             ") ((_ sign_extend 64) " + sbv + "))";
                    const std::string r = "(bvand ((_ extract 63 0) " + p128 + ") " +
                                          (i.size == 4 ? m32 : kAllOnes) + ")";
                    def_reg(S, static_cast<u32>(i.a.reg), wr_reg_expr(r, i.size));
                    const std::string sext_r = i.size == 4
                        ? "((_ sign_extend 96) ((_ extract 31 0) " + r + "))"
                        : "((_ sign_extend 64) " + r + ")";
                    const std::string ovf =
                        ite64("(distinct " + sext_r + " " + p128 + ")", kOne64, kZero64);
                    def_flag(S, 2, ovf);
                    def_flag(S, 3, ovf);
                    def_poison_flag(S, 0);
                    def_poison_flag(S, 1);
                    def_poison_flag(S, 4);
                    return true;
                }
                case BinOp::And:
                case BinOp::Or:
                case BinOp::Xor: {
                    const char* opn = i.bin == BinOp::And ? "bvand"
                                  : i.bin == BinOp::Or ? "bvor" : "bvxor";
                    const std::string res =
                        "(" + std::string(opn) + " " + a + " " + b + ")";
                    def_reg(S, static_cast<u32>(i.a.reg), wr_reg_expr(res, i.size));
                    emit_logic_flags(S, res, i.size);
                    return true;
                }
                default: return false;
            }
        }
        case IOp::ShiftImm:
        case IOp::ShiftCl: {
            const std::string a = rd_reg(S, i.a, i.size);
            // the count: constant for ShiftImm, the rcx operand for ShiftCl
            // (x86 masks the count to 5/6 bits)
            const std::string c =
                i.op == IOp::ShiftImm
                    ? hex64(static_cast<u64>(i.b.imm) &
                            (i.size == 4 ? 31u : 63u))
                    : "(bvand " + S.r[static_cast<u32>(R::Rcx)] + " " +
                      (i.size == 4 ? "#x000000000000001f" : "#x000000000000003f") + ")";
            const std::string c_is_0 = "(= " + c + " " + kZero64 + ")";
            const std::string c_is_1 = "(= " + c + " " + kOne64 + ")";

            std::string res, cf, of_defined;
            if (i.bin == BinOp::Shl) {
                res = "(bvand (bvshl " + a + " " + c + ") " +
                      (i.size == 4 ? m32 : kAllOnes) + ")";
                cf = "(bvand (bvlshr " + a + " (bvsub " +
                     (i.size == 4 ? "#x0000000000000020" : "#x0000000000000040") +
                     " " + c + ")) " + kOne64 + ")";
                of_defined = "(bvxor " + bv1_of(res, sb) + " " + cf + ")";
            } else if (i.sar) {
                const std::string sa = i.size == 4
                    ? "((_ sign_extend 32) ((_ extract 31 0) " + a + "))"
                    : a;
                res = "(bvand (bvashr " + sa + " " + c + ") " +
                      (i.size == 4 ? m32 : kAllOnes) + ")";
                cf = "(bvand (bvlshr " + a + " (bvsub " + c + " " + kOne64 +
                     ")) " + kOne64 + ")";
                of_defined = kZero64;
            } else {
                res = "(bvlshr " + a + " " + c + ")";
                cf = "(bvand (bvlshr " + a + " (bvsub " + c + " " + kOne64 +
                     ")) " + kOne64 + ")";
                of_defined = bv1_of(a, sb);
            }

            // count == 0: pure write (flags pass through untouched)
            def_reg(S, static_cast<u32>(i.a.reg),
                    ite64(c_is_0, wr_reg_expr(rd_reg(S, i.a, i.size), i.size),
                          wr_reg_expr(res, i.size)));
            // count != 0 flags
            const std::string nz = "(not " + c_is_0 + ")";
            const std::string zf = ite64("(= " + res + " " + kZero64 + ")", kOne64, kZero64);
            const std::string sf = bv1_of(res, sb);
            const std::string pf = parity64(res);
            def_flag(S, 0, ite64(nz, zf, S.f[0]));
            def_flag(S, 1, ite64(nz, sf, S.f[1]));
            def_flag(S, 3, ite64(nz, cf, S.f[3]));
            def_flag(S, 4, ite64(nz, pf, S.f[4]));
            // OF: count==1 -> defined; count!=1 -> poison (fresh symbol)
            {
                const std::string pname = S.tag + "U2!" + std::to_string(S.step);
                S.decls += "(declare-fun " + pname + " () (_ BitVec 64))\n";
                S.poisons.push_back({pname, 2});
                def_flag(S, 2, ite64(nz, ite64(c_is_1, of_defined, pname), S.f[2]));
            }
            return true;
        }
        case IOp::Neg: {
            const std::string a = rd_reg(S, i.a, i.size);
            const std::string res = "(bvand (bvsub " + kZero64 + " " + a + ") " +
                                    (i.size == 4 ? m32 : kAllOnes) + ")";
            def_reg(S, static_cast<u32>(i.a.reg), wr_reg_expr(res, i.size));
            def_flag(S, 0, ite64("(= " + res + " " + kZero64 + ")", kOne64, kZero64));
            def_flag(S, 1, bv1_of(res, sb));
            def_flag(S, 2, ite64("(= " + a + " " + sbc + ")", kOne64, kZero64));
            def_flag(S, 3, ite64("(distinct " + a + " " + kZero64 + ")", kOne64, kZero64));
            def_flag(S, 4, parity64(res));
            return true;
        }
        case IOp::Not: {
            const std::string a = rd_reg(S, i.a, i.size);
            def_reg(S, static_cast<u32>(i.a.reg),
                    wr_reg_expr("(bvnot " + a + ")", i.size));
            return true;
        }
        case IOp::LeaRR: {
            const std::string b = S.r[static_cast<u32>(i.b.reg)]; // full 64
            const std::string val = "(bvadd (bvmul " + b + " " +
                                    hex64(static_cast<u64>(i.size)) + ") " +
                                    hex64(static_cast<u64>(i.b.imm)) + ")";
            def_reg(S, static_cast<u32>(i.a.reg), val);
            return true;
        }
        case IOp::Lea2: {
            const std::string b = S.r[static_cast<u32>(i.b.reg)];
            const std::string idx = S.r[static_cast<u32>(i.b.slot)];
            const std::string val =
                "(bvadd (bvadd " + b + " (bvmul " + idx + " " +
                hex64(static_cast<u64>(i.size)) + ")) " +
                hex64(static_cast<u64>(i.b.imm)) + ")";
            def_reg(S, static_cast<u32>(i.a.reg), val);
            return true;
        }
        case IOp::Cqo: {
            const std::string rax = S.r[static_cast<u32>(R::Rax)];
            def_reg(S, static_cast<u32>(R::Rdx),
                    ite64(bool_of(rax, 63), kAllOnes, kZero64));
            return true;
        }
        case IOp::IDiv: {
            const std::string d = rd_reg(S, i.a, 8);
            const std::string n128 = "(concat " + S.r[static_cast<u32>(R::Rdx)] +
                                     " " + S.r[static_cast<u32>(R::Rax)] + ")";
            const std::string d128 = "((_ sign_extend 64) " + d + ")";
            const std::string q = "(bvsdiv " + n128 + " " + d128 + ")";
            const std::string r = "(bvsrem " + n128 + " " + d128 + ")";
            const std::string div0 = "(= " + d + " " + kZero64 + ")";
            const std::string ovf = "(or (bvsgt " + q + " #x00000000000000007fffffffffffffff) (bvslt " + q + " #xffffffffffffffff8000000000000000))";
            const std::string flt = "(or " + div0 + " " + ovf + ")";
            S.fault = "(or " + S.fault + " " + flt + ")";
            S.divs.push_back({true, d, n128});
            def_reg(S, static_cast<u32>(R::Rax),
                    ite64(flt, S.r[static_cast<u32>(R::Rax)],
                          "((_ extract 63 0) " + q + ")"));
            def_reg(S, static_cast<u32>(R::Rdx),
                    ite64(flt, S.r[static_cast<u32>(R::Rdx)],
                          "((_ extract 63 0) " + r + ")"));
            for (u32 fi = 0; fi < kNF; ++fi) def_poison_flag(S, fi);
            return true;
        }
        case IOp::UDiv: {
            const std::string d = rd_reg(S, i.a, 8);
            const std::string n128 = "(concat " + S.r[static_cast<u32>(R::Rdx)] +
                                     " " + S.r[static_cast<u32>(R::Rax)] + ")";
            const std::string d128 = "((_ zero_extend 64) " + d + ")";
            const std::string q = "(bvudiv " + n128 + " " + d128 + ")";
            const std::string r = "(bvurem " + n128 + " " + d128 + ")";
            const std::string div0 = "(= " + d + " " + kZero64 + ")";
            const std::string ovf = "(bvugt " + q + " #x0000000000000000ffffffffffffffff)";
            const std::string flt = "(or " + div0 + " " + ovf + ")";
            S.fault = "(or " + S.fault + " " + flt + ")";
            S.divs.push_back({false, d, n128});
            def_reg(S, static_cast<u32>(R::Rax),
                    ite64(flt, S.r[static_cast<u32>(R::Rax)],
                          "((_ extract 63 0) " + q + ")"));
            def_reg(S, static_cast<u32>(R::Rdx),
                    ite64(flt, S.r[static_cast<u32>(R::Rdx)],
                          "((_ extract 63 0) " + r + ")"));
            for (u32 fi = 0; fi < kNF; ++fi) def_poison_flag(S, fi);
            return true;
        }
        case IOp::MulHi: {
            const std::string a = S.r[static_cast<u32>(R::Rax)];
            const std::string b = rd_reg(S, i.a, 8);
            const std::string p128 = "(bvmul ((_ zero_extend 64) " + a +
                                     ") ((_ zero_extend 64) " + b + "))";
            const std::string hi = "((_ extract 127 64) " + p128 + ")";
            const std::string lo = "((_ extract 63 0) " + p128 + ")";
            def_reg(S, static_cast<u32>(R::Rax), lo);
            def_reg(S, static_cast<u32>(R::Rdx), hi);
            const std::string ovf = ite64("(distinct " + hi + " " + kZero64 + ")",
                                          kOne64, kZero64);
            def_flag(S, 2, ovf);
            def_flag(S, 3, ovf);
            def_poison_flag(S, 0);
            def_poison_flag(S, 1);
            def_poison_flag(S, 4);
            return true;
        }
        case IOp::CmpRR: {
            const std::string a = rd_reg(S, i.a, i.size);
            const std::string b = rd_reg(S, i.b, i.size);
            const std::string res = "(bvand (bvsub " + a + " " + b + ") " +
                                    (i.size == 4 ? m32 : kAllOnes) + ")";
            emit_sub_flags(S, a, b, res, i.size);
            return true;
        }
        case IOp::CmpRImm: {
            u32 k = 0;
            while (k < nslots && !(i.a.k == Operand::K::Slot && slots[k] == i.a.slot)) ++k;
            const bool is_slot = i.a.k == Operand::K::Slot;
            if (is_slot && k >= nslots) return false;
            const std::string a = is_slot
                ? (i.size == 4 ? "(bvand " + S.s[k] + " " + m32 + ")" : S.s[k])
                : rd_reg(S, i.a, i.size);
            const std::string b = i.size == 4
                ? "(bvand " + hex64(static_cast<u64>(i.b.imm)) + " " + m32 + ")"
                : hex64(static_cast<u64>(i.b.imm));
            const std::string res = "(bvand (bvsub " + a + " " + b + ") " +
                                    (i.size == 4 ? m32 : kAllOnes) + ")";
            emit_sub_flags(S, a, b, res, i.size);
            return true;
        }
        case IOp::Test: {
            const std::string a = rd_reg(S, i.a, i.size);
            const std::string res = "(bvand " + a + " " + a + ")";
            emit_logic_flags(S, res, i.size);
            return true;
        }
        case IOp::Setcc: {
            const std::string old = S.r[static_cast<u32>(i.a.reg)];
            const std::string val = ite64(cond_bool(i.cond, S), kOne64, kZero64);
            def_reg(S, static_cast<u32>(i.a.reg),
                    "(bvor (bvand " + old + " " + kMaskNotLo8 + ") " + val + ")");
            return true;
        }
        case IOp::Cmov: {
            const std::string taken = wr_reg_expr(rd_reg(S, i.b, i.size), i.size);
            const std::string old = S.r[static_cast<u32>(i.a.reg)];
            def_reg(S, static_cast<u32>(i.a.reg), ite64(cond_bool(i.cond, S), taken, old));
            return true;
        }
        case IOp::MovZX: {
            def_reg(S, static_cast<u32>(i.a.reg),
                    "(bvand " + S.r[static_cast<u32>(R::Rax)] + " " +
                    "#x00000000000000ff" + ")");
            return true;
        }
        case IOp::SExt32: {
            def_reg(S, static_cast<u32>(R::Rax),
                    "((_ sign_extend 32) ((_ extract 31 0) " +
                        S.r[static_cast<u32>(R::Rax)] + "))");
            return true;
        }
        case IOp::XorEax: {
            def_reg(S, static_cast<u32>(R::Rax), kZero64);
            def_flag(S, 0, kOne64);
            def_flag(S, 1, kZero64);
            def_flag(S, 2, kZero64);
            def_flag(S, 3, kZero64);
            def_flag(S, 4, kOne64);
            return true;
        }
        default: return false; // not a searchable row
    }
}

// ---- query construction ---------------------------------------------------

void init_side(Side& S, const char* tag, const Tier4Query& q) {
    S.tag = tag;
    for (u32 g = 0; g < kMaxGpr; ++g)
        S.r[g] = ((q.livein_gpr >> g) & 1)
                     ? ("X_R" + std::to_string(g))
                     : ("U_R" + std::to_string(g));
    for (u32 k = 0; k < q.nslots; ++k)
        S.s[k] = ((q.livein_slots >> k) & 1)
                     ? ("X_S" + std::to_string(k))
                     : ("U_S" + std::to_string(k));
    for (u32 fi = 0; fi < kNF; ++fi) S.f[fi] = "X_F" + std::to_string(fi);
}

// Shared between the two sides: the live-in inputs (X_) and the
// never-read-by-construction locations (U_ — the engine's definedness rule
// forbids reading them before a write; leaving them UNCONSTRAINED in the
// equivalence query is the sound direction, and the cross-probe catches any
// violation loudly).
std::string shared_decls(const Tier4Query& q) {
    std::string d;
    for (u32 g = 0; g < kMaxGpr; ++g)
        d += "(declare-fun " +
             std::string(((q.livein_gpr >> g) & 1) ? "X_R" : "U_R") +
             std::to_string(g) + " () (_ BitVec 64))\n";
    for (u32 k = 0; k < q.nslots; ++k)
        d += "(declare-fun " +
             std::string(((q.livein_slots >> k) & 1) ? "X_S" : "U_S") +
             std::to_string(k) + " () (_ BitVec 64))\n";
    for (u32 fi = 0; fi < kNF; ++fi)
        d += "(declare-fun X_F" + std::to_string(fi) + " () (_ BitVec 64))\n";
    return d;
}

// ---- the Z3 subprocess ----------------------------------------------------

std::string z3_discover() {
    if (const char* e = std::getenv("JULES_Z3_BIN"))
        if (*e) return std::string(e);
    static const char* kPaths[] = {"/usr/bin/z3", "/usr/local/bin/z3",
                                   "/opt/z3/bin/z3", nullptr};
    for (const char** p = kPaths; *p; ++p)
        if (::access(*p, X_OK) == 0) return std::string(*p);
    if (const char* home = std::getenv("HOME")) {
        std::string p = std::string(home) + "/.venv/bin/z3";
        if (::access(p.c_str(), X_OK) == 0) return p;
    }
    FILE* p = ::popen("command -v z3 2>/dev/null", "r");
    if (!p) return "";
    char buf[512];
    std::string out;
    size_t n;
    while ((n = ::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    ::pclose(p);
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

const std::string& z3_binary() {
    static const std::string bin = z3_discover();
    return bin;
}

u32 smt_timeout_s() {
    if (const char* v = std::getenv("JULES_SUPEROPT_SMT_TIMEOUT")) {
        long x = std::atol(v);
        if (x > 0) return static_cast<u32>(x);
    }
    return 5;
}

bool run_z3(const std::string& smt, std::string& out) {
    const std::string& bin = z3_binary();
    if (bin.empty()) return false;
    char tmpl[] = "/tmp/jules_smt_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) return false;
    FILE* f = ::fdopen(fd, "w");
    if (!f) {
        ::close(fd);
        ::unlink(tmpl);
        return false;
    }
    if (::fwrite(smt.data(), 1, smt.size(), f) != smt.size()) {
        ::fclose(f);
        ::unlink(tmpl);
        return false;
    }
    ::fclose(f);
    std::string cmd = bin + " -T:" + std::to_string(smt_timeout_s()) + " " +
                      tmpl + " 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) {
        ::unlink(tmpl);
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    ::pclose(p);
    ::unlink(tmpl);
    return true;
}

// check-sat answer (scan line-wise; "unsat" tested first — "sat" is a
// substring of it)
Verdict parse_verdict(const std::string& out) {
    bool sat = false;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t e = out.find('\n', pos);
        std::string line = out.substr(
            pos, e == std::string::npos ? std::string::npos : e - pos);
        // trim
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b != std::string::npos) {
            if (line.compare(b, 5, "unsat") == 0 && line.size() - b == 5)
                return Verdict::Proven;
            if (line.compare(b, 3, "sat") == 0 && line.size() - b == 3) sat = true;
        }
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    if (sat) return Verdict::Refuted;
    return Verdict::Unknown;
}

bool parse_value(const std::string& out, const std::string& name, u64& v) {
    const std::string needle = "(" + name + " ";
    size_t p = out.find(needle);
    if (p == std::string::npos) return false;
    size_t h = out.find("#x", p + needle.size());
    if (h == std::string::npos || h > p + needle.size() + 4) return false;
    v = std::strtoull(out.c_str() + h + 2, nullptr, 16);
    return true;
}

} // namespace

// ---- public entries ---------------------------------------------------------

bool x64_super_smt_available() { return !z3_binary().empty(); }

superopt::Verdict x64_super_tier4(const superopt::Tier4Query& q) {
    Side O;
    init_side(O, "o_", q);
    for (u32 p = q.w0; p < q.w1; ++p)
        if (!emit_inst(O, q.lf->code[p], q.slots, q.nslots))
            return Verdict::Unknown; // unhandled op: fail open, telemetry

    Side C;
    init_side(C, "c_", q);
    for (u32 p = 0; p < q.ncand; ++p)
        if (!emit_inst(C, q.cand[p], q.slots, q.nslots))
            return Verdict::Unknown;

    // contract equality over the live-out set
    std::vector<std::string> eqs;
    for (u32 g = 0; g < kMaxGpr; ++g)
        if ((q.contract_gpr >> g) & 1)
            eqs.push_back("(= " + O.r[g] + " " + C.r[g] + ")");
    for (u32 k = 0; k < q.nslots; ++k)
        if ((q.contract_slots >> k) & 1)
            eqs.push_back("(= " + O.s[k] + " " + C.s[k] + ")");
    if (q.contract_flags)
        for (u32 fi = 0; fi < kNF; ++fi)
            eqs.push_back("(= " + O.f[fi] + " " + C.f[fi] + ")");
    std::string ceq = "true";
    if (!eqs.empty()) {
        ceq = "(and";
        for (const std::string& e : eqs) ceq += " " + e;
        ceq += ")";
    }

    const std::string equiv = "(and (= " + O.fault + " " + C.fault + ") (or " +
                              O.fault + " " + ceq + "))";
    std::string smt = "(set-logic QF_BV)\n";
    smt += "(set-info :source |JULES superoptimizer tier-4|)\n";
    smt += shared_decls(q) + O.decls + O.defs + C.decls + C.defs;
    smt += "(assert (not " + equiv + "))\n(check-sat)\n";

    std::string out;
    if (!run_z3(smt, out)) return Verdict::Unknown;
    return parse_verdict(out);
}

bool x64_super_smt_selftest(const superopt::Tier4Query& q) {
    Side O;
    init_side(O, "o_", q);
    for (u32 p = q.w0; p < q.w1; ++p)
        if (!emit_inst(O, q.lf->code[p], q.slots, q.nslots)) {
            std::fprintf(stderr,
                         "[superopt][SMT-SELFTEST] no emitter for op %d at "
                         "inst %u\n",
                         static_cast<int>(q.lf->code[p].op), p);
            std::abort();
        }

    // Bind the batch-0 LANE-0 inputs to the simulator's seed values, the
    // poison symbols to the simulator's lane-0 poison, and demand no fault
    // (the sim completed the original on batch 0).
    std::string binds;
    for (u32 g = 0; g < kMaxGpr; ++g)
        if ((q.livein_gpr >> g) & 1)
            binds += " (= X_R" + std::to_string(g) + " " +
                     hex64(static_cast<u64>(q.seed->r[0][g])) + ")";
    for (u32 k = 0; k < q.nslots; ++k)
        if ((q.livein_slots >> k) & 1)
            binds += " (= X_S" + std::to_string(k) + " " +
                     hex64(static_cast<u64>(q.seed->s[0][k])) + ")";
    for (u32 fi = 0; fi < kNF; ++fi)
        binds += " (= X_F" + std::to_string(fi) + " " +
                 hex64(static_cast<u64>(q.seed->f[0][fi])) + ")";
    for (const auto& pr : O.poisons)
        binds += " (= " + pr.first + " " +
                 hex64(sm_poison_lane(0, pr.second)) + ")";
    binds += " (not " + O.fault + ")";

    std::vector<std::string> names; // (term, expected) pairs below
    std::vector<u64> expect;
    for (u32 g = 0; g < kMaxGpr; ++g)
        if ((q.contract_gpr >> g) & 1) {
            names.push_back(O.r[g]);
            expect.push_back(static_cast<u64>(q.orig_final->r[0][g]));
        }
    for (u32 k = 0; k < q.nslots; ++k)
        if ((q.contract_slots >> k) & 1) {
            names.push_back(O.s[k]);
            expect.push_back(static_cast<u64>(q.orig_final->s[0][k]));
        }
    if (q.contract_flags)
        for (u32 fi = 0; fi < kNF; ++fi) {
            names.push_back(O.f[fi]);
            expect.push_back(static_cast<u64>(q.orig_final->f[0][fi]));
        }

    std::string smt = "(set-logic QF_BV)\n";
    smt += shared_decls(q) + O.decls + O.defs;
    smt += "(assert (and" + binds + "))\n(check-sat)\n";
    if (!names.empty()) {
        smt += "(get-value (";
        for (const std::string& n : names) smt += n + " ";
        smt += "))\n";
    }

    std::string out;
    if (!run_z3(smt, out)) {
        std::fprintf(stderr,
                     "[superopt][SMT-SELFTEST] z3 unavailable/failed — "
                     "cannot cross-check this window\n");
        return false;
    }
    if (parse_verdict(out) != Verdict::Refuted) { // "sat" expected: bindings
                                                  // must be consistent
        std::fprintf(stderr,
                     "[superopt][SMT-SELFTEST] probe check-sat != sat — the "
                     "encoder FAULTS or contradicts the simulator\n");
        std::fprintf(stderr, "%s\n", out.c_str());
        std::abort();
    }
    for (size_t i = 0; i < names.size(); ++i) {
        u64 v = 0;
        if (!parse_value(out, names[i], v)) {
            std::fprintf(stderr,
                         "[superopt][SMT-SELFTEST] missing get-value for %s\n",
                         names[i].c_str());
            std::abort();
        }
        if (v != expect[i]) {
            std::fprintf(stderr,
                         "[superopt][SMT-SELFTEST] MISMATCH %s: smt=%llx "
                         "sim=%llx\n",
                         names[i].c_str(),
                         static_cast<unsigned long long>(v),
                         static_cast<unsigned long long>(expect[i]));
            std::abort();
        }
    }

    // FAULT-LIVENESS INJECTIONS: the value probes above can only catch an
    // encoder that fires faults too eagerly — a DROPPED fault condition is
    // invisible because the probe inputs never fault. For every division in
    // the window, pin the operands to a faulting valuation and require the
    // fault term to fire (the query must be UNSAT):
    //   * divisor = 0            -> div-by-zero must fault;
    //   * (idiv only) n = INT128_MIN-style i64min, d = -1 -> the quotient
    //     overflows i64 and must fault.
    // If the encoder lost a condition, the (and <pinning> (not fault))
    // query comes back SAT and we abort.
    for (const auto& dv : O.divs) {
        const bool is_idiv = std::get<0>(dv);
        const std::string& d = std::get<1>(dv);
        const std::string& n128 = std::get<2>(dv);
        std::string q1 = "(set-logic QF_BV)\n" + shared_decls(q) + O.decls +
                         O.defs +
                         "(assert (and (= " + d + " " + kZero64 +
                         ") (not " + O.fault + ")))\n(check-sat)\n";
        std::string o1;
        if (!run_z3(q1, o1) || parse_verdict(o1) != Verdict::Proven) {
            std::fprintf(stderr,
                         "[superopt][SMT-SELFTEST] div-by-0 did not force the "
                         "fault term — a fault condition is missing\n");
            std::abort();
        }
        if (is_idiv) {
            std::string q2 =
                "(set-logic QF_BV)\n" + shared_decls(q) + O.decls + O.defs +
                "(assert (and (= " + d + " #xffffffffffffffff) (= " + n128 +
                " #xffffffffffffffff8000000000000000) (not " + O.fault +
                ")))\n(check-sat)\n";
            std::string o2;
            if (!run_z3(q2, o2) || parse_verdict(o2) != Verdict::Proven) {
                std::fprintf(stderr,
                             "[superopt][SMT-SELFTEST] i64min/-1 did not force "
                             "the idiv overflow fault — range check "
                             "missing\n");
                std::abort();
            }
        }
    }
    return true;
}

} // namespace jules
