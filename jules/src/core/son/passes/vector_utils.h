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
    // minpd/maxpd: FP lanes only (SSE2); operand order is load-bearing
    // (dst operand returned on unordered) — never commute these.
    if (op == BinOp::Min || op == BinOp::Max) return ty_is_float(scalar);
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

// ---- element-index linear form (passes 54/58 pair machinery) ----------------
// Decompose an element index into idx == m + k, k a compile-time constant:
// the Reassociation-normalized shapes are a bare symbolic node M (k = 0) or
// Add(M, const). Pure-constant indices report m = kNoNode. m == kNoNode is
// the CONST pair family (pass 54); a symbolic m is the interleaved/symbolic
// family (pass 58: a[2i], a[2i+1] share m = Mul(i,2)).
struct LinearIdx {
    NodeId m = kNoNode;
    i64 k = 0;
    bool ok = false;
};
inline LinearIdx linear_idx(Graph& g, NodeId idx) {
    LinearIdx out;
    if (idx == kNoNode) { out.ok = true; return out; } // folded Add(x,0): 0
    if (g.is_dead(idx)) return out;
    ConstVal c;
    if (const_of(g, idx, c)) { out.k = c.iv; out.ok = true; return out; }
    const Node& n = g.node(idx);
    if (n.op == Op::Bin && static_cast<BinOp>(n.sub) == BinOp::Add) {
        if (const_of(g, n.in[2], c)) { out.m = n.in[1]; out.k = c.iv; out.ok = true; return out; }
        if (const_of(g, n.in[1], c)) { out.m = n.in[2]; out.k = c.iv; out.ok = true; return out; }
    }
    out.m = idx; out.k = 0; out.ok = true; return out; // bare symbolic index
}

// Structural value-equivalence for PURE value expressions (same op, same
// operands; commutative bins accept mirrored sides). The pair machinery
// compares symbolic index parts that the first-sweep schedule leaves
// DUPLICATED: the frontend builds one `2*i` mul per source occurrence, and
// load forwarding makes them mergeable only after GVN's catalog slot (the
// post-inline cleanup re-runs GVN, but the Phase 5 pair passes do not
// re-run there). Structurally identical pure trees over identical leaves
// compute identical values, so treating them as one index is sound. Phis,
// params and memory ops compare by node identity only (depth-bounded).
inline bool pure_equiv(Graph& g, NodeId a, NodeId b, u32 depth = 4) {
    if (a == b) return true;
    if (a == kNoNode || b == kNoNode || depth == 0) return false;
    const Node& x = g.node(a);
    const Node& y = g.node(b);
    if (x.op != y.op || x.sub != y.sub || x.ty != y.ty) return false;
    switch (x.op) {
        case Op::Const: return x.ival == y.ival && x.fval == y.fval;
        case Op::Bin: {
            BinOp s = static_cast<BinOp>(x.sub);
            if (pure_equiv(g, x.in[1], y.in[1], depth - 1) &&
                pure_equiv(g, x.in[2], y.in[2], depth - 1)) return true;
            bool comm = s == BinOp::Add || s == BinOp::Mul || s == BinOp::And ||
                        s == BinOp::Or || s == BinOp::Xor;
            return comm && pure_equiv(g, x.in[1], y.in[2], depth - 1) &&
                   pure_equiv(g, x.in[2], y.in[1], depth - 1);
        }
        case Op::Cast:
            return pure_equiv(g, x.in[1], y.in[1], depth - 1);
        default: return false; // phi / param / memory: identity only
    }
}

