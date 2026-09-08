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
    // Packed vector types (internal to the optimizer: the language has no
    // vector surface; passes 54-66 construct them). 128-bit SSE2-wide —
    // the universal x86-64 baseline. The `pointee` field carries the LANE
    // type.
    V2F64,   // 2 x f64 (addpd/mulpd/...)
    V2I64,   // 2 x i64 (paddq/psubq — no SIMD i64 multiply below AVX512DQ)
    V4I32,   // 4 x i32 (paddd/psubd; pmulld needs SSE4.1 — cost model gates)
    V4F32,   // 4 x f32 (addps/mulps/...)
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
    {Ty::V2F64, Ty::F64},    // 18 2 x f64
    {Ty::V2I64, Ty::I64},    // 19 2 x i64
    {Ty::V4I32, Ty::I32},    // 20 4 x i32
    {Ty::V4F32, Ty::F32},    // 21 4 x f32
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
inline constexpr TypeId ty_v2f64() { return 18; }
inline constexpr TypeId ty_v2i64() { return 19; }
inline constexpr TypeId ty_v4i32() { return 20; }
inline constexpr TypeId ty_v4f32() { return 21; }

inline constexpr TypeId ty_mem()  { return 16; }
inline constexpr TypeId ty_ctrl() { return 17; }

inline constexpr TypeDesc type_desc(TypeId t) {
    return (t < kTypeCount) ? kTypeTable[t] : kTypeTable[0];
}
inline constexpr bool ty_is_vector(TypeId t) {
    Ty k = type_desc(t).ty;
    return k == Ty::V2F64 || k == Ty::V2I64 || k == Ty::V4I32 || k == Ty::V4F32;
}
inline constexpr u32 ty_lanes(TypeId t) {
    if (!ty_is_vector(t)) return 1;
    return type_desc(t).ty == Ty::V2F64 || type_desc(t).ty == Ty::V2I64 ? 2 : 4;
}
// The scalar lane type of a vector TypeId (ty_none() for non-vectors).
inline constexpr TypeId ty_lane_type(TypeId t) {
    if (!ty_is_vector(t)) return ty_none();
    switch (type_desc(t).pointee) {
        case Ty::F64: return ty_f64();
        case Ty::I64: return ty_i64();
        case Ty::I32: return ty_i32();
        case Ty::F32: return ty_f32();
        default:      return ty_none();
    }
}
// Vector type of `lanes` x `scalar` (ty_none() when the pair is not packed
// in the MVP lattice).
inline constexpr TypeId ty_vector_of(TypeId scalar, u32 lanes) {
    if (lanes == 2) {
        if (scalar == ty_f64()) return ty_v2f64();
        if (scalar == ty_i64()) return ty_v2i64();
    } else if (lanes == 4) {
        if (scalar == ty_i32()) return ty_v4i32();
        if (scalar == ty_f32()) return ty_v4f32();
    }
    return ty_none();
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
// Does this type live in the XMM register file? (scalar FP + packed vectors)
inline constexpr bool ty_in_xmm(TypeId t) { return ty_is_float(t) || ty_is_vector(t); }
inline constexpr bool ty_is_scalar(TypeId t) {
    return ty_is_int(t) || ty_is_float(t) || ty_is_ptr(t);
}
inline constexpr u32 ty_bits(TypeId t) {
    switch (type_desc(t).ty) {
        case Ty::I1:  return 8;   // storage width; logical 1 bit
        case Ty::I32: case Ty::U32: case Ty::F32: return 32;
        case Ty::V2F64: case Ty::V2I64: case Ty::V4I32: case Ty::V4F32: return 128;
        default: return 64;       // i64/u64/f64/ptr
    }
}
inline constexpr u32 ty_store_bytes(TypeId t) {
    switch (type_desc(t).ty) {
        case Ty::I1: return 1;
        case Ty::I32: case Ty::U32: case Ty::F32: return 4;
        case Ty::V2F64: case Ty::V2I64: case Ty::V4I32: case Ty::V4F32: return 16;
        default: return 8;
    }
}
inline constexpr bool ty_is_bool(TypeId t) { return type_desc(t).ty == Ty::I1; }

// Pointee type of a pointer TypeId (ty_none() when not a pointer or when
// the pointee is not in the MVP lattice).
inline constexpr TypeId ty_pointee(TypeId ptr) {
    TypeDesc d = type_desc(ptr);
    if (d.ty != Ty::Ptr) return ty_none();
    switch (d.pointee) {
        case Ty::I64: return ty_i64();
        case Ty::I32: return ty_i32();
        case Ty::U64: return ty_u64();
        case Ty::U32: return ty_u32();
        case Ty::F32: return ty_f32();
        case Ty::F64: return ty_f64();
        case Ty::I1:  return ty_i1();
        default:      return ty_none();
    }
}

const char* ty_name(TypeId t);

} // namespace jules
