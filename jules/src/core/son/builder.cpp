// GraphBuilder: typed AST -> Sea-of-Nodes.
//
// Key MVP decisions (documented in docs/son_spec.md):
//   * Locals are memory-backed (Alloc + Store/Load) — scalar replacement
//     (pass 26, SROA) is what promotes them to SSA values, Graal-style.
//   * Structured control: If/Region/Phi(mem) per merge; loops build the
//     header Region + memory Phi optimistically and append backedges later.
//   * Short-circuit && / || lower to real control flow (If + Region + Phi),
//     never to eager bitwise ops.
//   * 'for i in a..b' desugars to a memory-backed counter loop with an
//     increment block as the 'continue' target.
#include "core/parser/ast.h"
#include "core/sema/sema.h"
#include "core/son/graph.h"

namespace jules {

namespace {

class FnBuilder {
public:
    FnBuilder(const FnDecl& fn, const SemaModule& sema, FunctionGraph& fg,
              SymbolTable& syms, Diagnostics& diag)
        : g_(fg.g), fn_(fn), sema_(sema), fg_(fg), syms_(syms), diag_(diag) {}

    bool build() {
        cur_ctrl_ = g_.start();
        cur_mem_ = g_.start();
        stop_ = g_.make(Op::Stop, ty_void(), {});

        for (size_t i = 0; i < fn_.params.size(); ++i) {
            NodeId p = g_.make(Op::Param, fn_.params[i].second, {g_.start()}, 0,
                               static_cast<u32>(i));
            params_.insert(fn_.params[i].first, p);
        }
        for (const StmtP& s : fn_.body) emit_stmt(*s);

        // Implicit / fallback returns so Stop is never empty.
        if (fn_.ret == ty_void() && cur_ctrl_ != kDeadCtrl) {
            add_return(g_.make(Op::Return, ty_void(), {cur_ctrl_, cur_mem_}));
        } else if (stop_input_count() == 0) {
            NodeId pin = cur_ctrl_ != kDeadCtrl ? cur_ctrl_ : g_.start();
            NodeId mem = cur_ctrl_ != kDeadCtrl ? cur_mem_ : g_.start();
            NodeId v = fn_.ret == ty_void() ? kNoNode : zero_of(fn_.ret, pin);
            NodeId r = v == kNoNode
                ? g_.make(Op::Return, ty_void(), {pin, mem})
                : g_.make(Op::Return, ty_void(), {pin, mem, v});
            add_return(r);
        }
        g_.set_stop(stop_);
        fg_.param_types.clear();
        for (auto& p : fn_.params) fg_.param_types.push_back(p.second);
        fg_.ret = fn_.ret;
        return ok_;
    }

private:
    static constexpr NodeId kDeadCtrl = kNoNode; // unreachable-control sentinel

    bool alive() const { return cur_ctrl_ != kDeadCtrl; }

    u32 stop_input_count() { return g_.node(stop_).n_in; }
    void add_return(NodeId r) {
        if (g_.node(stop_).n_in >= kMaxInputs - 1) {
            diag_.error(fn_.pos, "function exceeds the MVP return-count control limit");
            ok_ = false;
            return;
        }
        g_.append_input(stop_, r);
    }

    // ---- constants ---------------------------------------------------------
    NodeId make_int_const(u64 v, TypeId ty, NodeId pin) {
        NodeId n = g_.make(Op::Const, ty, {pin});
        g_.node(n).ival = static_cast<i64>(v);
        return n;
    }
    NodeId make_fp_const(f64 v, TypeId ty, NodeId pin) {
        NodeId n = g_.make(Op::Const, ty, {pin});
        g_.node(n).fval = v;
        return n;
    }
    NodeId zero_of(TypeId ty, NodeId pin) {
        return ty_is_float(ty) ? make_fp_const(0.0, ty, pin) : make_int_const(0, ty, pin);
    }

