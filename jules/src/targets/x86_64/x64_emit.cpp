// x86-64 System V backend: SoN linear blocks -> MIR (pass 84), frame layout
// (pass 85), and AT&T assembly serialization. Correctness-first design:
// spill-everywhere slot allocator (every value lives in a frame slot; the
// linear-scan upgrade path is documented in docs/architecture.md).
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"
#include "core/son/son.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <functional>

namespace jules {

namespace {

// ---- helpers -----------------------------------------------------------------
const char* rname(R r) {
    switch (r) {
        case R::Rax: return "rax"; case R::Rcx: return "rcx";
        case R::Rdx: return "rdx"; case R::Rsi: return "rsi";
        case R::Rdi: return "rdi"; case R::R8:  return "r8";
        case R::R9:  return "r9";  case R::R10: return "r10";
        case R::R11: return "r11"; case R::Rbx: return "rbx";
        case R::R12: return "r12"; case R::R13: return "r13";
        case R::R14: return "r14"; case R::R15: return "r15";
        case R::Rbp: return "rbp"; case R::Rsp: return "rsp";
        case R::Xmm0: return "xmm0"; case R::Xmm1: return "xmm1";
        case R::Xmm2: return "xmm2"; case R::Xmm3: return "xmm3";
        case R::Xmm4: return "xmm4"; case R::Xmm5: return "xmm5";
        case R::Xmm6: return "xmm6"; case R::Xmm7: return "xmm7";
        case R::Xmm8: return "xmm8"; case R::Xmm9: return "xmm9";
        case R::Xmm10: return "xmm10"; case R::Xmm11: return "xmm11";
        case R::Xmm12: return "xmm12"; case R::Xmm13: return "xmm13";
        case R::Xmm14: return "xmm14"; case R::Xmm15: return "xmm15";
    }
    return "rax";
}

const char* rname32(R r) {
    switch (r) {
        case R::Rax: return "eax"; case R::Rcx: return "ecx";
        case R::Rdx: return "edx"; case R::Rsi: return "esi";
        case R::Rdi: return "edi"; case R::R8:  return "r8d";
        case R::R9:  return "r9d";  case R::R10: return "r10d";
        case R::R11: return "r11d"; case R::Rbx: return "ebx";
        case R::R12: return "r12d"; case R::R13: return "r13d";
        case R::R14: return "r14d"; case R::R15: return "r15d";
        case R::Rbp: return "ebp"; case R::Rsp: return "esp";
        default: return rname(r); // xmm regs keep their names
    }
}

const char* cc(Cond c) {
    switch (c) {
        case Cond::E: return "e";  case Cond::NE: return "ne";
        case Cond::L: return "l";  case Cond::LE: return "le";
        case Cond::G: return "g";  case Cond::GE: return "ge";
        case Cond::B: return "b";  case Cond::BE: return "be";
        case Cond::A: return "a";  case Cond::AE: return "ae";
    }
    return "e";
}

const char* ssz(u8 size) { return size == 8 ? "q" : size == 4 ? "l" : "b"; }
const char* fpsz(u8 size) { return size == 8 ? "sd" : "ss"; }

Cond cmp_cond(CmpOp op, bool is_signed) {
    switch (op) {
        case CmpOp::Eq: return Cond::E;
        case CmpOp::Ne: return Cond::NE;
        case CmpOp::Lt: return is_signed ? Cond::L : Cond::B;
        case CmpOp::Le: return is_signed ? Cond::LE : Cond::BE;
        case CmpOp::Gt: return is_signed ? Cond::G : Cond::A;
        case CmpOp::Ge: return is_signed ? Cond::GE : Cond::AE;
    }
    return Cond::E;
}

// Operand-order mirror: cond(a, b) === mirror(cond)(b, a).
Cond mirror_cond(Cond c) {
    switch (c) {
        case Cond::L:  return Cond::G;
        case Cond::LE: return Cond::GE;
        case Cond::G:  return Cond::L;
        case Cond::GE: return Cond::LE;
        case Cond::B:  return Cond::A;
        case Cond::BE: return Cond::AE;
        case Cond::A:  return Cond::B;
        case Cond::AE: return Cond::BE;
        case Cond::E:
        case Cond::NE: return c;
    }
    return c;
}

Cond inv_cond(Cond c); // defined below (same anonymous namespace)

const R kArgGpRegs[] = {R::Rdi, R::Rsi, R::Rdx, R::Rcx, R::R8, R::R9};
constexpr u8 kArgGpCount = 6;
const R kArgXmmRegs[] = {R::Xmm0, R::Xmm1, R::Xmm2, R::Xmm3, R::Xmm4, R::Xmm5,
                         R::Xmm6, R::Xmm7};
constexpr u8 kArgXmmCount = 8;
constexpr int kBlockLabelBase = 0;     // block labels: 0..N-1
constexpr int kLocalLabelBase = 100000; // local labels (epilogue, fp selects)
constexpr int kEntryLabelId = 999999;   // function entry: BEFORE the prologue

u8 sz_of(TypeId t) { return ty_bits(t) == 32 ? 4 : 8; }
bool fp_of(TypeId t) { return ty_is_float(t); }

struct Emitter {
    Emitter(LFunction& lf, FunctionGraph& fg, SymbolTable& syms)
        : lf_(lf), fg_(fg), g_(fg.g), syms_(syms) {
        lf_.label_counter = kLocalLabelBase;
    }

    // ---- slots ---------------------------------------------------------------
    i32 slot(NodeId n) {
        if (const i32* s = lf_.slot_of.find(n)) return *s;
        i32 s = lf_.slot_count++;
        lf_.slot_of.insert(n, s);
        return s;
    }

    // ---- FP constant pool ------------------------------------------------
    // f64/f32 constants are materialized into the high XMM bank, growing down
    // from xmm15. The bank is never used for argument passing (SysV vector
    // args use xmm0-7) and dead across nothing except calls, where the cache
    // is invalidated and the constant re-materialized — a loop without calls
    // sees one materialization total instead of one per use per iteration.
    // The pool watermark (lf_.fp_const_min_xmm) tells the register allocator
    // where to stop so the two never collide: with few live FP values the
    // pool grows (more loop constants cached), with many it stays small.
    R fp_const_reg(u64 bits, u8 size) {
        if (const R* r = fp_const_cache_.find(bits)) return *r;
        if (next_const_xmm_ < 8) {
            // pool exhausted: re-materialize per use; the loop-invariant
            // hoist in pass 87 moves repeated materializations out of
            // loops, so per-iteration cost stays bounded
            imm_reg(IOp::MovRImm, R::Rax, static_cast<i64>(bits));
            Inst& mv = reg2(IOp::MovFpFromGpr, R::Xmm1, R::Rax);
            (void)mv;
            (void)size;
            return R::Xmm1;
        }
        R reg = static_cast<R>(static_cast<int>(R::Xmm0) + next_const_xmm_--);
        fp_const_cache_.insert(bits, reg);
        int idx = static_cast<int>(reg) - static_cast<int>(R::Xmm0);
        if (idx < lf_.fp_const_min_xmm) lf_.fp_const_min_xmm = idx;
        imm_reg(IOp::MovRImm, R::Rax, static_cast<i64>(bits));
        Inst& mv = reg2(IOp::MovFpFromGpr, reg, R::Rax);
        (void)mv;
        (void)size;
        return reg;
    }
    void invalidate_fp_consts() { fp_const_cache_.clear(); next_const_xmm_ = 15; }

    // ---- emit shorthands --------------------------------------------------------
    Inst& emit(IOp op) {
        lf_.code.push_back(Inst{});
        Inst& i = lf_.code.back();
        i.op = op;
        return i;
    }
    Inst& reg2(IOp op, R a, R b, u8 size = 8) {
        Inst& i = emit(op);
        i.a.k = Operand::K::Reg; i.a.reg = a;
        i.b.k = Operand::K::Reg; i.b.reg = b;
        i.size = size;
        return i;
    }
    Inst& st_slot(IOp op, R a, i32 s, u8 size = 8) {
        Inst& i = emit(op);
        i.a.k = Operand::K::Reg; i.a.reg = a;
        i.b.k = Operand::K::Slot; i.b.slot = s;
        i.size = size;
        return i;
    }
    Inst& ld_slot(IOp op, R a, i32 s, u8 size = 8) {
        Inst& i = emit(op);
        i.a.k = Operand::K::Reg; i.a.reg = a;
        i.b.k = Operand::K::Slot; i.b.slot = s;
        i.size = size;
        return i;
    }
    Inst& imm_reg(IOp op, R a, i64 v) {
        Inst& i = emit(op);
        i.a.k = Operand::K::Reg; i.a.reg = a;
        i.b.k = Operand::K::Imm; i.b.imm = v;
        return i;
    }
    Inst& imm_slot(i32 s, i64 v) {
        Inst& i = emit(IOp::MovSImm);
        i.a.k = Operand::K::Slot; i.a.slot = s;
        i.b.k = Operand::K::Imm; i.b.imm = v;
        return i;
    }
    Inst& label(int id) {
        Inst& i = emit(IOp::Label);
        i.a.k = Operand::K::Label; i.a.label = id;
        return i;
    }
    Inst& jump(int id) {
        Inst& i = emit(IOp::Jmp);
        i.a.k = Operand::K::Label; i.a.label = id;
        return i;
    }
    Inst& jcc(Cond c, int id) {
        Inst& i = emit(IOp::Jcc);
        i.cond = c;
        i.a.k = Operand::K::Label; i.a.label = id;
        return i;
    }
    int new_label() { return lf_.label_counter++; }

    // ---- operand loading ----------------------------------------------------------
    void load_value(NodeId n, R r, u8 size = 8) {
        const Node& nd = g_.node(n);
        if (nd.op == Op::Const) {
            imm_reg(IOp::MovRImm, r, nd.ival);
            return;
        }
        if (nd.op == Op::Alloc && (nd.flags & kFlagStackPromoted)) {
            Inst& i = emit(IOp::LeaSlot);
            i.a.k = Operand::K::Reg; i.a.reg = r;
            i.b.k = Operand::K::Slot; i.b.slot = slot(n);
            return;
        }
        ld_slot(IOp::MovSR, r, slot(n), size);
    }
    void load_fp(NodeId n, R r) {
        const Node& nd = g_.node(n);
        u8 size = sz_of(nd.ty);
        if (nd.op == Op::Const) {
            if (nd.fval == 0.0) {
                // +0.0: one xorpd instead of a pool slot + a move
                Inst& z = emit(IOp::FpZero);
                z.a.k = Operand::K::Reg;
                z.a.reg = r;
                z.size = size;
                return;
            }
            u64 bits = 0;
            if (size == 8) {
                std::memcpy(&bits, &nd.fval, sizeof bits);
            } else {
                f32 f = static_cast<f32>(nd.fval);
                u32 b32 = 0;
                std::memcpy(&b32, &f, sizeof b32);
                bits = b32;
            }
            R creg = fp_const_reg(bits, size);
            if (creg != r) {
                Inst& i = reg2(IOp::MovFpFp, r, creg);
                i.size = size;
            }
            return;
        }
        Inst& i = emit(IOp::MovFpR);
        i.a.k = Operand::K::Reg; i.a.reg = r;
        i.b.k = Operand::K::Slot; i.b.slot = slot(n);
        i.size = size;
    }
    void store_result(NodeId n, u8 size = 8) { st_slot(IOp::MovRS, R::Rax, slot(n), size); }

    // ---- function ---------------------------------------------------------------------
    bool run() {
        epilogue_label_ = new_label();

        // the entry label sits BEFORE the prologue: call targets land here
        label(kEntryLabelId);
        emit(IOp::PushRbp);
        reg2(IOp::MovRR, R::Rbp, R::Rsp);
        {
            Inst& i = emit(IOp::FrameSub);
            i.b.k = Operand::K::Imm;
            i.b.imm = 0;
        }

        u8 gp = 0, xm = 0;
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op != Op::Param) continue;
            i32 s = slot(id);
            if (fp_of(n.ty)) {
                if (xm >= kArgXmmCount) return false;
                st_slot(IOp::MovFpS, kArgXmmRegs[xm++], s, sz_of(n.ty));
            } else {
                if (gp >= kArgGpCount) return false;
                st_slot(IOp::MovRS, kArgGpRegs[gp++], s, 8);
            }
        }

        for (LBlock& b : lf_.blocks) {
            label(kBlockLabelBase + b.index);
            sc_begin_block(b); // fused short-circuit: suppress chain nodes
            for (NodeId n : b.nodes) {
                const bool* sup = suppress_.find(n);
                if (sup && *sup) continue;
                emit_node(n);
            }
            for (int ci : b.phi_copy_indices) {
                const LPhiCopy& cp = lf_.phi_copies[static_cast<size_t>(ci)];
                emit_phi_copy(cp);
            }
            emit_terminator(b);
            sc_active_ = false;
        }

        label(epilogue_label_);
        emit(IOp::Ret);
        return true;
    }

    // ---- nodes -------------------------------------------------------------------------
    void emit_node(NodeId n) {
        const Node& nd = g_.node(n);
        switch (nd.op) {
            case Op::Bin:
                if (fp_of(nd.ty)) emit_fp_bin(n);
                else emit_int_bin(n);
                break;
            case Op::Cmp:  emit_cmp(n);  break;
            case Op::Un:   emit_un(n);   break;
            case Op::Cast: emit_cast(n); break;
            case Op::Select: emit_select(n); break;
            case Op::Load:  emit_load(n);  break;
            case Op::Store: emit_store(n); break;
            case Op::Alloc: emit_alloc(n); break;
            case Op::Call:  emit_call(n);  break;
            default: break; // Const/Param/Phi/control handled elsewhere
        }
    }

    // signed i32 division needs sign-extension to 64-bit before idivq
    void load_div_operand(NodeId n, R r) {
        const Node& nd = g_.node(n);
        u8 size = sz_of(nd.ty);
        if (nd.op == Op::Const) {
            imm_reg(IOp::MovRImm, r, nd.ival);
            if (size == 4 && ty_is_signed(nd.ty)) return; // imm already exact
            return;
        }
        ld_slot(IOp::MovSR, r, slot(n), size);
        if (size == 4 && ty_is_signed(nd.ty)) emit(IOp::SExt32);
    }

