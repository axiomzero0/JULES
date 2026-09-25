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
        mod_ = &mod;
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
        if (check(Tok::KwImport)) return parse_import(mod);
        Attrs attrs = parse_attrs();
        if (check(Tok::KwConst)) return parse_const(mod);
        bool is_comptime = accept(Tok::KwComptime);
        if (check(Tok::KwFn)) return parse_fn(mod, is_comptime, attrs);
        if (check(Tok::KwExtern)) return parse_extern_fn(mod);
        if (check(Tok::KwStruct)) return parse_struct(mod);
        if (check(Tok::KwEnum)) return parse_enum(mod);
        if (check(Tok::KwBitmask)) return parse_bitmask(mod);
        if (check(Tok::KwBitfield)) return parse_bitfield(mod);
        if (check(Tok::KwAlias)) return parse_alias(mod);
        if (check(Tok::KwTrait)) return parse_trait(mod);
        if (check(Tok::KwImpl)) return parse_impl(mod);
        if (check(Tok::KwClass)) {
            diag_.error(cur().pos, "'class' is not part of the JULES surface: the language is a "
                        "static systems language with value structs and explicit memory "
                        "(see docs/language_surface.md — 'struct' covers aggregate types, "
                        "allocation is alloc())");
            errored_ = true;
            while (!check(Tok::RBrace) && !check(Tok::Eof)) advance();
            accept(Tok::RBrace);
            return !check(Tok::Eof);
        }
        if (check(Tok::KwDyn)) {
            diag_.error(cur().pos, "'dyn' trait objects are not in the frontend yet: they require "
                        "vtable emission + indirect calls in the IR/backend (a backend "
                        "milestone); use static 'impl Trait for Type' dispatch");
            errored_ = true;
            advance();
            return !check(Tok::Eof);
        }
        diag_.error(cur().pos, "expected a declaration ('fn', 'const', 'struct', 'enum', 'bitmask', "
                    "'bitfield', 'alias', 'trait', 'impl', 'extern') at top level");
        errored_ = true;
        advance();
        return !check(Tok::Eof);
    }

    bool parse_import(ModuleAst& mod) {
        ImportDecl im;
        im.pos = cur().pos;
        advance(); // import
        if (!check(Tok::Ident)) {
            diag_.error(cur().pos, "expected an importable unit name after 'import' "
                        "(built-ins: 'strict' — the strict borrow checker)");
            errored_ = true;
            while (!check(Tok::Semi) && !check(Tok::Eof)) advance();
            accept(Tok::Semi);
            return !check(Tok::Eof);
        }
        im.name = intern_name(cur().text);
        im.pos = cur().pos;
        advance();
        // Optional :: path segments (recorded, single-module MVP; rejected
        // by sema — no importable units exist beyond the built-ins).
        while (check(Tok::Colon)) {
            advance(); advance();
            if (check(Tok::Ident)) {
                diag_.error(cur().pos, "'import' paths are not part of the MVP subset; "
                            "import built-in units by bare name ('import strict;')");
                errored_ = true;
                while (!check(Tok::Semi) && !check(Tok::Eof)) advance();
                accept(Tok::Semi);
                return !check(Tok::Eof);
            } else break;
        }
        expect(Tok::Semi, "';' after import");
        mod.imports.push_back(im);
        return true;
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
        if (!parse_fn_header(fn, "function name")) return true;
        parse_block(fn.body);
        mod.fns.push_back(std::move(fn));
        return true;
    }

    // ---- new declaration forms ------------------------------------------------
    bool expect_ident(const char* what, SymbolId& out) {
        if (!check(Tok::Ident)) {
            diag_.error(cur().pos, std::string("expected ") + what);
            errored_ = true;
            return false;
        }
        out = intern_name(cur().text);
        advance();
        return true;
    }

    bool parse_struct(ModuleAst& mod) {
        StructDecl sd;
        sd.pos = cur().pos;
        advance(); // struct
        if (!expect_ident("struct name", sd.name)) return true;
        if (!expect(Tok::LBrace, "'{' after struct name")) return true;
        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            SymbolId fname;
            if (!expect_ident("field name", fname)) break;
            expect(Tok::Colon, "':' after field name");
            TypeId ft = parse_type();
            sd.fields.emplace_back(fname, ft);
            if (!accept(Tok::Comma)) break;
        }
        expect(Tok::RBrace, "'}' after struct fields");
        accept(Tok::Semi); // optional
        mod.structs.push_back(std::move(sd));
        return true;
    }

    bool parse_enum(ModuleAst& mod) {
        EnumDecl ed;
        ed.pos = cur().pos;
        advance(); // enum
        if (!expect_ident("enum name", ed.name)) return true;
        if (accept(Tok::Colon)) {
            // backing integer: i32 (default) | i64 | u32 | u64
            if (!check(Tok::Ident)) {
                diag_.error(cur().pos, "expected backing integer type after 'enum Name:'");
                errored_ = true;
            } else {
                std::string b = cur().text; advance();
                if (b == "i32") ed.backing = ty_i32();
                else if (b == "i64") ed.backing = ty_i64();
                else if (b == "u32") ed.backing = ty_u32();
                else if (b == "u64") ed.backing = ty_u64();
                else {
                    diag_.error(ed.pos, "enum backing must be one of i32/i64/u32/u64 (got '" + b + "')");
                    errored_ = true;
                }
            }
        }
        if (!expect(Tok::LBrace, "'{' after enum name")) return true;
        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            SymbolId vname;
            if (!expect_ident("variant name", vname)) break;
            ExprP value;
            if (accept(Tok::Assign)) value = parse_expr();
            ed.variants.emplace_back(vname, std::move(value));
            if (!accept(Tok::Comma)) break;
        }
        expect(Tok::RBrace, "'}' after enum variants");
        accept(Tok::Semi);
        mod.enums.push_back(std::move(ed));
        return true;
    }

    bool parse_bitmask(ModuleAst& mod) {
        BitmaskDecl bd;
        bd.pos = cur().pos;
        advance(); // bitmask
        if (!expect_ident("bitmask name", bd.name)) return true;
        if (!expect(Tok::LBrace, "'{' after bitmask name")) return true;
        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            SymbolId vname;
            if (!expect_ident("flag name", vname)) break;
            ExprP value;
            if (accept(Tok::Assign)) value = parse_expr();
            bd.variants.emplace_back(vname, std::move(value));
            if (!accept(Tok::Comma)) break;
        }
        expect(Tok::RBrace, "'}' after bitmask flags");
        accept(Tok::Semi);
        mod.bitmasks.push_back(std::move(bd));
        return true;
    }

    bool parse_bitfield(ModuleAst& mod) {
        BitfieldDecl bd;
        bd.pos = cur().pos;
        advance(); // bitfield
        if (!expect_ident("bitfield name", bd.name)) return true;
        if (!expect(Tok::LParen, "'(' after bitfield name")) return true;
        if (!check(Tok::Ident) || cur().text != "u64") {
            diag_.error(cur().pos, "bitfield backing type must be u64 in the MVP");
            errored_ = true;
        } else {
            advance();
        }
        expect(Tok::RParen, "')' after bitfield backing");
        if (!expect(Tok::LBrace, "'{' after bitfield header")) return true;
        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            SymbolId fname;
            if (!expect_ident("segment name", fname)) break;
            expect(Tok::Colon, "':' after segment name");
            if (!check(Tok::IntLit)) {
                diag_.error(cur().pos, "expected a bit-width integer literal after ':'");
                errored_ = true;
                break;
            }
            u32 width = static_cast<u32>(cur().int_value);
            if (width == 0 || width > 64) {
                diag_.error(cur().pos, "bitfield segment width must be in 1..64");
                errored_ = true;
            }
            advance();
            bd.segs.emplace_back(fname, width);
            if (!accept(Tok::Comma)) break;
        }
        expect(Tok::RBrace, "'}' after bitfield segments");
        accept(Tok::Semi);
        mod.bitfields.push_back(std::move(bd));
        return true;
    }

    bool parse_alias(ModuleAst& mod) {
        AliasDecl ad;
        ad.pos = cur().pos;
        advance(); // alias
        if (!expect_ident("alias name", ad.name)) return true;
        expect(Tok::Assign, "'=' in alias declaration");
        ad.target = parse_type();
        expect(Tok::Semi, "';' after alias declaration");
        mod.aliases.push_back(std::move(ad));
        return true;
    }

    // fn header + body parsing shared by plain fns, impl methods, trait sigs.
    // header: name (self-able) params ret. Returns false on hard error.
    bool parse_fn_header(FnDecl& fn, const char* what) {
        advance(); // fn
        if (!check(Tok::Ident)) {
            diag_.error(cur().pos, std::string("expected ") + what);
            errored_ = true;
            return false;
        }
        fn.name = intern_name(cur().text);
        fn.pos = cur().pos;
        advance();
        expect(Tok::LParen, "'(' after function name");
        bool first = true;
        while (!check(Tok::RParen)) {
            if (!first) {
                if (!expect(Tok::Comma, "',' between parameters")) break;
                if (check(Tok::RParen)) break; // trailing comma
            }
            first = false;
            // self receiver: `&self` | `&mut self` | `self`
            if (check(Tok::Amp) || check_ident("self")) {
                u8 self_kind = kSelfNone;
                if (accept(Tok::Amp)) {
                    if (check_ident("mut")) { advance(); self_kind = kSelfMut; }
                    else self_kind = kSelfRef;
                } else {
                    self_kind = kSelfNone; // by value (sema rejects on structs)
                }
                if (!check_ident("self")) {
                    diag_.error(cur().pos, "expected 'self' after '" +
                                std::string(self_kind == kSelfMut ? "&mut" : "&") + "'");
                    errored_ = true;
                    break;
                }
                advance();
                fn.self_kind = self_kind == kSelfNone ? 3 : self_kind; // 3 = by-value self
                fn.params.emplace_back("self", ty_none()); // type filled by sema
                continue;
            }
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
        }
        expect(Tok::RParen, "')' after parameters");
        if (accept(Tok::Arrow)) fn.ret = parse_type();
        if (fn.params.size() > kMaxParams) {
            diag_.error(fn.pos, "too many parameters (MVP node input arity limit)");
            errored_ = true;
        }
        return true;
    }

    bool parse_trait(ModuleAst& mod) {
        TraitDecl td;
        td.pos = cur().pos;
        advance(); // trait
        if (!expect_ident("trait name", td.name)) return true;
        if (!expect(Tok::LBrace, "'{' after trait name")) return true;
        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            if (check(Tok::KwFn)) {
                TraitMethod tm;
                tm.pos = cur().pos;
                FnDecl header;
                if (!parse_fn_header(header, "method name")) break;
                tm.name = header.name;
                tm.params = std::move(header.params);
                tm.ret = header.ret;
                tm.self_kind = header.self_kind;
                td.methods.push_back(std::move(tm));
                expect(Tok::Semi, "';' after trait method signature");
            } else {
                diag_.error(cur().pos, "expected 'fn' inside trait declaration");
                errored_ = true;
                advance();
            }
        }
        expect(Tok::RBrace, "'}' after trait methods");
        accept(Tok::Semi);
        mod.traits.push_back(std::move(td));
        return true;
    }

    bool parse_impl(ModuleAst& mod) {
        ImplDecl id;
        id.pos = cur().pos;
        advance(); // impl
        SymbolId first;
        if (!expect_ident("type or trait name after 'impl'", first)) return true;
        if (check(Tok::KwFor)) { // 'for' is a keyword, not an identifier
            advance();
            id.trait_name = first;
            if (!expect_ident("type name after 'for'", id.type_name)) return true;
        } else {
            id.type_name = first; // inherent impl
        }
        if (!expect(Tok::LBrace, "'{' after impl header")) return true;
        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            if (check(Tok::KwFn)) {
                FnDecl m;
                m.self_kind = kSelfNone;
                if (!parse_fn_header(m, "method name")) break;
                parse_block(m.body);
                id.methods.push_back(std::move(m));
            } else {
                diag_.error(cur().pos, "expected 'fn' inside impl block");
                errored_ = true;
                advance();
            }
        }
        expect(Tok::RBrace, "'}' after impl methods");
        accept(Tok::Semi);
        mod.impls.push_back(std::move(id));
        return true;
    }

    bool parse_extern_fn(ModuleAst& mod) {
        FnDecl fn;
        fn.is_extern = true;
        fn.pos = cur().pos;
        advance(); // extern
        if (!expect(Tok::KwFn, "'fn' after 'extern'")) return true;
        if (!check(Tok::Ident)) {
            diag_.error(cur().pos, "expected extern function name");
            errored_ = true;
            return true;
        }
        fn.name = intern_name(cur().text);
        fn.pos = cur().pos;
        advance();
        expect(Tok::LParen, "'(' after extern function name");
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
        expect(Tok::Semi, "';' after extern declaration (no body)");
        if (fn.params.size() > 6) {
            diag_.error(fn.pos, "extern functions are limited to 6 parameters in the MVP "
                        "(SysV GP register count; no stack-passing support yet)");
            errored_ = true;
        }
        mod.fns.push_back(std::move(fn));
        return true;
    }
    TypeId parse_type() {
        TypeId t = parse_type_base();
        return t;
    }
    TypeId parse_type_base() {
        // '*' 'const' type | '*' 'mut' type | scalar ident | user type name (pending)
        if (accept(Tok::Star)) {
            bool is_const = true;
            if (check(Tok::KwConst)) { advance(); is_const = true; } // 'const' is a KEYWORD
            else if (check_ident("mut")) { advance(); is_const = false; }
            else {
                diag_.error(cur().pos, "expected 'const' or 'mut' after '*' in pointer type");
                errored_ = true;
            }
            last_type_ptr_const_ = is_const; // consumed by the `as`-cast site
            SourcePos ppos = cur().pos;
            // Reject pointer-to-pointer outright (not in the MVP lattice,
            // and no user type is ever a pointer).
            if (check(Tok::Star)) {
                diag_.error(ppos, "pointer to pointer is not in the MVP subset");
                errored_ = true;
                return ty_none();
            }
            TypeId pointee = parse_type_base();
            if (pointee == ty_none()) return ty_none();
            if (!ModuleAst::is_pending(pointee)) {
                TypeId p = ty_ptr(pointee);
                if (p == ty_none()) {
                    diag_.error(cur().pos, "pointer to non-scalar type is not in the MVP type lattice");
                    errored_ = true;
                }
                return p;
            }
            // Pointer to a (possibly user) type: record a pending pointer slot.
            const TypeSlot& slot = mod_->type_slots[pointee & ~kPendingTyFlag];
            TypeId p = mod_->pending_type(slot.name, true, ppos);
            if (p == ty_none()) {
                diag_.error(ppos, "too many distinct types (parser slot limit)");
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
        // User type name: resolve scalar lattice candidates via ty_ptr check is
        // not needed — sema resolves; record a pending slot.
        TypeId p = mod_->pending_type(name, false, pos);
        if (p == ty_none()) {
            diag_.error(pos, "too many distinct types (parser slot limit)");
            errored_ = true;
        }
        return p;
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
            case Tok::LBrace: {
                // bare block statement: its own scope
                s = make_stmt(StmtKind::Block);
                parse_block(s->body);
                break;
            }
            case Tok::KwDefer: {
                // defer <single stmt> | defer { ... }  (no semicolon required
                // after the block form; the single-statement form consumes
                // its own terminator)
                s = make_stmt(StmtKind::Defer);
                advance();
                if (check(Tok::LBrace)) {
                    parse_block(s->body);
                } else {
                    std::vector<StmtP> one;
                    parse_stmt(one);
                    if (one.size() == 1) {
                        s->body.push_back(std::move(one[0]));
                    } else if (!errored_) {
                        diag_.error(s->pos, "expected a statement after 'defer'");
                        errored_ = true;
                        s.reset();
                    }
                }
                break;
            }
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
        if (accept(Tok::Assign)) {
            // Field store: base.name = value (parsed as a Field expression)
            if (s->value && s->value->kind == ExprKind::Field) {
                StmtP a = make_stmt(StmtKind::AssignField);
                a->pos = s->value->pos;
                a->target = std::move(s->value->lhs); // struct base
                a->name = s->value->name;             // field name
                a->value = parse_expr();
                expect(Tok::Semi, "';' after assignment");
                return a;
            }
            // Indexed store: base[idx] = value (parsed as an Index expression).
            if (s->value && s->value->kind == ExprKind::Index) {
                StmtP a = make_stmt(StmtKind::AssignIndex);
                a->pos = s->value->pos;
                a->target = std::move(s->value->lhs); // base pointer
                a->to = std::move(s->value->rhs);     // index expression
                a->value = parse_expr();
                expect(Tok::Semi, "';' after assignment");
                return a;
            }
            diag_.error(cur().pos,
                        "left side of assignment must be a name, '*pointer' or 'base[index]'");
            errored_ = true;
            expect(Tok::Semi, "';' after assignment");
            return s;
        }
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
        else if (check(Tok::Amp)) {
            // address-of: &expr (unary position; binary & never starts an expr)
            ExprP e = std::make_unique<Expr>();
            e->pos = cur().pos;
            e->kind = ExprKind::AddrOf;
            advance();
            e->lhs = parse_unary();
            return e;
        }
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
                last_type_ptr_const_ = false;
                TypeId t = parse_type();
                ExprP c = std::make_unique<Expr>();
                c->pos = pos;
                c->kind = ExprKind::Cast;
                c->cast_target = t;
                c->cast_ptr_const = last_type_ptr_const_; // *const vs *mut
                c->lhs = std::move(e);
                e = std::move(c);
                continue;
            }
            if (check(Tok::LBracket)) {
                // pointer indexing: base[expr] — arrays are *mut T from alloc(T, n)
                SourcePos pos = cur().pos;
                ExprP c = std::make_unique<Expr>();
                c->pos = pos;
                c->kind = ExprKind::Index;
                advance();
                c->lhs = std::move(e);
                c->rhs = parse_expr();
                expect(Tok::RBracket, "']' after index");
                e = std::move(c);
                continue;
            }
            if (check(Tok::Dot)) {
                // field access / qualified constant / method call
                SourcePos pos = cur().pos;
                advance();
                if (!check(Tok::Ident)) {
                    diag_.error(pos, "expected a field or method name after '.'");
                    errored_ = true;
                    break;
                }
                std::string member = cur().text;
                advance();
                if (check(Tok::LParen)) {
                    ExprP c = std::make_unique<Expr>();
                    c->pos = pos;
                    c->kind = ExprKind::MethodCall;
                    c->name = member;
                    advance(); // '('
                    while (!check(Tok::RParen) && !check(Tok::Eof)) {
                        c->args.push_back(parse_expr());
                        if (!accept(Tok::Comma)) break;
                    }
                    expect(Tok::RParen, "')' after method arguments");
                    if (c->args.size() > kMaxCallArgs) {
                        diag_.error(pos, "too many method arguments (MVP node input arity limit)");
                        errored_ = true;
                    }
                    c->lhs = std::move(e); // receiver
                    e = std::move(c);
                } else {
                    ExprP c = std::make_unique<Expr>();
                    c->pos = pos;
                    c->kind = ExprKind::Field;
                    c->name = member;
                    c->lhs = std::move(e); // base
                    e = std::move(c);
                }
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
                // Struct literal lookahead: Ident '{' (Ident ':' | '}') — the
                // Rust rule. Disambiguates `if x { y }` (block) from
                // `Point { x: 1.0 }` (literal). Named fields only.
                if (check(Tok::LBrace)) {
                    // named-field literal only: Ident '{' Ident ':' — never
                    // '}' (an empty block after a condition is a block, and
                    // empty struct literals are not expressible anyway)
                    size_t la = i_ + 1;
                    bool is_lit = toks_[la].kind == Tok::Ident &&
                                  toks_[la + 1].kind == Tok::Colon;
                    if (is_lit) {
                        e->kind = ExprKind::StructLit;
                        advance(); // '{'
                        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
                            if (!check(Tok::Ident)) {
                                diag_.error(cur().pos, "expected a field name in struct literal");
                                errored_ = true;
                                break;
                            }
                            e->field_names.push_back(cur().text);
                            advance();
                            expect(Tok::Colon, "':' after field name in struct literal");
                            e->args.push_back(parse_expr());
                            if (!accept(Tok::Comma)) break;
                        }
                        expect(Tok::RBrace, "'}' after struct literal fields");
                    }
                }
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
    ModuleAst* mod_ = nullptr; // pending-type slot registry (set by parse_module)
    bool last_type_ptr_const_ = false; // parse_type_base's '*' const bit,
                                       // consumed by the `as`-cast site
};

} // namespace

bool parse_tokens(const std::vector<Token>& toks, ModuleAst& out, Diagnostics& diag,
                  SymbolTable& syms) {
    Parser p(toks, diag, syms);
    out = p.parse_module();
    return !diag.has_errors();
}

} // namespace jules
