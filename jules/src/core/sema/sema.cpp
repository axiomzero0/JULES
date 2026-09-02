#include "core/sema/sema.h"

#include <cmath>

namespace jules {

namespace {

constexpr u64 kComptimeStepLimit = 1'000'000; // halts runaway comptime loops

struct Value {
    TypeId ty = ty_none();
    u64 iv = 0;
    f64 fv = 0;
};

bool val_is_fp(const Value& v) { return ty_is_float(v.ty); }

class Sema {
public:
    Sema(ModuleAst& mod, SemaModule& out, Diagnostics& diag, SymbolTable& syms)
        : mod_(mod), out_(out), diag_(diag), syms_(syms) {}

    bool run() {
        declare_globals();
        for (FnDecl& fn : mod_.fns) check_fn(fn);
        if (!out_.has_main && !diag_.has_errors()) {
            SourcePos pos = mod_.fns.empty() ? SourcePos{} : mod_.fns[0].pos;
            diag_.error(pos, "no 'fn main' found: an executable entry point is required");
        }
        return !diag_.has_errors();
    }

private:
    // ---- globals ---------------------------------------------------------
    void declare_globals() {
        // functions first: consts may call comptime functions
        for (size_t fi = 0; fi < mod_.fns.size(); ++fi) {
            FnDecl& f = mod_.fns[fi];
            if (fn_index(f.name) != SIZE_MAX) {
                diag_.error(f.pos, "redeclaration of function '" + std::string(syms_.name(f.name)) + "'");
                continue;
            }
            SemaFn sf;
            sf.name = f.name;
            sf.ret = f.ret;
            for (auto& p : f.params) sf.params.push_back(p.second);
            sf.is_comptime = f.is_comptime;
            sf.always_inline = f.always_inline;
            sf.no_inline = f.no_inline;
            sf.ast_index = fi;
            sf.node_estimate = estimate_fn(f);
            out_.fns.push_back(sf);
            out_.fn_by_name.insert(f.name, out_.fns.size() - 1);
            if (f.name == syms_.intern("main")) {
                out_.has_main = true;
                if (!f.params.empty())
                    diag_.error(f.pos, "'main' must take no parameters in the MVP subset");
                if (f.ret != ty_void() && f.ret != ty_i64() && f.ret != ty_i32())
                    diag_.error(f.pos, "'main' must return void, i32 or i64");
            }
        }
        FlatMap<SymbolId, bool> seen_consts;
        for (ConstDecl& c : mod_.consts) {
            if (seen_consts.contains(c.name) || fn_index(c.name) != SIZE_MAX) {
                diag_.error(c.pos, "redeclaration of '" + std::string(syms_.name(c.name)) + "'");
                continue;
            }
            seen_consts.insert(c.name, true);
            std::optional<Value> v = eval_const(*c.value, c.ty);
            if (v) {
                c.iv = v->iv; c.fv = v->fv; c.is_fp = val_is_fp(*v);
                if (c.ty == ty_none()) c.ty = v->ty;
            }
        }
        for (size_t fi = 0; fi < mod_.fns.size(); ++fi) {
            // duplicate registration guard (fns registered above for const eval)
            (void)fi;
        }
    }

    size_t const_decl_index(SymbolId s) {
        for (size_t i = 0; i < mod_.consts.size(); ++i)
            if (mod_.consts[i].name == s) return i;
        return SIZE_MAX;
    }
    size_t fn_index(SymbolId s) {
        const size_t* p = out_.fn_by_name.find(s);
        return p ? *p : SIZE_MAX;
    }

    u32 estimate_fn(const FnDecl& f) {
        u32 n = 4;
        for (const StmtP& s : f.body) n += estimate_stmt(*s);
        return n;
    }
    u32 estimate_stmt(const Stmt& s) {
        u32 n = 1;
        if (s.value) n += estimate_expr(*s.value);
        if (s.cond) n += estimate_expr(*s.cond);
        if (s.target) n += estimate_expr(*s.target);
        if (s.to) n += estimate_expr(*s.to);
        for (const StmtP& c : s.body) n += estimate_stmt(*c);
        for (const StmtP& c : s.else_body) n += estimate_stmt(*c);
        return n;
    }
    u32 estimate_expr(const Expr& e) {
        u32 n = 1;
        if (e.lhs) n += estimate_expr(*e.lhs);
        if (e.rhs) n += estimate_expr(*e.rhs);
        for (const ExprP& a : e.args) n += estimate_expr(*a);
        return n;
    }

    // ---- scopes ------------------------------------------------------------
    struct Local {
        SymbolId sym;
        std::string name;
        TypeId ty;
        bool immutable;
        SourcePos pos;
    };
    struct Scope { std::vector<Local> locals; };

    const Local* find_local(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            for (auto lit = it->locals.rbegin(); lit != it->locals.rend(); ++lit)
                if (lit->name == name) return &*lit;
        return nullptr;
    }
    void declare_local(const std::string& name, TypeId ty, bool immutable, SourcePos pos) {
        if (!scopes_.empty()) {
            for (const Local& l : scopes_.back().locals)
                if (l.name == name) {
                    diag_.error(pos, "redeclaration of '" + name + "' in the same scope");
                    return;
                }
        }
        scopes_.back().locals.push_back(Local{syms_.intern(name), name, ty, immutable, pos});
    }

