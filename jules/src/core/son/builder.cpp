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
//
// User-type lowering (docs/language_surface.md):
//   * Struct locals decompose into per-field slots (one Alloc per leaf field)
//     — SROA-by-construction — unless the sema escape analysis forced one
//     contiguous allocation (address taken / passed by value / method
//     receiver). Contiguous structs, struct params (address passed + entry
//     copy) and struct returns (hidden sret pointer, LAST parameter) keep
//     the IR scalar-only: user TypeIds NEVER appear on IR nodes.
//   * Enum/bitmask/bitfield values are their backing integers in the IR;
//     bitfield field reads/writes lower to shift/mask arithmetic.
//   * defer duplicates its (checked control-flow-free) body at every scope
//     exit: fall-through, break, continue, return — sound because JULES has
//     no exceptions/unwinding.
//   * extern fns call through CallSym pseudo-FnIds (resolved by the emitter
//     through Module::externs).
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

        // Hidden sret parameter (LAST) for struct-returning functions.
        const bool ret_is_struct = sema_.is_struct_type(fn_.ret);
        if (ret_is_struct) {
            sret_param_ = g_.make(Op::Param, ty_ptr(ty_i64()), {g_.start()},
                                  0, static_cast<u32>(fn_.params.size()));
        }

        for (size_t i = 0; i < fn_.params.size(); ++i) {
            const auto& [pname, pty] = fn_.params[i];
            TypeId ir = ir_type_of(pty);
            NodeId p = g_.make(Op::Param, ir, {g_.start()}, 0, static_cast<u32>(i));
            if (sema_.is_struct_type(pty) && fn_.self_kind == kSelfNone) {
                // by-value struct param: copy into an owned local, then the
                // local is the binding (mutation stays local — value semantics)
                SRef dst = declare_struct_mem(pname, pty);
                struct_copy_from_addr(dst, p);
            } else if (fn_.self_kind != kSelfNone && pname == "self") {
                vars_.insert(pname, p); // borrowed receiver: the address itself
            } else {
                params_.insert(pname, p);
            }
        }
        for (const StmtP& s : fn_.body) emit_stmt(*s);

        // Fall-through defers (scope depth 0 = function scope), then the
        // implicit / fallback returns so Stop is never empty.
        if (alive()) emit_defers_from(0);
        if (fn_.ret == ty_void() && cur_ctrl_ != kDeadCtrl) {
            add_return(g_.make(Op::Return, ty_void(), {cur_ctrl_, cur_mem_}));
        } else if (stop_input_count() == 0) {
            NodeId pin = cur_ctrl_ != kDeadCtrl ? cur_ctrl_ : g_.start();
            NodeId mem = cur_ctrl_ != kDeadCtrl ? cur_mem_ : g_.start();
            NodeId v = fn_.ret == ty_void() ? kNoNode : zero_of(ir_type_of(fn_.ret), pin);
            NodeId r = v == kNoNode
                ? g_.make(Op::Return, ty_void(), {pin, mem})
                : g_.make(Op::Return, ty_void(), {pin, mem, v});
            add_return(r);
        }
        g_.set_stop(stop_);
        fg_.param_types.clear();
        for (auto& p : fn_.params) fg_.param_types.push_back(ir_type_of(p.second));
        if (ret_is_struct) fg_.param_types.push_back(ty_ptr(ty_i64()));
        fg_.ret = ret_is_struct ? ty_void() : ir_type_of(fn_.ret);
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

    // ---- user-type helpers --------------------------------------------------
    TypeId ir_type_of(TypeId t) const { return sema_.ir_ty(t); }

    const UserType& ut(TypeId t) const { return sema_.user_types.get(t); }
    TypeId base_struct(TypeId t) const { return sema_.user_types.struct_of(t); }
    u32 struct_bytes(TypeId t) const {
        // t may be the struct OR a pointer-to-struct (array element type)
        TypeId b = sema_.is_struct_ptr(t) ? sema_.user_types.pointee_struct(t)
                                          : base_struct(t);
        return b == ty_none() ? 8 : ut(b).size;
    }
    const StructField* sfield(TypeId struct_ty, const std::string& name) {
        // auto-deref pointer-to-struct bases (self.x on *Point)
        TypeId b = sema_.is_struct_ptr(struct_ty)
            ? sema_.user_types.pointee_struct(struct_ty)
            : base_struct(struct_ty);
        if (b == ty_none()) return nullptr;
        SymbolId sym = syms_.find(name);
        if (sym == kNoSymbol) return nullptr;
        for (const StructField& f : ut(b).fields)
            if (f.name == sym) return &f;
        return nullptr;
    }
    const BitSeg* bitseg(TypeId bf_ty, const std::string& name) {
        const UserType* u = &ut(bf_ty);
        int depth = 0;
        while (u->kind == UserKind::Alias && depth++ < 16)
            u = &sema_.user_types.get(u->target);
        if (u->kind != UserKind::Bitfield) return nullptr;
        SymbolId sym = syms_.find(name);
        if (sym == kNoSymbol) return nullptr;
        for (const BitSeg& s : u->segs)
            if (s.name == sym) return &s;
        return nullptr;
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

    // i64 <-> pointer bit-exact conversions (the existing convention)
    NodeId to_i64(NodeId p) {
        if (g_.node(p).ty == ty_i64()) return p;
        return g_.make(Op::Cast, ty_i64(), {cur_ctrl_, p}, static_cast<u8>(CastOp::Ptr));
    }
    NodeId to_ptr(NodeId a, TypeId pointee) {
        TypeId p = ty_ptr(pointee);
        if (p == ty_none()) p = ty_ptr(ty_i64());
        return g_.make(Op::Cast, p, {cur_ctrl_, a}, static_cast<u8>(CastOp::Ptr));
    }
    NodeId add_const_i64(NodeId a, u64 off) {
        if (off == 0) return a;
        NodeId k = make_int_const(off, ty_i64(), cur_ctrl_);
        return g_.make(Op::Bin, ty_i64(), {cur_ctrl_, a, k}, static_cast<u8>(BinOp::Add));
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
        erase_name_slots(name);
        vars_.insert(name, alloc);
        return alloc;
    }
    // A (re)declaration must hide every decomposed field slot of the name.
    void erase_name_slots(const std::string& name) {
        const std::string prefix = name + ".";
        std::vector<std::string> kill;
        for (const auto& e : vars_.entries())
            if (e.first == name || e.first.rfind(prefix, 0) == 0) kill.push_back(e.first);
        for (const std::string& k : kill) vars_.erase(k);
    }
    NodeId read_var(const std::string& name, TypeId ty) {
        return g_.make(Op::Load, ty, {cur_ctrl_, cur_mem_, *vars_.find(name)});
    }
    void write_var(const std::string& name, NodeId value) {
        NodeId* alloc = vars_.find(name);
        if (!alloc) { // first assignment to a param: give it a slot now
            TypeId t = g_.node(value).ty;
            NodeId a = declare_var(name, t);
            (void)a;
            alloc = vars_.find(name);
        }
        cur_mem_ = g_.make(Op::Store, ty_mem(), {cur_ctrl_, cur_mem_, *alloc, value});
    }

    // ---- struct storage ------------------------------------------------------
    // A reference to a struct value: decomposed slot path OR an address.
    struct SRef {
        bool is_addr = false;
        std::string path;      // decomposed slot prefix ("p", "p.inner")
        NodeId addr = kNoNode; // contiguous address (ptr-typed node)
        TypeId sty = ty_none();
    };

    // Decomposed: one Alloc per LEAF field, keyed "name.path.to.field".
    void declare_struct_slots(const std::string& prefix, TypeId sty) {
        TypeId b = base_struct(sty);
        for (const StructField& f : ut(b).fields) {
            std::string fname(syms_.name(f.name));
            if (sema_.is_struct_type(f.ty)) {
                declare_struct_slots(prefix + "." + fname, f.ty);
            } else {
                declare_var(prefix + "." + fname, ir_type_of(f.ty));
            }
        }
    }
    // Contiguous: one Alloc of the struct size; the binding IS the address.
    SRef declare_struct_mem(const std::string& name, TypeId sty) {
        TypeId b = base_struct(sty);
        NodeId size = make_int_const(ut(b).size, ty_i64(), cur_ctrl_);
        NodeId alloc = g_.make(Op::Alloc, ty_ptr(ty_i64()), {cur_ctrl_, cur_mem_, size});
        cur_mem_ = alloc;
        erase_name_slots(name);
        vars_.insert(name, alloc);
        SRef r;
        r.is_addr = true;
        r.addr = alloc;
        r.sty = b;
        return r;
    }

    // Reference to the struct VALUE of an expression. For decomposed bases
    // this is only valid in copy contexts (field-wise access); general value
    // positions go through materialize_struct().
    SRef struct_ref(Expr& e) {
        SRef r;
        r.sty = base_struct(e.ty);
        switch (e.kind) {
            case ExprKind::Ident: {
                if (const NodeId* a = vars_.find(e.name)) {
                    if (sema_.is_struct_type(e.ty)) {
                        // memory-resident struct binding: the alloc IS the
                        // struct's address
                        r.is_addr = true;
                        r.addr = *a;
                    } else if (g_.node(*a).op == Op::Alloc) {
                        // scalar slot holding a pointer-to-struct value
                        // (e.g. `let pp = &p;`): the LOAD is the address
                        r.is_addr = true;
                        r.addr = g_.make(Op::Load, ty_ptr(ty_i64()),
                                         {cur_ctrl_, cur_mem_, *a});
                    } else {
                        // &self: the binding IS the receiver pointer value
                        r.is_addr = true;
                        r.addr = *a;
                    }
                } else if (const NodeId* p = params_.find(e.name)) {
                    // pointer-typed param: the SSA value IS the address
                    r.is_addr = true;
                    r.addr = *p;
                } else {
                    r.is_addr = false;
                    r.path = e.name; // decomposed binding (copy contexts only)
                }
                return r;
            }
            case ExprKind::Field: {
                // nested field: extend the parent ref (sfield auto-derefs
                // pointer-to-struct bases)
                SRef p = struct_ref(*e.lhs);
                const StructField* f = sfield(e.lhs->ty, e.name);
                if (!f) break;
                if (p.is_addr) {
                    NodeId faddr = add_const_i64(to_i64(p.addr), f->offset);
                    if (sema_.is_struct_ptr(f->ty)) {
                        // POINTER field as a base: the loaded pointer value
                        // IS the address of the next link (p.next.x chains)
                        r.is_addr = true;
                        r.addr = g_.make(Op::Load, ty_ptr(ty_i64()),
                                         {cur_ctrl_, cur_mem_, to_ptr(faddr, ty_i64())});
                    } else {
                        // VALUE (nested struct) field: offset continuation
                        r.is_addr = true;
                        r.addr = faddr;
                    }
                } else if (sema_.is_struct_ptr(f->ty)) {
                    // decomposed parent: the pointer sits in its slot
                    const NodeId* slot = vars_.find(p.path + "." + e.name);
                    if (!slot) break;
                    r.is_addr = true;
                    r.addr = g_.make(Op::Load, ty_ptr(ty_i64()),
                                     {cur_ctrl_, cur_mem_, *slot});
                } else {
                    r.is_addr = false;
                    r.path = p.path + "." + e.name;
                }
                return r;
            }
            case ExprKind::Deref: {
                r.is_addr = true;
                r.addr = emit_expr(*e.lhs); // pointer-to-struct value
                return r;
            }
            case ExprKind::Index: {
                r.is_addr = true;
                r.addr = struct_elem_addr(*e.lhs, *e.rhs);
                return r;
            }
            case ExprKind::AddrOf: {
                SRef p = struct_ref(*e.lhs);
                if (!p.is_addr) break; // sema forced & sources to memory
                r.is_addr = true;
                r.addr = p.addr;
                return r;
            }
            default:
                break;
        }
        // General rvalue (literal / call / method result): emit_expr IS the
        // address under the struct contract (temps for literals, sret temps
        // for struct-returning calls, element addresses for Index).
        r.is_addr = true;
        r.addr = emit_expr(e);
        return r;
    }

    // Address of the element `idx` of a struct array at `arr` (i64 address).
    NodeId struct_elem_addr(Expr& arr, Expr& idx) {
        NodeId base = emit_expr(arr); // pointer-to-struct (ptr-typed node)
        NodeId i = emit_expr(idx);
        TypeId ity = ir_type_of(idx.ty);
        if (ity != ty_i64()) {
            CastOp k = ty_is_signed(ity) ? CastOp::SExt : CastOp::Ptr;
            i = g_.make(Op::Cast, ty_i64(), {cur_ctrl_, i}, static_cast<u8>(k));
        }
        u32 esz = struct_bytes(arr.ty);
        NodeId base64 = to_i64(base);
        NodeId off = i;
        if (esz != 1) {
            // scale by element size (Shl for pow2, Mul otherwise)
            if ((esz & (esz - 1)) == 0) {
                u8 sh = 0;
                while ((u64(1) << sh) < esz) ++sh;
                NodeId k = make_int_const(sh, ty_i64(), cur_ctrl_);
                off = g_.make(Op::Bin, ty_i64(), {cur_ctrl_, i, k},
                              static_cast<u8>(BinOp::Shl));
            } else {
                NodeId k = make_int_const(esz, ty_i64(), cur_ctrl_);
                off = g_.make(Op::Bin, ty_i64(), {cur_ctrl_, i, k},
                              static_cast<u8>(BinOp::Mul));
            }
        }
        return g_.make(Op::Bin, ty_i64(), {cur_ctrl_, base64, off},
                       static_cast<u8>(BinOp::Add));
    }

    // Field access on a struct ref: scalar leaf load.
    NodeId struct_field_load(const SRef& ref, const StructField& f) {
        std::string fname(syms_.name(f.name));
        if (sema_.is_struct_type(f.ty)) {
            diag_.error(cur_pos_, "internal: whole-struct field read in scalar context");
            ok_ = false;
            return zero_of(ty_i64(), cur_ctrl_);
        }
        TypeId fty = ir_type_of(f.ty);
        if (!ref.is_addr) {
            const NodeId* slot = vars_.find(ref.path + "." + fname);
            if (!slot) {
                diag_.error(cur_pos_, "internal: unresolved struct slot '" +
                            ref.path + "." + fname + "'");
                ok_ = false;
                return zero_of(fty, cur_ctrl_);
            }
            return g_.make(Op::Load, fty, {cur_ctrl_, cur_mem_, *slot});
        }
        NodeId addr = add_const_i64(to_i64(ref.addr), f.offset);
        return g_.make(Op::Load, fty, {cur_ctrl_, cur_mem_, to_ptr(addr, fty)});
    }

    void struct_field_store(const SRef& ref, const StructField& f, NodeId v) {
        std::string fname(syms_.name(f.name));
        TypeId fty = ir_type_of(f.ty);
        if (!ref.is_addr) {
            const NodeId* slot = vars_.find(ref.path + "." + fname);
            if (!slot) {
                diag_.error(cur_pos_, "internal: unresolved struct slot '" +
                            ref.path + "." + fname + "'");
                ok_ = false;
                return;
            }
            cur_mem_ = g_.make(Op::Store, ty_mem(), {cur_ctrl_, cur_mem_, *slot, v});
            return;
        }
        NodeId addr = add_const_i64(to_i64(ref.addr), f.offset);
        cur_mem_ = g_.make(Op::Store, ty_mem(), {cur_ctrl_, cur_mem_, to_ptr(addr, fty), v});
    }

    // Sub-reference to a nested struct field (for copies).
    SRef struct_field_ref(const SRef& ref, const StructField& f) {
        SRef r;
        std::string fname(syms_.name(f.name));
        if (ref.is_addr) {
            r.is_addr = true;
            r.addr = to_ptr(add_const_i64(to_i64(ref.addr), f.offset), ty_i64());
        } else {
            r.is_addr = false;
            r.path = ref.path + "." + fname;
        }
        r.sty = base_struct(f.ty);
        return r;
    }

    // Copy a struct value from an EXPRESSION into a destination ref.
    // Literals store their field values directly (no temp materialized).
    void copy_struct_expr(const SRef& dst, Expr& src) {
        TypeId b = dst.sty;
        if (src.kind == ExprKind::StructLit && !sema_.is_struct_ptr(src.ty)) {
            // literal: map field name -> arg index
            for (const StructField& f : ut(b).fields) {
                std::string fname(syms_.name(f.name));
                for (size_t i = 0; i < src.field_names.size(); ++i) {
                    if (src.field_names[i] != fname) continue;
                    if (sema_.is_struct_type(f.ty)) {
                        copy_struct_expr(struct_field_ref(dst, f), *src.args[i]);
                    } else {
                        NodeId v = emit_expr(*src.args[i]);
                        struct_field_store(dst, f, v);
                    }
                }
            }
            return;
        }
        if (sema_.is_struct_ptr(src.ty)) {
            // *pp source: address is the pointer value
            SRef s;
            s.is_addr = true;
            s.addr = emit_expr(src);
            s.sty = b;
            copy_struct_ref(dst, s);
            return;
        }
        SRef s = struct_ref(src);
        copy_struct_ref(dst, s);
    }

    void copy_struct_ref(const SRef& dst, const SRef& src) {
        for (const StructField& f : ut(dst.sty).fields) {
            if (sema_.is_struct_type(f.ty)) {
                copy_struct_ref(struct_field_ref(dst, f), struct_field_ref(src, f));
            } else {
                NodeId v = struct_field_load(src, f);
                struct_field_store(dst, f, v);
            }
        }
    }

    // Copy a struct from a raw address (param / sret source) into a ref.
    void struct_copy_from_addr(const SRef& dst, NodeId src_addr) {
        SRef s;
        s.is_addr = true;
        s.addr = src_addr;
        s.sty = dst.sty;
        copy_struct_ref(dst, s);
    }

    // Materialize a struct VALUE in memory; returns its address (ptr node).
    // Literals get a fresh temp; every other struct-typed expression already
    // evaluates to an address under the struct contract (emit_expr).
    NodeId materialize_struct(Expr& e) {
        if (e.comptime_value) // folded bitfield literal: the u64 value itself
            return make_int_const(e.iv, ty_i64(), cur_ctrl_);
        if (e.kind == ExprKind::StructLit) {
            SRef tmp = declare_struct_mem("__lit", e.ty);
            copy_struct_expr(tmp, e);
            return tmp.addr;
        }
        if (e.kind == ExprKind::Field || e.kind == ExprKind::Deref) {
            // by-value sources read from a nested field or *p: copy into a
            // temp (field-wise), then pass the temp's address
            SRef src = struct_ref(e);
            SRef tmp = declare_struct_mem("__arg", e.ty);
            if (src.is_addr) copy_struct_ref(tmp, src);
            else copy_struct_ref(tmp, src); // path sources copy slot-wise
            return tmp.addr;
        }
        return emit_expr(e);
    }

    // ---- defer machinery ------------------------------------------------------
    // defers_ is registration-ordered; each entry (depth, body). A scope at
    // depth D runs, in LIFO order, every defer registered with depth >= D
    // that is still pending, when it exits.
    std::vector<std::pair<u32, const std::vector<StmtP>*>> defers_;
    u32 depth_ = 0;

    void emit_defers_from(u32 min_depth_inclusive) {
        // snapshot: emitting a body may REGISTER nested defers (defers_
        // reallocates); the snapshot keeps the iterators valid
        std::vector<std::pair<u32, const std::vector<StmtP>*>> run;
        for (auto it = defers_.rbegin(); it != defers_.rend(); ++it) {
            if (it->first < min_depth_inclusive) break;
            run.push_back(*it);
        }
        for (auto& e : run) {
            if (!alive()) return;
            for (const StmtP& s : *e.second) emit_stmt(*s);
        }
    }
    void close_defers_at(u32 d) {
        // emit pending defers at depth >= d, then drop them
        emit_defers_from(d);
        std::vector<std::pair<u32, const std::vector<StmtP>*>> keep;
        for (auto& e : defers_)
            if (e.first < d) keep.push_back(e);
        defers_.swap(keep);
    }

    // ---- statements -----------------------------------------------------------
    void emit_stmt(const Stmt& s) {
        if (!alive()) return; // unreachable code: sema already warned
        cur_pos_ = s.pos;
        switch (s.kind) {
            case StmtKind::Let: {
                if (sema_.is_struct_type(s.decl_ty)) {
                    SRef dst = s.struct_in_mem
                        ? declare_struct_mem(s.name, s.decl_ty)
                        : SRef{false, s.name, kNoNode, base_struct(s.decl_ty)};
                    if (!s.struct_in_mem) declare_struct_slots(s.name, s.decl_ty);
                    copy_struct_expr(dst, *s.value);
                    break;
                }
                NodeId v = emit_expr(*s.value);
                declare_var(s.name, ir_type_of(s.decl_ty));
                write_var(s.name, v);
                break;
            }
            case StmtKind::Assign: {
                // whole-struct assignment: field-wise copy into the dst
                // binding, whatever its representation
                if (sema_.is_struct_type(s.decl_ty)) {
                    const NodeId* a = vars_.find(s.name);
                    SRef dst;
                    dst.sty = base_struct(s.decl_ty);
                    if (a) {
                        // contiguous (escape-forced / param / self): the
                        // binding IS the struct's address
                        dst.is_addr = true;
                        dst.addr = *a;
                    } else {
                        // decomposed: slots named "<name>.<field path>"
                        dst.is_addr = false;
                        dst.path = s.name;
                    }
                    copy_struct_expr(dst, *s.value);
                    break;
                }
                NodeId v = emit_expr(*s.value);
                write_var(s.name, v);
                break;
            }
            case StmtKind::AssignDeref: {
                if (sema_.is_struct_ptr(s.target->ty)) {
                    // *pp = struct value: field stores through the pointer
                    NodeId addr = emit_expr(*s.target);
                    SRef dst;
                    dst.is_addr = true;
                    dst.addr = addr;
                    dst.sty = base_struct(sema_.user_types.pointee_struct(s.target->ty));
                    copy_struct_expr(dst, *s.value);
                    break;
                }
                NodeId addr = emit_expr(*s.target);
                NodeId v = emit_expr(*s.value);
                cur_mem_ = g_.make(Op::Store, ty_mem(), {cur_ctrl_, cur_mem_, addr, v});
                break;
            }
            case StmtKind::AssignIndex: {
                if (sema_.is_struct_ptr(s.target->ty)) {
                    // arr[i] = struct value
                    NodeId addr = to_ptr(struct_elem_addr(*s.target, *s.to), ty_i64());
                    SRef dst;
                    dst.is_addr = true;
                    dst.addr = addr;
                    dst.sty = base_struct(sema_.user_types.pointee_struct(s.target->ty));
                    copy_struct_expr(dst, *s.value);
                    break;
                }
                NodeId base = emit_expr(*s.target);
                NodeId idx = emit_expr(*s.to);
                NodeId v = emit_expr(*s.value);
                NodeId addr = index_address(base, idx, s.to->ty, s.target->ty);
                cur_mem_ = g_.make(Op::Store, ty_mem(), {cur_ctrl_, cur_mem_, addr, v});
                break;
            }
            case StmtKind::AssignField: {
                // Bitfield leaf inside a struct FIELD: `r.f.seg = v` — the
                // target (r.f) names the bitfield value, s.name the segment.
                // Route to bitfield_store with the OUTER struct ref.
                if (bitseg_able(s.target->ty) && s.target->kind == ExprKind::Field &&
                    s.target->lhs) {
                    Expr& outer = *s.target->lhs;
                    const StructField* of = sfield(outer.ty, s.target->name);
                    if (of) {
                        SRef oref = struct_ref(outer);
                        bitfield_store(oref, *of, s.name, *s.value);
                        break;
                    }
                }
                // Direct bitfield VARIABLE (target is a name): f.seg = v
                // reads the backing u64, read-modify-writes it, stores it
                // back.
                if (bitseg_able(s.target->ty) && s.target->kind == ExprKind::Ident) {
                    const BitSeg* seg = bitseg(s.target->ty, s.name);
                    if (!seg) {
                        diag_.error(s.pos, "internal: unknown bitfield segment '" +
                                    s.name + "'");
                        ok_ = false;
                        break;
                    }
                    NodeId backing = emit_expr(*s.target);
                    NodeId v = emit_expr(*s.value);
                    TypeId vt = g_.node(v).ty;
                    if (vt != ty_u64()) {
                        CastOp k = ty_is_signed(vt) ? CastOp::ZExt : CastOp::Ptr;
                        v = g_.make(Op::Cast, ty_u64(), {cur_ctrl_, v}, static_cast<u8>(k));
                    }
                    u64 mask = (seg->width >= 64) ? ~u64(0) : ((u64(1) << seg->width) - 1);
                    NodeId vm = v;
                    if (mask != ~u64(0)) {
                        NodeId mk = make_int_const(mask, ty_u64(), cur_ctrl_);
                        vm = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, v, mk},
                                     static_cast<u8>(BinOp::And));
                    }
                    NodeId shifted = vm;
                    if (seg->shift > 0) {
                        NodeId sh = make_int_const(seg->shift, ty_u64(), cur_ctrl_);
                        shifted = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, vm, sh},
                                          static_cast<u8>(BinOp::Shl));
                    }
                    NodeId cleared = backing;
                    if (seg->shift == 0 && mask == ~u64(0)) {
                        cleared = make_int_const(0, ty_u64(), cur_ctrl_);
                    } else {
                        NodeId cl = make_int_const(~(mask << seg->shift), ty_u64(), cur_ctrl_);
                        cleared = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, backing, cl},
                                          static_cast<u8>(BinOp::And));
                    }
                    NodeId result = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, cleared, shifted},
                                            static_cast<u8>(BinOp::Or));
                    if (s.target->kind == ExprKind::Ident) {
                        write_var(s.target->name, result);
                    } else {
                        diag_.error(s.pos, "internal: bitfield store target is not a name");
                        ok_ = false;
                    }
                    break;
                }
                // struct path; s.target is the struct expr, s.name the leaf
                SRef ref = struct_ref(*s.target);
                const StructField* f = sfield(s.target->ty, s.name);
                if (!f) {
                    diag_.error(s.pos, "internal: unknown field '" + s.name + "'");
                    ok_ = false;
                    break;
                }
                // bitfield-typed leaf: read-modify-write the backing integer
                if (ty_is_user(f->ty) && bitseg_able(f->ty)) {
                    bitfield_store(ref, *f, s.name, *s.value);
                    break;
                }
                NodeId v = emit_expr(*s.value);
                struct_field_store(ref, *f, v);
                break;
            }
            case StmtKind::Return: {
                if (s.value && sema_.is_struct_type(s.value->ty)) {
                    if (sret_param_ == kNoNode) {
                        diag_.error(s.pos, "internal: struct return without sret param");
                        ok_ = false;
                        break;
                    }
                    // the return VALUE is evaluated first (Zig rule), THEN
                    // the defers run — the caller observes the pre-defer value
                    SRef dst;
                    dst.is_addr = true;
                    dst.addr = sret_param_;
                    dst.sty = base_struct(s.value->ty);
                    copy_struct_expr(dst, *s.value);
                    emit_defers_from(0); // all scopes exit
                    add_return(g_.make(Op::Return, ty_void(), {cur_ctrl_, cur_mem_}));
                    cur_ctrl_ = kDeadCtrl;
                    break;
                }
                NodeId v = s.value ? emit_expr(*s.value) : kNoNode;
                emit_defers_from(0);
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
                emit_defers_from(lc.body_depth); // innermost..loop body inclusive
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
                emit_defers_from(lc.body_depth); // the iteration scope exits too
                g_.append_input(lc.continue_region, cur_ctrl_);
                g_.append_input(lc.continue_memphi, cur_mem_);
                cur_ctrl_ = kDeadCtrl;
                break;
            }
            case StmtKind::Defer:
                // registered, not emitted: runs at scope exit however it exits
                defers_.emplace_back(depth_, &s.body);
                break;
            case StmtKind::Block: {
                FlatMap<std::string, NodeId> outer_vars = vars_;
                ++depth_;
                for (const StmtP& b : s.body) emit_stmt(*b);
                close_defers_at(depth_);
                --depth_;
                vars_ = outer_vars;
                break;
            }
        }
    }

    bool bitseg_able(TypeId t) {
        const UserType* u = &ut(t);
        int depth = 0;
        while (u->kind == UserKind::Alias && depth++ < 16)
            u = &sema_.user_types.get(u->target);
        return u->kind == UserKind::Bitfield;
    }

    // bitfield leaf store: backing = (backing & ~(mask << shift)) | ((v & mask) << shift)
    void bitfield_store(const SRef& ref, const StructField& f,
                        const std::string& fname, Expr& value) {
        const BitSeg* seg = bitseg(f.ty, fname);
        if (!seg) {
            diag_.error(cur_pos_, "internal: unknown bitfield segment '" + fname + "'");
            ok_ = false;
            return;
        }
        u64 mask = (seg->width >= 64) ? ~u64(0) : ((u64(1) << seg->width) - 1);
        NodeId backing = struct_field_load(ref, f); // u64
        NodeId v = emit_expr(value);
        TypeId vt = g_.node(v).ty;
        if (vt != ty_u64()) {
            CastOp k = ty_is_signed(vt) ? CastOp::ZExt : CastOp::Ptr; // zero-extend
            v = g_.make(Op::Cast, ty_u64(), {cur_ctrl_, v}, static_cast<u8>(k));
        }
        NodeId vm = v;
        if (mask != ~u64(0)) {
            NodeId mk = make_int_const(mask, ty_u64(), cur_ctrl_);
            vm = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, v, mk}, static_cast<u8>(BinOp::And));
        }
        NodeId shifted = vm;
        if (seg->shift > 0) {
            NodeId sh = make_int_const(seg->shift, ty_u64(), cur_ctrl_);
            shifted = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, vm, sh},
                              static_cast<u8>(BinOp::Shl));
        }
        NodeId cleared = backing;
        if (seg->shift == 0 && mask == ~u64(0)) {
            cleared = make_int_const(0, ty_u64(), cur_ctrl_);
        } else {
            NodeId cl = make_int_const(~(mask << seg->shift), ty_u64(), cur_ctrl_);
            cleared = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, backing, cl},
                              static_cast<u8>(BinOp::And));
        }
        NodeId result = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, cleared, shifted},
                                static_cast<u8>(BinOp::Or));
        struct_field_store(ref, f, result);
    }

    // ---- control -----------------------------------------------------------
    struct LoopCtx {
        NodeId header;
        NodeId header_memphi;
        NodeId exit_region;
        NodeId exit_memphi;
        NodeId continue_region;   // header (while) or increment merge (for)
        NodeId continue_memphi;
        u32 body_depth;           // defer scope depth INSIDE the body
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
        ++depth_;
        for (const StmtP& t : s.body) emit_stmt(*t);
        close_defers_at(depth_);
        --depth_;
        NodeId then_end = cur_ctrl_;
        NodeId then_mem = cur_mem_;

        vars_ = outer_vars;
        cur_ctrl_ = fproj;
        cur_mem_ = mem_at_if;
        ++depth_;
        for (const StmtP& e : s.else_body) emit_stmt(*e);
        close_defers_at(depth_);
        --depth_;
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

        ++depth_;
        LoopCtx lc{header, mphi, exit_region, exit_memphi, header, mphi, depth_};
        loops_.push_back(lc);

        cur_ctrl_ = tproj;
        cur_mem_ = mem_at_if;
        for (const StmtP& b : s.body) emit_stmt(*b);
        // per-iteration scope exit: body defers run before the backedge
        // (close_defers_at emits them, LIFO, then drops them)
        close_defers_at(depth_);
        --depth_;

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

        ++depth_;
        LoopCtx lc{header, mphi, exit_region, exit_memphi, inc_merge, inc_mphi, depth_};
        loops_.push_back(lc);

        cur_ctrl_ = tproj;
        cur_mem_ = mem_at_if;
        for (const StmtP& b : s.body) emit_stmt(*b);
        // per-iteration scope exit before the increment target
        close_defers_at(depth_);
        --depth_;
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
        cur_pos_ = e.pos;
        switch (e.kind) {
            case ExprKind::IntLit:
                return make_int_const(e.iv, ir_type_of(e.ty), cur_ctrl_);
            case ExprKind::FloatLit:
                return make_fp_const(e.fv, e.ty, cur_ctrl_);
            case ExprKind::BoolLit:
                return make_int_const(e.iv, ty_i1(), cur_ctrl_);
            case ExprKind::Ident: {
                if (e.comptime_value) {
                    if (ty_is_float(e.ty)) return make_fp_const(e.fv, e.ty, cur_ctrl_);
                    return make_int_const(e.iv, ir_type_of(e.ty), cur_ctrl_);
                }
                // Binding kinds: (a) memory-resident struct locals — the
                // alloc IS the struct's address; (b) &self — the param value
                // IS the receiver address; (c) every other local — a scalar
                // slot (pointer VALUES like `let pa = alloc(T)` included):
                // LOAD it.
                if (const NodeId* a = vars_.find(e.name)) {
                    if (sema_.is_struct_type(e.ty))
                        return *a;
                    if (sema_.is_struct_ptr(e.ty) && g_.node(*a).op != Op::Alloc)
                        return *a;
                    return g_.make(Op::Load, ir_type_of(e.ty),
                                   {cur_ctrl_, cur_mem_, *a});
                }
                if (const NodeId* p = params_.find(e.name)) {
                    return *p; // unassigned parameters are direct SSA values
                }
                diag_.error(e.pos, "internal: unresolved local '" + e.name + "' in graph builder");
                ok_ = false;
                return zero_of(ir_type_of(e.ty), cur_ctrl_);
            }
            case ExprKind::Unary: {
                if (e.uop == UnKind::BNot && sema_.is_int_backed(e.lhs->ty)) {
                    // ~bitmask: backing-width complement
                    NodeId x = emit_expr(*e.lhs);
                    TypeId b = ir_type_of(e.ty);
                    NodeId all = make_int_const(~u64(0), b, cur_ctrl_);
                    return g_.make(Op::Bin, b, {cur_ctrl_, x, all},
                                   static_cast<u8>(BinOp::Xor));
                }
                NodeId x = emit_expr(*e.lhs);
                return g_.make(Op::Un, ir_type_of(e.ty), {cur_ctrl_, x},
                               static_cast<u8>(e.uop));
            }
            case ExprKind::Binary:
                return emit_binary(e);
            case ExprKind::Cast: {
                NodeId x = emit_expr(*e.lhs);
                TypeId from = ir_type_of(e.lhs->ty);
                TypeId to = ir_type_of(e.cast_target);
                if (from == to) return x; // enum -> its backing: identity
                CastOp k = pick_cast(from, to);
                return g_.make(Op::Cast, to, {cur_ctrl_, x}, static_cast<u8>(k));
            }
            case ExprKind::Deref: {
                // pointer-to-struct: the pointer IS the struct's address
                if (sema_.is_struct_ptr(e.lhs->ty)) return emit_expr(*e.lhs);
                NodeId addr = emit_expr(*e.lhs);
                return g_.make(Op::Load, e.ty, {cur_ctrl_, cur_mem_, addr});
            }
            case ExprKind::Index: {
                if (sema_.is_struct_ptr(e.lhs->ty)) {
                    // element struct value: its address
                    return to_ptr(struct_elem_addr(*e.lhs, *e.rhs), ty_i64());
                }
                NodeId base = emit_expr(*e.lhs);
                NodeId idx = emit_expr(*e.rhs);
                NodeId addr = index_address(base, idx, e.rhs->ty, e.lhs->ty);
                return g_.make(Op::Load, e.ty, {cur_ctrl_, cur_mem_, addr});
            }
            case ExprKind::Field:
                return emit_field(e);
            case ExprKind::StructLit:
                if (e.comptime_value)
                    return make_int_const(e.iv, ir_type_of(e.ty), cur_ctrl_);
                return materialize_struct(const_cast<Expr&>(e));
            case ExprKind::MethodCall:
                return emit_method_call(const_cast<Expr&>(e));
            case ExprKind::AddrOf: {
                // sema restricts & to struct lvalues
                SRef r = struct_ref(*const_cast<Expr&>(e).lhs);
                if (!r.is_addr) {
                    diag_.error(e.pos, "internal: & of a decomposed struct reached codegen");
                    ok_ = false;
                    return zero_of(ty_i64(), cur_ctrl_);
                }
                return r.addr;
            }
            case ExprKind::Call:
                return emit_call(const_cast<Expr&>(e));
            case ExprKind::ComptimeBlock:
                diag_.error(e.pos, "internal: comptime block reached the builder unfolded");
                ok_ = false;
                return zero_of(ty_i32(), cur_ctrl_);
        }
        return zero_of(ty_i32(), cur_ctrl_);
    }

    // Field read: struct fields, bitfield segments, folded enum/bitmask consts.
    NodeId emit_field(const Expr& e) {
        // folded qualified constant (Color.Red / Perm.Read)
        if (e.comptime_value) return make_int_const(e.iv, ir_type_of(e.ty), cur_ctrl_);
        Expr& base = *e.lhs;
        // bitfield segment read: (v >> shift) & mask
        if (ty_is_user(base.ty) && bitseg_able(base.ty)) {
            const BitSeg* seg = bitseg(base.ty, e.name);
            if (!seg) {
                diag_.error(e.pos, "internal: unknown bitfield segment '" + e.name + "'");
                ok_ = false;
                return zero_of(ty_u64(), cur_ctrl_);
            }
            NodeId v = emit_expr(base);
            NodeId shifted = v;
            if (seg->shift > 0) {
                NodeId sh = make_int_const(seg->shift, ty_u64(), cur_ctrl_);
                shifted = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, v, sh},
                                  static_cast<u8>(BinOp::Shr));
            }
            u64 mask = (seg->width >= 64) ? ~u64(0) : ((u64(1) << seg->width) - 1);
            if (mask == ~u64(0)) return shifted;
            NodeId mk = make_int_const(mask, ty_u64(), cur_ctrl_);
            return g_.make(Op::Bin, ty_u64(), {cur_ctrl_, shifted, mk},
                           static_cast<u8>(BinOp::And));
        }
        // struct field read
        SRef ref = struct_ref(base);
        const StructField* f = sfield(base.ty, e.name);
        if (!f) {
            diag_.error(e.pos, "internal: unknown field '" + e.name + "'");
            ok_ = false;
            return zero_of(ty_i64(), cur_ctrl_);
        }
        return struct_field_load(ref, *f);
    }

    // Address of element `idx` in the array at `base` (pointer of type
    // `base_ty`): base + idx * elem_size, all in i64 with bit-exact Ptr
    // casts at the boundaries. Scaling uses Shl for power-of-two element
    // sizes (1/2/4/8 bytes) so the LEA formation in pass 87 fires.
    NodeId index_address(NodeId base, NodeId idx, TypeId idx_ty, TypeId base_ty) {
        // widen/normalize the index to i64 (SExt for i32, bit-cast for u64,
        // identity for i64)
        if (idx_ty != ty_i64()) {
            CastOp k = ty_is_signed(idx_ty) ? CastOp::SExt : CastOp::Ptr;
            idx = g_.make(Op::Cast, ty_i64(), {cur_ctrl_, idx}, static_cast<u8>(k));
        }
        u32 esz = ty_store_bytes(ty_pointee(base_ty));
        NodeId base64 = g_.make(Op::Cast, ty_i64(), {cur_ctrl_, base},
                                static_cast<u8>(CastOp::Ptr));
        NodeId addr64 = base64;
        if (esz == 1) {
            addr64 = g_.make(Op::Bin, ty_i64(), {cur_ctrl_, base64, idx},
                             static_cast<u8>(BinOp::Add));
        } else {
            // esz in {2,4,8}: shl by log2(esz)
            u8 sh = esz == 2 ? 1 : esz == 4 ? 2 : 3;
            NodeId off = idx;
            if (sh != 0) {
                NodeId k = make_int_const(sh, ty_i64(), cur_ctrl_);
                off = g_.make(Op::Bin, ty_i64(), {cur_ctrl_, idx, k},
                              static_cast<u8>(BinOp::Shl));
            }
            addr64 = g_.make(Op::Bin, ty_i64(), {cur_ctrl_, base64, off},
                             static_cast<u8>(BinOp::Add));
        }
        return g_.make(Op::Cast, base_ty, {cur_ctrl_, addr64},
                       static_cast<u8>(CastOp::Ptr));
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
        return g_.make(Op::Bin, ir_type_of(e.ty), {cur_ctrl_, a, b}, static_cast<u8>(e.bop));
    }

    NodeId emit_method_call(Expr& e) {
        Expr& recv = *e.lhs;
        // bitmask sugar: m.has(Flag)  ==>  (m & Flag) != 0
        if (e.name == "has" && ty_is_user(recv.ty) && !sema_.is_struct_type(recv.ty) &&
            !sema_.is_struct_ptr(recv.ty)) {
            NodeId m = emit_expr(recv);
            NodeId flag = emit_expr(*e.args[0]);
            NodeId anded = g_.make(Op::Bin, ty_u64(), {cur_ctrl_, m, flag},
                                   static_cast<u8>(BinOp::And));
            NodeId zero = make_int_const(0, ty_u64(), cur_ctrl_);
            return g_.make(Op::Cmp, ty_i1(), {cur_ctrl_, anded, zero},
                           static_cast<u8>(CmpOp::Ne));
        }
        // receiver address: pointer receivers pass through, values materialize
        NodeId self_addr = sema_.is_struct_ptr(recv.ty)
            ? emit_expr(recv)
            : materialize_struct(recv);

        const size_t* fi = sema_.fn_by_name.find(e.callee);
        if (!fi) {
            diag_.error(e.pos, "internal: unresolved method '" + e.name + "'");
            ok_ = false;
            return zero_of(ty_i64(), cur_ctrl_);
        }
        const SemaFn& m = sema_.fns[*fi];
        bool ret_struct = sema_.is_struct_type(m.ret);

        NodeId ins[kMaxInputs];
        ins[0] = cur_ctrl_;
        u8 n = 2;
        ins[2] = self_addr; // self (a pointer)
        n = 3;
        for (size_t i = 0; i < e.args.size(); ++i) {
            if (n >= kMaxInputs) {
                diag_.error(e.pos, "call argument count exceeds the MVP node arity limit");
                ok_ = false;
                break;
            }
            Expr& a = *e.args[i];
            if (sema_.is_struct_type(m.params[i + 1])) {
                ins[n++] = materialize_struct(a);
            } else {
                ins[n++] = emit_expr(a);
            }
        }
        NodeId temp = kNoNode;
        if (ret_struct) {
            SRef t = declare_struct_mem("__mret", m.ret);
            temp = t.addr;
            if (n < kMaxInputs) ins[n++] = temp;
        }
        ins[1] = cur_mem_;
        TypeId rty = ret_struct ? ty_void() : ir_type_of(m.ret);
        cur_mem_ = g_.make_arr(Op::Call, rty, ins, n, 0, static_cast<FnId>(*fi));
        return ret_struct ? temp : cur_mem_;
    }

    NodeId emit_call(Expr& e) {
        if (e.name == "alloc") {
            TypeId pointee = e.args[0]->cast_target;
            bool pointee_is_struct = sema_.is_struct_type(pointee);
            u32 esz = pointee_is_struct ? struct_bytes(pointee)
                                        : ty_store_bytes(pointee);
            TypeId res_ty = ty_ptr(pointee);
            if (res_ty == ty_none()) res_ty = ty_ptr(ty_i64()); // no ptr-to-ptr/struct
            NodeId size;
            if (e.args.size() >= 2) {
                // alloc(T, n): byte size = n * elem_size (computed at runtime
                // when n is dynamic; constant-folded when n is a literal).
                NodeId count = emit_expr(*e.args[1]);
                if (g_.node(count).op == Op::Const) {
                    size = make_int_const(static_cast<u64>(g_.node(count).ival) * esz,
                                          ty_i64(), cur_ctrl_);
                } else {
                    NodeId cnt64 = count;
                    TypeId cty = g_.node(count).ty;
                    if (cty != ty_i64()) {
                        CastOp k = ty_is_signed(cty) ? CastOp::SExt : CastOp::Ptr;
                        cnt64 = g_.make(Op::Cast, ty_i64(), {cur_ctrl_, count},
                                        static_cast<u8>(k));
                    }
                    if (esz == 1) {
                        size = cnt64;
                    } else if ((esz & (esz - 1)) == 0) {
                        u8 sh = 0;
                        while ((u64(1) << sh) < esz) ++sh;
                        NodeId k = make_int_const(sh, ty_i64(), cur_ctrl_);
                        size = g_.make(Op::Bin, ty_i64(), {cur_ctrl_, cnt64, k},
                                       static_cast<u8>(BinOp::Shl));
                    } else {
                        NodeId k = make_int_const(esz, ty_i64(), cur_ctrl_);
                        size = g_.make(Op::Bin, ty_i64(), {cur_ctrl_, cnt64, k},
                                       static_cast<u8>(BinOp::Mul));
                    }
                }
            } else {
                size = make_int_const(esz, ty_i64(), cur_ctrl_);
            }
            cur_mem_ = g_.make(Op::Alloc, res_ty, {cur_ctrl_, cur_mem_, size});
            return cur_mem_;
        }

        // extern?
        if (const size_t* xi = sema_.extern_by_name.find(e.callee); xi != nullptr) {
            const SemaExtern& ex = sema_.externs[*xi];
            NodeId ins[kMaxInputs];
            ins[0] = cur_ctrl_;
            u8 n = 2;
            for (const ExprP& a : e.args) {
                if (n >= kMaxInputs) {
                    diag_.error(e.pos, "extern call argument count exceeds the node arity limit");
                    ok_ = false;
                    break;
                }
                ins[n++] = emit_expr(*a);
            }
            ins[1] = cur_mem_;
            cur_mem_ = g_.make_arr(Op::Call, ir_type_of(ex.ret), ins, n, 0,
                                   extern_fn_id(static_cast<u32>(*xi)));
            return cur_mem_;
        }

        FnId target = kNoFn;
        if (e.name == "print") target = kFnPrint;
        else if (e.name == "free") target = kFnFree;
        else {
            const size_t* p = sema_.fn_by_name.find(e.callee);
            if (!p) {
                diag_.error(e.pos, "internal: unresolved function '" + e.name + "'");
                ok_ = false;
                return zero_of(ir_type_of(e.ty), cur_ctrl_);
            }
            target = static_cast<FnId>(*p);
        }

        const SemaFn* sf = nullptr;
        if (target != kFnPrint && target != kFnFree)
            sf = &sema_.fns[static_cast<size_t>(target)];
        bool ret_struct = sf && sema_.is_struct_type(sf->ret);

        NodeId ins[kMaxInputs];
        ins[0] = cur_ctrl_;
        u8 n = 2;
        for (size_t i = 0; i < e.args.size(); ++i) {
            if (n >= kMaxInputs) {
                diag_.error(e.pos, "call argument count exceeds the MVP node arity limit");
                ok_ = false;
                break;
            }
            Expr& a = *e.args[i];
            if (sf && sema_.is_struct_type(sf->params[i])) {
                ins[n++] = materialize_struct(a);
            } else {
                ins[n++] = emit_expr(a);
            }
        }
        NodeId temp = kNoNode;
        if (ret_struct) {
            SRef t = declare_struct_mem("__ret", sf->ret);
            temp = t.addr;
            if (n < kMaxInputs) ins[n++] = temp;
        }
        // Memory input is captured AFTER argument evaluation: nested calls in
        // the argument list advance cur_mem_, and this Call must observe their
        // effects (eval order = argument effects, then the call itself).
        // Capturing before the loop orphaned the argument calls from the
        // memory chain and broke effect ordering in the linearizer.
        ins[1] = cur_mem_;
        TypeId rty = ret_struct ? ty_void() : ir_type_of(e.ty);
        cur_mem_ = g_.make_arr(Op::Call, rty, ins, n, 0, target);
        return ret_struct ? temp : cur_mem_;
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
    NodeId sret_param_ = kNoNode;
    SourcePos cur_pos_{};
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
