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

namespace jules {

namespace {

// ---- helpers -----------------------------------------------------------------
const char* rname(R r) {
    switch (r) {
        case R::Rax: return "rax"; case R::Rcx: return "rcx";
        case R::Rdx: return "rdx"; case R::Rsi: return "rsi";
        case R::Rdi: return "rdi"; case R::R8:  return "r8";
        case R::R9:  return "r9";  case R::R10: return "r10";
        case R::R11: return "r11"; case R::Rbp: return "rbp";
        case R::Rsp: return "rsp";
        case R::Xmm0: return "xmm0"; case R::Xmm1: return "xmm1";
        case R::Xmm2: return "xmm2"; case R::Xmm3: return "xmm3";
        case R::Xmm4: return "xmm4"; case R::Xmm5: return "xmm5";
        case R::Xmm6: return "xmm6"; case R::Xmm7: return "xmm7";
    }
    return "rax";
}

const char* rname32(R r) {
    switch (r) {
        case R::Rax: return "eax"; case R::Rcx: return "ecx";
        case R::Rdx: return "edx"; case R::Rsi: return "esi";
        case R::Rdi: return "edi"; case R::R8:  return "r8d";
        case R::R9:  return "r9d";  case R::R10: return "r10d";
        case R::R11: return "r11d"; case R::Rbp: return "ebp";
        case R::Rsp: return "esp";
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
            u64 bits = 0;
            if (size == 8) {
                std::memcpy(&bits, &nd.fval, sizeof bits);
                imm_reg(IOp::MovRImm, R::Rax, static_cast<i64>(bits));
                reg2(IOp::MovFpFromGpr, r, R::Rax);
            } else {
                f32 f = static_cast<f32>(nd.fval);
                u32 b32 = 0;
                std::memcpy(&b32, &f, sizeof b32);
                imm_reg(IOp::MovRImm, R::Rax, static_cast<i64>(b32));
                reg2(IOp::MovFpFromGpr32, r, R::Rax);
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
            for (NodeId n : b.nodes) emit_node(n);
            for (int ci : b.phi_copy_indices) {
                const LPhiCopy& cp = lf_.phi_copies[static_cast<size_t>(ci)];
                emit_phi_copy(cp);
            }
            emit_terminator(b);
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
            Inst& i = emit(IOp::ArithRImm);
            i.bin = op;
            i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
            i.b.k = Operand::K::Imm; i.b.imm = c.iv;
            i.size = size;
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
        Inst& st = emit(IOp::MovFpS);
        st.a.k = Operand::K::Reg; st.a.reg = R::Xmm0;
        st.b.k = Operand::K::Slot; st.b.slot = slot(n);
        st.size = size;
    }

    void emit_cmp(NodeId n) {
        const Node& nd = g_.node(n);
        const Node& an = g_.node(nd.in[1]);
        u8 size = sz_of(an.ty);
        Cond cond = cmp_cond(static_cast<CmpOp>(nd.sub), ty_is_signed(an.ty));
        if (fp_of(an.ty)) {
            load_fp(nd.in[1], R::Xmm0);
            load_fp(nd.in[2], R::Xmm1);
            emit(IOp::FpCmp).size = sz_of(an.ty);
        } else {
            load_value(nd.in[1], R::Rax, size);
            if (g_.node(nd.in[2]).op == Op::Const) {
                ConstVal c;
                const_of(g_, nd.in[2], c);
                Inst& i = emit(IOp::CmpRImm);
                i.a.k = Operand::K::Reg; i.a.reg = R::Rax;
                i.b.k = Operand::K::Imm; i.b.imm = c.iv;
                i.size = size;
            } else {
                load_value(nd.in[2], R::Rcx, size);
                reg2(IOp::CmpRR, R::Rax, R::Rcx, size);
            }
        }
        Inst& sc = emit(IOp::Setcc);
        sc.cond = cond;
        emit(IOp::MovZX);
        store_result(n, 8);
    }

    void emit_un(NodeId n) {
        const Node& nd = g_.node(n);
        UnOp op = static_cast<UnOp>(nd.sub);
        if (fp_of(nd.ty)) {
            load_fp(nd.in[1], R::Xmm0);
            Inst& i = emit(IOp::FpNeg);
            i.size = sz_of(nd.ty);
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
                load_value(src, R::Rax, src_size);
                store_result(n, 8);
                return;
            case CastOp::SExt:
                if (src_size == 4) {
                    ld_slot(IOp::MovSR, R::Rax, slot(src), 4);
                    emit(IOp::SExt32);
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
        const Node& size = g_.node(nd.in[2]);
        imm_reg(IOp::MovRImm, R::Rdi, size.ival);
        Inst& c = emit(IOp::CallSym);
        c.a.k = Operand::K::Sym; c.a.sym = "malloc";
        store_result(n, 8);
    }

    void emit_call(NodeId n) {
        const Node& nd = g_.node(n);
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

// ---- pass 87 -----------------------------------------------------------------------
bool x64_machine_peephole(LFunction& lf) {
    bool changed = false;
    for (Inst& i : lf.code) {
        if (i.op == IOp::CmpRImm && i.b.k == Operand::K::Imm && i.b.imm == 0) {
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
    out.reserve(lf.code.size());
    for (const Inst& i : lf.code)
        if (i.op != IOp::Nop) out.push_back(i);
    if (out.size() != lf.code.size()) changed = true;
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
        case IOp::MovZX: os << "\tmovzbq %al, %rax\n"; break;
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
        case IOp::CmpRImm: os << "\tcmp" << ssz(i.size) << " $" << i.b.imm << ", " << rs(i.a.reg, i.size) << "\n"; break;
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
            os << "\t" << mn << fpsz(i.size) << " %xmm1, %xmm0\n";
            break;
        }
        case IOp::FpCmp: os << "\tucomi" << fpsz(i.size) << " %xmm1, %xmm0\n"; break;
        case IOp::FpNeg:
            if (i.size == 8) os << "\txorpd .Lnegmask64(%rip), %xmm0\n";
            else os << "\txorps .Lnegmask32(%rip), %xmm0\n";
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