    // ---- fn body checking ---------------------------------------------------
    struct FnCtx {
        const FnDecl* fn = nullptr;
        const SemaFn* sema = nullptr;
        bool saw_return = false;
    };

    void check_fn(FnDecl& fn) {
        FnCtx ctx;
        size_t fidx = fn_index(fn.name);
        ctx.fn = &fn;
        ctx.sema = fidx != SIZE_MAX ? &out_.fns[fidx] : nullptr;
        scopes_.clear();
        scopes_.push_back(Scope{});
        for (auto& p : fn.params) declare_local(p.first, p.second, false, fn.pos);
        check_stmts(fn.body, ctx);
        if (!fn.body.empty() && !ctx.saw_return && fn.ret != ty_void()) {
            diag_.warn(fn.pos, "non-void function may finish without returning a value");
        }
    }

    void check_stmts(std::vector<StmtP>& stmts, FnCtx& ctx) {
        bool unreachable_reported = false;
        for (StmtP& s : stmts) {
            if (ctx.saw_return && !unreachable_reported) {
                diag_.warn(s->pos, "unreachable code after 'return'");
                unreachable_reported = true;
            }
            check_stmt(*s, ctx);
        }
    }

    void check_stmt(Stmt& s, FnCtx& ctx) {
        switch (s.kind) {
            case StmtKind::Let: {
                if (!s.value) {
                    diag_.error(s.pos, "binding has no initializer");
                    return;
                }
                TypeId it = check_expr(*s.value, ctx);
                if (s.decl_ty != ty_none()) {
                    if (it != s.decl_ty) {
                        if (!unify_literal(*s.value, s.decl_ty)) {
                            diag_.error(s.pos, "initializer type " + std::string(ty_name(it)) +
                                        " does not match declared type " + std::string(ty_name(s.decl_ty)) +
                                        " (JULES has no implicit conversions; use an explicit 'as' cast)");
                        }
                    }
                } else {
                    s.decl_ty = it;
                }
                if (s.decl_ty == ty_void() || s.decl_ty == ty_none()) {
                    diag_.error(s.pos, "cannot bind a void value");
                    return;
                }
                declare_local(s.name, s.decl_ty, s.immutable, s.pos);
                break;
            }
            case StmtKind::Assign: {
                const Local* l = find_local(s.name);
                if (!l) {
                    diag_.error(s.pos, "assignment to undeclared variable '" + s.name + "'");
                    return;
                }
                if (l->immutable) {
                    diag_.error(s.pos, "cannot assign to immutable 'let' binding '" + s.name +
                                "' (declare it with 'var' to allow mutation)");
                    return;
                }
                TypeId vt = check_expr(*s.value, ctx);
                if (vt != l->ty && vt != ty_none()) {
                    if (!unify_literal(*s.value, l->ty))
                        diag_.error(s.pos, "cannot assign " + std::string(ty_name(vt)) +
                                    " to variable of type " + std::string(ty_name(l->ty)) +
                                    " (use an explicit cast)");
                }
                s.decl_ty = l->ty;
                break;
            }
            case StmtKind::AssignDeref: {
                TypeId pt = check_expr(*s.target, ctx);
                TypeId vt = check_expr(*s.value, ctx);
                if (!ty_is_ptr(pt)) {
                    diag_.error(s.pos, "left side of deref-assign must be a pointer");
                    return;
                }
                TypeId pointee = pointee_of(pt);
                if (vt != pointee && vt != ty_none()) {
                    if (!unify_literal(*s.value, pointee))
                        diag_.error(s.pos, "cannot store " + std::string(ty_name(vt)) +
                                    " through pointer to " + std::string(ty_name(pointee)));
                }
                break;
            }
            case StmtKind::Return: {
                TypeId vt = s.value ? check_expr(*s.value, ctx) : ty_void();
                if (vt == ty_none()) return;
                if (vt != ctx.fn->ret &&
                    !(s.value && unify_literal(*s.value, ctx.fn->ret))) {
                    diag_.error(s.pos, "return type " + std::string(ty_name(vt)) +
                                " does not match function return type " + std::string(ty_name(ctx.fn->ret)));
                }
                ctx.saw_return = true;
                break;
            }
            case StmtKind::If: {
                TypeId ct = check_expr(*s.cond, ctx);
                if (ct != ty_none() && !ty_is_bool(ct))
                    diag_.error(s.pos, "'if' condition must be bool, found " + std::string(ty_name(ct)));
                scopes_.push_back(Scope{});
                check_stmts(s.body, ctx);
                scopes_.pop_back();
                scopes_.push_back(Scope{});
                check_stmts(s.else_body, ctx);
                scopes_.pop_back();
                break;
            }
            case StmtKind::While: {
                TypeId ct = check_expr(*s.cond, ctx);
                if (ct != ty_none() && !ty_is_bool(ct))
                    diag_.error(s.pos, "'while' condition must be bool, found " + std::string(ty_name(ct)));
                scopes_.push_back(Scope{});
                check_stmts(s.body, ctx);
                scopes_.pop_back();
                break;
            }
            case StmtKind::For: {
                TypeId ft = check_expr(*s.target, ctx);
                TypeId tt = check_expr(*s.to, ctx);
                if (ft != tt && ft != ty_none() && tt != ty_none()) {
                    diag_.error(s.pos, "range bounds must have the same type (" +
                                std::string(ty_name(ft)) + " vs " + std::string(ty_name(tt)) + ")");
                } else if (ft == ty_none() || tt == ty_none()) {
                    return;
                }
                if (!ty_is_int(ft) || ty_is_bool(ft)) {
                    diag_.error(s.pos, "range loop requires an integer type");
                    return;
                }
                s.decl_ty = ft;
                scopes_.push_back(Scope{});
                declare_local(s.name, ft, false, s.pos);
                check_stmts(s.body, ctx);
                scopes_.pop_back();
                break;
            }
            case StmtKind::ExprStmt: {
                TypeId vt = check_expr(*s.value, ctx);
                if (vt != ty_none() && vt != ty_void() && s.value->kind == ExprKind::Binary)
                    diag_.info(s.pos, "computed value is unused");
                break;
            }
            case StmtKind::Break:
            case StmtKind::Continue:
                break;
        }
    }

