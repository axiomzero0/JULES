// Recursive-descent + Pratt-expression parser for the JULES MVP subset.
// Grammar (MVP):
//   file       := module_decl? (const_decl | fn_decl)*
//   fn_decl    := attr* ("comptime")? "fn" IDENT "(" params ")" ("->" type)? block
//   block      := "{" stmt* "}"
//   stmt       := let | var | assign | deref-assign | return | if | while | for
//               | break | continue | expr ";"
//   expr       := pratt with `as` postfix and call postfix
#include "core/parser/ast.h"
#include "core/lexer/token.h"

#include <cstdlib>

namespace jules {

namespace {

constexpr int kMaxCallArgs = 10;      // matches SoN node input arity (MVP)
constexpr int kMaxParams   = 10;
constexpr u32 kMaxParseDepth = 512;   // guards pathological nesting

class Parser {
public:
    Parser(const std::vector<Token>& toks, Diagnostics& diag, SymbolTable& syms)
        : toks_(toks), diag_(diag), sym_(&syms) {}

    ModuleAst parse_module() {
        ModuleAst mod;
        maybe_module_decl(mod);
        while (!check(Tok::Eof)) {
            if (!parse_top_level(mod)) break;
        }
        expect(Tok::Eof, "end of file");
        return mod;
    }

private:
    // ---- token helpers -------------------------------------------------
    const Token& cur() const { return toks_[i_]; }
    bool check(Tok k) const { return cur().kind == k; }
    bool check_ident(const char* s) const {
        return cur().kind == Tok::Ident && cur().text == s;
    }
    const Token& advance() { return toks_[i_ < toks_.size() - 1 ? i_++ : i_]; }
    bool accept(Tok k) {
        if (check(k)) { advance(); return true; }
        return false;
    }
    bool expect(Tok k, const char* what) {
        if (check(k)) { advance(); return true; }
        if (!errored_) {
            diag_.error(cur().pos,
                        std::string("expected ") + what + " (" + tok_name(k) +
                        ") but found " + tok_name(cur().kind) + " '" + cur().text + "'");
            errored_ = true;
        } else {
            diag_.error(cur().pos, std::string("expected ") + what);
        }
        if (!check(Tok::Eof)) advance(); // resync
        return false;
    }

    // Recursion guard for pathological nesting.
    struct DepthGuard {
        Parser& p;
        SourcePos pos;
        explicit DepthGuard(Parser& parser, SourcePos at) : p(parser), pos(at) {
            if (++p.depth_ > kMaxParseDepth) {
                p.diag_.error(pos, "expression nesting too deep");
                p.errored_ = true;
            }
        }
        ~DepthGuard() { --p.depth_; }
    };

    // ---- attributes ----------------------------------------------------
    struct Attrs { bool always_inline = false; bool no_inline = false; };

    Attrs parse_attrs() {
        Attrs a;
        while (check(Tok::Attr)) {
            SourcePos pos = cur().pos;
            advance(); // '#['
            if (!check(Tok::Ident)) {
                diag_.error(pos, "expected attribute name after '#['");
                errored_ = true;
                break;
            }
            std::string name = cur().text;
            advance();
            std::string arg;
            if (accept(Tok::LParen)) {
                if (!check(Tok::Ident)) {
                    diag_.error(cur().pos, "expected attribute argument identifier");
                } else {
                    arg = cur().text;
                    advance();
                }
                expect(Tok::RParen, "')'");
            }
            expect(Tok::RBracket, "']'");
            if (name == "inline") {
                if (arg == "always") a.always_inline = true;
                else diag_.warn(pos, "#[inline] without 'always' treated as no_inline hint absent");
            } else if (name == "no_inline") {
                a.no_inline = true;
            } else if (name == "deprecated") {
                diag_.warn(pos, "#[deprecated] attribute noted");
            } else {
                diag_.error(pos, "unknown attribute '" + name + "' (attributes are validated; unknown ones are errors)");
                errored_ = true;
            }
        }
        return a;
    }

    // ---- top level -----------------------------------------------------
    void maybe_module_decl(ModuleAst& mod) {
        if (accept(Tok::KwModule)) {
            if (!check(Tok::Ident)) {
                diag_.error(cur().pos, "expected module name after 'module'");
                errored_ = true;
                return;
            }
            mod.module_name = cur().text;
            advance();
            // Optional :: path segments (recorded, single-module MVP).
            while (check(Tok::Colon)) {
                advance(); advance();
                if (check(Tok::Ident)) advance(); else break;
            }
            expect(Tok::Semi, "';' after module declaration");
        }
    }

