// Shared machinery for the vectorization family (passes 54-66).
//
// The catalog puts the reporting passes (59 reduction recognizer, 64 cost
// model, 66 width selection) AFTER the emitting passes (54 SLP, 56 loop
// vectorizer) in schedule order; the underlying ANALYSES must therefore
// live here as pure functions so the transforms can consult them at
// decision time and the reporting passes re-run them for telemetry and
// verdict recording. (Production compilers do the same: LLVM's
// ReductionAnalyzer and InnerLoopVectorizer share cost/reduction code.)
#pragma once

#include "core/son/passes/pass_utils.h"

namespace jules {
namespace vecx {

// ---- width selection (pass 66 semantics) ---------------------------------
//
// 128-bit SSE2 baseline (universal x86-64): lane count = 16 / lane bytes.
// Returns 0 when the scalar type has no packed form in the MVP lattice
// (u64/u32/bool/ptr: no lane-extract typing in the fixed table).
inline u32 vector_lanes_for(TypeId scalar) {
    if (scalar == ty_f64() || scalar == ty_i64()) return 2;
    if (scalar == ty_f32() || scalar == ty_i32()) return 4;
    return 0;
}
inline TypeId vector_ty_for(TypeId scalar) {
    return ty_vector_of(scalar, vector_lanes_for(scalar));
}

// ---- legality (pass 60 semantics) -----------------------------------------
// Which BinOps lower for a packed form of `scalar` on the SSE2 baseline?
//   f64/f32: add/sub/mul/div        (addpd/subpd/mulpd/divpd)
//   i64:     add/sub only           (paddq/psubq — no SIMD i64 mul)
//   i32:     add/sub only           (paddd/psubd — pmulld needs SSE4.1)
// and/or/xor are legal for every integer lane kind.
inline bool packed_bin_legal(TypeId scalar, BinOp op) {
    if (op == BinOp::And || op == BinOp::Or || op == BinOp::Xor)
        return ty_is_int(scalar) && !ty_is_bool(scalar);
    if (ty_is_float(scalar)) return op <= BinOp::Div;
    if (scalar == ty_i64() || scalar == ty_i32()) return op == BinOp::Add || op == BinOp::Sub;
    return false;
}

// ---- cost model (pass 64 semantics) ---------------------------------------
struct CostVerdict {
    bool profitable = false;
    const char* reason = "";
};
// `trip` < 0: unknown (dynamic). Element-wise loops amortize the vector
// body immediately; reductions need enough iterations to pay for the
// horizontal finish (guard + shuffle tree). Size-biased levels reject
// dynamic trips: the guarded epilogue doubles loop code.
inline CostVerdict vector_cost_ok(TypeId elem, i64 trip, bool is_reduction,
                                  bool size_biased) {
    u32 lanes = vector_lanes_for(elem);
    if (lanes == 0) return {false, "no-packed-form"};
    if (size_biased && trip < 0) return {false, "size-biased-dynamic-trip"};
    if (trip >= 0 && trip < static_cast<i64>(lanes)) return {false, "trip-lt-vf"};
    if (trip >= 0 && is_reduction && trip < 2 * static_cast<i64>(lanes))
        return {false, "reduction-too-short"};
    return {true, "ok"};
}

// ---- reduction recognition (pass 59 semantics) -----------------------------
struct Reduction {
    NodeId phi = kNoNode;   // loop-header phi carrying the accumulator
    NodeId update = kNoNode; // Bin(op, phi-or-load, other)
    BinOp op = BinOp::Add;
    NodeId feed = kNoNode;  // the per-iteration value (usually the load)
    bool phi_on_lhs = true; // update = op(phi, feed) vs op(feed, phi)
};
// Recognize `acc = phi(init, Bin(op, acc, x))` at `header`. Commutative
// ops accept either side; non-commutative (Sub) only the canonical side.
inline bool match_reduction(Graph& g, NodeId phi, NodeId header, Reduction& out) {
    if (phi == kNoNode || g.is_dead(phi)) return false;
    const Node& p = g.node(phi);
    if (p.op != Op::Phi || p.in[0] != header || p.ty == ty_mem()) return false;
    if (p.n_in < 3) return false;
    NodeId update = p.in[p.n_in - 1]; // latch input
    if (update == kNoNode || g.is_dead(update)) return false;
    const Node& upd = g.node(update);
    if (upd.op != Op::Bin) return false;
    BinOp op = static_cast<BinOp>(upd.sub);
    bool commutative = op == BinOp::Add || op == BinOp::Mul || op == BinOp::And ||
                       op == BinOp::Or || op == BinOp::Xor;
    if (upd.in[1] == phi) {
        out = {phi, update, op, upd.in[2], true};
        return true;
    }
    if (upd.in[2] == phi && commutative) {
        out = {phi, update, op, upd.in[1], false};
        return true;
    }
    return false;
}

// ---- address pattern (shared by 56 and 54/55) -------------------------------
// Recognizes the builder's `base[idx]` lowering:
//   addr = Cast(Ptr, Add(Cast(i64, base), scale(idx)))
//     scale(idx) = Shl(idx, log2(esz)) | Mul(idx, esz) | idx   (esz == 1)
// `idx` may be any node (the loop IV, a constant, a computed index).
struct AddrPattern {
    NodeId base = kNoNode;   // the pointer VALUE node (pre-cast)
    NodeId idx = kNoNode;    // index expression (element units)
    u32 esz = 0;             // element byte size the scale encodes
    bool ok = false;
};
namespace detail {
inline bool is_i64_ptrcast(const Node& cn) {
    return cn.op == Op::Cast && static_cast<CastOp>(cn.sub) == CastOp::Ptr &&
           cn.ty == ty_i64();
}
} // namespace detail
inline AddrPattern match_addr(Graph& g, NodeId addr, u32 expect_esz = 0) {
    using detail::is_i64_ptrcast;
    AddrPattern out;
    if (addr == kNoNode || g.is_dead(addr)) return out;
    const Node& a = g.node(addr);
    if (a.op != Op::Cast || static_cast<CastOp>(a.sub) != CastOp::Ptr ||
        !ty_is_ptr(a.ty))
        return out;
    NodeId add = a.in[1];
    if (g.is_dead(add)) return out;
    const Node& an = g.node(add);
    if (an.op != Op::Bin || static_cast<BinOp>(an.sub) != BinOp::Add) {
        // fully folded: addr = Cast(i64, base) — index 0 (Add(x, 0) was
        // collapsed by IdentityCollapse)
        if (is_i64_ptrcast(an)) {
            out.base = an.in[1];
            out.idx = kNoNode; // caller: index 0 (no node — build a const)
            out.esz = expect_esz;
            out.ok = expect_esz != 0;
        }
        return out;
    }

    // (base64, scaled-index) in either operand order
    NodeId other = kNoNode;
    NodeId base64 = kNoNode;
    if (is_i64_ptrcast(g.node(an.in[1]))) { base64 = an.in[1]; other = an.in[2]; }
    else if (is_i64_ptrcast(g.node(an.in[2]))) { base64 = an.in[2]; other = an.in[1]; }
    else return out;
    out.base = g.node(base64).in[1];

    const Node& on = g.node(other);
    NodeId idx = kNoNode;
    u32 esz = 1;
    if (on.op == Op::Bin) {
        BinOp s = static_cast<BinOp>(on.sub);
        ConstVal c;
        if (s == BinOp::Shl && const_of(g, on.in[2], c) && c.iv >= 0 && c.iv <= 3) {
            idx = on.in[1];
            esz = 1u << static_cast<u32>(c.iv);
        } else if (s == BinOp::Mul && const_of(g, on.in[1], c) &&
                   (c.iv == 2 || c.iv == 4 || c.iv == 8)) {
            idx = on.in[2];
            esz = static_cast<u32>(c.iv);
        } else if (s == BinOp::Mul && const_of(g, on.in[2], c) &&
                   (c.iv == 2 || c.iv == 4 || c.iv == 8)) {
            idx = on.in[1];
            esz = static_cast<u32>(c.iv);
        }
    } else if (on.op == Op::Const && expect_esz != 0 && on.ival >= 0 &&
               on.ival % expect_esz == 0) {
        // constant byte offset folded by ConstantFolding: recover the
        // element index by the known element size
        ConstVal c;
        c.ty = on.ty;
        c.is_fp = false;
        c.iv = on.ival / static_cast<i64>(expect_esz);
        idx = make_const_node(g, add /*pin: the address block*/, c);
        esz = expect_esz;
    } else {
        idx = other; // unscaled i64 (element size 1)
    }
    if (idx == kNoNode || g.is_dead(idx)) return out;
    if (expect_esz != 0 && esz != expect_esz) return out; // scale mismatch
    out.idx = idx;
    out.esz = esz;
    out.ok = true;
    return out;
}

} // namespace vecx
} // namespace jules