    void emit_int_bin(NodeId n) {
        const Node& nd = g_.node(n);
        BinOp op = static_cast<BinOp>(nd.sub);
        u8 size = sz_of(nd.ty);
        if (op == BinOp::Div || op == BinOp::Mod) {
            load_div_operand(nd.in[1], R::Rax);
            load_div_operand(nd.in[2], R::Rcx);
            if (ty_is_signed(nd.ty)) {
                emit(IOp::Cqo);
            } else {
                // unsigned divide needs edx zeroed (rdx:rax dividend)
                Inst& z = emit(IOp::ArithRImm);
                z.bin = BinOp::Xor;
                z.a.k = Operand::K::Reg; z.a.reg = R::Rdx;
                z.b.k = Operand::K::Imm; z.b.imm = 0;
            }
            Inst& dv = emit(ty_is_signed(nd.ty) ? IOp::IDiv : IOp::UDiv);
            dv.a.k = Operand::K::Reg; dv.a.reg = R::Rcx;
            if (op == BinOp::Mod) reg2(IOp::MovRR, R::Rax, R::Rdx, 8);
            store_result(n, size);
            return;
        }
        if (op == BinOp::Shl || op == BinOp::Shr) {
            load_value(nd.in[1], R::Rax, size);
            if (g_.node(nd.in[2]).op == Op::Const) {
                ConstVal c;
                const_of(g_, nd.in[2], c);
                Inst& i = emit(IOp::ShiftImm);
                i.bin = op;
                i.sar = (op == BinOp::Shr && ty_is_signed(nd.ty));
                i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
                i.b.k = Operand::K::Imm; i.b.imm = c.iv;
                i.size = size;
            } else {
                load_value(nd.in[2], R::Rcx, size);
                Inst& i = emit(IOp::ShiftCl);
                i.bin = op;
                i.sar = (op == BinOp::Shr && ty_is_signed(nd.ty));
                i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
                i.size = size;
            }
            store_result(n, size);
            return;
        }
        load_value(nd.in[1], R::Rax, size);
        if (g_.node(nd.in[2]).op == Op::Const) {
            ConstVal c;
            const_of(g_, nd.in[2], c);
            if (c.iv == 0 && (op == BinOp::Add || op == BinOp::Sub)) {
                store_result(n, size);
                return;
            }
            // Immediate-operand forms (add/sub/imul/and/or/xor $imm, reg)
            // only encode a sign-extended imm32; wider constants must be
            // materialized into a register (movq $imm picks movabs as needed).
            i64 sv = static_cast<i64>(c.iv);
            i32 low = static_cast<i32>(static_cast<u32>(c.iv));
            bool fits32 = (sv >= -2147483648LL && sv <= 2147483647LL) ||
                          (static_cast<u64>(static_cast<i64>(low)) == c.iv);
            if (fits32) {
                Inst& i = emit(IOp::ArithRImm);
                i.bin = op;
                i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
                i.b.k = Operand::K::Imm; i.b.imm = c.iv;
                i.size = size;
            } else {
                imm_reg(IOp::MovRImm, R::Rcx, static_cast<i64>(c.iv));
                Inst& i = reg2(IOp::ArithRR, R::Rax, R::Rcx, size);
                i.bin = op;
            }
        } else {
            load_value(nd.in[2], R::Rcx, size);
            Inst& i = reg2(IOp::ArithRR, R::Rax, R::Rcx, size);
            i.bin = op;
        }
        store_result(n, size);
    }

    void emit_fp_bin(NodeId n) {
        const Node& nd = g_.node(n);
        u8 size = sz_of(nd.ty);
        load_fp(nd.in[1], R::Xmm0);
        load_fp(nd.in[2], R::Xmm1);
        Inst& i = emit(IOp::FpBin);
        i.bin = static_cast<BinOp>(nd.sub);
        i.size = size;
        i.a.k = Operand::K::Reg; i.a.reg = R::Xmm0; // dst (accumulates)
        i.b.k = Operand::K::Reg; i.b.reg = R::Xmm1; // src
        Inst& st = emit(IOp::MovFpS);
        st.a.k = Operand::K::Reg; st.a.reg = R::Xmm0;
        st.b.k = Operand::K::Slot; st.b.slot = slot(n);
        st.size = size;
    }

    // Emits the bare compare for Cmp node `n` (flags only — no setcc, no
    // result store). Returns the condition the flags encode for
    // (in[1] OP in[2]); shared by emit_cmp and the fused short-circuit
    // branch lowering, which branches directly off the compare flags.
    Cond emit_cmp_flags(NodeId n) {
        const Node& nd = g_.node(n);
        const Node& an = g_.node(nd.in[1]);
        u8 size = sz_of(an.ty);
        Cond cond = cmp_cond(static_cast<CmpOp>(nd.sub), ty_is_signed(an.ty));
        if (fp_of(an.ty)) {
            load_fp(nd.in[1], R::Xmm0);
            load_fp(nd.in[2], R::Xmm1);
            Inst& c = emit(IOp::FpCmp);
            c.size = sz_of(an.ty);
            c.a.k = Operand::K::Reg; c.a.reg = R::Xmm0;
            c.b.k = Operand::K::Reg; c.b.reg = R::Xmm1;
            return cond;
        }
        // Can `v` be a CmpRImm immediate? cmpq has no imm64 form: the
        // sign-extended imm32 must reproduce the full constant (fixes
        // assembler errors on large i64 constants). 32-bit compares take
        // imm32 as-is, so any 32-bit constant is encodable.
        auto imm_ok = [&](i64 v) {
            if (size == 4) return true;
            return static_cast<i64>(static_cast<i32>(v)) == v;
        };
        // Swapped-operand form: cmp has a register-immediate form for the
        // B side only, and GVN/branch inversion leaves constants on the
        // left operand of loop guards ([mov rax,$c][cmp rax,rX] — the
        // constant re-materialized every iteration). Emit the value into
        // rax and compare it with the immediate directly; the condition
        // mirrors (a < c  <=>  c > a).
        if (an.op == Op::Const && g_.node(nd.in[2]).op != Op::Const) {
            ConstVal c;
            const_of(g_, nd.in[1], c);
            if (imm_ok(c.iv)) {
                load_value(nd.in[2], R::Rax, size);
                Inst& i = emit(IOp::CmpRImm);
                i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
                i.b.k = Operand::K::Imm; i.b.imm = c.iv;
                i.size = size;
                return mirror_cond(cond);
            }
        }
        load_value(nd.in[1], R::Rax, size);
        if (g_.node(nd.in[2]).op == Op::Const) {
            ConstVal c;
            const_of(g_, nd.in[2], c);
            if (imm_ok(c.iv)) {
                Inst& i = emit(IOp::CmpRImm);
                i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
                i.b.k = Operand::K::Imm; i.b.imm = c.iv;
                i.size = size;
            } else {
                // not imm32-encodable: materialize like a register
                // operand (cmp has no imm64 encoding)
                load_value(nd.in[2], R::Rcx, size);
                reg2(IOp::CmpRR, R::Rax, R::Rcx, size);
            }
        } else {
            load_value(nd.in[2], R::Rcx, size);
            reg2(IOp::CmpRR, R::Rax, R::Rcx, size);
        }
        return cond;
    }

    void emit_cmp(NodeId n) {
        Cond cond = emit_cmp_flags(n);
        Inst& sc = emit(IOp::Setcc);
        sc.cond = cond;
        Inst& zx = emit(IOp::MovZX);
        zx.a.k = Operand::K::Reg;
        zx.a.reg = R::Rax;
        store_result(n, 8);
    }

    void emit_un(NodeId n) {
        const Node& nd = g_.node(n);
        UnOp op = static_cast<UnOp>(nd.sub);
        if (fp_of(nd.ty)) {
            load_fp(nd.in[1], R::Xmm0);
            Inst& i = emit(IOp::FpNeg);
            i.size = sz_of(nd.ty);
            i.a.k = Operand::K::Reg; i.a.reg = R::Xmm0;
            Inst& st = emit(IOp::MovFpS);
            st.a.k = Operand::K::Reg; st.a.reg = R::Xmm0;
            st.b.k = Operand::K::Slot; st.b.slot = slot(n);
            st.size = i.size;
            return;
        }
        load_value(nd.in[1], R::Rax, 8);
        switch (op) {
            case UnOp::Neg: {
                Inst& i = emit(IOp::Neg);
                i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
                break;
            }
            case UnOp::Not: {
                Inst& i = emit(IOp::ArithRImm);
                i.bin = BinOp::Xor;
                i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
                i.b.k = Operand::K::Imm; i.b.imm = 1;
                break;
            }
            case UnOp::BNot: {
                Inst& i = emit(IOp::Not);
                i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
                break;
            }
        }
        store_result(n, 8);
    }

    void emit_cast(NodeId n) {
        const Node& nd = g_.node(n);
        NodeId src = nd.in[1];
        const Node& sn = g_.node(src);
        u8 src_size = sz_of(sn.ty);
        u8 dst_size = sz_of(nd.ty);

        switch (static_cast<CastOp>(nd.sub)) {
            case CastOp::ZExt:
                // Const sources are materialized, never stored to their
                // slot (load_value handles them); extend the constant
                // directly so negative i32 constants zero-extend correctly.
                if (sn.op == Op::Const) {
                    imm_reg(IOp::MovRImm, R::Rax,
                            static_cast<i64>(static_cast<u32>(static_cast<i32>(sn.ival))));
                } else {
                    load_value(src, R::Rax, src_size);
                }
                store_result(n, 8);
                return;
            case CastOp::SExt:
                if (src_size == 4) {
                    if (sn.op == Op::Const) {
                        // same as above: constants never live in slots; the
                        // sign-extended 64-bit value IS the constant.
                        imm_reg(IOp::MovRImm, R::Rax,
                                static_cast<i64>(static_cast<i32>(sn.ival)));
                    } else {
                        ld_slot(IOp::MovSR, R::Rax, slot(src), 4);
                        emit(IOp::SExt32);
                    }
                } else {
                    load_value(src, R::Rax, 8);
                }
                store_result(n, 8);
                return;
            case CastOp::Trunc:
                load_value(src, R::Rax, dst_size);
                store_result(n, dst_size);
                return;
            case CastOp::SiToFp: {
                load_value(src, R::Rax, src_size);
                Inst& i = emit(IOp::CvtToFp);
                i.size = dst_size;
                Inst& st = emit(IOp::MovFpS);
                st.a.k = Operand::K::Reg; st.a.reg = R::Xmm0;
                st.b.k = Operand::K::Slot; st.b.slot = slot(n);
                st.size = dst_size;
                return;
            }
            case CastOp::FpToSi: {
                load_fp(src, R::Xmm0);
                emit(IOp::CvtToInt).size = dst_size;
                store_result(n, dst_size);
                return;
            }
            case CastOp::FpExt: {
                load_fp(src, R::Xmm0);
                emit(IOp::FpExt);
                Inst& st = emit(IOp::MovFpS);
                st.a.k = Operand::K::Reg; st.a.reg = R::Xmm0;
                st.b.k = Operand::K::Slot; st.b.slot = slot(n);
                st.size = 8;
                return;
            }
            case CastOp::FpTrunc: {
                load_fp(src, R::Xmm0);
                emit(IOp::FpTrunc);
                Inst& st = emit(IOp::MovFpS);
                st.a.k = Operand::K::Reg; st.a.reg = R::Xmm0;
                st.b.k = Operand::K::Slot; st.b.slot = slot(n);
                st.size = 4;
                return;
            }
            case CastOp::Ptr:
                load_value(src, R::Rax, 8);
                store_result(n, 8);
                return;
        }
    }

    void emit_select(NodeId n) {
        const Node& nd = g_.node(n);
        NodeId c = nd.in[1], t = nd.in[2], f = nd.in[3];
        bool fp = fp_of(nd.ty);
        u8 size = sz_of(nd.ty);
        load_value(c, R::Rdx, 8);
        if (!fp) {
            // cmovcc src, dst: dst = cond ? src : dst — so the FALSE value
            // lives in the destination and the TRUE value in the source.
            load_value(f, R::Rax, size);
            load_value(t, R::Rcx, size);
            Inst& i = reg2(IOp::Cmov, R::Rax, R::Rcx, size);
            i.cond = Cond::NE;
            store_result(n, size);
            return;
        }
        int ltrue = new_label();
        int lend = new_label();
        jcc(Cond::NE, ltrue);
        load_fp(f, R::Xmm0);
        jump(lend);
        label(ltrue);
        load_fp(t, R::Xmm0);
        label(lend);
        Inst& st = emit(IOp::MovFpS);
        st.a.k = Operand::K::Reg; st.a.reg = R::Xmm0;
        st.b.k = Operand::K::Slot; st.b.slot = slot(n);
        st.size = size;
    }

    void emit_load(NodeId n) {
        const Node& nd = g_.node(n);
        u8 size = sz_of(nd.ty);
        load_value(nd.in[2], R::Rax, 8);
        Inst& i = emit(IOp::LoadMem);
        i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
        i.b.k = Operand::K::Reg; i.b.reg = R::Rax;
        i.size = size;
        store_result(n, size);
    }

    void emit_store(NodeId n) {
        const Node& nd = g_.node(n);
        NodeId val = nd.in[3];
        bool fp = fp_of(g_.node(val).ty);
        u8 size = sz_of(g_.node(val).ty);
        load_value(nd.in[2], R::Rcx, 8);
        if (fp) {
            load_fp(val, R::Xmm0);
            Inst& i = emit(IOp::StoreMem);
            i.a.k = Operand::K::Reg; i.a.reg = R::Rcx;
            i.b.k = Operand::K::Reg; i.b.reg = R::Xmm0;
            i.size = size;
        } else {
            load_value(val, R::Rdx, size);
            Inst& i = emit(IOp::StoreMem);
            i.a.k = Operand::K::Reg; i.a.reg = R::Rcx;
            i.b.k = Operand::K::Reg; i.b.reg = R::Rdx;
            i.size = size;
        }
    }