    // ---- variables (memory-backed) -------------------------------------------
    NodeId declare_var(const std::string& name, TypeId ty) {
        NodeId size = make_int_const(ty_store_bytes(ty), ty_i64(), cur_ctrl_);
        // The slot is a pointer to one element of `ty`. The MVP type lattice
        // has no pointer-to-pointer, so a slot for a pointer-typed local
        // degrades to *i64: the address width is identical and the Load
        // result type carries the real pointee information.
        TypeId slot_ty = ty_ptr(ty);
        if (slot_ty == ty_none()) slot_ty = ty_ptr(ty_i64());
        NodeId alloc = g_.make(Op::Alloc, slot_ty, {cur_ctrl_, cur_mem_, size});
        cur_mem_ = alloc;
        if (vars_.contains(name)) vars_.erase(name);
        vars_.insert(name, alloc);
        return alloc;
    }
    NodeId read_var(const std::string& name, TypeId ty) {
        return g_.make(Op::Load, ty, {cur_ctrl_, cur_mem_, *vars_.find(name)});
    }
    void write_var(const std::string& name, NodeId value) {
        NodeId alloc = *vars_.find(name);
        cur_mem_ = g_.make(Op::Store, ty_mem(), {cur_ctrl_, cur_mem_, alloc, value});
    }

    // ---- statements -----------------------------------------------------------
    void emit_stmt(const Stmt& s) {
        if (!alive()) return; // unreachable code: sema already warned
        switch (s.kind) {
            case StmtKind::Let: {
                NodeId v = emit_expr(*s.value);
                declare_var(s.name, s.decl_ty);
                write_var(s.name, v);
                break;
            }
            case StmtKind::Assign: {
                NodeId v = emit_expr(*s.value);
                write_var(s.name, v);
                break;
            }
            case StmtKind::AssignDeref: {
                NodeId addr = emit_expr(*s.target);
                NodeId v = emit_expr(*s.value);
                cur_mem_ = g_.make(Op::Store, ty_mem(), {cur_ctrl_, cur_mem_, addr, v});
                break;
            }
            case StmtKind::Return: {
                NodeId v = s.value ? emit_expr(*s.value) : kNoNode;
                NodeId r = v == kNoNode
                    ? g_.make(Op::Return, ty_void(), {cur_ctrl_, cur_mem_})
                    : g_.make(Op::Return, ty_void(), {cur_ctrl_, cur_mem_, v});
                add_return(r);
                cur_ctrl_ = kDeadCtrl;
                break;
            }
            case StmtKind::If:    emit_if(s);    break;
            case StmtKind::While: emit_while(s); break;
            case StmtKind::For:   emit_for(s);   break;
            case StmtKind::ExprStmt:
                emit_expr(*s.value);
                break;
            case StmtKind::Break: {
                if (loops_.empty()) {
                    diag_.error(s.pos, "'break' outside of a loop");
                    ok_ = false;
                    return;
                }
                LoopCtx& lc = loops_.back();
                g_.append_input(lc.exit_region, cur_ctrl_);
                g_.append_input(lc.exit_memphi, cur_mem_);
                cur_ctrl_ = kDeadCtrl;
                break;
            }
            case StmtKind::Continue: {
                if (loops_.empty()) {
                    diag_.error(s.pos, "'continue' outside of a loop");
                    ok_ = false;
                    return;
                }
                LoopCtx& lc = loops_.back();
                g_.append_input(lc.continue_region, cur_ctrl_);
                g_.append_input(lc.continue_memphi, cur_mem_);
                cur_ctrl_ = kDeadCtrl;
                break;
            }
        }
    }

    // ---- control -----------------------------------------------------------
    struct LoopCtx {
        NodeId header;
        NodeId header_memphi;
        NodeId exit_region;
        NodeId exit_memphi;
        NodeId continue_region;   // header (while) or increment merge (for)
        NodeId continue_memphi;
    };

    void emit_if(const Stmt& s) {
        NodeId cond = emit_expr(*s.cond);
        NodeId ifn = g_.make(Op::If, ty_ctrl(), {cur_ctrl_, cond});
        NodeId tproj = g_.make(Op::IfTrue, ty_ctrl(), {ifn});
        NodeId fproj = g_.make(Op::IfFalse, ty_ctrl(), {ifn});
        NodeId mem_at_if = cur_mem_;

        FlatMap<std::string, NodeId> outer_vars = vars_;

        cur_ctrl_ = tproj;
        cur_mem_ = mem_at_if;
        for (const StmtP& t : s.body) emit_stmt(*t);
        NodeId then_end = cur_ctrl_;
        NodeId then_mem = cur_mem_;

        vars_ = outer_vars;
        cur_ctrl_ = fproj;
        cur_mem_ = mem_at_if;
        for (const StmtP& e : s.else_body) emit_stmt(*e);
        NodeId else_end = cur_ctrl_;
        NodeId else_mem = cur_mem_;
        vars_ = outer_vars;

        if (then_end == kDeadCtrl && else_end == kDeadCtrl) {
            cur_ctrl_ = kDeadCtrl;
            return;
        }
        if (then_end == kDeadCtrl) {
            cur_ctrl_ = else_end;
            cur_mem_ = else_mem;
            return;
        }
        if (else_end == kDeadCtrl) {
            cur_ctrl_ = then_end;
            cur_mem_ = then_mem;
            return;
        }
        NodeId region = g_.make(Op::Region, ty_ctrl(), {then_end, else_end});
        cur_mem_ = g_.make(Op::Phi, ty_mem(), {region, then_mem, else_mem});
        cur_ctrl_ = region;
    }