    TypeId pointee_of(TypeId ptr) { return pointee_scalar(ptr); }
    static TypeId pointee_scalar(TypeId ptr) {
        TypeDesc d = type_desc(ptr);
        switch (d.pointee) {
            case Ty::I64: return ty_i64();
            case Ty::I32: return ty_i32();
            case Ty::U64: return ty_u64();
            case Ty::U32: return ty_u32();
            case Ty::F32: return ty_f32();
            case Ty::F64: return ty_f64();
            case Ty::I1:  return ty_i1();
            default: return ty_i64();
        }
    }

    // ---- literal typing --------------------------------------------------------------
    // Integer literals are typed by context (when the value fits): this is
    // literal inference, NOT implicit conversion of computed values — the
    // strict rule still applies to variables and expressions.
    static bool fits(u64 v, TypeId t) {
        switch (type_desc(t).ty) {
            case Ty::I32: return v <= static_cast<u64>(INT32_MAX);
            case Ty::U32: return v <= 0xFFFFFFFFull;
            case Ty::I64: return v <= static_cast<u64>(INT64_MAX);
            case Ty::U64: return true;
            default: return false;
        }
    }
    static bool unify_literal(Expr& e, TypeId expected) {
        if (e.kind == ExprKind::IntLit && ty_is_int(expected) && !ty_is_bool(expected) &&
            fits(e.iv, expected)) {
            e.ty = expected;
            return true;
        }
        // negative literals: -(lit) unifies when the magnitude fits
        if (e.kind == ExprKind::Unary && e.uop == UnKind::Neg && e.lhs &&
            e.lhs->kind == ExprKind::IntLit && ty_is_int(expected) &&
            !ty_is_bool(expected) && fits(e.lhs->iv, expected)) {
            e.lhs->ty = expected;
            e.ty = expected;
            return true;
        }
        return false;
    }
    // ---- expressions ---------------------------------------------------------
    TypeId check_expr(Expr& e, FnCtx& ctx) {
        switch (e.kind) {
            case ExprKind::IntLit:
                return e.ty;
            case ExprKind::FloatLit:
                return e.ty;
            case ExprKind::BoolLit:
                return ty_i1();
            case ExprKind::Ident: {
                size_t ci = const_decl_index(e.sym);
                if (ci != SIZE_MAX) {
                    const ConstDecl& c = mod_.consts[ci];
                    e.comptime_value = true;
                    e.iv = c.is_fp ? 0 : c.iv;
                    e.fv = c.fv;
                    e.ty = c.ty;
                    return e.ty;
                }
                const Local* l = find_local(e.name);
                if (l) {
                    e.ty = l->ty;
                    return e.ty;
                }
                diag_.error(e.pos, "unknown name '" + e.name + "'");
                return ty_none();
            }
            case ExprKind::Unary: {
                TypeId t = check_expr(*e.lhs, ctx);
                if (t == ty_none()) return ty_none();
                switch (e.uop) {
                    case UnKind::Neg:
                        if (!ty_is_int(t) && !ty_is_float(t)) {
                            diag_.error(e.pos, "unary '-' requires a numeric operand");
                            return ty_none();
                        }
                        break;
                    case UnKind::Not:
                        if (!ty_is_bool(t)) {
                            diag_.error(e.pos, "'!' requires a bool operand");
                            return ty_none();
                        }
                        break;
                    case UnKind::BNot:
                        if (!ty_is_int(t) || ty_is_bool(t)) {
                            diag_.error(e.pos, "'~' requires an integer operand");
                            return ty_none();
                        }
                        break;
                }
                e.ty = t;
                return t;
            }
            case ExprKind::Binary: {
                TypeId lt = check_expr(*e.lhs, ctx);
                TypeId rt = check_expr(*e.rhs, ctx);
                if (lt == ty_none() || rt == ty_none()) return ty_none();
                // literal adoption: unify an integer literal with the other side
                if (lt != rt) {
                    if (unify_literal(*e.lhs, rt)) lt = rt;
                    else if (unify_literal(*e.rhs, lt)) rt = lt;
                }
                bool is_cmp = e.bop == BinKind::Eq || e.bop == BinKind::Ne || e.bop == BinKind::Lt ||
                              e.bop == BinKind::Le || e.bop == BinKind::Gt || e.bop == BinKind::Ge;
                bool is_logic = e.bop == BinKind::LogicAnd || e.bop == BinKind::LogicOr;
                if (is_logic) {
                    if (!ty_is_bool(lt) || !ty_is_bool(rt)) {
                        diag_.error(e.pos, "'&&'/'||' require bool operands");
                        return ty_none();
                    }
                    e.ty = ty_i1();
                    return e.ty;
                }
                if (e.bop == BinKind::Shl || e.bop == BinKind::Shr) {
                    if (!ty_is_int(lt) || ty_is_bool(lt)) {
                        diag_.error(e.pos, "shift requires integer left operand");
                        return ty_none();
                    }
                    if (!ty_is_int(rt) || ty_is_bool(rt)) {
                        diag_.error(e.pos, "shift requires integer right operand");
                        return ty_none();
                    }
                    e.ty = lt;
                    return e.ty;
                }
                if (lt != rt) {
                    diag_.error(e.pos, "mixed-type operands (" + std::string(ty_name(lt)) + " vs " +
                                std::string(ty_name(rt)) + "); JULES has no implicit conversions — cast one side with 'as'");
                    return ty_none();
                }
                if (is_cmp) {
                    if (ty_is_ptr(lt) && !(e.bop == BinKind::Eq || e.bop == BinKind::Ne)) {
                        diag_.error(e.pos, "pointers support only ==/!= comparison in MVP");
                        return ty_none();
                    }
                    e.ty = ty_i1();
                    return e.ty;
                }
                if (ty_is_ptr(lt)) {
                    diag_.error(e.pos, "arithmetic on raw pointers is not in the MVP subset");
                    return ty_none();
                }
                switch (e.bop) {
                    case BinKind::And: case BinKind::Or: case BinKind::Xor:
                        if (!ty_is_int(lt) && !ty_is_bool(lt)) {
                            diag_.error(e.pos, "bitwise operator requires integer or bool operands");
                            return ty_none();
                        }
                        break;
                    default:
                        if (!ty_is_int(lt) && !ty_is_float(lt)) {
                            diag_.error(e.pos, "arithmetic requires numeric operands");
                            return ty_none();
                        }
                        break;
                }
                e.ty = lt;
                return e.ty;
            }
            case ExprKind::Cast: {
                TypeId st = check_expr(*e.lhs, ctx);
                TypeId target = e.cast_target;
                if (st == ty_none()) return ty_none();
                if (!ty_is_scalar(st) || !ty_is_scalar(target)) {
                    diag_.error(e.pos, "cast requires scalar types");
                    return ty_none();
                }
                if (ty_is_ptr(st) != ty_is_ptr(target)) {
                    diag_.error(e.pos, "casting between pointers and non-pointers is not in the MVP subset");
                    return ty_none();
                }
                e.ty = target;
                return target;
            }
            case ExprKind::Deref: {
                TypeId t = check_expr(*e.lhs, ctx);
                if (t == ty_none()) return ty_none();
                if (!ty_is_ptr(t)) {
                    diag_.error(e.pos, "cannot dereference non-pointer type " + std::string(ty_name(t)));
                    return ty_none();
                }
                e.ty = pointee_scalar(t);
                return e.ty;
            }
            case ExprKind::ComptimeBlock: {
                FlatMap<std::string, Value> env;
                steps_ = 0;
                std::optional<Value> v = interpret_stmts(e.block_body, env, ty_none());
                if (!v) {
                    diag_.error(e.pos, "comptime block must return a value at compile time");
                    return ty_none();
                }
                rewrite_to_literal(e, *v);
                return e.ty;
            }
            case ExprKind::Call:
                return check_call(e, ctx);
        }
        return ty_none();
    }