    void emit_alloc(NodeId n) {
        const Node& nd = g_.node(n);
        if (nd.flags & kFlagStackPromoted) {
            (void)slot(n); // reserve the slot; address computed via LeaSlot
            return;
        }
        invalidate_fp_consts(); // malloc clobbers caller-saved XMMs
        const Node& size = g_.node(nd.in[2]);
        imm_reg(IOp::MovRImm, R::Rdi, size.ival);
        Inst& c = emit(IOp::CallSym);
        c.a.k = Operand::K::Sym; c.a.sym = "malloc";
        store_result(n, 8);
    }

    void emit_call(NodeId n) {
        const Node& nd = g_.node(n);
        invalidate_fp_consts(); // all XMMs are caller-saved across calls
        if (nd.aux == kFnPrint) {
            emit_print(n);
            return;
        }
        if (nd.aux == kFnFree) {
            // free(ptr): single pointer argument
            load_value(nd.in[2], R::Rdi, 8);
            Inst& c = emit(IOp::CallSym);
            c.a.k = Operand::K::Sym; c.a.sym = "free";
            return;
        }
        u8 gp = 0, xm = 0;
        for (u8 i = 2; i < nd.n_in; ++i) {
            NodeId a = nd.in[i];
            if (fp_of(g_.node(a).ty)) {
                if (xm >= kArgXmmCount) return;
                load_fp(a, kArgXmmRegs[xm++]);
            } else {
                if (gp >= kArgGpCount) return;
                load_value(a, kArgGpRegs[gp++], sz_of(g_.node(a).ty));
            }
        }
        if (nd.flags & kFlagTailCall) {
            Inst& tc = emit(IOp::TailCallFn);
            tc.a.k = Operand::K::Label; tc.a.label = static_cast<int>(nd.aux);
            return;
        }
        Inst& c = emit(IOp::CallFn);
        c.a.k = Operand::K::Label; c.a.label = static_cast<int>(nd.aux);
        if (nd.ty != ty_void()) {
            if (fp_of(nd.ty)) {
                Inst& st = emit(IOp::MovFpS);
                st.a.k = Operand::K::Reg; st.a.reg = R::Xmm0;
                st.b.k = Operand::K::Slot; st.b.slot = slot(n);
                st.size = sz_of(nd.ty);
            } else {
                store_result(n, 8);
            }
        }
    }

    void emit_print(NodeId n) {
        const Node& nd = g_.node(n);
        NodeId v = nd.in[2];
        const Node& vn = g_.node(v);
        const char* fmt = "%ld\n";
        bool promote_f32 = false;
        switch (vn.ty) {
            case 3: fmt = "%d\n"; break;
            case 4: fmt = "%ld\n"; break;
            case 5: fmt = "%u\n"; break;
            case 6: fmt = "%lu\n"; break;
            case 2: fmt = "%d\n"; break;
            case 7: fmt = "%f\n"; promote_f32 = true; break;
            case 8: fmt = "%f\n"; break;
            default: break;
        }
        std::string lbl = ".Lstr_" + std::to_string(lf_.fid) + "_" +
                          std::to_string(lf_.strings.size());
        StringConst sc;
        sc.label = lbl;
        sc.text = fmt;
        lf_.strings.push_back(sc);

        Inst& lea = emit(IOp::LeaSym);
        lea.a.k = Operand::K::Reg; lea.a.reg = R::Rdi;
        lea.b.k = Operand::K::Sym;
        lea.b.slot = static_cast<i32>(lf_.strings.size() - 1); // index into lf.strings

        if (fp_of(vn.ty)) {
            load_fp(v, R::Xmm0);
            if (promote_f32) emit(IOp::FpExt);
            // SysV varargs: al = number of vector registers used
            imm_reg(IOp::MovRImm, R::Rax, 1);
        } else {
            load_value(v, R::Rsi, sz_of(vn.ty));
            emit(IOp::XorEax);
        }
        Inst& c = emit(IOp::CallSym);
        c.a.k = Operand::K::Sym; c.a.sym = "printf";
        if (nd.ty != ty_void()) store_result(n, 8);
    }

    void emit_phi_copy(const LPhiCopy& cp) {
        const Node& src = g_.node(cp.src);
        const Node& dst = g_.node(cp.dst);
        if (fp_of(dst.ty)) {
            load_fp(cp.src, R::Xmm0);
            Inst& st = emit(IOp::MovFpS);
            st.a.k = Operand::K::Reg; st.a.reg = R::Xmm0;
            st.b.k = Operand::K::Slot; st.b.slot = slot(cp.dst);
            st.size = sz_of(dst.ty);
            return;
        }
        if (src.op == Op::Const) {
            imm_slot(slot(cp.dst), src.ival);
            return;
        }
        if (src.op == Op::Alloc && (src.flags & kFlagStackPromoted)) {
            Inst& i = emit(IOp::LeaSlot);
            i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
            i.b.k = Operand::K::Slot; i.b.slot = slot(cp.src);
            st_slot(IOp::MovRS, R::Rax, slot(cp.dst), 8);
            return;
        }
        ld_slot(IOp::MovSR, R::Rax, slot(cp.src), sz_of(dst.ty));
        st_slot(IOp::MovRS, R::Rax, slot(cp.dst), sz_of(dst.ty));
    }

    // ---- fused short-circuit branches ----------------------------------
    // A branch whose condition is a bool And/Or chain over compares (the
    // p48 predication shape, and the classic `while a && b` guard) lowers
    // as a chain of compare+branch pairs — exactly what the control-flow
    // form would emit, with zero setcc/movzx/and round trips. Each Cmp
    // leaf is single-use (consumed only by the chain) and scheduled in the
    // branch block, so suppressing its standalone emission and branching
    // straight off its flags preserves semantics: short-circuit evaluation
    // skips the remaining leaves on the first decisive result.
    struct ScLeaf {
        NodeId node;
        bool is_cmp;
    };
    void sc_begin_block(LBlock& b) {
        sc_active_ = false;
        suppress_.clear();
        sc_leaves_.clear();
        if (b.terminator_if == kNoNode) return;
        NodeId cond = g_.node(b.terminator_if).in[1];
        if (cond == kNoNode) return;
        const Node& cn = g_.node(cond);
        if (cn.op != Op::Bin) return;
        if (cn.sub != static_cast<u32>(BinOp::And) &&
            cn.sub != static_cast<u32>(BinOp::Or))
            return;
        if (cn.ty != ty_i1()) return; // integer bitwise & / |: NOT a short-circuit
        sc_and_ = (cn.sub == static_cast<u32>(BinOp::And));
        FlatMap<NodeId, bool> in_blk;
        for (NodeId n : b.nodes) in_blk.insert(n, true);
        if (!sc_flatten(cond, in_blk)) {
            sc_active_ = false;
            suppress_.clear();
            sc_leaves_.clear();
            return;
        }
        bool any_cmp = false;
        for (const ScLeaf& l : sc_leaves_) any_cmp = any_cmp || l.is_cmp;
        if (!any_cmp) { // pure test chain: materialization is already fine
            suppress_.clear();
            sc_leaves_.clear();
            return;
        }
        sc_active_ = true;
    }
    // killed nodes stay in the lazy use lists (see SROA's dead-user fix):
    // count only live users when proving single-use.
    u32 live_uses(NodeId n) {
        u32 c = 0;
        for (NodeId u : g_.uses_of(n))
            if (g_.node(u).op != Op::Dead) ++c;
        return c;
    }
    bool sc_flatten(NodeId n, const FlatMap<NodeId, bool>& in_blk) {
        const Node& nd = g_.node(n);
        if (nd.op == Op::Bin && nd.ty == ty_i1() &&
            (nd.sub == static_cast<u32>(BinOp::And) ||
             nd.sub == static_cast<u32>(BinOp::Or))) {
            // mixed && / || nesting keeps the materialized form (bail)
            if (nd.sub != (sc_and_ ? static_cast<u32>(BinOp::And)
                                   : static_cast<u32>(BinOp::Or)))
                return false;
            // the chain node must feed exactly one consumer (its parent or
            // the If) — otherwise its value is observed elsewhere
            if (live_uses(n) != 1) return false;
            if (!suppress_.contains(n)) suppress_.insert(n, true);
            return sc_flatten(nd.in[1], in_blk) && sc_flatten(nd.in[2], in_blk);
        }
        if (nd.op == Op::Cmp) {
            if (live_uses(n) != 1) return false;   // value read elsewhere
            if (!in_blk.contains(n)) return false; // emitted in an earlier block
            suppress_.insert(n, true);
            sc_leaves_.push_back(ScLeaf{n, true});
            return true;
        }
        // other operand: allowed as a materialized bool (test leaf)
        if (nd.ty != ty_i1()) return false;
        sc_leaves_.push_back(ScLeaf{n, false});
        return true;
    }
    void emit_sc_branch(LBlock& b) {
        // resolve the two successor labels exactly like the plain path
        int true_label = kBlockLabelBase, false_label = kBlockLabelBase;
        bool have_true = false, have_false = false;
        for (int s : b.succs) {
            if (s < 0 || static_cast<size_t>(s) >= lf_.blocks.size()) continue;
            Op so = g_.node(lf_.blocks[static_cast<size_t>(s)].head).op;
            if (so == Op::IfTrue) { true_label = kBlockLabelBase + s; have_true = true; }
            if (so == Op::IfFalse) { false_label = kBlockLabelBase + s; have_false = true; }
        }
        if (!have_true || !have_false) {
            if (!b.succs.empty()) {
                true_label = kBlockLabelBase + b.succs[0];
                have_true = true;
            }
            if (b.succs.size() > 1) {
                false_label = kBlockLabelBase + b.succs[1];
                have_false = true;
            }
        }
        if (!have_true && !have_false) return;
        bool ft_true = false, ft_false = false;
        int next = b.index + 1;
        if (static_cast<size_t>(next) < lf_.blocks.size()) {
            int nb = kBlockLabelBase + next;
            if (nb == true_label) ft_true = true;
            if (nb == false_label) ft_false = true;
        }
        size_t k = sc_leaves_.size();
        for (size_t i = 0; i < k; ++i) {
            const ScLeaf& leaf = sc_leaves_[i];
            bool last = (i + 1 == k);
            Cond pass; // condition under which THIS leaf votes "true"
            if (leaf.is_cmp) {
                pass = emit_cmp_flags(leaf.node); // flags only: no setcc round trip
            } else {
                load_value(leaf.node, R::Rax, 8);
                Inst& t = emit(IOp::Test);
                t.a.k = Operand::K::Reg; t.a.reg = R::Rax;
                pass = Cond::NE;
            }
            if (sc_and_) {
                // any false leaf decides FALSE; all-true falls out TRUE
                if (last && ft_false) {
                    jcc(pass, true_label);        // fail falls into the false side
                } else {
                    jcc(inv_cond(pass), false_label); // fail short-circuits out
                }
            } else {
                // any true leaf decides TRUE; all-false falls out FALSE
                if (last && ft_true) {
                    jcc(inv_cond(pass), false_label); // pass falls into the true side
                } else {
                    jcc(pass, true_label);            // pass short-circuits out
                }
            }
        }
        // chain fallout: And -> all passed -> TRUE; Or -> all failed -> FALSE
        if (!ft_true && !ft_false) jump(sc_and_ ? true_label : false_label);
    }
    bool sc_active_ = false;
    bool sc_and_ = false;
    std::vector<ScLeaf> sc_leaves_;
    FlatMap<NodeId, bool> suppress_;

    void emit_terminator(LBlock& b) {
        if (b.terminator_return != kNoNode) {
            const Node& r = g_.node(b.terminator_return);
            if (r.n_in == 3) {
                if (fp_of(g_.node(r.in[2]).ty)) load_fp(r.in[2], R::Xmm0);
                else load_value(r.in[2], R::Rax, sz_of(g_.node(r.in[2]).ty));
            }
            jump(epilogue_label_);
            return;
        }
        if (b.terminator_if != kNoNode) {
            if (sc_active_) {
                emit_sc_branch(b);
                return;
            }
            const Node& ifn = g_.node(b.terminator_if);
            load_value(ifn.in[1], R::Rax, 8);
            Inst& t = emit(IOp::Test);
            t.a.k = Operand::K::Reg; t.a.reg = R::Rax;

            int true_label = kBlockLabelBase, false_label = kBlockLabelBase;
            bool have_true = false, have_false = false;
            for (int s : b.succs) {
                if (s < 0 || static_cast<size_t>(s) >= lf_.blocks.size()) continue;
                Op so = g_.node(lf_.blocks[static_cast<size_t>(s)].head).op;
                if (so == Op::IfTrue) { true_label = kBlockLabelBase + s; have_true = true; }
                if (so == Op::IfFalse) { false_label = kBlockLabelBase + s; have_false = true; }
            }
            if (!have_true || !have_false) {
                // degenerate: fall back to successor order
                if (!b.succs.empty()) {
                    true_label = kBlockLabelBase + b.succs[0];
                    have_true = true;
                }
                if (b.succs.size() > 1) {
                    false_label = kBlockLabelBase + b.succs[1];
                    have_false = true;
                }
            }
            if (!have_true && !have_false) return;
            int next = b.index + 1;
            if (static_cast<size_t>(next) < lf_.blocks.size()) {
                int nb = kBlockLabelBase + next;
                if (nb == true_label) { jcc(Cond::E, false_label); return; }
                if (nb == false_label) { jcc(Cond::NE, true_label); return; }
            }
            jcc(Cond::NE, true_label);
            if (have_false && false_label != true_label) jump(false_label);
            return;
        }
        // Jump / fallthrough block
        if (!b.succs.empty()) {
            int next = b.index + 1;
            if (static_cast<size_t>(next) >= lf_.blocks.size() ||
                kBlockLabelBase + next != kBlockLabelBase + b.succs[0]) {
                jump(kBlockLabelBase + b.succs[0]);
            }
        }
    }

    int epilogue_label_ = 0;
    LFunction& lf_;
    FunctionGraph& fg_;
    Graph& g_;
    SymbolTable& syms_;
    FlatMap<u64, R> fp_const_cache_;
    int next_const_xmm_ = 15;
};

} // namespace