    void emit_while(const Stmt& s) {
        NodeId entry_ctrl = cur_ctrl_;
        NodeId entry_mem = cur_mem_;

        NodeId header = g_.make(Op::Region, ty_ctrl(), {entry_ctrl});
        NodeId mphi = g_.make(Op::Phi, ty_mem(), {header, entry_mem});

        cur_ctrl_ = header;
        cur_mem_ = mphi;

        NodeId cond = emit_expr(*s.cond);
        NodeId ifn = g_.make(Op::If, ty_ctrl(), {cur_ctrl_, cond});
        NodeId tproj = g_.make(Op::IfTrue, ty_ctrl(), {ifn});
        NodeId fproj = g_.make(Op::IfFalse, ty_ctrl(), {ifn});
        NodeId mem_at_if = cur_mem_;

        NodeId exit_region = g_.make(Op::Region, ty_ctrl(), {fproj});
        NodeId exit_memphi = g_.make(Op::Phi, ty_mem(), {exit_region, mem_at_if});

        FlatMap<std::string, NodeId> outer_vars = vars_;

        LoopCtx lc{header, mphi, exit_region, exit_memphi, header, mphi};
        loops_.push_back(lc);

        cur_ctrl_ = tproj;
        cur_mem_ = mem_at_if;
        for (const StmtP& b : s.body) emit_stmt(*b);

        if (alive()) { // natural backedge
            g_.append_input(header, cur_ctrl_);
            g_.append_input(mphi, cur_mem_);
        }
        loops_.pop_back();
        vars_ = outer_vars;

        cur_ctrl_ = exit_region;
        cur_mem_ = exit_memphi;
    }

    void emit_for(const Stmt& s) {
        // for i in from..to  ==>  i = from; while (i < to) { body; i = i + 1 }
        TypeId ity = s.decl_ty;
        NodeId from = emit_expr(*s.target);
        NodeId to = emit_expr(*s.to);

        declare_var(s.name, ity);
        write_var(s.name, from);

        NodeId entry_ctrl = cur_ctrl_;
        NodeId entry_mem = cur_mem_;

        NodeId header = g_.make(Op::Region, ty_ctrl(), {entry_ctrl});
        NodeId mphi = g_.make(Op::Phi, ty_mem(), {header, entry_mem});

        cur_ctrl_ = header;
        cur_mem_ = mphi;
        NodeId iv = read_var(s.name, ity);
        NodeId cond = g_.make(Op::Cmp, ty_i1(), {cur_ctrl_, iv, to}, static_cast<u8>(CmpOp::Lt));
        NodeId ifn = g_.make(Op::If, ty_ctrl(), {cur_ctrl_, cond});
        NodeId tproj = g_.make(Op::IfTrue, ty_ctrl(), {ifn});
        NodeId fproj = g_.make(Op::IfFalse, ty_ctrl(), {ifn});
        NodeId mem_at_if = cur_mem_;

        NodeId exit_region = g_.make(Op::Region, ty_ctrl(), {fproj});
        NodeId exit_memphi = g_.make(Op::Phi, ty_mem(), {exit_region, mem_at_if});

        // Increment merge: body end + all 'continue's flow here, then the
        // increment block, then the backedge.
        NodeId inc_merge = g_.make(Op::Region, ty_ctrl(), {});   // preds appended
        NodeId inc_mphi = g_.make(Op::Phi, ty_mem(), {inc_merge}); // values appended
        NodeId inc = g_.make(Op::Jump, ty_ctrl(), {inc_merge});

        FlatMap<std::string, NodeId> outer_vars = vars_;

        LoopCtx lc{header, mphi, exit_region, exit_memphi, inc_merge, inc_mphi};
        loops_.push_back(lc);

        cur_ctrl_ = tproj;
        cur_mem_ = mem_at_if;
        for (const StmtP& b : s.body) emit_stmt(*b);
        if (alive()) {
            g_.append_input(inc_merge, cur_ctrl_);
            g_.append_input(inc_mphi, cur_mem_);
        }
        loops_.pop_back();
        vars_ = outer_vars;

        // Increment block: i = i + 1, then backedge to the header.
        cur_ctrl_ = inc;
        cur_mem_ = inc_mphi;
        if (g_.node(inc_merge).n_in > 0) {
            NodeId one = make_int_const(1, ity, cur_ctrl_);
            NodeId cur = read_var(s.name, ity);
            NodeId bumped = g_.make(Op::Bin, ity, {cur_ctrl_, cur, one}, static_cast<u8>(BinOp::Add));
            write_var(s.name, bumped);
            g_.append_input(header, cur_ctrl_);
            g_.append_input(mphi, cur_mem_);
        }

        cur_ctrl_ = exit_region;
        cur_mem_ = exit_memphi;
    }