    TypeId check_call(Expr& e, FnCtx& ctx) {
        std::string callee = e.name;
        if (callee == "alloc") return check_alloc(e);
        if (callee == "free") return check_free(e, ctx);
        if (callee == "print") return check_print(e, ctx);

        size_t fi = fn_index(e.callee);
        if (fi == SIZE_MAX) {
            diag_.error(e.pos, "call to unknown function '" + callee + "'");
            return ty_none();
        }
        SemaFn& sf = out_.fns[fi];
        if (e.args.size() != sf.params.size()) {
            diag_.error(e.pos, "function '" + callee + "' expects " +
                        std::to_string(sf.params.size()) + " argument(s), got " +
                        std::to_string(e.args.size()));
            return ty_none();
        }
        for (size_t i = 0; i < e.args.size(); ++i) {
            TypeId at = check_expr(*e.args[i], ctx);
            if (at == ty_none()) return ty_none();
            if (at != sf.params[i]) {
                if (!unify_literal(*e.args[i], sf.params[i])) {
                    diag_.error(e.pos, "argument " + std::to_string(i + 1) + " of '" + callee + "' has type " +
                                std::string(ty_name(at)) + " but " + std::string(ty_name(sf.params[i])) +
                                " is expected (no implicit conversions)");
                    return ty_none();
                }
            }
        }
        if ((sf.is_comptime || e.comptime_call) && !ctx.fn->is_comptime) {
            // Inside a comptime fn body, calls (including self-recursion) are
            // evaluated by the interpreter when an outer comptime call runs.
            std::optional<Value> v = eval_comptime_call(e, fi);
            if (v) {
                rewrite_to_literal(e, *v);
                return e.ty;
            }
            diag_.error(e.pos, std::string(e.comptime_call ? "comptime call '" : "comptime function '") +
                        callee + "' could not be evaluated at compile time (arguments must be comptime-known)");
            return ty_none();
        }
        e.ty = sf.ret;
        return e.ty;
    }

