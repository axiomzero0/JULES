// Semantic analysis: name resolution, type checking (no implicit conversions),
// comptime evaluation of const decls and comptime-fn calls, builtin resolution
// (print / free / alloc<T>), user-defined types (struct / enum / bitmask /
// bitfield / alias), trait/impl method resolution (static dispatch), extern "C"
// declarations, and defer checking. Annotates the AST with result types.
#pragma once

#include "core/parser/ast.h"
#include "core/sema/sema_types.h"
#include "core/son/graph.h" // kExternFnBase / extern_fn_id (IR-side encoding)

namespace jules {

struct SemaFn {
    SymbolId name = kNoSymbol;
    TypeId ret = ty_void();
    std::vector<TypeId> params;
    bool is_comptime = false;
    bool always_inline = false;
    bool no_inline = false;
    size_t ast_index = 0;   // index into ModuleAst::fns
    u32 node_estimate = 0;  // rough size metric for the inlining cost model
    bool is_extern = false; // extern "C" declaration (bodyless, no IR graph)
};

struct SemaExtern {
    SymbolId name = kNoSymbol;
    TypeId ret = ty_void();
    std::vector<TypeId> params;
};

// Method resolution key: (receiver struct type, trait or kNoSymbol, method
// name) -> index into SemaModule::fns. Static dispatch only; `dyn` trait
// objects are deliberately not in the frontend yet (need vtable emission +
// indirect calls in the IR/backend first).
struct MethodKey {
    TypeId type = ty_none();
    u32 trait = kNoSymbol;
    u32 method = kNoSymbol;
    bool operator==(const MethodKey& o) const {
        return type == o.type && trait == o.trait && method == o.method;
    }
    bool operator<(const MethodKey& o) const {
        if (type != o.type) return type < o.type;
        if (trait != o.trait) return trait < o.trait;
        return method < o.method;
    }
};

struct SemaModule {
    std::vector<SemaFn> fns;
    FlatMap<SymbolId, size_t> fn_by_name;
    bool has_main = false;

    // User-defined types (sema-only TypeIds >= kUserTyBase; the builder
    // degrades them to lattice types — they never reach the IR).
    UserTypeTable user_types;

    // extern "C" declarations
    std::vector<SemaExtern> externs;
    FlatMap<SymbolId, size_t> extern_by_name;

    // trait name -> index into ModuleAst::traits (AST owns the signatures)
    FlatMap<SymbolId, u32> trait_index;

    // static method resolution table
    FlatMap<MethodKey, size_t> methods;

    // Semantic type helpers shared with the builder.
    // IR-facing type of a sema type: user int types degrade to their backing
    // integer; pointer-to-struct degrades to *i64 (address width; loads carry
    // the real pointee type — same convention as scalar slots).
    TypeId ir_ty(TypeId t) const {
        if (!ty_is_user(t)) return t;
        // fold alias chains to their base (user or lattice); an alias to a
        // scalar IS that scalar — never the pointer fallback
        const UserType* u = &user_types.get(t);
        int depth = 0;
        while (u->kind == UserKind::Alias && depth++ < 16) {
            if (!ty_is_user(u->target)) return u->target;
            u = &user_types.get(u->target);
        }
        switch (u->kind) {
            case UserKind::Enum:
            case UserKind::Bitmask:
            case UserKind::Bitfield:
                return u->backing;
            default:
                return ty_ptr(ty_i64()); // Struct + StructPtr degrade to *i64
        }
    }
    bool is_struct_type(TypeId t) const {
        return ty_is_user(t) && user_types.struct_of(t) != ty_none();
    }
    bool is_struct_ptr(TypeId t) const {
        return ty_is_user(t) && user_types.pointee_struct(t) != ty_none();
    }
    // User integer-backed (enum / bitmask / bitfield)?
    bool is_int_backed(TypeId t) const {
        return ty_is_user(t) && user_types.backing_of(t) != ty_none();
    }
};

bool run_sema(ModuleAst& ast, SemaModule& out, Diagnostics& diag, SymbolTable& syms);

} // namespace jules