// ---- element-adjacent address pair (shared by 54 and 58) --------------------
struct PairAddr {
    NodeId base = kNoNode;      // shared base node
    NodeId m = kNoNode;         // shared symbolic index part (kNoNode: const)
    i64 k_first = 0;            // constant offset of the FIRST element
    u32 esz = 0;                // element byte size (8 in the 2-lane family)
    NodeId addr_first = kNoNode;// first element's address node (reused by the pack)
    bool ok = false;
};
// addr2 addresses the element at addr1's index + 1 (same base, same symbolic
// part, constant difference exactly 1) — the 16 bytes at addr1 cover both.
inline PairAddr match_pair_addrs(Graph& g, NodeId addr1, NodeId addr2, u32 esz) {
    PairAddr out;
    AddrPattern a1 = match_addr(g, addr1, esz);
    AddrPattern a2 = match_addr(g, addr2, esz);
    if (!a1.ok || !a2.ok) return out;
    if (a1.base != a2.base || a1.esz != esz || a2.esz != esz) return out;
    LinearIdx i1 = linear_idx(g, a1.idx);
    LinearIdx i2 = linear_idx(g, a2.idx);
    if (!i1.ok || !i2.ok) return out;
    if (i2.k != i1.k + 1 || !pure_equiv(g, i1.m, i2.m)) return out;
    out.base = a1.base; out.m = i1.m; out.k_first = i1.k;
    out.esz = esz; out.addr_first = addr1; out.ok = true;
    return out;
}

// Do the 8-byte element (base_b, m_b + k_b) and the element accessed by
// address node `addr_n` touch the same bytes? Conservative under MayAlias
// and under unprovable shapes (different symbolic part or scale).
inline bool elem_accesses_conflict(Graph& g, AliasInfo& aa, NodeId addr_n,
                                   NodeId base_b, NodeId m_b, i64 k_b) {
    AddrPattern aA = match_addr(g, addr_n, 8);
    if (!aA.ok) return true;
    AliasResult ar = aa.alias(aA.base, base_b);
    if (ar == AliasResult::NoAlias) return false;
    if (ar != AliasResult::MustAlias) return true; // MayAlias: any offset
    LinearIdx la = linear_idx(g, aA.idx);
    if (!la.ok || aA.esz != 8 || !pure_equiv(g, la.m, m_b)) return true; // unprovable
    return la.k == k_b; // same base, value-equal symbolic part: element identity
}

// ---- pack-pair soundness gate (shared by 54 and 58) --------------------------
// Replacing the scalar pair (s1, s2) fed by loads (l1, l2) with one packed
// load (at l1's version) + one packed store (at s1's chain position) is sound
// when:
//  (1) all four nodes sit in one control region (same pin) — cross-region
//      pairs are rejected (documented reduction);
//  (2) the loads' memory versions: the scalar reads of l1 and l2 happen at
//      possibly different versions; the packed load reads BOTH elements at
//      l1's version. Lane 0 is exact by construction (same version); lane 1
//      requires every store between the two versions to be proven not to
//      write lane 1's element (the packed form would read the pre-store
//      value where the scalar saw the post-store one);
//  (3) every other user of s1's memory version: the packed store writes the
//      SECOND element one chain position earlier than scalar s2 did; any
//      load still reading between s1 and s2 must not read that element, and
//      any other memory user (forked load, store fork, call, return, phi)
//      rejects the pair.
namespace detail {
// Walk the store chain from `later` back to `earlier`, collecting the stores
// strictly between. Fails (false) when the versions are divergent or a
// non-store node (phi/region/call/alloc root) intervenes — control flow
// between the loads is out of scope for the pair family.
inline bool version_path_stores(Graph& g, NodeId later, NodeId earlier, NodeId pin,
                                SmallVec<NodeId, 6>& out) {
    NodeId cur = later;
    u32 guard = 0;
    while (cur != earlier) {
        if (cur == kNoNode || g.is_dead(cur) || ++guard > g.size()) return false;
        const Node& n = g.node(cur);
        if (n.op != Op::Store) return false;  // root reached / phi / call
        if (n.in[0] != pin) return false;     // left the region
        out.push_back(cur);
        cur = n.in[1];
    }
    return true;
}
} // namespace detail