    TypeId check_alloc(Expr& e) {
        if (e.args.size() != 1 || e.args[0]->kind != ExprKind::Ident) {
            diag_.error(e.pos, "alloc expects exactly one type argument, e.g. alloc(i64)");
            return ty_none();
        }
        TypeId pointee = ty_none();
        std::string tn = e.args[0]->name;
        if (tn == "i32") pointee = ty_i32();
        else if (tn == "i64" || tn == "usize") pointee = ty_i64();
        else if (tn == "u32") pointee = ty_u32();
        else if (tn == "u64") pointee = ty_u64();
        else if (tn == "f32") pointee = ty_f32();
        else if (tn == "f64") pointee = ty_f64();
        else if (tn == "bool") pointee = ty_i1();
        if (pointee == ty_none()) {
            diag_.error(e.pos, "alloc requires a scalar type argument (got '" + tn + "')");
            return ty_none();
        }
        e.args[0]->cast_target = pointee; // builder reads this
        e.ty = ty_ptr(pointee);
        return e.ty;
    }

    TypeId check_free(Expr& e, FnCtx& ctx) {
        if (e.args.size() != 1) {
            diag_.error(e.pos, "free expects exactly one pointer argument");
            return ty_none();
        }
        TypeId at = check_expr(*e.args[0], ctx);
        if (at == ty_none()) return ty_none();
        if (!ty_is_ptr(at)) {
            diag_.error(e.pos, "free expects a pointer, got " + std::string(ty_name(at)));
            return ty_none();
        }
        e.ty = ty_void();
        return e.ty;
    }

    TypeId check_print(Expr& e, FnCtx& ctx) {
        if (e.args.size() != 1) {
            diag_.error(e.pos, "print expects exactly one value in the MVP subset");
            return ty_none();
        }
        TypeId at = check_expr(*e.args[0], ctx);
        if (at == ty_none()) return ty_none();
        if (!ty_is_scalar(at) || ty_is_ptr(at)) {
            diag_.error(e.pos, "print supports numeric/bool values in MVP (got " +
                        std::string(ty_name(at)) + ")");
            return ty_none();
        }
        e.ty = ty_void();
        return e.ty;
    }

    static void rewrite_to_literal(Expr& e, const Value& v) {
        if (ty_is_float(v.ty)) {
            e.kind = ExprKind::FloatLit;
            e.fv = v.fv;
        } else {
            e.kind = ExprKind::IntLit;
            e.iv = v.iv;
        }
        e.ty = v.ty;
        e.comptime_value = true;
        e.lhs.reset(); e.rhs.reset(); e.args.clear();
    }

    // ---- comptime evaluation -------------------------------------------------
    std::optional<Value> eval_comptime_call(Expr& e, size_t fi) {
        FnDecl& fn = mod_.fns[out_.fns[fi].ast_index];
        FlatMap<std::string, Value> env;
        for (size_t i = 0; i < e.args.size(); ++i) {
            std::optional<Value> av = eval_const(*e.args[i], ty_none());
            if (!av) return std::nullopt;
            env.insert(fn.params[i].first, *av);
        }
        steps_ = 0;
        return interpret_fn(fn, env);
    }