    bool parse_top_level(ModuleAst& mod) {
        if (check(Tok::KwUse)) {
            diag_.error(cur().pos, "'use' imports are not part of the MVP subset; single-file modules only");
            errored_ = true;
            while (!check(Tok::Semi) && !check(Tok::Eof)) advance();
            accept(Tok::Semi);
            return !check(Tok::Eof);
        }
        Attrs attrs = parse_attrs();
        if (check(Tok::KwConst)) return parse_const(mod);
        bool is_comptime = accept(Tok::KwComptime);
        if (check(Tok::KwFn)) return parse_fn(mod, is_comptime, attrs);
        for (Tok reserved : {Tok::KwStruct, Tok::KwClass, Tok::KwEnum, Tok::KwBitfield,
                             Tok::KwBitmask, Tok::KwAlias, Tok::KwTrait, Tok::KwImpl}) {
            if (check(reserved)) {
                diag_.error(cur().pos, std::string(tok_name(reserved)) +
                            " declarations are planned for the next milestone (not in MVP subset)");
                errored_ = true;
                while (!check(Tok::RBrace) && !check(Tok::Eof)) advance();
                accept(Tok::RBrace);
                return !check(Tok::Eof);
            }
        }
        diag_.error(cur().pos, "expected 'fn', 'const' or attribute at top level");
        errored_ = true;
        advance();
        return !check(Tok::Eof);
    }

    bool parse_const(ModuleAst& mod) {
        ConstDecl c;
        c.pos = cur().pos;
        advance(); // const
        if (!check(Tok::Ident)) {
            diag_.error(cur().pos, "expected constant name");
            errored_ = true;
            return true;
        }
        c.name = intern_name(cur().text);
        c.pos = cur().pos;
        advance();
        if (accept(Tok::Colon)) c.ty = parse_type();
        expect(Tok::Assign, "'=' in const declaration");
        c.value = parse_expr();
        expect(Tok::Semi, "';' after const declaration");
        mod.consts.push_back(std::move(c));
        return true;
    }

    bool parse_fn(ModuleAst& mod, bool is_comptime, const Attrs& attrs) {
        FnDecl fn;
        fn.is_comptime = is_comptime;
        fn.always_inline = attrs.always_inline;
        fn.no_inline = attrs.no_inline;
        advance(); // fn
        if (!check(Tok::Ident)) {
            diag_.error(cur().pos, "expected function name");
            errored_ = true;
            return true;
        }
        fn.name = intern_name(cur().text);
        fn.pos = cur().pos;
        advance();
        expect(Tok::LParen, "'(' after function name");
        while (!check(Tok::RParen)) {
            if (!check(Tok::Ident)) {
                diag_.error(cur().pos, "expected parameter name");
                errored_ = true;
                break;
            }
            std::string pname = cur().text;
            advance();
            expect(Tok::Colon, "':' after parameter name");
            TypeId pt = parse_type();
            fn.params.emplace_back(std::move(pname), pt);
            if (!accept(Tok::Comma)) break;
        }
        expect(Tok::RParen, "')' after parameters");
        if (accept(Tok::Arrow)) fn.ret = parse_type();
        if (fn.params.size() > kMaxParams) {
            diag_.error(fn.pos, "too many parameters (MVP node input arity limit)");
            errored_ = true;
        }
        parse_block(fn.body);
        mod.fns.push_back(std::move(fn));
        return true;
    }

    // ---- types ---------------------------------------------------------
    TypeId parse_type() {
        TypeId t = parse_type_base();
        // pointer suffixes / prefixes handled in base
        return t;
    }
    TypeId parse_type_base() {
        // '*' 'const' type | '*' 'mut' type | scalar ident
        if (accept(Tok::Star)) {
            bool is_const = true;
            if (check_ident("const")) { advance(); is_const = true; }
            else if (check_ident("mut")) { advance(); is_const = false; }
            else {
                diag_.error(cur().pos, "expected 'const' or 'mut' after '*' in pointer type");
                errored_ = true;
            }
            (void)is_const; // mutability is a frontend check only (no alias model yet)
            TypeId pointee = parse_type_base();
            TypeId p = ty_ptr(pointee);
            if (p == ty_none()) {
                diag_.error(cur().pos, "pointer to non-scalar type is not in the MVP type lattice");
                errored_ = true;
            }
            return p;
        }
        if (!check(Tok::Ident)) {
            diag_.error(cur().pos, "expected type name");
            errored_ = true;
            return ty_i32();
        }
        std::string name = cur().text;
        SourcePos pos = cur().pos;
        advance();
        if (name == "i32") return ty_i32();
        if (name == "i64" || name == "usize") return ty_i64();
        if (name == "u32") return ty_u32();
        if (name == "u64") return ty_u64();
        if (name == "f32") return ty_f32();
        if (name == "f64") return ty_f64();
        if (name == "bool") return ty_i1();
        diag_.error(pos, "unknown type '" + name + "' in MVP subset (i32,i64,u32,u64,f32,f64,bool,usize, *const T, *mut T)");
        errored_ = true;
        return ty_i32();
    }