// ---- pass 84 ------------------------------------------------------------------
bool x64_select_instructions(LFunction& lf, FunctionGraph& fg, SymbolTable& syms) {
    Emitter e(lf, fg, syms);
    return e.run();
}

// ---- pass 85: frame layout (spill-everywhere allocator) --------------------------
bool x64_allocate_frame(LFunction& lf) {
    lf.slot_offset.assign(static_cast<size_t>(lf.slot_count), 0);
    for (i32 s = 0; s < lf.slot_count; ++s)
        lf.slot_offset[static_cast<size_t>(s)] = -(s + 1) * 8;
    i32 frame = lf.slot_count * 8;
    frame = (frame + 15) & ~15; // 16-byte call alignment
    lf.frame_size = frame;
    for (Inst& i : lf.code)
        if (i.op == IOp::FrameSub) i.b.imm = frame;
    return true;
}

// ---- pass 86 ---------------------------------------------------------------------
namespace {
// Is `slot` read by any instruction at or after `from`? Folding a store+load
// pair is only sound when nothing else reads the slot afterwards.
bool slot_read_later(const std::vector<Inst>& code, size_t from, i32 slot) {
    for (size_t i = from; i < code.size(); ++i) {
        const Inst& c = code[i];
        if ((c.op == IOp::MovSR || c.op == IOp::MovFpR) && c.b.k == Operand::K::Slot &&
            c.b.slot == slot)
            return true;
    }
    return false;
}
} // namespace

bool x64_post_ra_cleanup(LFunction& lf) {
    bool changed = false;
    std::vector<Inst> out;
    out.reserve(lf.code.size());
    for (size_t i = 0; i < lf.code.size(); ++i) {
        const Inst& cur = lf.code[i];
        if (cur.op == IOp::MovRS && i + 1 < lf.code.size()) {
            const Inst& nxt = lf.code[i + 1];
            if (nxt.op == IOp::MovSR && nxt.b.k == Operand::K::Slot &&
                cur.b.k == Operand::K::Slot && nxt.b.slot == cur.b.slot &&
                nxt.a.k == Operand::K::Reg &&
                !slot_read_later(lf.code, i + 2, cur.b.slot)) {
                Inst mv;
                mv.op = IOp::MovRR;
                mv.a.k = Operand::K::Reg; mv.a.reg = nxt.a.reg;
                mv.b.k = Operand::K::Reg; mv.b.reg = cur.a.reg;
                mv.size = 8;
                out.push_back(mv);
                ++i;
                changed = true;
                continue;
            }
        }
        if (cur.op == IOp::MovFpS && i + 1 < lf.code.size()) {
            const Inst& nxt = lf.code[i + 1];
            if (nxt.op == IOp::MovFpR && nxt.b.k == Operand::K::Slot &&
                cur.b.k == Operand::K::Slot && nxt.b.slot == cur.b.slot &&
                cur.a.k == Operand::K::Reg && nxt.a.k == Operand::K::Reg &&
                !slot_read_later(lf.code, i + 2, cur.b.slot)) {
                if (nxt.a.reg == cur.a.reg) {
                    // store+load back into the same register: nothing at all
                    ++i;
                } else {
                    Inst mv;
                    mv.op = IOp::MovFpFp;
                    mv.a.k = Operand::K::Reg; mv.a.reg = nxt.a.reg;
                    mv.b.k = Operand::K::Reg; mv.b.reg = cur.a.reg;
                    mv.size = cur.size;
                    out.push_back(mv);
                    ++i;
                }
                changed = true;
                continue;
            }
        }
        if (cur.op == IOp::MovSImm && i + 1 < lf.code.size()) {
            const Inst& nxt = lf.code[i + 1];
            if (nxt.op == IOp::MovSR && nxt.b.k == Operand::K::Slot &&
                cur.a.k == Operand::K::Slot && nxt.b.slot == cur.a.slot &&
                nxt.a.k == Operand::K::Reg &&
                !slot_read_later(lf.code, i + 2, cur.a.slot)) {
                Inst mv;
                mv.op = IOp::MovRImm;
                mv.a.k = Operand::K::Reg; mv.a.reg = nxt.a.reg;
                mv.b.k = Operand::K::Imm; mv.b.imm = cur.b.imm;
                out.push_back(mv);
                ++i;
                changed = true;
                continue;
            }
        }
        out.push_back(cur);
    }
    lf.code = std::move(out);
    return changed;
}

// ---- pass 87: fused branches + accumulator folds ------------------------------
namespace {

// mov/lea-only opcodes: these do not touch the flags register, so a compare
// can stay live across them until a fused jcc consumes it.
bool flags_preserving(IOp op) {
    switch (op) {
        case IOp::MovRR:
        case IOp::MovRS:
        case IOp::MovSR:
        case IOp::MovRImm:
        case IOp::MovSImm:
        case IOp::MovFpS:
        case IOp::MovFpR:
        case IOp::MovFpFp:
        case IOp::LeaSlot:
        case IOp::LeaSym:
        case IOp::LeaRR:
        case IOp::Nop:
        case IOp::Comment:
            return true;
        default:
            return false;
    }
}

Cond inv_cond(Cond c) {
    switch (c) {
        case Cond::E:  return Cond::NE;
        case Cond::NE: return Cond::E;
        case Cond::L:  return Cond::GE;
        case Cond::LE: return Cond::G;
        case Cond::G:  return Cond::LE;
        case Cond::GE: return Cond::L;
        case Cond::B:  return Cond::AE;
        case Cond::BE: return Cond::A;
        case Cond::A:  return Cond::BE;
        case Cond::AE: return Cond::B;
    }
    return Cond::E;
}

bool is_cmp_op(IOp op) {
    return op == IOp::CmpRR || op == IOp::CmpRImm || op == IOp::FpCmp;
}

// Is `i` the defining store of the comparison boolean, in either its
// spill-everywhere form (mov [slot], rax) or post-RA promoted form
// (mov regC, rax)?
bool is_bool_store(const Inst& i) {
    if (i.op == IOp::MovRS && i.a.k == Operand::K::Reg && i.a.reg == R::Rax)
        return true;
    if (i.op == IOp::MovRR && i.a.k == Operand::K::Reg && i.b.k == Operand::K::Reg &&
        i.b.reg == R::Rax)
        return true;
    return false;
}

// Slot-reference counts (defs/uses) across the whole function.
struct SlotCounts {
    FlatMap<i32, u32> defs;
    FlatMap<i32, u32> uses;
};

SlotCounts count_slot_refs(const std::vector<Inst>& code) {
    SlotCounts c;
    for (const Inst& i : code) {
        if ((i.op == IOp::MovRS || i.op == IOp::MovFpS) && i.b.k == Operand::K::Slot)
            c.defs.insert(i.b.slot, c.defs.contains(i.b.slot) ? *c.defs.find(i.b.slot) + 1 : 1);
        else if (i.op == IOp::MovSImm && i.a.k == Operand::K::Slot)
            c.defs.insert(i.a.slot, c.defs.contains(i.a.slot) ? *c.defs.find(i.a.slot) + 1 : 1);
        else if ((i.op == IOp::MovSR || i.op == IOp::MovFpR) && i.b.k == Operand::K::Slot)
            c.uses.insert(i.b.slot, c.uses.contains(i.b.slot) ? *c.uses.find(i.b.slot) + 1 : 1);
        else if (i.op == IOp::LeaSlot && i.b.k == Operand::K::Slot)
            c.uses.insert(i.b.slot, c.uses.contains(i.b.slot) ? *c.uses.find(i.b.slot) + 1 : 1);
    }
    return c;
}

} // namespace

// Machine loop rotation (pass 88): one taken branch per iteration.
//
// The pre-rotation while-loop shape costs TWO taken control transfers per
// iteration (the guard's taken jcc into the body + the unconditional latch
// jump back to the head):
//     [L: cond][jcc body] [exit code] [body: ...][jmp L]
// Rotated layout (guard moved to the bottom, latch deleted, entry shim):
//     [L: jmp check] [body: ...] [check: cond][jcc body (backedge)] [exit]
// Fallthrough from check reaches the exit; every entry (fallthrough into L
// or a jump to L) runs through the shim and evaluates the guard BEFORE the
// body — semantics preserved exactly; per-iteration taken branches: ONE.
//
// Validity (conservative):
//   * the head [L+1 .. guard] is one basic block: no labels, no jumps, one
//     jcc (the guard) whose target label (the body) lies inside the region
//   * the guard's fallthrough segment (exit code) is nonempty and ends in
//     a full terminator (jmp/ret/tail-call) — no fallthrough INTO the body
//   * label-based jumps are position-independent in this MIR, so blocks
//     move freely; only fallthrough adjacency constrains the layout, and
//     the three fallthrough edges (body->check, check->exit, into-L->shim)
//     are exactly what the new order preserves
//   * multiple latches / conditional backedges stay correct: they target
//     the entry label and re-run the guard through the shim
bool x64_loop_rotate(LFunction& lf) {
    if (lf.code.empty()) return false;
    bool any = false;
    for (int guard_round = 0; guard_round < 256; ++guard_round) {
        FlatMap<int, size_t> label_pos;
        for (size_t i = 0; i < lf.code.size(); ++i)
            if (lf.code[i].op == IOp::Label) label_pos.insert(lf.code[i].a.label, i);

        bool found = false;
        size_t best_span = SIZE_MAX;
        size_t L = 0, guard_pos = 0, body_pos = 0, latch_pos = 0;
        bool form_b = false; // Form B: guard jcc targets the EXIT, body = fallthrough

        for (size_t p = 0; p < lf.code.size(); ++p) {
            if (lf.code[p].op != IOp::Jmp) continue;
            const size_t* lp = label_pos.find(lf.code[p].a.label);
            if (!lp || *lp >= p) continue; // not a backedge latch
            size_t l = *lp;
            size_t span = p - l;
            if (span >= best_span) continue;

            // head must be a single basic block ending in the guard jcc:
            // walk through ordinary instructions; a Label or Jmp means the
            // head is multi-block (skip). Multiple Jcc's are allowed — the
            // fused short-circuit guards emit [cmp][jcc exit][cmp][jcc
            // body] chains in one block; the LAST jcc is the backedge
            // candidate and the earlier ones jump into the exit segment.
            size_t j = l + 1;
            size_t last_jcc = SIZE_MAX;
            while (j < p) {
                const Inst& c = lf.code[j];
                if (c.op == IOp::Nop) { ++j; continue; }
                if (c.op == IOp::Jcc) { last_jcc = j; ++j; continue; }
                if (c.op == IOp::Label || c.op == IOp::Jmp) break;
                ++j; // ordinary head instruction (loads, cmp, arith, ...)
            }
            if (last_jcc == SIZE_MAX) continue;
            size_t gp = last_jcc;
            // guard target must be the body label inside the region
            if (lf.code[gp].a.k != Operand::K::Label) continue;
            const size_t* bp = label_pos.find(lf.code[gp].a.label);
            size_t bpos = 0;
            bool this_form_b = false;
            if (bp && *bp > gp && *bp < p) {
                // Form A: jcc jumps INTO the body when the condition holds;
                // the exit code sits between the guard and the body label.
                bpos = *bp;
                // exit segment must be nonempty and end in a full terminator
                size_t e_end = bpos;
                while (e_end > gp + 1 && lf.code[e_end - 1].op == IOp::Nop) --e_end;
                if (e_end == gp + 1) continue;
                IOp last = lf.code[e_end - 1].op;
                if (last != IOp::Jmp && last != IOp::Ret && last != IOp::RetNaked &&
                    last != IOp::TailCallFn && last != IOp::TailCallNaked)
                    continue;
            } else {
                // Form B: guard jcc targets the EXIT (jump out on failure);
                // the body is the guard's fallthrough. Inlined/unrolled loops
                // linearize in this polarity. The body's first instruction
                // must carry a Label (every LBlock emits one) so the rotated
                // backedge jcc can target it.
                if (!bp || *bp == l) continue;      // exit label unresolved / self
                if (bp && *bp > l && *bp < p) continue; // target inside region: not B
                if (gp + 1 >= p) continue;          // empty body
                if (lf.code[gp + 1].op != IOp::Label) continue;
                if (lf.code[gp + 1].a.k != Operand::K::Label) continue;
                // the body region must be full-terminated at its end: the
                // latch (at p) is the jmp being deleted, so the last body
                // instruction is at p-1 — any instruction is fine, the latch
                // WAS the terminator; nothing falls out of the body.
                bpos = gp + 1;
                this_form_b = true;
            }

            found = true;
            best_span = span;
            L = l; guard_pos = gp; body_pos = bpos; latch_pos = p;
            form_b = this_form_b;
        }
        if (!found) break;

        int check_label = lf.label_counter++;
        std::vector<Inst> out;
        out.reserve(lf.code.size() + 2);
        if (!form_b) {
            for (size_t i = 0; i <= L; ++i) out.push_back(lf.code[i]);          // entry label
            Inst shim;
            shim.op = IOp::Jmp;
            shim.a.k = Operand::K::Label;
            shim.a.label = check_label;
            out.push_back(shim);                                                 // [L: jmp check]
            for (size_t i = body_pos; i < latch_pos; ++i) out.push_back(lf.code[i]); // body
            Inst cl;
            cl.op = IOp::Label;
            cl.a.k = Operand::K::Label;
            cl.a.label = check_label;
            out.push_back(cl);                                                   // check:
            for (size_t i = L + 1; i <= guard_pos; ++i) out.push_back(lf.code[i]);   // cond + guard jcc
            for (size_t i = guard_pos + 1; i < body_pos; ++i) out.push_back(lf.code[i]); // exit
            for (size_t i = latch_pos + 1; i < lf.code.size(); ++i) out.push_back(lf.code[i]);
        } else {
            // Form B rotation:
            //   [L: head; jcc EXIT][body ...][jmp L][exit ...]
            // becomes
            //   [L: jmp check][body ...][check: head; jcc(inv) body][exit ...]
            // The inverted jcc takes the backedge when the condition HOLDS;
            // fallthrough (condition fails) continues into the exit code,
            // which sits right where it was — after the latch position. When
            // the exit block is NOT the instruction after the latch, an
            // explicit jmp keeps the not-taken path correct.
            int body_label = lf.code[body_pos].a.label;
            for (size_t i = 0; i <= L; ++i) out.push_back(lf.code[i]);          // entry label
            Inst shim;
            shim.op = IOp::Jmp;
            shim.a.k = Operand::K::Label;
            shim.a.label = check_label;
            out.push_back(shim);                                                 // [L: jmp check]
            for (size_t i = body_pos; i < latch_pos; ++i) out.push_back(lf.code[i]); // body
            Inst cl;
            cl.op = IOp::Label;
            cl.a.k = Operand::K::Label;
            cl.a.label = check_label;
            out.push_back(cl);                                                   // check:
            for (size_t i = L + 1; i < guard_pos; ++i) out.push_back(lf.code[i]);   // head instrs (no jcc)
            Inst j2 = lf.code[guard_pos];                                        // the guard jcc
            j2.cond = inv_cond(j2.cond);
            j2.a.label = body_label;
            out.push_back(j2);                                                   // jcc(inv) → body
            // not-taken fallthrough must reach the original exit: it reached
            // it via the jcc's target label. If the code following the latch
            // is not that label, jump there explicitly.
            bool exit_follows = latch_pos + 1 < lf.code.size() &&
                                lf.code[latch_pos + 1].op == IOp::Label &&
                                lf.code[latch_pos + 1].a.label == lf.code[guard_pos].a.label;
            if (!exit_follows) {
                Inst ex;
                ex.op = IOp::Jmp;
                ex.a.k = Operand::K::Label;
                ex.a.label = lf.code[guard_pos].a.label;
                out.push_back(ex);                                               // jmp EXIT
            }
            for (size_t i = latch_pos + 1; i < lf.code.size(); ++i) out.push_back(lf.code[i]);
        }
        lf.code = std::move(out);
        any = true;
    }
    return any;
}