    std::optional<Value> eval_const(const Expr& e, TypeId expect) {
        (void)expect;
        switch (e.kind) {
            case ExprKind::IntLit:  return Value{e.ty, e.iv, 0};
            case ExprKind::FloatLit: return Value{e.ty, 0, e.fv};
            case ExprKind::BoolLit: return Value{ty_i1(), e.iv, 0};
            case ExprKind::Ident: {
                size_t ci = const_decl_index(e.sym);
                if (ci == SIZE_MAX) return std::nullopt;
                const ConstDecl& c = mod_.consts[ci];
                return Value{c.ty, c.iv, c.fv};
            }
            case ExprKind::Unary: {
                auto v = eval_const(*e.lhs, ty_none());
                if (!v) return std::nullopt;
                switch (e.uop) {
                    case UnKind::Neg:
                        if (val_is_fp(*v)) return Value{v->ty, 0, -v->fv};
                        return Value{v->ty, static_cast<u64>(-static_cast<i64>(v->iv)), 0};
                    case UnKind::Not:  return Value{ty_i1(), v->iv ? u64(0) : u64(1), 0};
                    case UnKind::BNot: return Value{v->ty, ~v->iv, 0};
                }
                return std::nullopt;
            }
            case ExprKind::Cast: {
                auto v = eval_const(*e.lhs, ty_none());
                if (!v) return std::nullopt;
                return cast_value(*v, e.cast_target);
            }
            case ExprKind::Binary: {
                auto l = eval_const(*e.lhs, ty_none());
                auto r = eval_const(*e.rhs, ty_none());
                if (!l || !r) return std::nullopt;
                return eval_binop(*l, *r, e.bop, e.pos);
            }
            case ExprKind::Call: {
                size_t fi = fn_index(e.callee);
                if (fi == SIZE_MAX) return std::nullopt;
                FnDecl& fn = mod_.fns[out_.fns[fi].ast_index];
                FlatMap<std::string, Value> env;
                for (size_t i = 0; i < e.args.size(); ++i) {
                    auto av = eval_const(*e.args[i], ty_none());
                    if (!av) return std::nullopt;
                    env.insert(fn.params[i].first, *av);
                }
                u64 saved = steps_;
                steps_ = 0;
                auto res = interpret_fn(fn, env);
                steps_ = saved;
                return res;
            }
            case ExprKind::ComptimeBlock: {
                FlatMap<std::string, Value> env;
                u64 saved = steps_;
                steps_ = 0;
                auto res = interpret_stmts(e.block_body, env, ty_none());
                steps_ = saved;
                return res;
            }
            default: return std::nullopt;
        }
    }

    std::optional<Value> eval_binop(const Value& l, const Value& r, BinKind op, SourcePos pos) {
        bool fp = val_is_fp(l) || val_is_fp(r);
        if (fp) {
            f64 a = val_is_fp(l) ? l.fv : static_cast<f64>(static_cast<i64>(l.iv));
            f64 b = val_is_fp(r) ? r.fv : static_cast<f64>(static_cast<i64>(r.iv));
            switch (op) {
                case BinKind::Add: return Value{ty_f64(), 0, a + b};
                case BinKind::Sub: return Value{ty_f64(), 0, a - b};
                case BinKind::Mul: return Value{ty_f64(), 0, a * b};
                case BinKind::Div:
                    if (b == 0.0) { diag_.error(pos, "comptime division by zero"); return std::nullopt; }
                    return Value{ty_f64(), 0, a / b};
                case BinKind::Eq: return Value{ty_i1(), a == b ? u64(1) : u64(0), 0};
                case BinKind::Ne: return Value{ty_i1(), a != b ? u64(1) : u64(0), 0};
                case BinKind::Lt: return Value{ty_i1(), a <  b ? u64(1) : u64(0), 0};
                case BinKind::Le: return Value{ty_i1(), a <= b ? u64(1) : u64(0), 0};
                case BinKind::Gt: return Value{ty_i1(), a >  b ? u64(1) : u64(0), 0};
                case BinKind::Ge: return Value{ty_i1(), a >= b ? u64(1) : u64(0), 0};
                default: return std::nullopt;
            }
        }
        bool signed_cmp = ty_is_signed(l.ty) || ty_is_signed(r.ty);
        switch (op) {
            case BinKind::Add: return Value{l.ty, l.iv + r.iv, 0};
            case BinKind::Sub: return Value{l.ty, l.iv - r.iv, 0};
            case BinKind::Mul: return Value{l.ty, l.iv * r.iv, 0};
            case BinKind::Div:
            case BinKind::Mod: {
                if (r.iv == 0) { diag_.error(pos, "comptime division by zero"); return std::nullopt; }
                if (signed_cmp) {
                    i64 a = static_cast<i64>(l.iv), b = static_cast<i64>(r.iv);
                    if (op == BinKind::Div) return Value{l.ty, static_cast<u64>(a / b), 0};
                    return Value{l.ty, static_cast<u64>(a % b), 0};
                }
                if (op == BinKind::Div) return Value{l.ty, l.iv / r.iv, 0};
                return Value{l.ty, l.iv % r.iv, 0};
            }
            case BinKind::And: return Value{l.ty, l.iv & r.iv, 0};
            case BinKind::Or:  return Value{l.ty, l.iv | r.iv, 0};
            case BinKind::Xor: return Value{l.ty, l.iv ^ r.iv, 0};
            case BinKind::Shl: return Value{l.ty, l.iv << (r.iv & 63), 0};
            case BinKind::Shr: {
                if (signed_cmp) return Value{l.ty, static_cast<u64>(static_cast<i64>(l.iv) >> (r.iv & 63)), 0};
                return Value{l.ty, l.iv >> (r.iv & 63), 0};
            }
            case BinKind::LogicAnd: return Value{ty_i1(), (l.iv != 0 && r.iv != 0) ? u64(1) : u64(0), 0};
            case BinKind::LogicOr:  return Value{ty_i1(), (l.iv != 0 || r.iv != 0) ? u64(1) : u64(0), 0};
            case BinKind::Eq: return Value{ty_i1(), l.iv == r.iv ? u64(1) : u64(0), 0};
            case BinKind::Ne: return Value{ty_i1(), l.iv != r.iv ? u64(1) : u64(0), 0};
            case BinKind::Lt: return Value{ty_i1(), cmp_lt(l.iv, r.iv, signed_cmp) ? u64(1) : u64(0), 0};
            case BinKind::Le: return Value{ty_i1(), cmp_le(l.iv, r.iv, signed_cmp) ? u64(1) : u64(0), 0};
            case BinKind::Gt: return Value{ty_i1(), cmp_lt(r.iv, l.iv, signed_cmp) ? u64(1) : u64(0), 0};
            case BinKind::Ge: return Value{ty_i1(), cmp_le(r.iv, l.iv, signed_cmp) ? u64(1) : u64(0), 0};
        }
        return std::nullopt;
    }
    static bool cmp_lt(u64 a, u64 b, bool is_signed) {
        return is_signed ? static_cast<i64>(a) < static_cast<i64>(b) : a < b;
    }
    static bool cmp_le(u64 a, u64 b, bool is_signed) {
        return is_signed ? static_cast<i64>(a) <= static_cast<i64>(b) : a <= b;
    }