    // ---- statements ----------------------------------------------------
    void parse_block(std::vector<StmtP>& out) {
        if (!expect(Tok::LBrace, "'{' starting block")) return;
        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            if (parse_stmt(out) && errored_) {
                // error recovery: skip to next ';'
                while (!check(Tok::Semi) && !check(Tok::RBrace) && !check(Tok::Eof)) advance();
                accept(Tok::Semi);
                errored_ = false;
            }
        }
        expect(Tok::RBrace, "'}' closing block");
    }

    bool parse_stmt(std::vector<StmtP>& out) {
        StmtP s;
        switch (cur().kind) {
            case Tok::KwLet:
            case Tok::KwVar:  s = parse_let(); break;
            case Tok::KwReturn: s = parse_return(); break;
            case Tok::KwIf:   s = parse_if(); break;
            case Tok::KwWhile: s = parse_while(); break;
            case Tok::KwFor:  s = parse_for(); break;
            case Tok::KwBreak: {
                s = make_stmt(StmtKind::Break);
                advance();
                expect(Tok::Semi, "';' after 'break'");
                break;
            }
            case Tok::KwContinue: {
                s = make_stmt(StmtKind::Continue);
                advance();
                expect(Tok::Semi, "';' after 'continue'");
                break;
            }
            case Tok::KwDefer:
            case Tok::KwExtern:
                diag_.error(cur().pos, std::string(tok_name(cur().kind)) + " is not part of the MVP subset");
                errored_ = true;
                advance();
                return true;
            default: s = parse_expr_or_assign(); break;
        }
        if (s) out.push_back(std::move(s));
        return true;
    }

    StmtP make_stmt(StmtKind k) {
        auto s = std::make_unique<Stmt>();
        s->kind = k;
        s->pos = cur().pos;
        return s;
    }

    StmtP parse_let() {
        StmtP s = make_stmt(StmtKind::Let);
        s->immutable = check(Tok::KwLet);
        advance(); // let / var
        if (!check(Tok::Ident)) {
            diag_.error(cur().pos, "expected variable name");
            errored_ = true;
            return nullptr;
        }
        s->name = cur().text;
        advance();
        if (accept(Tok::Colon)) s->decl_ty = parse_type();
        if (accept(Tok::Assign)) s->value = parse_expr();
        else if (s->immutable) {
            diag_.error(s->pos, "'let' binding requires an initializer (no default init in MVP)");
            errored_ = true;
        }
        expect(Tok::Semi, "';' after local declaration");
        return s;
    }

    StmtP parse_return() {
        StmtP s = make_stmt(StmtKind::Return);
        advance();
        if (!check(Tok::Semi)) s->value = parse_expr();
        expect(Tok::Semi, "';' after 'return'");
        return s;
    }

    StmtP parse_if() {
        StmtP s = make_stmt(StmtKind::If);
        advance();
        s->cond = parse_expr();
        parse_block(s->body);
        if (accept(Tok::KwElse)) {
            if (check(Tok::KwIf)) {
                std::vector<StmtP> wrapper;
                wrapper.push_back(parse_if()); // else-if chains
                s->else_body = std::move(wrapper);
            } else {
                parse_block(s->else_body);
            }
        }
        return s;
    }

    StmtP parse_while() {
        StmtP s = make_stmt(StmtKind::While);
        advance();
        s->cond = parse_expr();
        parse_block(s->body);
        return s;
    }

    StmtP parse_for() {
        StmtP s = make_stmt(StmtKind::For);
        advance();
        if (!check(Tok::Ident)) {
            diag_.error(cur().pos, "expected loop variable name after 'for'");
            errored_ = true;
            return nullptr;
        }
        s->name = cur().text;
        advance();
        expect(Tok::KwIn, "'in' after loop variable");
        s->target = parse_expr(); // lower bound
        expect(Tok::DotDot, "'..' in range loop");
        s->to = parse_expr();     // upper bound (exclusive)
        parse_block(s->body);
        return s;
    }