// Loop-entry fallthrough layout. After rotation the shape is
//   [entry: ...code...][shim: jmp CHECK][body ...][CHECK: cond; jcc body]
//   [base: ...; jmp EPI][EPI: ...; ret]
// The shim is a taken jump on EVERY physical entry (158M on tak: every
// recursive call), and the base's jmp-to-epilogue is a taken jump on every
// LEAF exit (118M). Moving the [CHECK .. EPI] cluster to immediately after
// the entry code makes both edges fallthrough:
//   [entry: ...code...][CHECK: cond; jcc body][base: ...][EPI: ...; ret]
//   [body ...][jmp CHECK]
// The leaf path (guard not taken -> base -> epilogue -> ret) becomes zero
// taken jumps. Label-based jumps make any contiguous range movable; only
// fallthrough adjacency constrains layout, and each cut preserves its edge:
// the entry's out-edge (was: shim jmp CHECK, now: fallthrough into the
// moved CHECK), the body's out-edge (was: fallthrough into CHECK, now: the
// appended latch jmp), and the cluster's own end (a full terminator, so
// nothing falls out of it). The cluster chains through [jmp L][L: ...]
// adjacency (the base->EPI edge) so the epilogue travels with it and the
// jmp becomes elidable fallthrough.
bool x64_loop_entry_fallthrough(LFunction& lf) {
    if (lf.code.empty()) return false;
    bool any = false;

    auto is_full_term = [](const Inst& q) {
        switch (q.op) {
            case IOp::Jmp: case IOp::Ret: case IOp::RetNaked:
            case IOp::TailCallFn: case IOp::TailCallNaked:
                return true;
            default: return false;
        }
    };

    for (int round = 0; round < 64; ++round) {
        auto& code = lf.code;
        FlatMap<int, size_t> label_pos;
        for (size_t i = 0; i < code.size(); ++i)
            if (code[i].op == IOp::Label) label_pos.insert(code[i].a.label, i);

        bool found = false;
        for (size_t i = 0; i + 1 < code.size(); ++i) {
            if (code[i].op != IOp::Jmp || code[i].a.k != Operand::K::Label) continue;
            const size_t* tp = label_pos.find(code[i].a.label);
            if (!tp || *tp <= i + 1) continue;             // forward, body nonempty
            size_t t = *tp;
            if (code[i + 1].op != IOp::Label ||
                code[i + 1].a.k != Operand::K::Label) continue; // body head

            // cluster: [t .. end], ending at a full terminator, chaining
            // through [jmp L][L: ...] adjacency
            size_t u = t;
            bool ok = false;
            while (u < code.size()) {
                const Inst& q = code[u];
                if (q.op == IOp::Nop || q.op == IOp::Label) { ++u; continue; }
                if (!is_full_term(q)) { ++u; continue; }
                if (q.op == IOp::Jmp && q.a.k == Operand::K::Label) {
                    // chain: jmp whose target label immediately follows
                    size_t v = u + 1;
                    while (v < code.size() && code[v].op == IOp::Nop) ++v;
                    if (v < code.size() && code[v].op == IOp::Label &&
                        code[v].a.k == Operand::K::Label && code[v].a.label == q.a.label) {
                        u = v + 1;
                        continue;
                    }
                }
                ok = true; // full terminator ends the cluster here (inclusive)
                break;
            }
            if (!ok) continue;
            size_t cluster_end = u;

            // the cluster must carry a guard jcc — a rotated loop check,
            // not an arbitrary skip-over jump
            bool has_jcc = false;
            for (size_t k = t; k <= cluster_end; ++k)
                if (code[k].op == IOp::Jcc) { has_jcc = true; break; }
            if (!has_jcc) continue;

            std::vector<Inst> out;
            out.reserve(code.size() + 2);
            for (size_t k = 0; k < i; ++k) out.push_back(code[k]);          // entry
            for (size_t k = t; k <= cluster_end; ++k) out.push_back(code[k]); // cluster
            for (size_t k = i + 1; k < t; ++k) out.push_back(code[k]);      // body
            Inst latch;
            latch.op = IOp::Jmp;
            latch.a.k = Operand::K::Label;
            latch.a.label = code[i].a.label;                                 // jmp CHECK
            out.push_back(latch);
            for (size_t k = cluster_end + 1; k < code.size(); ++k) out.push_back(code[k]);
            code = std::move(out);
            found = true;
            any = true;
            break; // positions shifted; restart the scan
        }
        if (!found) break;
    }

    // [jmp L][Label L] -> fallthrough (the base->epilogue edge after the
    // cluster move; also catches rotation's explicit exit jumps)
    {
        auto& code = lf.code;
        for (size_t i = 0; i + 1 < code.size(); ++i) {
            if (code[i].op != IOp::Jmp || code[i].a.k != Operand::K::Label) continue;
            size_t j = i + 1;
            while (j < code.size() && code[j].op == IOp::Nop) ++j;
            if (j >= code.size()) continue;
            if (code[j].op == IOp::Label && code[j].a.k == Operand::K::Label &&
                code[j].a.label == code[i].a.label) {
                code[i].op = IOp::Nop;
                any = true;
            }
        }
    }
    return any;
}


// Loop-invariant FP constant hoisting (machine level). Extracted from the
// pass-87 peephole family into pass 88 (MachineLICM) where it belongs:
// hoisting is a loop transform, not a peephole. Runs AFTER loop rotation,
// so backedge regions are the rotated [body .. check] spans and hoisted
// pairs land before the rotation shim (after the entry label) — every
// entry path (fallthrough or jump to the entry label) executes them.
bool x64_hoist_loop_constants(LFunction& lf) {
    if (lf.code.empty()) return false;
    bool changed = false;
    auto& code = lf.code;
    // The isel constant pool (xmm8-15) materializes each f64/f32 constant at
    // its FIRST USE — which for loop-carried constants sits inside the loop
    // body and re-executes every iteration. A materialization pair
    //     [movq $bits, %rax] [movq %rax, %xmmN]      (N >= 8)
    // whose target register nothing else writes, inside a loop region with
    // no calls, moves to immediately before the loop-top label (backedge
    // jumps land at the label, after the pair — the pair runs once per
    // loop ENTRY, which is all a write-free register needs).
    {
        // label id -> instruction position
        FlatMap<int, size_t> label_pos;
        for (size_t i = 0; i < code.size(); ++i)
            if (code[i].op == IOp::Label) label_pos.insert(code[i].a.label, i);
        // backedge regions: [top_pos, end_pos]
        struct Region {
            size_t top, end; // inclusive bounds
        };
        std::vector<Region> regions;
        for (size_t i = 0; i < code.size(); ++i) {
            if (code[i].op != IOp::Jcc && code[i].op != IOp::Jmp) continue;
            const size_t* tp = label_pos.find(code[i].a.label);
            if (!tp || *tp >= i) continue;
            regions.push_back(Region{*tp, i});
        }
        if (!regions.empty()) {
            // sort regions outermost-first (larger span first)
            std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) {
                return (a.end - a.top) > (b.end - b.top);
            });
            // candidate materialization pairs
            struct Pair {
                size_t imm_idx;
                R target;
            };
            std::vector<Pair> pairs;
            for (size_t i = 0; i + 1 < code.size(); ++i) {
                if (code[i].op == IOp::MovRImm && code[i].a.k == Operand::K::Reg &&
                    code[i].a.reg == R::Rax && code[i + 1].op == IOp::MovFpFromGpr &&
                    code[i + 1].a.k == Operand::K::Reg &&
                    reg_is_const_pool_xmm(code[i + 1].a.reg) &&
                    code[i + 1].b.k == Operand::K::Reg && code[i + 1].b.reg == R::Rax)
                    pairs.push_back(Pair{i, code[i + 1].a.reg});
            }
            // decide a destination (loop entry) per pair
            FlatMap<size_t, i64> remove;      // pair imm_idx -> unused
            FlatMap<size_t, size_t> dest;     // pair imm_idx -> insert-before pos
            FlatMap<size_t, u32> dest_count;  // insert pos -> number of pairs
            for (const Pair& pr : pairs) {
                for (const Region& rg : regions) {
                    if (pr.imm_idx <= rg.top || pr.imm_idx >= rg.end) continue;
                    // constraints on the WHOLE region: no calls, target not
                    // written by anything except this pair's own movq
                    bool bad = false;
                    for (size_t k = rg.top; k <= rg.end && !bad; ++k) {
                        const Inst& q = code[k];
                        if (q.op == IOp::CallFn || q.op == IOp::CallSym ||
                            q.op == IOp::TailCallFn)
                            { bad = true; break; }
                        if (k == pr.imm_idx || k == pr.imm_idx + 1) continue;
                        if ((q.op == IOp::MovFpFp || q.op == IOp::FpBin ||
                             q.op == IOp::MovFpR || q.op == IOp::FpNeg ||
                             q.op == IOp::MovFpFromGpr) &&
                            q.a.k == Operand::K::Reg && q.a.reg == pr.target)
                            { bad = true; break; }
                    }
                    if (bad) continue;
                    // Rotated regions carry a shim [jmp check] right before
                    // the region-top (body) label; inserting before the BODY
                    // label would strand the pair after the shim (dead). The
                    // shim is recognized by its jump target lying INSIDE the
                    // region. Inserting before the shim = right after the
                    // entry label: fallthrough AND jump entries both run it.
                    size_t ins = rg.top;
                    if (rg.top > 0 && code[rg.top - 1].op == IOp::Jmp &&
                        code[rg.top - 1].a.k == Operand::K::Label) {
                        const size_t* sp = label_pos.find(code[rg.top - 1].a.label);
                        if (sp && *sp > rg.top && *sp <= rg.end) ins = rg.top - 1;
                    }
                    dest.insert(pr.imm_idx, ins);
                    remove.insert(pr.imm_idx, 0);
                    if (const u32* c = dest_count.find(ins))
                        dest_count.insert(ins, *c + 1);
                    else
                        dest_count.insert(ins, 1);
                    break; // outermost qualifying region wins
                }
            }
            if (!remove.empty()) {
                // hoisted instructions per loop-top position, deterministic
                // (pair imm index ascending)
                FlatMap<size_t, std::vector<Inst>> hoisted;
                for (const auto& e : dest.entries()) {
                    size_t pi = static_cast<size_t>(e.first);
                    size_t di = e.second;
                    if (const std::vector<Inst>* v = hoisted.find(di)) {
                        std::vector<Inst> nv = *v;
                        nv.push_back(code[pi]);
                        nv.push_back(code[pi + 1]);
                        hoisted.insert(di, std::move(nv));
                    } else {
                        std::vector<Inst> nv;
                        nv.push_back(code[pi]);
                        nv.push_back(code[pi + 1]);
                        hoisted.insert(di, std::move(nv));
                    }
                }
                std::vector<Inst> out;
                out.reserve(code.size());
                for (size_t i = 0; i < code.size(); ++i) {
                    if (remove.contains(i)) {
                        ++i; // skip the MovFpFromGpr too
                        changed = true;
                        continue;
                    }
                    if (const std::vector<Inst>* v = hoisted.find(i)) {
                        for (const Inst& h : *v) out.push_back(h);
                        // (the label itself follows below)
                    }
                    out.push_back(code[i]);
                }
                code = std::move(out);
            }
        }
    }

    return changed;
}