    std::optional<Value> cast_value(const Value& v, TypeId target) {
        if (ty_is_float(v.ty) && ty_is_float(target)) {
            return Value{target, 0, ty_bits(target) == 32 ? static_cast<f64>(static_cast<f32>(v.fv)) : v.fv};
        }
        if (ty_is_float(v.ty) && ty_is_int(target)) {
            i64 iv = static_cast<i64>(v.fv);
            if (ty_bits(target) == 32) iv = static_cast<i32>(iv);
            return Value{target, static_cast<u64>(iv), 0};
        }
        if (ty_is_int(v.ty) && ty_is_float(target)) {
            f64 fv = ty_is_signed(v.ty) ? static_cast<f64>(static_cast<i64>(v.iv))
                                        : static_cast<f64>(v.iv);
            if (ty_bits(target) == 32) fv = static_cast<f64>(static_cast<f32>(fv));
            return Value{target, 0, fv};
        }
        if (ty_is_ptr(v.ty) && ty_is_ptr(target)) return Value{target, v.iv, 0};
        // int -> int truncate/extend
        u64 raw = v.iv;
        if (ty_bits(target) == 32) {
            if (ty_is_signed(target))
                raw = static_cast<u64>(static_cast<i64>(static_cast<i32>(static_cast<u32>(raw))));
            else
                raw = static_cast<u32>(raw);
        } else if (ty_bits(v.ty) == 32 && ty_is_signed(v.ty)) {
            raw = static_cast<u64>(static_cast<i64>(static_cast<i32>(static_cast<u32>(raw))));
        }
        return Value{target, raw, 0};
    }

    // ---- comptime interpreter (pure subset) ------------------------------------
    std::optional<Value> interpret_fn(const FnDecl& fn, const FlatMap<std::string, Value>& env) {
        FlatMap<std::string, Value> locals = env;
        return interpret_stmts(fn.body, locals, fn.ret);
    }

    std::optional<Value> interpret_stmts(const std::vector<StmtP>& stmts,
                                         FlatMap<std::string, Value>& locals, TypeId ret_ty) {
        for (const StmtP& s : stmts) {
            auto r = interpret_stmt(*s, locals, ret_ty);
            if (r) return r; // return value propagated
        }
        if (ret_ty == ty_void()) return Value{ty_void(), 0, 0};
        return std::nullopt; // fell off the end (sema warns separately)
    }

