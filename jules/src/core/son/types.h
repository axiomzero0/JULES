// JULES type system: interned type ids over a fixed MVP type lattice.
// Shared by sema (semantic types) and SoN IR (node result types).
// Integer types carry signedness + width; Ptr carries a pointee.
#pragma once

#include "core/support/common.h"

namespace jules {

enum class Ty : u8 {
    None,    // internal sentinel
    Void,    // unit / no value
    I1,      // bool
    I32, I64, U32, U64,
    F32, F64,
    Ptr,     // *const T / *mut T (pointee below)
    Mem,     // memory-version pseudo type (internal)
    Ctrl,    // control pseudo type (internal)
};

struct TypeDesc {
    Ty ty = Ty::None;
    Ty pointee = Ty::None; // only meaningful for Ty::Ptr
};

using TypeId = u16;

// MVP lattice is small and fixed; the table is a constexpr global.
constexpr TypeDesc kTypeTable[] = {
    {Ty::None,  Ty::None},   // 0  None
    {Ty::Void,  Ty::None},   // 1  void
    {Ty::I1,    Ty::None},   // 2  bool
    {Ty::I32,   Ty::None},   // 3
    {Ty::I64,   Ty::None},   // 4
    {Ty::U32,   Ty::None},   // 5
    {Ty::U64,   Ty::None},   // 6
    {Ty::F32,   Ty::None},   // 7
    {Ty::F64,   Ty::None},   // 8
    {Ty::Ptr,   Ty::I64},    // 9  ptr to i64
    {Ty::Ptr,   Ty::I32},    // 10
    {Ty::Ptr,   Ty::U64},    // 11
    {Ty::Ptr,   Ty::U32},    // 12
    {Ty::Ptr,   Ty::F32},    // 13
    {Ty::Ptr,   Ty::F64},    // 14
    {Ty::Ptr,   Ty::I1},     // 15
    {Ty::Mem,   Ty::None},   // 16
    {Ty::Ctrl,  Ty::None},   // 17
};
constexpr u16 kTypeCount = sizeof(kTypeTable) / sizeof(kTypeTable[0]);

inline constexpr TypeId ty_none()   { return 0; }
inline constexpr TypeId ty_void()   { return 1; }
inline constexpr TypeId ty_i1()     { return 2; }
inline constexpr TypeId ty_i32()    { return 3; }
inline constexpr TypeId ty_i64()    { return 4; }
inline constexpr TypeId ty_u32()    { return 5; }
inline constexpr TypeId ty_u64()    { return 6; }
inline constexpr TypeId ty_f32()    { return 7; }
inline constexpr TypeId ty_f64()    { return 8; }
inline constexpr TypeId ty_ptr(TypeId pointee) {
    // ptr table entries start at 9, ordered as above; pointee must be scalar.
    switch (pointee) {
        case 4: return 9;   // ptr i64
        case 3: return 10;  // ptr i32
        case 6: return 11;  // ptr u64
        case 5: return 12;  // ptr u32
        case 7: return 13;  // ptr f32
        case 8: return 14;  // ptr f64
        case 2: return 15;  // ptr i1
        default: return 0;  // ptr to non-scalar pointee not in MVP lattice
    }
}
inline constexpr TypeId ty_mem()  { return 16; }
inline constexpr TypeId ty_ctrl() { return 17; }

inline constexpr TypeDesc type_desc(TypeId t) {
    return (t < kTypeCount) ? kTypeTable[t] : kTypeTable[0];
}
inline constexpr bool ty_is_int(TypeId t) {
    Ty k = type_desc(t).ty;
    return k == Ty::I32 || k == Ty::I64 || k == Ty::U32 || k == Ty::U64 || k == Ty::I1;
}
inline constexpr bool ty_is_signed(TypeId t) {
    Ty k = type_desc(t).ty;
    return k == Ty::I32 || k == Ty::I64 || k == Ty::I1;
}
inline constexpr bool ty_is_float(TypeId t) {
    Ty k = type_desc(t).ty;
    return k == Ty::F32 || k == Ty::F64;
}
inline constexpr bool ty_is_ptr(TypeId t) { return type_desc(t).ty == Ty::Ptr; }
inline constexpr bool ty_is_scalar(TypeId t) {
    return ty_is_int(t) || ty_is_float(t) || ty_is_ptr(t);
}
inline constexpr u32 ty_bits(TypeId t) {
    switch (type_desc(t).ty) {
        case Ty::I1:  return 8;   // storage width; logical 1 bit
        case Ty::I32: case Ty::U32: case Ty::F32: return 32;
        default: return 64;       // i64/u64/f64/ptr
    }
}
inline constexpr u32 ty_store_bytes(TypeId t) {
    switch (type_desc(t).ty) {
        case Ty::I1: return 1;
        case Ty::I32: case Ty::U32: case Ty::F32: return 4;
        default: return 8;
    }
}
inline constexpr bool ty_is_bool(TypeId t) { return type_desc(t).ty == Ty::I1; }

const char* ty_name(TypeId t);

} // namespace jules
