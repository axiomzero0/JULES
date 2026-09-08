// Shared helpers for graph passes (const evaluation, repinning, block walks).
#pragma once

#include "core/son/passes/pass.h"

namespace jules {

struct ConstVal {
    bool is_fp = false;
    TypeId ty = ty_none();
    i64 iv = 0;
    f64 fv = 0;
};

inline bool node_is_const(const Graph& g, NodeId n) {
    return n != kNoNode && g.node(n).op == Op::Const;
}

inline bool const_of(const Graph& g, NodeId n, ConstVal& out) {
    if (n == kNoNode) return false;
    const Node& nd = g.node(n);
    if (nd.op != Op::Const) return false;
    out.is_fp = ty_is_float(nd.ty);
    out.ty = nd.ty;
    out.iv = nd.ival;
    out.fv = nd.fval;
    return true;
}

inline NodeId make_const_node(Graph& g, NodeId pin, const ConstVal& v) {
    NodeId n = g.make(Op::Const, v.ty, {pin});
    g.node(n).ival = v.iv;
    g.node(n).fval = v.fv;
    return n;
}

// Constant binop evaluation. Returns false when the operation must stay
// runtime (division by zero); overflow wraps (documented UB model).
inline bool eval_bin_const(BinOp op, const ConstVal& a, const ConstVal& b, ConstVal& out) {
    if (a.is_fp || b.is_fp) {
        f64 x = a.is_fp ? a.fv : static_cast<f64>(a.iv);
        f64 y = b.is_fp ? b.fv : static_cast<f64>(b.iv);
        out.is_fp = true;
        out.ty = a.ty; // same-typed operands per IR invariant
        switch (op) {
            case BinOp::Add: out.fv = x + y; return true;
            case BinOp::Sub: out.fv = x - y; return true;
            case BinOp::Mul: out.fv = x * y; return true;
            case BinOp::Div: if (y == 0.0) return false; out.fv = x / y; return true;
            // min/max fold with the exact select semantics (NaN -> y / x)
            case BinOp::Min: out.fv = x < y ? x : y; return true;
            case BinOp::Max: out.fv = x < y ? y : x; return true;
            default: return false; // no fp mod/bitwise in MVP
        }
    }
    out.is_fp = false;
    out.ty = a.ty;
    i64 x = a.iv, y = b.iv;
    bool sgn = ty_is_signed(a.ty);
    switch (op) {
        case BinOp::Add: out.iv = x + y; return true;
        case BinOp::Sub: out.iv = x - y; return true;
        case BinOp::Mul: out.iv = x * y; return true;
        case BinOp::Div: case BinOp::Mod: {
            if (y == 0) return false;
            if (op == BinOp::Div) out.iv = sgn ? x / y : static_cast<i64>(static_cast<u64>(x) / static_cast<u64>(y));
            else out.iv = sgn ? x % y : static_cast<i64>(static_cast<u64>(x) % static_cast<u64>(y));
            return true;
        }
        case BinOp::And: out.iv = x & y; return true;
        case BinOp::Or:  out.iv = x | y; return true;
        case BinOp::Xor: out.iv = x ^ y; return true;
        case BinOp::Shl: out.iv = x << (y & 63); return true;
        case BinOp::Shr: out.iv = sgn ? x >> (y & 63) : static_cast<i64>(static_cast<u64>(x) >> (y & 63)); return true;
        case BinOp::Min:
            // exact select semantics: Lt(x,y) ? x : y (NaN -> y for fp)
            if (a.is_fp || b.is_fp) return false; // handled on the fp path above
            out.iv = (sgn ? x < y : static_cast<u64>(x) < static_cast<u64>(y)) ? x : y;
            return true;
        case BinOp::Max:
            if (a.is_fp || b.is_fp) return false;
            out.iv = (sgn ? x < y : static_cast<u64>(x) < static_cast<u64>(y)) ? y : x;
            return true;
    }
    return false;
}

inline bool eval_cmp_const(CmpOp op, const ConstVal& a, const ConstVal& b, ConstVal& out) {
    if (a.is_fp || b.is_fp) {
        f64 x = a.is_fp ? a.fv : static_cast<f64>(a.iv);
        f64 y = b.is_fp ? b.fv : static_cast<f64>(b.iv);
        out.is_fp = false;
        out.ty = ty_i1();
        switch (op) {
            case CmpOp::Eq: out.iv = x == y; return true;
            case CmpOp::Ne: out.iv = x != y; return true;
            case CmpOp::Lt: out.iv = x <  y; return true;
            case CmpOp::Le: out.iv = x <= y; return true;
            case CmpOp::Gt: out.iv = x >  y; return true;
            case CmpOp::Ge: out.iv = x >= y; return true;
        }
        return false;
    }
    bool sgn = ty_is_signed(a.ty);
    i64 x = a.iv, y = b.iv;
    auto lt = [&](i64 p, i64 q) { return sgn ? p < q : static_cast<u64>(p) < static_cast<u64>(q); };
    out.is_fp = false;
    out.ty = ty_i1();
    switch (op) {
        case CmpOp::Eq: out.iv = x == y; return true;
        case CmpOp::Ne: out.iv = x != y; return true;
        case CmpOp::Lt: out.iv = lt(x, y); return true;
        case CmpOp::Le: out.iv = !lt(y, x); return true;
        case CmpOp::Gt: out.iv = lt(y, x); return true;
        case CmpOp::Ge: out.iv = !lt(x, y); return true;
    }
    return false;
}

inline bool eval_un_const(UnOp op, const ConstVal& a, ConstVal& out) {
    out.ty = a.ty;
    out.is_fp = a.is_fp;
    switch (op) {
        case UnOp::Neg:
            if (a.is_fp) { out.fv = -a.fv; return true; }
            out.iv = -a.iv;
            return true;
        case UnOp::Not:
            if (a.ty != ty_i1()) return false;
            out.iv = a.iv ? 0 : 1;
            return true;
        case UnOp::BNot:
            if (a.is_fp) return false;
            out.iv = ~a.iv;
            return true;
    }
    return false;
}

inline bool eval_cast_const(CastOp op, const ConstVal& a, TypeId target, ConstVal& out) {
    out.ty = target;
    out.is_fp = ty_is_float(target);
    switch (op) {
        case CastOp::Broadcast:
        case CastOp::Extract:
            // Vector casts never constant-fold: a vector Const cannot exist
            // in the MVP (no constexpr vector surface; SCCP must leave
            // broadcasts/extracts as runtime values).
            return false;
        case CastOp::ZExt:
            out.iv = static_cast<i64>(static_cast<u64>(a.iv) &
                                      (ty_bits(a.ty) == 32 ? 0xFFFFFFFFull : 0xFFull));
            return true;
        case CastOp::SExt:
            if (ty_bits(a.ty) == 8) out.iv = static_cast<i64>(static_cast<i8>(static_cast<u8>(a.iv)));
            else out.iv = static_cast<i64>(static_cast<i32>(static_cast<u32>(a.iv)));
            return true;
        case CastOp::Trunc:
            if (ty_bits(target) == 32) out.iv = static_cast<i64>(static_cast<i32>(static_cast<u32>(a.iv)));
            else if (ty_bits(target) == 8) out.iv = static_cast<i64>(static_cast<i8>(static_cast<u8>(a.iv)));
            else out.iv = a.iv;
            return true;
        case CastOp::SiToFp:
            out.fv = static_cast<f64>(a.iv);
            if (target == ty_f32()) out.fv = static_cast<f64>(static_cast<f32>(out.fv));
            return true;
        case CastOp::FpToSi:
            out.iv = static_cast<i64>(a.fv);
            return true;
        case CastOp::FpExt:  out.fv = a.fv; return true;
        case CastOp::FpTrunc: out.fv = static_cast<f64>(static_cast<f32>(a.fv)); return true;
        case CastOp::Ptr:    out.iv = a.iv; return true;
    }
    return false;
}

inline NodeId block_pin(const Graph& g, NodeId n) {
    return g.node(n).in[0];
}

} // namespace jules
