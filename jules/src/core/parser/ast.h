// AST for the JULES MVP subset. Tagged structs (no RTTI, no exceptions).
// Statements/expressions own children through unique_ptr.
#pragma once

#include "core/diagnostics/diag.h"
#include "core/support/common.h"
#include "core/support/symbols.h"
#include "core/son/types.h"

#include <memory>

namespace jules {

enum class ExprKind : u8 {
    IntLit, FloatLit, BoolLit, Ident, Unary, Binary, Call, Cast, Deref,
    Index,        // base[expr]: pointer indexing (arrays) — lhs = pointer, rhs = index
    ComptimeBlock, // comptime { ... } expression: sema evaluates to a literal
};
enum class BinKind : u8 {
    Add, Sub, Mul, Div, Mod, And, Or, Xor, Shl, Shr,
    LogicAnd, LogicOr, Eq, Ne, Lt, Le, Gt, Ge,
};
enum class UnKind : u8 { Neg, Not, BNot };

struct Expr;
struct Stmt;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

struct Expr {
    ExprKind kind;
    SourcePos pos;
    TypeId ty = ty_none();

    // literals / identifier
    u64 iv = 0;
    f64 fv = 0;
    std::string name;
    SymbolId sym = kNoSymbol;

    // operators
    BinKind bop = BinKind::Add;
    UnKind uop = UnKind::Neg;
    ExprP lhs, rhs;

    // call
    std::vector<ExprP> args;
    SymbolId callee = kNoSymbol;
    bool comptime_call = false; // `comptime f(x)` — force compile-time evaluation

    // comptime block body (ExprKind::ComptimeBlock)
    std::vector<StmtP> block_body;

    // cast target type (ExprKind::Cast); sema computes result `ty`
    TypeId cast_target = ty_none();

    // comptime-folded marker (set by sema when value was compile-time evaluated)
    bool comptime_value = false;
};

enum class StmtKind : u8 {
    Let,         // name, decl_ty (may be ty_none() for inferred), immutable, init
    Assign,      // target ident, value
    AssignDeref, // target = pointer expr, value
    AssignIndex, // base[index] = value  (target = base ptr, to = index expr)
    Return,      // value (optional)
    If,          // cond, body, else_body
    While,       // cond, body
    For,         // name, from, to, body  (desugared before codegen)
    ExprStmt,    // value
    Break,
    Continue,
};

struct Stmt {
    StmtKind kind;
    SourcePos pos;
    std::string name;   // Let / For loop var / Assign target
    TypeId decl_ty = ty_none();
    bool immutable = true;
    ExprP value;        // Return / Assign / Let init / ExprStmt
    ExprP cond;         // If / While
    ExprP target;       // AssignDeref pointer / For from / AssignIndex base
    ExprP to;           // For bound / AssignIndex index expression
    std::vector<StmtP> body;
    std::vector<StmtP> else_body;
};

struct FnDecl {
    SymbolId name = kNoSymbol;
    SourcePos pos;
    std::vector<std::pair<std::string, TypeId>> params;
    TypeId ret = ty_void();
    bool is_comptime = false;
    bool always_inline = false;
    bool no_inline = false;
    std::vector<StmtP> body;
};

struct ConstDecl {
    SymbolId name = kNoSymbol;
    SourcePos pos;
    TypeId ty = ty_none();
    ExprP value;        // original expression (kept for diagnostics)
    u64 iv = 0;         // evaluated value
    f64 fv = 0.0;
    bool is_fp = false;
};

struct ModuleAst {
    std::string module_name = "main";
    std::vector<ConstDecl> consts;
    std::vector<FnDecl> fns;
};

} // namespace jules