    std::optional<Value> interpret_stmt(const Stmt& s, FlatMap<std::string, Value>& locals, TypeId ret_ty) {
        if (++steps_ > kComptimeStepLimit) {
            diag_.error(s.pos, "comptime evaluation exceeded the step limit (infinite loop?)");
            return Value{ty_void(), 0, 0};
        }
        switch (s.kind) {
            case StmtKind::Return:
                if (!s.value) return Value{ty_void(), 0, 0};
                return eval_env(*s.value, locals);
            case StmtKind::Let: {
                auto v = eval_env(*s.value, locals);
                if (!v) return std::nullopt;
                locals.insert(s.name, *v);
                return std::nullopt;
            }
            case StmtKind::Assign: {
                auto v = eval_env(*s.value, locals);
                if (!v) return std::nullopt;
                locals.insert(s.name, *v);
                return std::nullopt;
            }
            case StmtKind::If: {
                auto c = eval_cond(*s.cond, locals);
                if (!c) return std::nullopt;
                if (*c) return interpret_stmts(s.body, locals, ret_ty);
                return interpret_stmts(s.else_body, locals, ret_ty);
            }
            case StmtKind::While: {
                for (;;) {
                    if (++steps_ > kComptimeStepLimit) {
                        diag_.error(s.pos, "comptime evaluation exceeded the step limit (infinite loop?)");
                        return Value{ty_void(), 0, 0};
                    }
                    auto c = eval_cond(*s.cond, locals);
                    if (!c) return std::nullopt;
                    if (!*c) break;
                    auto r = interpret_stmts(s.body, locals, ret_ty);
                    if (r) return r;
                }
                return std::nullopt;
            }
            case StmtKind::For: {
                auto from = eval_env(*s.target, locals);
                auto to = eval_env(*s.to, locals);
                if (!from || !to) return std::nullopt;
                for (u64 i = from->iv; i < to->iv; ++i) {
                    if (++steps_ > kComptimeStepLimit) {
                        diag_.error(s.pos, "comptime evaluation exceeded the step limit (infinite loop?)");
                        return Value{ty_void(), 0, 0};
                    }
                    locals.insert(s.name, Value{from->ty, i, 0});
                    auto r = interpret_stmts(s.body, locals, ret_ty);
                    if (r) return r;
                }
                return std::nullopt;
            }
            default:
                return std::nullopt; // break/continue/expr-stmt have no comptime semantics
        }
    }

    std::optional<bool> eval_cond(const Expr& e, FlatMap<std::string, Value>& locals) {
        auto v = eval_env(e, locals);
        if (!v) return std::nullopt;
        return v->iv != 0;
    }

    // Environment-aware evaluator: like eval_const but let-bound locals are
    // visible at any depth (recursive comptime functions need this).
    std::optional<Value> eval_env(const Expr& e, const FlatMap<std::string, Value>& locals) {
        switch (e.kind) {
            case ExprKind::Ident: {
                if (const Value* v = locals.find(e.name)) return *v;
                return eval_const(e, ty_none());
            }
            case ExprKind::Unary: {
                auto a = eval_env(*e.lhs, locals);
                if (!a) return std::nullopt;
                switch (e.uop) {
                    case UnKind::Neg:
                        if (val_is_fp(*a)) return Value{a->ty, 0, -a->fv};
                        return Value{a->ty, static_cast<u64>(-static_cast<i64>(a->iv)), 0};
                    case UnKind::Not: return Value{ty_i1(), a->iv ? u64(0) : u64(1), 0};
                    case UnKind::BNot: return Value{a->ty, ~a->iv, 0};
                }
                return std::nullopt;
            }
            case ExprKind::Binary: {
                auto l = eval_env(*e.lhs, locals);
                auto r = eval_env(*e.rhs, locals);
                if (!l || !r) return std::nullopt;
                return eval_binop(*l, *r, e.bop, e.pos);
            }
            case ExprKind::Cast: {
                auto v = eval_env(*e.lhs, locals);
                if (!v) return std::nullopt;
                return cast_value(*v, e.cast_target);
            }
            case ExprKind::Call: {
                size_t fi = fn_index(e.callee);
                if (fi == SIZE_MAX) return std::nullopt;
                FnDecl& fn = mod_.fns[out_.fns[fi].ast_index];
                FlatMap<std::string, Value> env;
                for (size_t i = 0; i < e.args.size(); ++i) {
                    auto av = eval_env(*e.args[i], locals);
                    if (!av) return std::nullopt;
                    env.insert(fn.params[i].first, *av);
                }
                u64 saved = steps_;
                steps_ = 0;
                auto res = interpret_fn(fn, env);
                steps_ = saved;
                return res;
            }
            case ExprKind::ComptimeBlock:
                return eval_const(e, ty_none());
            default:
                return eval_const(e, ty_none());
        }
    }

    ModuleAst& mod_;
    SemaModule& out_;
    Diagnostics& diag_;
    SymbolTable& syms_;
    std::vector<Scope> scopes_;
    u64 steps_ = 0;
};

} // namespace

bool run_sema(ModuleAst& ast, SemaModule& out, Diagnostics& diag, SymbolTable& syms) {
    Sema s(ast, out, diag, syms);
    return s.run();
}

const char* ty_name(TypeId t) {
    switch (t) {
        case 0: return "<none>";
        case 1: return "void";
        case 2: return "bool";
        case 3: return "i32";
        case 4: return "i64";
        case 5: return "u32";
        case 6: return "u64";
        case 7: return "f32";
        case 8: return "f64";
        default: break;
    }
    const TypeDesc& d = kTypeTable[t < kTypeCount ? t : 0];
    if (d.ty == Ty::Ptr) {
        switch (d.pointee) {
            case Ty::I64: return "*i64";   // pointee only; constness is frontend-only
            case Ty::I32: return "*i32";
            case Ty::U64: return "*u64";
            case Ty::U32: return "*u32";
            case Ty::F32: return "*f32";
            case Ty::F64: return "*f64";
            case Ty::I1:  return "*bool";
            default: return "ptr";
        }
    }
    if (d.ty == Ty::Mem) return "<mem>";
    if (d.ty == Ty::Ctrl) return "<ctrl>";
    return "<bad>";
}

} // namespace jules