inline bool pack_pair_sound(Graph& g, AliasInfo& aa, NodeId s1, NodeId s2,
                            NodeId l1, NodeId l2, const PairAddr& dest,
                            const PairAddr& src) {
    NodeId pin = g.node(s1).in[0];
    if (g.node(s2).in[0] != pin) return false;
    if (g.node(l1).in[0] != pin || g.node(l2).in[0] != pin) return false;

    // (2) stores between the loads' versions, either direction
    SmallVec<NodeId, 6> between;
    if (g.node(l1).in[1] != g.node(l2).in[1]) {
        if (!detail::version_path_stores(g, g.node(l2).in[1], g.node(l1).in[1], pin, between)) {
            between.clear();
            if (!detail::version_path_stores(g, g.node(l1).in[1], g.node(l2).in[1], pin, between)) {
                if (getenv("JULES_DEBUG_VEC"))
                    fprintf(stderr, "[gate] version-walk-fail l1v=%u l2v=%u\n",
                            g.node(l1).in[1], g.node(l2).in[1]);
                return false;
            }
        }
        for (NodeId st : between) {
            if (elem_accesses_conflict(g, aa, g.node(st).in[2], src.base, src.m,
                                       src.k_first + 1)) {
                if (getenv("JULES_DEBUG_VEC"))
                    fprintf(stderr, "[gate] lane1-conflict st=%u stbase=%u srcbase=%u\n",
                            st, vecx::match_addr(g, g.node(st).in[2], 8).base, src.base);
                return false;
            }
        }
    }

    // (3) other users of s1's memory version
    for (NodeId u : g.uses_of(s1)) {
        if (u == s2 || u == l2 || g.is_dead(u)) continue;
        const Node& un = g.node(u);
        bool uses_mem = un.in[1] == s1;
        if (!uses_mem && un.op == Op::Phi) {
            for (u8 i = 1; i < un.n_in; ++i) if (un.in[i] == s1) { uses_mem = true; break; }
        }
        if (!uses_mem) continue;  // mem-typed nodes are only memory users
        if (un.op != Op::Load) return false;      // store fork / call / return / phi
        if (un.in[0] != pin) return false;        // forked-region load
        if (elem_accesses_conflict(g, aa, un.in[2], dest.base, dest.m,
                                   dest.k_first + 1)) {
            if (getenv("JULES_DEBUG_VEC"))
                fprintf(stderr, "[gate] sibling-load-conflict u=%u\n", u);
            return false;  // reads the element the packed store writes early
        }
    }
    return true;
}

// ---- shared pair executor (54 const family, 58 interleaved family) -----------
enum class PairFamily { kConstAdjacent, kInterleaved };

// v1 = Bin(op, load1, shared) and v2 = Bin(op, load2, shared) with the SAME
// shared operand (or mirrored sides)?
inline bool same_shape_pair(Graph& g, NodeId v1, NodeId v2, NodeId& l1,
                            NodeId& l2, NodeId& shared) {
    const Node& a = g.node(v1);
    const Node& b = g.node(v2);
    NodeId a1 = a.in[1], a2 = a.in[2], b1 = b.in[1], b2 = b.in[2];
    if (g.node(a1).op == Op::Load && g.node(b1).op == Op::Load && a2 == b2) {
        l1 = a1; l2 = b1; shared = a2;
        return true;
    }
    if (g.node(a2).op == Op::Load && g.node(b2).op == Op::Load && a1 == b1) {
        l1 = a2; l2 = b2; shared = a1;
        return true;
    }
    return false;
}