// Fused compare-and-branch + post-RA accumulator folds (the assembly-level
// gap analysis: setcc/movzx/test sequences around branches, and rax
// round-trips through promoted registers).
bool x64_branch_fusion(LFunction& lf) {
    if (lf.code.empty()) return false;
    bool changed = false;
    auto& code = lf.code;

    // ---- 1) dead slot stores: a store whose slot is never read --------
    {
        SlotCounts sc = count_slot_refs(code);
        for (Inst& i : code) {
            if ((i.op == IOp::MovRS || i.op == IOp::MovFpS) && i.b.k == Operand::K::Slot) {
                const u32* u = sc.uses.find(i.b.slot);
                if (!u || *u == 0) { i.op = IOp::Nop; changed = true; }
            } else if (i.op == IOp::MovSImm && i.a.k == Operand::K::Slot) {
                const u32* u = sc.uses.find(i.a.slot);
                if (!u || *u == 0) { i.op = IOp::Nop; changed = true; }
            }
        }
    }

    // ---- 2) fused compare-and-branch ------------------------------------
    // Pattern (post-RA forms):
    //     [CmpXX cond-flags] [Setcc c] [MovZX] [mov slotC/regC, rax]
    //     ...flags-preserving movs only...
    //     [mov rax, slotC/regC] [Test rax, rax] [Jcc NE|E label]
    // The boolean is single-def/single-use; rewrite to [CmpXX] [Jcc(c|inv c)].
    // Post-RA pair folding (pass 85) often deletes the store+load pair
    // entirely, leaving the shorter chain
    //     [Cmp] [Setcc] [MovZX] [Test rax, rax] [Jcc]
    // which fuses whenever only flags-preserving instructions (or Nops)
    // sit between the MovZX and the Test and nothing else consumes rax.
    {
        SlotCounts sc = count_slot_refs(code);
        for (size_t t = 1; t + 1 < code.size(); ++t) {
            if (code[t].op != IOp::Test) continue;
            if (code[t].a.k != Operand::K::Reg || code[t].a.reg != R::Rax) continue;
            const Inst& jcc = code[t + 1];
            if (jcc.op != IOp::Jcc) continue;
            if (jcc.cond != Cond::NE && jcc.cond != Cond::E) continue;

            // ---- short chain: [Cmp][Setcc][MovZX] (Nop|flags-safe)* [Test] --
            {
                size_t m = t;
                bool clean = true;
                while (m > 0) {
                    --m;
                    if (code[m].op == IOp::Nop) continue;
                    if (code[m].op != IOp::MovZX) { clean = false; break; }
                    // only Nops allowed between MovZX and Test (rax consumers)
                    if (m < 2 || code[m - 1].op != IOp::Setcc ||
                        !is_cmp_op(code[m - 2].op))
                        { clean = false; break; }
                    Cond cmpcond = code[m - 1].cond;
                    code[m].op = IOp::Nop;      // movzx
                    code[m - 1].op = IOp::Nop;  // setcc
                    code[t].op = IOp::Nop;      // test
                    code[t + 1].cond =
                        (jcc.cond == Cond::NE) ? cmpcond : inv_cond(cmpcond);
                    changed = true;
                    break;
                }
                if (clean && code[t].op == IOp::Nop) continue; // fused
            }

            // the load feeding the test
            const Inst& load = code[t - 1];
            bool via_slot = false;
            i32 slot_c = 0;
            R reg_c = R::Rax;
            if (load.op == IOp::MovSR && load.a.k == Operand::K::Reg &&
                load.a.reg == R::Rax && load.b.k == Operand::K::Slot) {
                via_slot = true;
                slot_c = load.b.slot;
                const u32* d = sc.defs.find(slot_c);
                const u32* u = sc.uses.find(slot_c);
                if (!d || *d != 1 || !u || *u != 1) continue; // single-def/single-use
            } else if (load.op == IOp::MovRR && load.a.k == Operand::K::Reg &&
                       load.a.reg == R::Rax && load.b.k == Operand::K::Reg &&
                       load.b.reg != R::Rax) {
                reg_c = load.b.reg;
            } else {
                continue;
            }

            // walk back over flags-preserving movs to the defining store
            bool fused = false;
            for (size_t p = t - 1; p-- > 3;) {
                if (!flags_preserving(code[p].op)) break; // flags clobbered
                if (!is_bool_store(code[p])) continue;
                // value identity: store must write slot_c / reg_c
                if (via_slot) {
                    if (code[p].op != IOp::MovRS || code[p].b.k != Operand::K::Slot ||
                        code[p].b.slot != slot_c || code[p].a.reg != R::Rax)
                        continue;
                } else {
                    if (code[p].op != IOp::MovRR || code[p].a.k != Operand::K::Reg ||
                        code[p].a.reg != reg_c || code[p].b.k != Operand::K::Reg ||
                        code[p].b.reg != R::Rax)
                        continue;
                    // reg-home values: single def / single use globally
                    u32 defs = 0, uses = 0;
                    for (const Inst& q : code) {
                        if (q.op == IOp::MovRR && q.a.k == Operand::K::Reg &&
                            q.a.reg == reg_c && q.b.k == Operand::K::Reg && q.b.reg == R::Rax)
                            ++defs;
                        if (q.op == IOp::MovRR && q.a.k == Operand::K::Reg &&
                            q.a.reg == R::Rax && q.b.k == Operand::K::Reg && q.b.reg == reg_c)
                            ++uses;
                    }
                    if (defs != 1 || uses != 1) continue;
                }
                if (code[p - 1].op != IOp::MovZX) continue;
                if (code[p - 2].op != IOp::Setcc) continue;
                if (!is_cmp_op(code[p - 3].op)) continue;

                Cond cmpcond = code[p - 2].cond;
                code[p].op = IOp::Nop;      // boolean store
                code[p - 1].op = IOp::Nop;  // movzx
                code[p - 2].op = IOp::Nop;  // setcc
                code[t - 1].op = IOp::Nop;  // boolean load
                code[t].op = IOp::Nop;      // test
                code[t + 1].cond =
                    (jcc.cond == Cond::NE) ? cmpcond : inv_cond(cmpcond);
                changed = true;
                fused = true;
                break;
            }
            (void)fused;
        }
    }

    // ---- 3) mov+test fold: [mov rax, R] [test rax, rax] -> [test R, R] --
    //         slot form: [mov rax, [s]] [test] -> [cmpq $0, [s]] (jcc E/NE)
    {
        for (size_t t = 1; t < code.size(); ++t) {
            if (code[t].op != IOp::Test || code[t].a.k != Operand::K::Reg) continue;
            Inst& mov = code[t - 1];
            if (mov.op == IOp::MovRR && mov.a.k == Operand::K::Reg &&
                mov.a.reg == R::Rax && mov.b.k == Operand::K::Reg &&
                mov.b.reg != R::Rax) {
                code[t].a.reg = mov.b.reg;
                mov.op = IOp::Nop;
                changed = true;
            } else if (mov.op == IOp::MovSR && mov.a.k == Operand::K::Reg &&
                       mov.a.reg == R::Rax && mov.b.k == Operand::K::Slot &&
                       t + 1 < code.size() && code[t + 1].op == IOp::Jcc &&
                       (code[t + 1].cond == Cond::E || code[t + 1].cond == Cond::NE)) {
                code[t].op = IOp::CmpRImm;
                code[t].a.k = Operand::K::Slot;
                code[t].a.slot = mov.b.slot;
                code[t].b.k = Operand::K::Imm;
                code[t].b.imm = 0;
                code[t].size = 8;
                mov.op = IOp::Nop;
                changed = true;
            }
        }
    }

    // ---- 4) accumulator fold: [mov rax, B] [op rax, ...] [mov B, rax] --
    //         -> [op B, ...] (one 2-operand op through the rax accumulator)
    {
        auto mid_ok = [&](const Inst& m) {
            switch (m.op) {
                case IOp::ArithRR:
                case IOp::ArithRImm:
                case IOp::ShiftImm:
                case IOp::ShiftCl:
                case IOp::Neg:
                case IOp::Not:
                    return m.a.k == Operand::K::Reg && m.a.reg == R::Rax;
                default:
                    return false;
            }
        };
        for (size_t i = 0; i + 2 < code.size(); ++i) {
            Inst& m1 = code[i];
            Inst& mid = code[i + 1];
            Inst& m2 = code[i + 2];
            if (m1.op != IOp::MovRR || m1.a.k != Operand::K::Reg || m1.a.reg != R::Rax ||
                m1.b.k != Operand::K::Reg || m1.b.reg == R::Rax)
                continue;
            R b = m1.b.reg;
            if (!mid_ok(mid)) continue;
            if (m2.op != IOp::MovRR || m2.a.k != Operand::K::Reg || m2.a.reg != b ||
                m2.b.k != Operand::K::Reg || m2.b.reg != R::Rax)
                continue;
            mid.a.reg = b;
            m1.op = IOp::Nop;
            m2.op = IOp::Nop;
            changed = true;
        }
    }

    // ---- 5) adjacent mov pair: [mov X, Y] [mov Y, X] -> [mov X, Y] -----
    //         (both GP MovRR and FP MovFpFp; the second swap is a no-op)
    {
        for (size_t i = 0; i + 1 < code.size(); ++i) {
            Inst& a1 = code[i];
            Inst& a2 = code[i + 1];
            if ((a1.op != IOp::MovRR && a1.op != IOp::MovFpFp) || a1.op != a2.op) continue;
            if (a1.a.k != Operand::K::Reg || a1.b.k != Operand::K::Reg) continue;
            if (a2.a.k != Operand::K::Reg || a2.b.k != Operand::K::Reg) continue;
            if (a1.a.reg == a1.b.reg) continue;
            if (a2.a.reg == a1.b.reg && a2.b.reg == a1.a.reg) {
                a2.op = IOp::Nop;
                changed = true;
            }
        }
    }

    // ---- 6) copy chains through isel scratch registers -------------------
    // Post-RA phi copies lower as [mov A<-B][mov C<-A] with A a scratch
    // register (rax/xmm0/xmm1 — never allocator-homed). If A is not read
    // again before its next redefinition, the pair collapses to [mov C<-B].
    {
        auto is_scratch = [](R r) {
            return r == R::Rax || r == R::Xmm0 || r == R::Xmm1;
        };
        auto writes_reg = [](const Inst& q, R a) {
            switch (q.op) {
                case IOp::MovRR: case IOp::MovFpFp: case IOp::ArithRR:
                case IOp::ArithRImm: case IOp::FpBin: case IOp::MovFpR:
                case IOp::FpNeg: case IOp::MovFpFromGpr: case IOp::Neg:
                case IOp::Not: case IOp::MovRImm: case IOp::ShiftImm:
                case IOp::ShiftCl: case IOp::SExt32:
                    return q.a.k == Operand::K::Reg && q.a.reg == a;
                default: return false;
            }
        };
        auto reads_reg = [](const Inst& q, R a) {
            if (q.b.k == Operand::K::Reg && q.b.reg == a) return true;
            switch (q.op) {
                case IOp::ArithRR: case IOp::CmpRR: case IOp::FpBin:
                case IOp::FpCmp: case IOp::Cmov:
                    return q.a.k == Operand::K::Reg && q.a.reg == a;
                case IOp::Test: case IOp::IDiv: case IOp::UDiv:
                    return q.a.k == Operand::K::Reg && q.a.reg == a;
                case IOp::MovFpFp: case IOp::MovRR:
                    return q.a.k == Operand::K::Reg && q.a.reg == a; // dst read for swaps? conservative
                default: return false;
            }
        };
        for (size_t i = 0; i + 1 < code.size(); ++i) {
            Inst& m1 = code[i];
            Inst& m2 = code[i + 1];
            if ((m1.op != IOp::MovRR && m1.op != IOp::MovFpFp) || m1.op != m2.op)
                continue;
            if (m1.a.k != Operand::K::Reg || m1.b.k != Operand::K::Reg) continue;
            if (m2.a.k != Operand::K::Reg || m2.b.k != Operand::K::Reg) continue;
            R a = m1.a.reg, b = m1.b.reg, c = m2.a.reg;
            if (!is_scratch(a)) continue;      // only isel scratch chains
            if (m2.b.reg != a) continue;       // must chain
            if (b == a || c == a) continue;
            // forward scan: A must be redefined before any read / boundary
            bool ok = false, abort = false;
            for (size_t j = i + 2; j < code.size(); ++j) {
                const Inst& q = code[j];
                if (q.op == IOp::Label || q.op == IOp::Jcc || q.op == IOp::Jmp ||
                    q.op == IOp::Ret || q.op == IOp::RetNaked || q.op == IOp::CallFn ||
                    q.op == IOp::CallSym || q.op == IOp::TailCallFn ||
                    q.op == IOp::TailCallNaked)
                    { abort = true; break; }
                if (writes_reg(q, a)) { ok = true; break; }
                if (reads_reg(q, a)) { abort = true; break; }
            }
            if (!ok || abort) continue;
            m1.op = IOp::Nop;
            m2.b.reg = b;
            changed = true;
        }
    }

    // ---- 7) lea formation --------------------------------------------------
    // Post-RA IV updates and address arithmetic lower as two-instruction
    // pairs around a copy: [mov R2, R1][add/sub $k, R2] and [mov R2,
    // R1][shl $k, R2]. The lea computes both in one instruction with the
    // same register reads/writes, so the rewrite is semantics-preserving.
    {
        for (size_t i = 0; i + 1 < code.size(); ++i) {
            Inst& m = code[i];
            if (m.op != IOp::MovRR || m.a.k != Operand::K::Reg ||
                m.b.k != Operand::K::Reg)
                continue;
            if (m.a.reg == m.b.reg) continue;        // no-op copy, other rules
            if (m.size != 8) continue;               // 64-bit lea only
            size_t u = i + 1;
            while (u < code.size() && code[u].op == IOp::Nop) ++u;
            if (u >= code.size()) continue;
            Inst& ar = code[u];
            R dst = m.a.reg, src = m.b.reg;
            if (ar.bin == BinOp::Add || ar.bin == BinOp::Sub) {
                if (ar.op != IOp::ArithRImm) continue;
                if (ar.a.k != Operand::K::Reg || ar.a.reg != dst) continue;
                if (ar.b.k != Operand::K::Imm) continue;
                i64 disp = (ar.bin == BinOp::Add) ? ar.b.imm : -ar.b.imm;
                if (static_cast<i64>(static_cast<i32>(disp)) != disp) continue;
                m.op = IOp::Nop;                     // the mov
                ar.op = IOp::LeaRR;
                ar.a.k = Operand::K::Reg; ar.a.reg = dst;
                ar.b.k = Operand::K::Reg; ar.b.reg = src;
                ar.b.imm = disp;
                ar.size = 1;                         // scale 1
                changed = true;
            } else if (ar.op == IOp::ShiftImm && ar.bin == BinOp::Shl) {
                if (ar.a.k != Operand::K::Reg || ar.a.reg != dst) continue;
                if (ar.b.k != Operand::K::Imm) continue;
                i64 k = ar.b.imm;
                if (k < 0 || k > 3) continue;        // scale 2/4/8 only
                // [mov R2, R1][shl $k, R2] === [lea R2, (,R1, 2^k)]: the
                // copy makes R2 == R1, and shifting is multiplying by 2^k
                // (k <= 3 = lea scale range). Same reads (R1) and writes (R2).
                m.op = IOp::Nop;                     // the mov
                ar.op = IOp::LeaRR;
                ar.a.k = Operand::K::Reg; ar.a.reg = dst;
                ar.b.k = Operand::K::Reg; ar.b.reg = src;
                ar.b.imm = 0;
                ar.size = static_cast<u8>(k == 0 ? 1 : (1 << k));
                changed = true;
            }
        }
    }

    // ---- sweep ------------------------------------------------------------
    std::vector<Inst> out;
    out.reserve(code.size());
    for (const Inst& i : code)
        if (i.op != IOp::Nop) out.push_back(i);
    if (out.size() != code.size()) {
        code = std::move(out);
        changed = true;
    }
    return changed;
}