    // ---- expressions -----------------------------------------------------------
    NodeId emit_expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:
                return make_int_const(e.iv, e.ty, cur_ctrl_);
            case ExprKind::FloatLit:
                return make_fp_const(e.fv, e.ty, cur_ctrl_);
            case ExprKind::BoolLit:
                return make_int_const(e.iv, ty_i1(), cur_ctrl_);
            case ExprKind::Ident: {
                if (e.comptime_value) {
                    if (ty_is_float(e.ty)) return make_fp_const(e.fv, e.ty, cur_ctrl_);
                    return make_int_const(e.iv, e.ty, cur_ctrl_);
                }
                if (const NodeId* p = params_.find(e.name)) {
                    return *p; // parameters are direct SSA values
                }
                if (const NodeId* a = vars_.find(e.name)) {
                    return g_.make(Op::Load, e.ty, {cur_ctrl_, cur_mem_, *a});
                }
                diag_.error(e.pos, "internal: unresolved local '" + e.name + "' in graph builder");
                ok_ = false;
                return zero_of(e.ty, cur_ctrl_);
            }
            case ExprKind::Unary: {
                NodeId x = emit_expr(*e.lhs);
                return g_.make(Op::Un, e.ty, {cur_ctrl_, x}, static_cast<u8>(e.uop));
            }
            case ExprKind::Binary:
                return emit_binary(e);
            case ExprKind::Cast: {
                NodeId x = emit_expr(*e.lhs);
                CastOp k = pick_cast(e.lhs->ty, e.cast_target);
                return g_.make(Op::Cast, e.cast_target, {cur_ctrl_, x}, static_cast<u8>(k));
            }
            case ExprKind::Deref: {
                NodeId addr = emit_expr(*e.lhs);
                return g_.make(Op::Load, e.ty, {cur_ctrl_, cur_mem_, addr});
            }
            case ExprKind::Call:
                return emit_call(e);
            case ExprKind::ComptimeBlock:
                diag_.error(e.pos, "internal: comptime block reached the builder unfolded");
                ok_ = false;
                return zero_of(ty_i32(), cur_ctrl_);
        }
        return zero_of(ty_i32(), cur_ctrl_);
    }

    static CastOp pick_cast(TypeId from, TypeId to) {
        if (ty_is_ptr(from) && ty_is_ptr(to)) return CastOp::Ptr;
        if (ty_is_float(from) && ty_is_float(to))
            return ty_bits(to) > ty_bits(from) ? CastOp::FpExt : CastOp::FpTrunc;
        if (ty_is_float(from) && ty_is_int(to)) return CastOp::FpToSi;
        if (ty_is_int(from) && ty_is_float(to)) return CastOp::SiToFp;
        if (ty_bits(from) < ty_bits(to)) return ty_is_signed(from) ? CastOp::SExt : CastOp::ZExt;
        return CastOp::Trunc;
    }