    StmtP parse_expr_or_assign() {
        // Lookahead: IDENT '='  → assignment;  '*' expr '=' → deref store.
        if (check(Tok::Star)) {
            // possible deref-store or deref expression; parse expr then check '='
            SourcePos pos = cur().pos;
            ExprP e = parse_expr();
            if (accept(Tok::Assign)) {
                if (!e || e->kind != ExprKind::Deref) {
                    diag_.error(pos, "left side of a deref assignment must be '*pointer'");
                    errored_ = true;
                }
                StmtP s = make_stmt(StmtKind::AssignDeref);
                s->pos = pos;
                s->target = e ? std::move(e->lhs) : nullptr; // the POINTER
                s->value = parse_expr();
                expect(Tok::Semi, "';' after assignment");
                return s;
            }
            StmtP s = make_stmt(StmtKind::ExprStmt);
            s->pos = pos;
            s->value = std::move(e);
            expect(Tok::Semi, "';' after expression statement");
            return s;
        }
        if (check(Tok::Ident) && toks_[i_ + 1].kind == Tok::Assign) {
            StmtP s = make_stmt(StmtKind::Assign);
            s->name = cur().text;
            s->pos = cur().pos;
            advance(); advance();
            s->value = parse_expr();
            expect(Tok::Semi, "';' after assignment");
            return s;
        }
        StmtP s = make_stmt(StmtKind::ExprStmt);
        s->value = parse_expr();
        expect(Tok::Semi, "';' after expression statement");
        return s;
    }

    // ---- expressions (Pratt) --------------------------------------------
    struct Binding { int prec; };
    static int prec_of(Tok k) {
        switch (k) {
            case Tok::OrOr:    return 1;
            case Tok::AndAnd:  return 2;
            case Tok::Eq: case Tok::Ne: return 3;
            case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge: return 4;
            case Tok::Pipe:    return 5;
            case Tok::Caret:   return 6;
            case Tok::Amp:     return 7;
            case Tok::Shl: case Tok::Shr: return 8;
            case Tok::Plus: case Tok::Minus: return 9;
            case Tok::Star: case Tok::Slash: case Tok::Percent: return 10;
            default: return 0;
        }
    }

    ExprP parse_expr(int min_prec = 0) {
        DepthGuard guard(*this, cur().pos);
        if (errored_) return nullptr;
        ExprP lhs = parse_unary();
        if (errored_ || !lhs) return lhs;
        for (;;) {
            Tok k = cur().kind;
            int p = prec_of(k);
            if (p == 0 || p < min_prec) break;
            ExprP e = std::make_unique<Expr>();
            e->pos = cur().pos;
            e->kind = ExprKind::Binary;
            e->bop = bin_of(k);
            advance();
            e->lhs = std::move(lhs);
            e->rhs = parse_expr(p + 1);
            lhs = std::move(e);
            if (errored_ || !lhs->rhs) return lhs;
        }
        // postfix: as-cast and call handled in parse_postfix inside unary
        return lhs;
    }

    static BinKind bin_of(Tok k) {
        switch (k) {
            case Tok::Plus: return BinKind::Add;
            case Tok::Minus: return BinKind::Sub;
            case Tok::Star: return BinKind::Mul;
            case Tok::Slash: return BinKind::Div;
            case Tok::Percent: return BinKind::Mod;
            case Tok::Amp: return BinKind::And;
            case Tok::Pipe: return BinKind::Or;
            case Tok::Caret: return BinKind::Xor;
            case Tok::Shl: return BinKind::Shl;
            case Tok::Shr: return BinKind::Shr;
            case Tok::AndAnd: return BinKind::LogicAnd;
            case Tok::OrOr: return BinKind::LogicOr;
            case Tok::Eq: return BinKind::Eq;
            case Tok::Ne: return BinKind::Ne;
            case Tok::Lt: return BinKind::Lt;
            case Tok::Le: return BinKind::Le;
            case Tok::Gt: return BinKind::Gt;
            case Tok::Ge: return BinKind::Ge;
            default: return BinKind::Add;
        }
    }

