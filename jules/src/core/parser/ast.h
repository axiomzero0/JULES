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
    StructLit,    // Name { field: expr, ... }: struct literal (named fields only)
    Field,        // base.name: struct field access / qualified enum-bitmask constant
    MethodCall,   // base.name(args): method call (receiver-bound)
    AddrOf,       // &expr: address of a struct lvalue / pointer-to-struct deref target
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

    // Struct-aware annotations (sema-only; the builder consumes them):
    //  - StructLit: `name` = struct name, `fields` = (field name, value) list
    //  - Field: `name` = field name; on enum/bitmask/bitfield types the
    //    qualified constant `Type.Variant` folds to a literal at check time
    //  - MethodCall: `name` = method name
    //  (the struct's representation — decomposed slots vs contiguous — is
    //  inferred at lowering from the builder's vars_ state + the Let's
    //  struct_in_mem flag; no per-expression annotation is needed)

    // StructLit field initializers: parallel names + values.
    std::vector<std::string> field_names;
};

enum class StmtKind : u8 {
    Let,         // name, decl_ty (may be ty_none() for inferred), immutable, init
    Assign,      // target ident, value
    AssignDeref, // target = pointer expr, value
    AssignIndex, // base[index] = value  (target = base ptr, to = index expr)
    AssignField, // base.name = value (target = struct base, name = field)
    Return,      // value (optional)
    If,          // cond, body, else_body
    While,       // cond, body
    For,         // name, from, to, body  (desugared before codegen)
    ExprStmt,    // value
    Break,
    Continue,
    Defer,       // body runs when the enclosing scope exits (LIFO), however it
                 // exits: fall-through, break, continue, return. No exceptions
                 // exist in JULES, so lowering = duplication at every exit.
    Block,       // bare '{ ... }' statement: its own scope (defers, shadowing)
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

    // Struct locals the escape analysis forced into contiguous memory
    // (address taken / passed by value). Consumed by the builder.
    bool struct_in_mem = false;
};

struct FnDecl {
    SymbolId name = kNoSymbol;
    SymbolId name_orig = kNoSymbol; // methods: pre-mangling name (dispatch key)
    SourcePos pos;
    std::vector<std::pair<std::string, TypeId>> params;
    TypeId ret = ty_void();
    bool is_comptime = false;
    bool always_inline = false;
    bool no_inline = false;
    bool is_extern = false;  // extern "C" declaration (bodyless): C ABI call via
                             // the system linker; scalar params/ret only
    u8 self_kind = 0;       // methods: 0 = plain fn, 1 = &self, 2 = &mut self
    TypeId self_type = ty_none(); // methods: the receiver struct (or ptr-to-struct)
    std::vector<StmtP> body;
};

// Method receiver modes (FnDecl::self_kind)
inline constexpr u8 kSelfNone = 0;
inline constexpr u8 kSelfRef = 1;
inline constexpr u8 kSelfMut = 2;

struct StructDecl {
    SymbolId name = kNoSymbol;
    SourcePos pos;
    std::vector<std::pair<SymbolId, TypeId>> fields; // in declaration order
};

struct EnumDecl {
    SymbolId name = kNoSymbol;
    SourcePos pos;
    TypeId backing = ty_i32();             // : i32|i64|u32|u64 (default i32)
    std::vector<std::pair<SymbolId, ExprP>> variants; // value may be null (auto)
};

struct BitmaskDecl {
    SymbolId name = kNoSymbol;
    SourcePos pos;
    std::vector<std::pair<SymbolId, ExprP>> variants; // null value = next pow2
};

struct BitfieldDecl {
    SymbolId name = kNoSymbol;
    SourcePos pos;
    TypeId backing = ty_u64();              // u64 backing in the MVP
    std::vector<std::pair<SymbolId, u32>> segs; // field name -> bit width
};

struct AliasDecl {
    SymbolId name = kNoSymbol;
    SourcePos pos;
    TypeId target = ty_none();
};

struct TraitMethod {
    SymbolId name = kNoSymbol;
    SourcePos pos;
    std::vector<std::pair<std::string, TypeId>> params; // includes self, if any
    TypeId ret = ty_void();
    u8 self_kind = kSelfNone;
    TypeId self_type = ty_none();
};

struct TraitDecl {
    SymbolId name = kNoSymbol;
    SourcePos pos;
    std::vector<TraitMethod> methods;
};

struct ImplDecl {
    SymbolId type_name = kNoSymbol;
    SymbolId trait_name = kNoSymbol; // kNoSymbol = inherent impl
    SourcePos pos;
    std::vector<FnDecl> methods;
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

// Parser-level type slots: the parser cannot know user type names (they are
// resolved by sema), so an unrecognized type name (or a pointer to one) is
// recorded here and encoded as TypeId kPendingTyFlag | slot. Sema resolves
// every pending TypeId in place during checking.
inline constexpr TypeId kPendingTyFlag = 0x4000;
inline constexpr TypeId kPendingTyMax = 0x7FFF; // 16k slots

struct TypeSlot {
    std::string name;
    bool is_ptr = false; // pointer to the named type
    SourcePos pos;
};

struct ModuleAst {
    std::string module_name = "main";
    std::vector<ConstDecl> consts;
    std::vector<FnDecl> fns;      // includes impl methods (moved + mangled by sema)
    std::vector<StructDecl> structs;
    std::vector<EnumDecl> enums;
    std::vector<BitmaskDecl> bitmasks;
    std::vector<BitfieldDecl> bitfields;
    std::vector<AliasDecl> aliases;
    std::vector<TraitDecl> traits;
    std::vector<ImplDecl> impls;
    std::vector<TypeSlot> type_slots; // parser -> sema pending type names

    TypeId pending_type(const std::string& name, bool is_ptr, SourcePos pos) {
        if (type_slots.size() > kPendingTyMax - kPendingTyFlag)
            return ty_none(); // caller reports the diagnostic
        TypeSlot s{name, is_ptr, pos};
        type_slots.push_back(std::move(s));
        return static_cast<TypeId>(kPendingTyFlag | (type_slots.size() - 1));
    }
    static bool is_pending(TypeId t) {
        // lattice tops out at 21; user/resolved ids live at >= kUserTyBase(32)
        // which never has bit 14 set alone. Pending = flag | slot, and the
        // slot range matches pending_type's 16k cap exactly.
        return t != ty_none() && (t & kPendingTyFlag) != 0 &&
               (t & ~kPendingTyFlag) < (kPendingTyMax - kPendingTyFlag + 1);
    }
};

} // namespace jules