    NodeId emit_binary(const Expr& e) {
        if (e.bop == BinKind::LogicAnd || e.bop == BinKind::LogicOr) {
            bool is_and = e.bop == BinKind::LogicAnd;
            NodeId l = emit_expr(*e.lhs);
            NodeId ifn = g_.make(Op::If, ty_ctrl(), {cur_ctrl_, l});
            NodeId tproj = g_.make(Op::IfTrue, ty_ctrl(), {ifn});
            NodeId fproj = g_.make(Op::IfFalse, ty_ctrl(), {ifn});
            NodeId mem0 = cur_mem_;

            NodeId rhs_pin = is_and ? tproj : fproj;
            NodeId const_pin = is_and ? fproj : tproj;
            cur_ctrl_ = rhs_pin;
            cur_mem_ = mem0;
            NodeId r = emit_expr(*e.rhs);
            NodeId r_end = cur_ctrl_;
            NodeId r_mem = cur_mem_;

            cur_ctrl_ = const_pin;
            cur_mem_ = mem0;
            NodeId c = make_int_const(is_and ? 0 : 1, ty_i1(), const_pin);

            NodeId region = g_.make(Op::Region, ty_ctrl(), {r_end, const_pin});
            NodeId memphi = g_.make(Op::Phi, ty_mem(), {region, r_mem, mem0});
            NodeId vphi = g_.make(Op::Phi, ty_i1(), {region, r, c});
            cur_ctrl_ = region;
            cur_mem_ = memphi;
            return vphi;
        }

        if (e.bop == BinKind::Eq || e.bop == BinKind::Ne || e.bop == BinKind::Lt ||
            e.bop == BinKind::Le || e.bop == BinKind::Gt || e.bop == BinKind::Ge) {
            NodeId a = emit_expr(*e.lhs);
            NodeId b = emit_expr(*e.rhs);
            CmpOp k;
            switch (e.bop) {
                case BinKind::Eq: k = CmpOp::Eq; break;
                case BinKind::Ne: k = CmpOp::Ne; break;
                case BinKind::Lt: k = CmpOp::Lt; break;
                case BinKind::Le: k = CmpOp::Le; break;
                case BinKind::Gt: k = CmpOp::Gt; break;
                default:   k = CmpOp::Ge; break;
            }
            return g_.make(Op::Cmp, ty_i1(), {cur_ctrl_, a, b}, static_cast<u8>(k));
        }

        NodeId a = emit_expr(*e.lhs);
        NodeId b = emit_expr(*e.rhs);
        return g_.make(Op::Bin, e.ty, {cur_ctrl_, a, b}, static_cast<u8>(e.bop));
    }

    NodeId emit_call(const Expr& e) {
        if (e.name == "alloc") {
            TypeId pointee = e.args[0]->cast_target;
            NodeId size = make_int_const(ty_store_bytes(pointee), ty_i64(), cur_ctrl_);
            TypeId res_ty = ty_ptr(pointee);
            if (res_ty == ty_none()) res_ty = ty_ptr(ty_i64()); // no ptr-to-ptr in MVP lattice
            cur_mem_ = g_.make(Op::Alloc, res_ty, {cur_ctrl_, cur_mem_, size});
            return cur_mem_;
        }
        FnId target = kNoFn;
        if (e.name == "print") target = kFnPrint;
        else if (e.name == "free") target = kFnFree;
        else {
            SymbolId s = syms_.find(e.name);
            const size_t* p = s == kNoSymbol ? nullptr : sema_.fn_by_name.find(s);
            if (!p) {
                diag_.error(e.pos, "internal: unresolved function '" + e.name + "'");
                ok_ = false;
                return zero_of(e.ty, cur_ctrl_);
            }
            target = static_cast<FnId>(*p);
        }

        NodeId ins[kMaxInputs];
        ins[0] = cur_ctrl_;
        u8 n = 2;
        for (const ExprP& a : e.args) {
            if (n >= kMaxInputs) {
                diag_.error(e.pos, "call argument count exceeds the MVP node arity limit");
                ok_ = false;
                break;
            }
            ins[n++] = emit_expr(*a);
        }
        // Memory input is captured AFTER argument evaluation: nested calls in
        // the argument list advance cur_mem_, and this Call must observe their
        // effects (eval order = argument effects, then the call itself).
        // Capturing before the loop orphaned the argument calls from the
        // memory chain and broke effect ordering in the linearizer.
        ins[1] = cur_mem_;
        cur_mem_ = g_.make_arr(Op::Call, e.ty, ins, n, 0, target);
        return cur_mem_;
    }

    Graph& g_;
    const FnDecl& fn_;
    const SemaModule& sema_;
    FunctionGraph& fg_;
    SymbolTable& syms_;
    Diagnostics& diag_;
    NodeId cur_ctrl_ = kNoNode;
    NodeId cur_mem_ = kNoNode;
    NodeId stop_ = kNoNode;
    FlatMap<std::string, NodeId> vars_;
    FlatMap<std::string, NodeId> params_;
    std::vector<LoopCtx> loops_;
    bool ok_ = true;
};

} // namespace

bool build_function(const FnDecl& fn, const SemaModule& sema, FunctionGraph& fg,
                    SymbolTable& syms, Diagnostics& diag) {
    FnBuilder b(fn, sema, fg, syms, diag);
    return b.build();
}

} // namespace jules