    ExprP parse_unary() {
        UnKind uop;
        if (check(Tok::KwComptime)) {
            // `comptime { ... }` block expression or `comptime f(args)` call
            SourcePos pos = cur().pos;
            advance();
            if (check(Tok::LBrace)) {
                ExprP e = std::make_unique<Expr>();
                e->pos = pos;
                e->kind = ExprKind::ComptimeBlock;
                parse_block(e->block_body);
                return e;
            }
            ExprP e = parse_postfix();
            if (!e || e->kind != ExprKind::Call) {
                diag_.error(pos, "'comptime' expression must be a call or a block");
                errored_ = true;
                return e;
            }
            e->comptime_call = true;
            return e;
        }
        if (check(Tok::Minus)) uop = UnKind::Neg;
        else if (check(Tok::Not)) uop = UnKind::Not;
        else if (check(Tok::Tilde)) uop = UnKind::BNot;
        else if (check(Tok::Star)) return parse_postfix(); // deref handled in postfix
        else return parse_postfix();
        ExprP e = std::make_unique<Expr>();
        e->pos = cur().pos;
        e->kind = ExprKind::Unary;
        e->uop = uop;
        advance();
        e->lhs = parse_unary();
        return e;
    }

    ExprP parse_postfix() {
        ExprP e;
        if (check(Tok::Star)) {
            e = std::make_unique<Expr>();
            e->pos = cur().pos;
            e->kind = ExprKind::Deref;
            advance();
            e->lhs = parse_postfix();
            return e;
        }
        e = parse_primary();
        if (errored_ || !e) return e;
        for (;;) {
            if (check(Tok::KwAs)) {
                SourcePos pos = cur().pos;
                advance();
                TypeId t = parse_type();
                ExprP c = std::make_unique<Expr>();
                c->pos = pos;
                c->kind = ExprKind::Cast;
                c->cast_target = t;
                c->lhs = std::move(e);
                e = std::move(c);
                continue;
            }
            if (check(Tok::LParen)) {
                // call
                SourcePos pos = cur().pos;
                ExprP c = std::make_unique<Expr>();
                c->pos = pos;
                c->kind = ExprKind::Call;
                advance();
                while (!check(Tok::RParen) && !check(Tok::Eof)) {
                    c->args.push_back(parse_expr());
                    if (!accept(Tok::Comma)) break;
                }
                expect(Tok::RParen, "')' after arguments");
                if (c->args.size() > kMaxCallArgs) {
                    diag_.error(pos, "too many call arguments (MVP node input arity limit)");
                    errored_ = true;
                }
                c->callee = intern_name(e->name);
                c->name = e->name;
                e = std::move(c);
                continue;
            }
            break;
        }
        return e;
    }

    ExprP parse_primary() {
        switch (cur().kind) {
            case Tok::IntLit: {
                ExprP e = std::make_unique<Expr>();
                e->pos = cur().pos;
                e->kind = ExprKind::IntLit;
                e->iv = cur().int_value;
                e->name = cur().text;
                e->ty = cur().int_value <= static_cast<u64>(INT32_MAX) && !cur().is_signed_overflow
                            ? ty_i32() : ty_i64();
                advance();
                return e;
            }
            case Tok::FloatLit: {
                ExprP e = std::make_unique<Expr>();
                e->pos = cur().pos;
                e->kind = ExprKind::FloatLit;
                e->fv = cur().fp_value;
                e->name = cur().text;
                e->ty = ty_f64();
                advance();
                return e;
            }
            case Tok::KwTrue:
            case Tok::KwFalse: {
                ExprP e = std::make_unique<Expr>();
                e->pos = cur().pos;
                e->kind = ExprKind::BoolLit;
                e->iv = check(Tok::KwTrue) ? 1 : 0;
                e->ty = ty_i1();
                advance();
                return e;
            }
            case Tok::Ident: {
                ExprP e = std::make_unique<Expr>();
                e->pos = cur().pos;
                e->kind = ExprKind::Ident;
                e->name = cur().text;
                e->sym = intern_name(cur().text);
                advance();
                return e;
            }
            case Tok::LParen: {
                advance();
                ExprP e = parse_expr();
                expect(Tok::RParen, "')'");
                return e;
            }
            default:
                diag_.error(cur().pos, "expected expression but found " +
                            std::string(tok_name(cur().kind)));
                errored_ = true;
                advance();
                return nullptr;
        }
    }

    SymbolId intern_name(const std::string& name) { return sym_->intern(name); }

    const std::vector<Token>& toks_;
    size_t i_ = 0;
    u32 depth_ = 0;
    Diagnostics& diag_;
    bool errored_ = false;
    SymbolTable* sym_;
};

} // namespace

bool parse_tokens(const std::vector<Token>& toks, ModuleAst& out, Diagnostics& diag,
                  SymbolTable& syms) {
    Parser p(toks, diag, syms);
    out = p.parse_module();
    return !diag.has_errors();
}

} // namespace jules
