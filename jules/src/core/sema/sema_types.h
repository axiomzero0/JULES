// User-defined types for the JULES frontend: structs, pointer-to-struct,
// enums, bitmasks, bitfields and aliases. These live ABOVE the fixed MVP
// scalar lattice (types.h) in TypeId space:
//
//   TypeId 0..21  : fixed lattice (scalars, pointers, vectors)
//   TypeId 32+    : per-module user types (index = t - kUserTyBase)
//
// User TypeIds are SEMA-ONLY: they appear on AST Expr::ty / decl types and
// in these tables, but never on IR nodes — the graph builder degrades them
// to lattice types (backing integer for enum/bitmask/bitfield, i64-width
// address for pointer-to-struct) at every materialization site. This keeps
// the 89-pass optimizer and the backends entirely unaware of user types.
//
// Zero IR/passes changes is a deliberate design constraint (documented in
// docs/language_surface.md).
#pragma once

#include "core/support/common.h"
#include "core/support/symbols.h"
#include "core/son/types.h"

#include <string>
#include <vector>

namespace jules {

inline constexpr TypeId kUserTyBase = 32;

enum class UserKind : u8 {
    Struct,    // value type: flat field list + memory layout
    StructPtr, // *const S / *mut S where S is a user struct
    Enum,      // integer-backed, named variants
    Bitmask,   // u64-backed flag set, | & ^ ~ ==
    Bitfield,  // u64-backed packed bit ranges
    Alias,     // transparent name for another type
};

struct StructField {
    SymbolId name = kNoSymbol;
    TypeId ty = ty_none(); // scalar | lattice ptr | user struct (nested value)
    u32 offset = 0;       // byte offset within the struct (after layout)
    u32 size = 0;          // flattened byte size of the field
};

struct BitSeg { // bitfield segment: value occupies bits [shift, shift+width)
    SymbolId name = kNoSymbol;
    u32 width = 0;
    u32 shift = 0;
};

struct UserType {
    UserKind kind = UserKind::Struct;
    SymbolId name = kNoSymbol;
    SourcePos pos;

    // Struct / StructPtr (StructPtr.target = the struct's TypeId)
    std::vector<StructField> fields;
    u32 size = 0;  // total flattened bytes (StructPtr: 8)
    u32 align = 1; // max field alignment (StructPtr: 8)

    // Enum / Bitmask / Bitfield
    TypeId backing = ty_i32(); // lattice integer type
    std::vector<std::pair<SymbolId, u64>> variants; // enum/bitmask constants
    std::vector<BitSeg> segs;                       // bitfield layout

    // Alias
    TypeId target = ty_none();

    // transient layout state (not part of the language semantics)
    bool laying_out = false;
};

inline constexpr bool ty_is_user(TypeId t) { return t >= kUserTyBase; }
inline constexpr TypeId user_index_to_type(u32 i) {
    return static_cast<TypeId>(kUserTyBase + i);
}

// ---- shared helpers over a user-type table (the module's SemaModule) --------
struct UserTypeTable {
    std::vector<UserType> types;
    FlatMap<SymbolId, TypeId> by_name;

    const UserType& get(TypeId t) const { return types[t - kUserTyBase]; }
    UserType& get_mut(TypeId t) { return types[t - kUserTyBase]; }

    bool is_kind(TypeId t, UserKind k) const {
        return ty_is_user(t) && get(t).kind == k;
    }
    // A struct value type (Struct, or Alias chain ending in Struct)
    TypeId struct_of(TypeId t) const {
        const UserType* u = &get(t);
        while (u->kind == UserKind::Alias) {
            if (!ty_is_user(u->target)) return ty_none();
            u = &get(u->target);
        }
        if (u->kind != UserKind::Struct) return ty_none();
        return kUserTyBase + static_cast<TypeId>(u - types.data());
    }
    // StructPtr: the struct TypeId it points at
    TypeId pointee_struct(TypeId t) const {
        const UserType* u = &get(t);
        while (u->kind == UserKind::Alias) {
            if (!ty_is_user(u->target)) return ty_none();
            u = &get(u->target);
        }
        return u->kind == UserKind::StructPtr ? u->target : ty_none();
    }
    // Any user type: resolve aliases, return the underlying kind
    UserKind kind_of(TypeId t) const { return get(t).kind; }
    // Backing lattice type for enum/bitmask/bitfield (ty_none otherwise)
    TypeId backing_of(TypeId t) const {
        const UserType* u = &get(t);
        while (u->kind == UserKind::Alias) {
            if (!ty_is_user(u->target)) return ty_none();
            u = &get(u->target);
        }
        switch (u->kind) {
            case UserKind::Enum: case UserKind::Bitmask: case UserKind::Bitfield:
                return u->backing;
            default: return ty_none();
        }
    }
    u32 store_size_of(TypeId t) const {
        const UserType* u = &get(t);
        while (u->kind == UserKind::Alias) {
            if (!ty_is_user(u->target)) return 0;
            u = &get(u->target);
        }
        switch (u->kind) {
            case UserKind::StructPtr: return 8;
            case UserKind::Enum: case UserKind::Bitmask: case UserKind::Bitfield:
                return ty_store_bytes(u->backing);
            default: return u->size;
        }
    }
};

} // namespace jules