bool x64_machine_peephole(LFunction& lf) {
    bool changed = false;
    auto& code = lf.code;

    // Register-reference predicates for the scan-based patterns below.
    auto reads_reg = [](const Inst& q, R x) {
        if (q.b.k == Operand::K::Reg && q.b.reg == x) return true;
        switch (q.op) {
            case IOp::ArithRR: case IOp::CmpRR: case IOp::FpBin:
            case IOp::FpCmp: case IOp::Cmov: case IOp::Test:
            case IOp::IDiv: case IOp::UDiv:
                return q.a.k == Operand::K::Reg && q.a.reg == x;
            default: return false;
        }
    };
    auto writes_reg = [](const Inst& q, R x) {
        switch (q.op) {
            case IOp::MovRR: case IOp::MovFpFp: case IOp::ArithRR:
            case IOp::ArithRImm: case IOp::FpBin: case IOp::FpNeg:
            case IOp::MovRImm: case IOp::ShiftImm: case IOp::ShiftCl:
            case IOp::Neg: case IOp::Not: case IOp::SExt32:
            case IOp::MovFpR: case IOp::MovFpFromGpr: case IOp::MovSR:
            case IOp::MovFpS: case IOp::MovRS: case IOp::MovZX:
            case IOp::CvtToFp: case IOp::CvtToInt: case IOp::Cmov:
            case IOp::Setcc:
                return q.a.k == Operand::K::Reg && q.a.reg == x;
            default: return false;
        }
    };
    // control-flow boundary: scans stop here (crossing a label needs real
    // liveness, not local scanning)
    auto is_boundary = [](const Inst& q) {
        switch (q.op) {
            case IOp::Label: case IOp::Jcc: case IOp::Jmp: case IOp::Ret:
            case IOp::RetNaked: case IOp::CallFn: case IOp::CallSym:
            case IOp::TailCallFn: case IOp::TailCallNaked:
                return true;
            default: return false;
        }
    };
    auto next_live = [&](size_t from) -> size_t {
        size_t j = from;
        while (j < code.size() && code[j].op == IOp::Nop) ++j;
        return j;
    };

    // ---- movzx destination retarget -------------------------------------
    // [movzbq %al, %rax][mov X, %rax] -> [movzbq %al, %X]: the extended
    // byte lands in its home directly. Sound when nothing reads rax
    // between the pair and rax's next def (a later reader would see the
    // pre-extension value instead of the extended one).
    for (size_t i = 0; i + 1 < code.size(); ++i) {
        Inst& zx = code[i];
        if (zx.op != IOp::MovZX || zx.a.k != Operand::K::Reg) continue;
        size_t mi = next_live(i + 1);
        if (mi >= code.size()) continue;
        Inst& mv = code[mi];
        if (mv.op != IOp::MovRR || mv.a.k != Operand::K::Reg ||
            mv.b.k != Operand::K::Reg || mv.b.reg != R::Rax) continue;
        if (mv.a.reg == R::Rax) continue;
        // forward scan: rax must be redefined before any read / boundary
        bool ok = false, abort = false;
        for (size_t j = mi + 1; j < code.size(); ++j) {
            const Inst& q = code[j];
            if (is_boundary(q)) { abort = true; break; }
            if (q.op == IOp::Nop) continue;
            if (writes_reg(q, R::Rax)) { ok = true; break; }
            if (reads_reg(q, R::Rax)) { abort = true; break; }
        }
        if (!ok || abort) continue;
        zx.a.reg = mv.a.reg;
        mv.op = IOp::Nop;
        changed = true;
    }

    // ---- producer destination retarget ----------------------------------
    // [lea/mov-imm/arith-imm → S][mov D, S] -> [producer → D]: the value
    // lands in its final home in one hop instead of two (gcc's
    // `lea -1(%rbx),%rdi` shape for recursive call arguments). Sound when
    // nothing reads S between the copy and S's next redefinition; a call
    // counts as a redefinition of rax only (the return register) — other
    // registers survive calls or become garbage, so the scan stops there.
    {
        auto scan_dead = [&](size_t from, R x) -> bool {
            for (size_t j = from; j < code.size(); ++j) {
                const Inst& q = code[j];
                if (q.op == IOp::Nop) continue;
                if (q.op == IOp::CallFn || q.op == IOp::CallSym) {
                    if (x == R::Rax) return true;  // rax = return value
                    return false;                  // survives or garbage: stop
                }
                if (is_boundary(q)) return false;
                if (writes_reg(q, x)) return true;
                if (reads_reg(q, x)) return false;
            }
            return false;
        };
        for (size_t i = 0; i + 1 < code.size(); ++i) {
            Inst& p = code[i];
            bool is_lea = (p.op == IOp::LeaRR);
            // MovRImm only: it WRITES its dst without reading it. ArithRImm
            // (add/sub/... $k, S) READS S — retargeting would compute
            // D+k instead of S+k (found by t01 @ -Og: fib args corrupted).
            bool is_imm = (p.op == IOp::MovRImm);
            if (!is_lea && !is_imm) continue;
            if (p.a.k != Operand::K::Reg) continue;
            R S = p.a.reg;
            size_t mi = next_live(i + 1);
            if (mi >= code.size()) continue;
            Inst& mv = code[mi];
            if (mv.op != IOp::MovRR || mv.a.k != Operand::K::Reg ||
                mv.b.k != Operand::K::Reg || mv.b.reg != S) continue;
            R D = mv.a.reg;
            if (D == S) continue;
            // LeaRR.size is the SCALE (1/2/4/8), not the width — leas are
            // 64-bit here; only the imm producers carry a real width.
            if (is_imm && p.size != mv.size) continue;      // width mismatch
            if (is_lea && p.b.k == Operand::K::Reg && p.b.reg == D) continue; // lea reads D
            if (!scan_dead(mi + 1, S)) continue;
            p.a.reg = D;
            mv.op = IOp::Nop;
            changed = true;
        }
    }

    // ---- commutative phi-backedge fusion ---------------------------------
    // [op X, Y][mov Y, X] (commutative op) -> [op X... -> [op Y, X]: the
    // operation writes its result straight into the phi home it is copied
    // to — `addq %rbx,%rax; movq %rax,%rbx` becomes `addq %rax,%rbx`, one
    // instruction instead of two on every loop iteration (fib's
    // accumulator). Sound when X is dead on every path from the copy to
    // X's next redefinition (the walk follows jmp targets and requires
    // BOTH jcc paths dead; calls redefine rax — the return register — and
    // Ret reads it).
    {
        FlatMap<int, size_t> label_pos;
        for (size_t i = 0; i < code.size(); ++i)
            if (code[i].op == IOp::Label) label_pos.insert(code[i].a.label, i);

        enum class LS { Dead, Live, Unknown };
        // budget-bounded walk: ~1500 instructions examined per path set
        std::function<LS(size_t, R, int)> walk = [&](size_t i, R x, int budget) -> LS {
            for (size_t j = i; j < code.size() && budget > 0; --budget) {
                const Inst& q = code[j];
                if (q.op == IOp::Label) { ++j; continue; }   // fallthrough merge
                if (q.op == IOp::Nop) { ++j; continue; }
                if (q.op == IOp::CallFn || q.op == IOp::CallSym) {
                    return (x == R::Rax) ? LS::Dead : LS::Unknown;
                }
                if (q.op == IOp::Ret || q.op == IOp::RetNaked) {
                    return (x == R::Rax) ? LS::Live : LS::Dead; // return value / dead at exit
                }
                if (q.op == IOp::TailCallFn || q.op == IOp::TailCallNaked)
                    return LS::Unknown; // control leaves; registers may be live at the target
                if (q.op == IOp::Jmp) {
                    if (q.a.k != Operand::K::Label) return LS::Unknown;
                    const size_t* tp = label_pos.find(q.a.label);
                    if (!tp) return LS::Unknown;
                    j = *tp; // continue at the target label
                    continue;
                }
                if (q.op == IOp::Jcc) {
                    if (q.a.k != Operand::K::Label) return LS::Unknown;
                    const size_t* tp = label_pos.find(q.a.label);
                    if (!tp) return LS::Unknown;
                    LS t = walk(*tp, x, budget / 2);      // taken path
                    LS f = walk(j + 1, x, budget / 2);    // fallthrough path
                    if (t == LS::Live || f == LS::Live) return LS::Live;
                    if (t == LS::Dead && f == LS::Dead) return LS::Dead;
                    return LS::Unknown;
                }
                if (writes_reg(q, x)) return LS::Dead;
                if (reads_reg(q, x)) return LS::Live;
                ++j;
            }
            return LS::Unknown;
        };

        for (size_t i = 0; i + 1 < code.size(); ++i) {
            Inst& op = code[i];
            if (op.op != IOp::ArithRR || op.a.k != Operand::K::Reg ||
                op.b.k != Operand::K::Reg) continue;
            if (op.bin != BinOp::Add && op.bin != BinOp::And &&
                op.bin != BinOp::Or && op.bin != BinOp::Xor &&
                op.bin != BinOp::Mul) continue;      // commutative only
            size_t mi = next_live(i + 1);
            if (mi >= code.size()) continue;
            Inst& mv = code[mi];
            if (mv.op != IOp::MovRR || mv.a.k != Operand::K::Reg ||
                mv.b.k != Operand::K::Reg) continue;
            if (mv.b.reg != op.a.reg || mv.a.reg != op.b.reg) continue;
            if (op.a.reg == op.b.reg) continue;
            if (mv.size != op.size) continue;
            R X = op.a.reg;
            if (walk(mi + 1, X, 1500) != LS::Dead) continue;
            std::swap(op.a.reg, op.b.reg);
            mv.op = IOp::Nop;
            changed = true;
        }
    }

    // ---- and/or accumulator + branch fold --------------------------------
    // [mov rax, B][op rax, X][test rax, rax][jcc NE|E] -> [op B, X][jcc]
    // (op must set flags — and/or/add/sub; the test is folded by the
    // redundant-test pattern below). B must be dead after (its old value
    // has no readers before its next def).
    for (size_t i = 0; i + 3 < code.size(); ++i) {
        Inst& mv = code[i];
        if (mv.op != IOp::MovRR || mv.a.k != Operand::K::Reg ||
            mv.b.k != Operand::K::Reg || mv.a.reg != R::Rax) continue;
        R B = mv.b.reg;
        if (B == R::Rax) continue;
        size_t oi = next_live(i + 1);
        if (oi >= code.size()) continue;
        Inst& op = code[oi];
        if (op.op != IOp::ArithRR || op.a.k != Operand::K::Reg || op.a.reg != R::Rax) continue;
        if (op.bin != BinOp::And && op.bin != BinOp::Or &&
            op.bin != BinOp::Add && op.bin != BinOp::Sub) continue;
        if (op.b.k != Operand::K::Reg || op.b.reg == B) continue;
        size_t ti = next_live(oi + 1);
        if (ti >= code.size()) continue;
        if (code[ti].op != IOp::Test || code[ti].a.k != Operand::K::Reg ||
            code[ti].a.reg != R::Rax) continue;
        size_t ji = next_live(ti + 1);
        if (ji >= code.size()) continue;
        if (code[ji].op != IOp::Jcc) continue;
        Cond jc = code[ji].cond;
        if (jc != Cond::NE && jc != Cond::E) continue;
        // B dead after: no reads of B before its next def / boundary
        bool ok = false, abort = false;
        for (size_t j = oi + 1; j < code.size(); ++j) {
            const Inst& q = code[j];
            if (is_boundary(q)) { abort = true; break; }
            if (q.op == IOp::Nop) continue;
            if (writes_reg(q, B)) { ok = true; break; }
            if (reads_reg(q, B)) { abort = true; break; }
        }
        if (!ok || abort) continue;
        op.a.reg = B;
        code[ti].a.reg = B;
        mv.op = IOp::Nop;
        changed = true;
    }

    // ---- redundant test after flag-setting ops ---------------------------
    // [and/or/add/sub/imul/cmp r, x][test r, r][jcc E|NE] -> [op][jcc]:
    // the arithmetic already set ZF/SF for exactly this result. The test
    // only survives when something between the op and the test clobbers
    // flags (conservative: only Nops allowed between).
    for (size_t i = 0; i + 1 < code.size(); ++i) {
        const Inst& op = code[i];
        bool sets_flags = false;
        switch (op.op) {
            case IOp::ArithRR: case IOp::ArithRImm:
                sets_flags = true; break;
            default: break;
        }
        if (!sets_flags) continue;
        size_t ti = next_live(i + 1);
        if (ti >= code.size()) continue;
        const Inst& tst = code[ti];
        if (tst.op != IOp::Test || tst.a.k != Operand::K::Reg) continue;
        if (op.a.k != Operand::K::Reg || tst.a.reg != op.a.reg) continue;
        size_t ji = next_live(ti + 1);
        if (ji >= code.size()) continue;
        if (code[ji].op != IOp::Jcc) continue;
        Cond jc = code[ji].cond;
        if (jc != Cond::NE && jc != Cond::E) continue;
        code[ti].op = IOp::Nop;
        changed = true;
    }

    // ---- existing sweeps --------------------------------------------------
    // Linear dead code: after an unconditional control transfer (jmp /
    // leave;jmp / leave;ret / ret) nothing is reachable until the next
    // label. TCO rewriting and epilogue threading leave zombie blocks
    // there (e.g. the original return path after a tail-call conversion).
    {
        bool dead = false;
        for (Inst& c : code) {
            if (c.op == IOp::Label) { dead = false; continue; }
            if (dead) {
                if (c.op != IOp::Nop) { c.op = IOp::Nop; changed = true; }
                continue;
            }
            if (false && (c.op == IOp::Jmp || c.op == IOp::Ret || c.op == IOp::RetNaked ||
                c.op == IOp::TailCallFn || c.op == IOp::TailCallNaked))
                dead = true;
        }
    }
    for (Inst& i : code) {
        if (i.op == IOp::CmpRImm && i.b.k == Operand::K::Imm && i.b.imm == 0 &&
            i.a.k == Operand::K::Reg) {
            i.op = IOp::Test;
            i.b = Operand{};
            changed = true;
            continue;
        }
        if (i.op == IOp::MovRR && i.a.k == Operand::K::Reg && i.b.k == Operand::K::Reg &&
            i.a.reg == i.b.reg) {
            i.op = IOp::Nop;
            changed = true;
            continue;
        }
    }
    std::vector<Inst> out;
    out.reserve(code.size());
    for (const Inst& i : code)
        if (i.op != IOp::Nop) out.push_back(i);
    if (out.size() != code.size()) changed = true;
    lf.code = std::move(out);
    return changed;
}