inline bool try_pack_pair(Graph& g, AliasInfo& aa, FpMode fp, PairFamily fam,
                          NodeId s1, NodeId s2) {
    Node& n1 = g.node(s1);
    Node& n2 = g.node(s2);
    TypeId ety = g.node(n1.in[3]).ty;
    if (vecx::vector_ty_for(ety) == ty_none()) return false;
    u32 esz = ty_store_bytes(ety);
    u32 lanes = vecx::vector_lanes_for(ety);
    if (lanes != 2) return false; // pairs only (4-lane needs 4 stores)
    if (lanes * esz != 16) return false;
    if (getenv("JULES_DEBUG_VEC"))
        fprintf(stderr, "[pair] try s%u/s%u ety-ok\n", s1, s2);

    // destination pair: same base, element-adjacent; the family owns either
    // the pure-const or the symbolic-index shapes
    PairAddr dest = match_pair_addrs(g, n1.in[2], n2.in[2], esz);
    if (!dest.ok) return false;
    if (getenv("JULES_DEBUG_VEC"))
        fprintf(stderr, "[pair] dest-ok base=%u m=%u k=%lld\n", dest.base, dest.m,
                (long long)dest.k_first);
    bool want_const = (fam == PairFamily::kConstAdjacent);
    if ((dest.m == kNoNode) != want_const) return false;

    // values: loads of the same source pair, optionally through ONE identical
    // pure op with identical extra operand
    NodeId v1 = n1.in[3], v2 = n2.in[3];
    if (g.node(v1).ty != ety || g.node(v2).ty != ety) return false;
    NodeId l1 = v1, l2 = v2;
    NodeId op_node = kNoNode;
    NodeId shared = kNoNode;
    if (g.node(v1).op == Op::Load && g.node(v2).op == Op::Load) {
        // pure copy
    } else if (g.node(v1).op == Op::Bin && g.node(v2).op == Op::Bin &&
               g.node(v1).sub == g.node(v2).sub &&
               same_shape_pair(g, v1, v2, l1, l2, shared)) {
        op_node = v1;
        if (!vecx::packed_bin_legal(ety, static_cast<BinOp>(g.node(v1).sub))) return false;
    } else {
        return false;
    }
    // the value loads: same source-pair shape (same family as the dest pair)
    PairAddr src = match_pair_addrs(g, g.node(l1).in[2], g.node(l2).in[2], esz);
    if (!src.ok) return false;
    if ((src.m == kNoNode) != want_const) return false;
    if (g.node(l1).ty != ety) return false;
    if (getenv("JULES_DEBUG_VEC"))
        fprintf(stderr, "[pair] src-ok base=%u m=%u\n", src.base, src.m);
    if (ty_is_float(ety) && op_node != kNoNode && !fp_fast_allowed(fp)) {
        // element-wise ops with a broadcast operand are lane-exact;
        // this check only fires for ops that reassociate — none here.
    }

    // soundness: pins, load versions, sibling users of s1's version
    if (!pack_pair_sound(g, aa, s1, s2, l1, l2, dest, src)) {
        if (getenv("JULES_DEBUG_VEC"))
            fprintf(stderr, "[pair] UNSOUND s%u/s%u\n", s1, s2);
        return false;
    }

    TypeId vty = vecx::vector_ty_for(ety);
    NodeId pin = n1.in[0];
    NodeId mem_in = g.node(l1).in[1]; // version the loads read

    // packed load at the source pair's first address (same 16 bytes)
    NodeId vload = g.make(Op::Load, vty, {pin, mem_in, g.node(l1).in[2]});

    NodeId vval = vload;
    if (op_node != kNoNode) {
        NodeId bcast = g.make(Op::Cast, vty, {pin, shared},
                               static_cast<u8>(CastOp::Broadcast));
        NodeId lhs = (g.node(op_node).in[1] == l1) ? vload : bcast;
        NodeId rhs = (g.node(op_node).in[1] == l1) ? bcast : vload;
        vval = g.make(Op::Bin, vty, {pin, lhs, rhs}, g.node(op_node).sub);
    }

    // packed store at the destination pair's first address; it replaces BOTH
    // scalar stores in the memory chain
    NodeId chain_in = n1.in[1];
    NodeId vstore = g.make(Op::Store, ty_mem(), {pin, chain_in, n1.in[2], vval});

    g.replace_uses_as_memory(s1, vstore);
    g.replace_uses_as_memory(s2, vstore);
    g.kill(s1);
    g.kill(s2);
    g.touch();
    return true;
}

// Scan every store for a memory-chain successor store and try to pack the
// pair in the given family. Returns the packed count.
inline u32 pack_store_pairs(Graph& g, AliasInfo& aa, FpMode fp, PairFamily fam) {
    u32 packed = 0;
    for (NodeId id = 0; id < g.size(); ++id) {
        Node& n = g.node(id);
        if (n.op != Op::Store) continue;
        NodeId s2 = kNoNode;
        for (NodeId u : g.uses_of(id)) {
            if (g.is_dead(u)) continue;
            Node& un = g.node(u);
            if (un.op == Op::Store && un.in[1] == id) {
                s2 = u;
                break;
            }
        }
        if (s2 == kNoNode) continue;
        if (try_pack_pair(g, aa, fp, fam, id, s2)) ++packed;
    }
    return packed;
}

} // namespace vecx
} // namespace jules