// ---- serialization --------------------------------------------------------------------
namespace {

void serialize_inst(std::ostringstream& os, const Inst& i, const LFunction& lf, u32 fid) {
    auto r = [&](R reg) { return std::string("%") + rname(reg); };
    auto rs = [&](R reg, u8 size) {
        return std::string("%") + (size == 4 ? rname32(reg) : rname(reg));
    };
    auto lbl = [&](int id) {
        if (id == kEntryLabelId) return ".L" + std::to_string(fid) + "_E";
        return ".L" + std::to_string(fid) + "_" + std::to_string(id);
    };
    auto off = [&](i32 s) {
        i32 o = (s >= 0 && static_cast<size_t>(s) < lf.slot_offset.size())
                    ? lf.slot_offset[static_cast<size_t>(s)]
                    : 0;
        return (o < 0 ? "-" + std::to_string(-o) : std::to_string(o)) + "(%rbp)";
    };
    auto slotstr = [&](const Operand& o) { return off(o.slot); };

    switch (i.op) {
        case IOp::Nop: break;
        case IOp::Label: os << lbl(i.a.label) << ":\n"; break;
        case IOp::PushRbp: os << "\tpushq %rbp\n"; break;
        case IOp::PopRbp: os << "\tpopq %rbp\n"; break;
        case IOp::Ret: os << "\tleave\n\tret\n"; break;
        case IOp::FrameSub: os << "\tsubq $" << i.b.imm << ", %rsp\n"; break;
        case IOp::MovRR: os << "\tmov" << ssz(i.size) << " " << rs(i.b.reg, i.size) << ", " << rs(i.a.reg, i.size) << "\n"; break;
        case IOp::MovRS: os << "\tmov" << ssz(i.size) << " " << rs(i.a.reg, i.size) << ", " << slotstr(i.b) << "\n"; break;
        case IOp::MovSR: os << "\tmov" << ssz(i.size) << " " << slotstr(i.b) << ", " << rs(i.a.reg, i.size) << "\n"; break;
        case IOp::MovRImm: os << "\tmovq $" << i.b.imm << ", " << r(i.a.reg) << "\n"; break;
        case IOp::MovSImm: os << "\tmovq $" << i.b.imm << ", " << slotstr(i.a) << "\n"; break;
        case IOp::MovZX: os << "\tmovzbq %al, " << r(i.a.reg) << "\n"; break;
        case IOp::LoadMem: os << "\tmov" << ssz(i.size) << " (" << r(i.b.reg) << "), " << rs(i.a.reg, i.size) << "\n"; break;
        case IOp::StoreMem: os << "\tmov" << ssz(i.size) << " " << rs(i.b.reg, i.size) << ", (" << r(i.a.reg) << ")\n"; break;
        case IOp::LeaSlot: os << "\tleaq " << slotstr(i.b) << ", " << r(i.a.reg) << "\n"; break;
        case IOp::LeaSym: {
            std::string l = "?";
            if (i.b.k == Operand::K::Sym && i.b.slot >= 0 &&
                static_cast<size_t>(i.b.slot) < lf.strings.size())
                l = lf.strings[static_cast<size_t>(i.b.slot)].label;
            os << "\tleaq " << l << "(%rip), " << r(i.a.reg) << "\n";
            break;
        }
        case IOp::ArithRR: {
            const char* mn = "add";
            switch (i.bin) {
                case BinOp::Add: mn = "add"; break;
                case BinOp::Sub: mn = "sub"; break;
                case BinOp::Mul: mn = "imul"; break;
                case BinOp::And: mn = "and"; break;
                case BinOp::Or:  mn = "or";  break;
                case BinOp::Xor: mn = "xor"; break;
                default: break;
            }
            os << "\t" << mn << ssz(i.size) << " " << rs(i.b.reg, i.size) << ", " << rs(i.a.reg, i.size) << "\n";
            break;
        }
        case IOp::ArithRImm: {
            if (i.bin == BinOp::Mul) {
                // imul has no add-style two-operand immediate form; use the
                // explicit three-operand encoding (dst = dst * imm). The
                // add/sub default here previously swallowed Mul and emitted
                // `add $imm` — a silent miscompile for reg*const.
                os << "\timul" << ssz(i.size) << " $" << i.b.imm << ", "
                   << rs(i.a.reg, i.size) << ", " << rs(i.a.reg, i.size) << "\n";
                break;
            }
            const char* mn = "add";
            switch (i.bin) {
                case BinOp::Add: mn = "add"; break;
                case BinOp::Sub: mn = "sub"; break;
                case BinOp::And: mn = "and"; break;
                case BinOp::Or:  mn = "or";  break;
                case BinOp::Xor: mn = "xor"; break;
                default: break;
            }
            os << "\t" << mn << ssz(i.size) << " $" << i.b.imm << ", " << rs(i.a.reg, i.size) << "\n";
            break;
        }
        case IOp::ShiftImm: {
            const char* mn = "shl";
            if (i.bin == BinOp::Shr) mn = i.sar ? "sar" : "shr";
            os << "\t" << mn << ssz(i.size) << " $" << i.b.imm << ", " << rs(i.a.reg, i.size) << "\n";
            break;
        }
        case IOp::ShiftCl: {
            const char* mn = "shl";
            if (i.bin == BinOp::Shr) mn = i.sar ? "sar" : "shr";
            os << "\t" << mn << ssz(i.size) << " %cl, " << rs(i.a.reg, i.size) << "\n";
            break;
        }
        case IOp::Cqo: os << "\tcqo\n"; break;
        case IOp::IDiv: os << "\tidivq " << r(i.a.reg) << "\n"; break;
        case IOp::UDiv: os << "\tdivq " << r(i.a.reg) << "\n"; break;
        case IOp::CmpRR: os << "\tcmp" << ssz(i.size) << " " << rs(i.b.reg, i.size) << ", " << rs(i.a.reg, i.size) << "\n"; break;
        case IOp::CmpRImm:
            if (i.a.k == Operand::K::Slot) // fold-3 form: cmp $imm, off(%rbp)
                os << "\tcmp" << ssz(i.size) << " $" << i.b.imm << ", " << slotstr(i.a) << "\n";
            else
                os << "\tcmp" << ssz(i.size) << " $" << i.b.imm << ", " << rs(i.a.reg, i.size) << "\n";
            break;
        case IOp::Test: os << "\ttestq " << r(i.a.reg) << ", " << r(i.a.reg) << "\n"; break;
        case IOp::Setcc: os << "\tset" << cc(i.cond) << " %al\n"; break;
        case IOp::Cmov: os << "\tcmov" << cc(i.cond) << ssz(i.size) << " " << rs(i.b.reg, i.size) << ", " << rs(i.a.reg, i.size) << "\n"; break;
        case IOp::Jcc: os << "\tj" << cc(i.cond) << " " << lbl(i.a.label) << "\n"; break;
        case IOp::Jmp: os << "\tjmp " << lbl(i.a.label) << "\n"; break;
        case IOp::CallSym: os << "\tcall " << i.a.sym << "@PLT\n"; break;
        case IOp::CallFn: os << "\tcall .L" << i.a.label << "_E\n"; break;
        case IOp::TailCallFn: os << "\tleave\n\tjmp .L" << i.a.label << "_E\n"; break;
        case IOp::XorEax: os << "\txorl %eax, %eax\n"; break;
        case IOp::FpBin: {
            const char* mn = "add";
            switch (i.bin) {
                case BinOp::Add: mn = "add"; break;
                case BinOp::Sub: mn = "sub"; break;
                case BinOp::Mul: mn = "mul"; break;
                case BinOp::Div: mn = "div"; break;
                default: break;
            }
            os << "\t" << mn << fpsz(i.size) << " " << r(i.b.reg) << ", " << r(i.a.reg) << "\n";
            break;
        }
        case IOp::FpCmp: os << "\tucomi" << fpsz(i.size) << " " << r(i.b.reg) << ", " << r(i.a.reg) << "\n"; break;
        case IOp::FpNeg:
            if (i.size == 8) os << "\txorpd .Lnegmask64(%rip), " << r(i.a.reg) << "\n";
            else os << "\txorps .Lnegmask32(%rip), " << r(i.a.reg) << "\n";
            break;
        case IOp::SExt32: os << "\tmovslq %eax, %rax\n"; break;
        case IOp::CvtToFp:
            os << (i.size == 8 ? "\tcvtsi2sdq %rax, %xmm0\n" : "\tcvtsi2sdl %eax, %xmm0\n");
            break;
        case IOp::CvtToInt:
            os << (i.size == 8 ? "\tcvttsd2siq %xmm0, %rax\n" : "\tcvttsd2sil %xmm0, %eax\n");
            break;
        case IOp::FpExt: os << "\tcvtss2sd %xmm0, %xmm0\n"; break;
        case IOp::FpTrunc: os << "\tcvtsd2ss %xmm0, %xmm0\n"; break;
        case IOp::MovFpS: os << "\tmov" << fpsz(i.size) << " " << r(i.a.reg) << ", " << slotstr(i.b) << "\n"; break;
        case IOp::MovFpR: os << "\tmov" << fpsz(i.size) << " " << slotstr(i.b) << ", " << r(i.a.reg) << "\n"; break;
        case IOp::Neg: os << "\tnegq " << r(i.a.reg) << "\n"; break;
        case IOp::Not: os << "\tnotq " << r(i.a.reg) << "\n"; break;
        case IOp::MovFpFromGpr: os << "\tmovq " << r(i.b.reg) << ", " << r(i.a.reg) << "\n"; break;
        case IOp::MovFpFromGpr32: os << "\tmovd " << r(i.b.reg) << ", " << r(i.a.reg) << "\n"; break;
        case IOp::MovFpFp:
            // Reg-reg FP moves emit the full-width form (movapd/movaps):
            // the merge-encoded movsd leaves the upper lane stale and is NOT
            // move-eliminable on Intel, adding a cycle to every FP dep chain
            // that threads a copy. Full-width moves rename away. The upper
            // bits are never read: this backend produces only scalar lanes.
            os << (i.size == 8 ? "\tmovapd " : "\tmovaps ") << r(i.b.reg) << ", "
               << r(i.a.reg) << "\n";
            break;
        case IOp::FpZero:
            os << (i.size == 8 ? "\txorpd " : "\txorps ") << r(i.a.reg) << ", "
               << r(i.a.reg) << "\n";
            break;
        case IOp::PushCal: os << "\tpushq " << r(i.a.reg) << "\n"; break;
        case IOp::PopCal: os << "\tpopq " << r(i.a.reg) << "\n"; break;
        case IOp::RetNaked: os << "\tret\n"; break;
        case IOp::TailCallNaked: os << "\tjmp .L" << i.a.label << "_E\n"; break;
        case IOp::LeaRR: {
            // dst = base*scale + disp (AT&T: leaq disp(,base,scale), dst)
            int scale = (i.size == 2 || i.size == 4 || i.size == 8) ? i.size : 1;
            os << "\tleaq " << i.b.imm << "(," << r(i.b.reg) << "," << scale << "), "
               << r(i.a.reg) << "\n";
            break;
        }
        case IOp::RestoreCal:
            os << "\tmovq " << (i.b.imm < 0 ? "-" : "") << (i.b.imm < 0 ? -i.b.imm : i.b.imm)
               << "(%rbp), " << r(i.a.reg) << "\n";
            break;
        case IOp::Comment: break;
    }
}

} // namespace

std::string serialize_module_asm(const LinearModule& lin, SymbolTable& syms) {
    (void)syms;
    std::ostringstream os;
    os << "\t.text\n";
    for (const LFunction& lf : lin.fns) {
        if (lf.is_main) {
            os << ".globl main\n";
            os << "main:\n";
        }
        for (const Inst& i : lf.code) serialize_inst(os, i, lf, lf.fid);
        os << "\n";
    }
    os << "\t.section .rodata\n";
    os << ".Lnegmask64:\n\t.quad 0x8000000000000000\n\t.quad 0\n";
    os << ".Lnegmask32:\n\t.long 0x80000000\n\t.long 0\n";
    for (const LFunction& lf : lin.fns) {
        for (const StringConst& s : lf.strings) {
            std::string esc;
            for (char ch : s.text) {
                if (ch == '\n') esc += "\\n";
                else if (ch == '\t') esc += "\\t";
                else if (ch == '\\') esc += "\\\\";
                else if (ch == '"') esc += "\\\"";
                else esc += ch;
            }
            os << s.label << ":\n\t.string \"" << esc << "\"\n";
        }
    }
    return os.str();
}

} // namespace jules
