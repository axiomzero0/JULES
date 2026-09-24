// x86-64 DP-on-DAG selector, solver side (see x64_dp_isel.h for the model).
//
// Per-block solve: pass A claims Load/Store address chains (always a win —
// the address never takes the slot round trip); pass B claims Bin nodes the
// rule set covers strictly cheaper than the hand emitter's mirror cost.
// Unclaimed nodes flow to the hand emitters unchanged.
#include "x64_dp_isel.h"

#include "core/son/passes/pass_utils.h"
#include "core/son/son.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace jules {
namespace {

// Latency-class cost units — aliases of the DpIsel public constants so the
// rule code below reads like the header's cost table (deltas drive decisions:
// a materialized intermediate costs store+load = 4; imul -> lea/shl saves 2;
// idiv -> magic saves ~20+).
constexpr i32 C_LD = DpIsel::kCLd;   // mov reg, [slot] (frame round trip)
constexpr i32 C_ST = DpIsel::kCSt;   // mov [slot], reg
constexpr i32 C_IMM = DpIsel::kCImm; // mov reg, imm (incl. movabs)
constexpr i32 C_OP = DpIsel::kCOp;   // arith / lea / shift-imm / mov rr
constexpr i32 C_IMUL = DpIsel::kCMul; // imul / mulhi
constexpr i32 C_DIV = DpIsel::kCDiv; // idiv

constexpr i32 kInf = std::numeric_limits<i32>::max() / 4;

bool int_scalar(TypeId t) { return !ty_is_vector(t) && !ty_in_xmm(t); }

bool lea_width(TypeId t) { return int_scalar(t) && ty_bits(t) == 64; }

bool chain_sub(BinOp op) {
    switch (op) {
        case BinOp::Add: case BinOp::Sub: case BinOp::Mul:
        case BinOp::And: case BinOp::Or: case BinOp::Xor:
            return true;
        default:
            return false; // Div/Mod (fixed regs), Shr (sar), Min/Max, AndNot
    }
}

bool op_commutes(BinOp op) {
    return op == BinOp::Add || op == BinOp::And || op == BinOp::Or ||
           op == BinOp::Xor || op == BinOp::Mul;
}

// v == 2^k for k in 0..3 -> k (the SHIFT amount); scale is 1<<k.
bool pow2_shift(i64 v, u8& k) {
    if (v == 1) { k = 0; return true; }
    if (v == 2) { k = 1; return true; }
    if (v == 4) { k = 2; return true; }
    if (v == 8) { k = 3; return true; }
    return false;
}

// Does `v` encode as a sign-extended imm32 (arith immediate form)?
bool fits32(i64 v) { return v >= -2147483648LL && v <= 2147483647LL; }

i32 op_cost(BinOp op) {
    return op == BinOp::Mul ? C_IMUL : (op == BinOp::Div || op == BinOp::Mod)
                                                 ? C_DIV : C_OP;
}

bool is_pow2_u64(u64 v) { return v && !(v & (v - 1)); }

// An index term for a SIB lea: Mul(i, 2^k) or Shl(i, k<=3).
bool index_term(const Graph& g, NodeId t, NodeId& i, u8& scale) {
    const Node& td = g.node(t);
    if (td.op != Op::Bin || !lea_width(td.ty)) return false;
    BinOp op = static_cast<BinOp>(td.sub);
    if (op == BinOp::Mul) {
        const Node& a = g.node(td.in[1]);
        const Node& b = g.node(td.in[2]);
        u8 k = 0;
        if (a.op == Op::Const && pow2_shift(a.ival, k)) {
            i = td.in[2]; scale = static_cast<u8>(1 << k); return true;
        }
        if (b.op == Op::Const && pow2_shift(b.ival, k)) {
            i = td.in[1]; scale = static_cast<u8>(1 << k); return true;
        }
        return false;
    }
    if (op == BinOp::Shl) {
        const Node& cnt = g.node(td.in[2]);
        if (cnt.op != Op::Const || cnt.ival < 0 || cnt.ival > 3) return false;
        i = td.in[1];
        scale = static_cast<u8>(1 << cnt.ival);
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// magic derivation (Granlund-Montgomery; verified in scripts/verify_magic_math.py)
// ---------------------------------------------------------------------------

bool DpIsel::derive_magic(u64 d, Magic& out) {
    // pow2 / tiny divisors are not ours (IR p12 shift/mask rules); giants
    // that would need shift == 64 keep the idiv fallback.
    if (d < 3 || is_pow2_u64(d)) return false;
    using U128 = unsigned __int128;
    const U128 two64 = (U128)1 << 64;
    for (int s = 0; s < 64; ++s) {
        const U128 pw = (U128)1 << (64 + s);
        const U128 num = (pw + d - 1) / d;   // M = ceil(2^(64+s) / d)
        const U128 e = num * d - pw;         // 0 <= e < d by construction
        if (num < two64) {
            if (e <= ((U128)1 << s)) {       // fitting form
                out = {(u64)num, (u8)s, false};
                return true;
            }
        } else {
            // M >= 2^64  <=>  2^s >= d: increment form with M' = M - 2^64.
            // Computes floor(x*M / 2^(64+s)) exactly in u64 arithmetic
            // (0 <= t <= x-1 since M' < 2^64 — no wrap anywhere).
            out = {(u64)(num - two64), (u8)s, true};
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// public interface
// ---------------------------------------------------------------------------

DpIsel::DpIsel(LFunction& lf, FunctionGraph& fg) : lf_(lf), g_(fg.g) {
    block_of_.assign(g_.size(), -1);
    for (const LBlock& b : lf.blocks)
        for (NodeId n : b.nodes)
            if (n < block_of_.size()) block_of_[n] = b.index;
}

DpIsel::Act DpIsel::act(NodeId n) const {
    const u8* a = act_.find(n);
    return a ? static_cast<Act>(*a) : Act::None;
}

const DpIsel::ValCell* DpIsel::cell(NodeId n) const { return cells_.find(n); }

void DpIsel::plan_block(const LBlock& b, const FlatMap<NodeId, bool>& suppressed) {
    act_.clear();
    cells_.clear();
    scsup_ = &suppressed;
    cur_block_ = b.index;

    // Pass A: chain roots — Load/Store addresses. Always a win when the
    // address is a single-use pure chain in this block: the intermediates
    // never touch slots, so at minimum the store+load round trip is saved.
    // The frontend wraps every computed address in a ptrcast (a machine-level
    // identity); the chain is claimed through it and the cast is suppressed
    // when this root is its only user.
    for (NodeId n : b.nodes) {
        if (suppressed.find(n)) continue;
        const Node& nd = g_.node(n);
        if (nd.op != Op::Load && nd.op != Op::Store) continue;
        if (nd.op == Op::Load && ty_is_vector(nd.ty)) continue;
        NodeId addr = nd.in[2];
        NodeId cast_node = kNoNode;
        {
            const Node* ad = &g_.node(addr);
            int hops = 0;
            while (ad->op == Op::Cast && static_cast<CastOp>(ad->sub) == CastOp::Ptr &&
                   hops++ < 4) {
                if (!single_user_is(addr, n)) break; // cast value needed elsewhere
                cast_node = addr;
                addr = ad->in[1];
                ad = &g_.node(addr);
            }
        }
        if (!chainable_here(addr) || !lea_width(g_.node(addr).ty)) continue;
        // Root-level single-user invariant: the chain is computed inline by
        // exactly this root, so its value must have no other consumers. With
        // a cast, the chain's only user is the (suppressed) cast; without
        // one, the root itself. Multi-use addresses materialize once and are
        // shared — the right tradeoff anyway (found by t25_slp @ -O1: `a[0]`
        // read by the store and a later load).
        if (cast_node != kNoNode ? !single_user_is(addr, cast_node)
                                 : !single_user_is(addr, n))
            continue;
        if (!try_commit(addr)) continue;
        if (std::getenv("JULES_DP_TRACE"))
            std::fprintf(stderr, "[dpA] blk=%d root=n%u addr=n%u addr_uses=%zu cast=n%u\n",
                         cur_block_, n, addr, g_.uses_of(addr).size(), cast_node);
        claim_root(n);
        if (cast_node != kNoNode) {
            act_.insert(cast_node, static_cast<u8>(Act::Suppressed));
            ++folds_;
        }
        mark_consumed(addr);
    }

    // Pass B: Bin roots the rule set covers strictly cheaper than the hand
    // emitter (no ties — quality never regresses to a different encoding).
    // Div/Mod with a nonzero constant divisor joins here for the magic rules
    // (pow2 divisors are IR p12's shifts/masks; |d| == 1 folds there too).
    for (NodeId n : b.nodes) {
        if (act(n) != Act::None) continue;
        if (suppressed.find(n)) continue;
        const Node& nd = g_.node(n);
        if (nd.op != Op::Bin || !int_scalar(nd.ty)) continue;
        BinOp sub = static_cast<BinOp>(nd.sub);
        bool shl_const = sub == BinOp::Shl && g_.node(nd.in[2]).op == Op::Const;
        bool div_const = false;
        if (sub == BinOp::Div || sub == BinOp::Mod) {
            ConstVal cd{};
            div_const = g_.node(nd.in[2]).op == Op::Const &&
                        const_of(g_, nd.in[2], cd) && cd.iv != 0;
        }
        if (!chain_sub(sub) && !shl_const && !div_const) continue;
        if (!try_commit(n)) continue;
        const ValCell* c = cells_.find(n);
        if (c->cost + C_ST < fallback_mat(n)) {
            claim_root(n);
            mark_children_consumed(n); // n's inline chain operands only
        }
        // else: explored but not claimed — the cells are stale memo data no
        // emission path consults (emission follows the claimed forms only)
    }
}

// ---------------------------------------------------------------------------
// solver internals
// ---------------------------------------------------------------------------

bool DpIsel::chainable_here(NodeId x) const {
    if (x == kNoNode || x >= g_.size()) return false;
    if (scsup_ && scsup_->find(x)) return false;
    if (static_cast<size_t>(x) >= block_of_.size()) return false;
    if (block_of_[x] != cur_block_) return false;
    const Node& nd = g_.node(x);
    if (nd.op != Op::Bin) return false;
    if (!int_scalar(nd.ty)) return false;
    BinOp op = static_cast<BinOp>(nd.sub);
    if (chain_sub(op)) return true;
    // Shl with a constant count is chainable (ShiftImm); variable counts
    // need %cl and keep the default path.
    return op == BinOp::Shl && g_.node(nd.in[2]).op == Op::Const;
}

bool DpIsel::single_user_is(NodeId c, NodeId parent) {
    const SmallVec<NodeId, 4>& u = g_.uses_of(c);
    return u.size() == 1 && u[0] == parent;
}

i32 DpIsel::leaf_cost(NodeId x) const {
    return g_.node(x).op == Op::Const ? C_IMM : C_LD;
}

DpIsel::OpRef DpIsel::consume(NodeId x, NodeId parent) {
    OpRef r;
    if (chainable_here(x) && single_user_is(x, parent)) {
        r.k = OpRef::K::Chain;
        r.node = x;
    } else {
        r.k = OpRef::K::Leaf;
        r.node = x;
    }
    return r;
}

i32 DpIsel::opref_cost(const OpRef& r) {
    switch (r.k) {
        case OpRef::K::Leaf: return leaf_cost(r.node);
        case OpRef::K::Chain: {
            const ValCell* c = cells_.find(r.node);
            return c ? c->cost : kInf; // committed by try_commit before use
        }
        case OpRef::K::Imm: return 0;
    }
    return kInf;
}

bool DpIsel::try_commit(NodeId n) {
    if (const ValCell* memo = cells_.find(n)) return memo->form != ValCell::Form::Leaf;
    if (depth_ > 96) return false; // pathological-chain guard
    ++depth_;
    struct Guard { int& d; ~Guard() { --d; } } guard{depth_};

    const Node& nd = g_.node(n);
    BinOp op = static_cast<BinOp>(nd.sub);
    u8 size = ty_bits(nd.ty) == 32 ? 4 : 8;

    ValCell best;
    i32 best_cost = kInf;
    int best_instrs = 0x7fffffff;
    int best_rank = 0x7fffffff;
    auto consider = [&](ValCell c, i32 cost, int instrs, int rank) {
        if (cost < best_cost ||
            (cost == best_cost &&
             (instrs < best_instrs ||
              (instrs == best_instrs && rank < best_rank)))) {
            best = c;
            best_cost = cost;
            best_instrs = instrs;
            best_rank = rank;
        }
    };

    // ---- rule: ShlImm — Mul(x, 2^k) or Shl(x, const k) --------------------
    {
        NodeId x = kNoNode;
        u8 k = 0;
        if (op == BinOp::Mul) {
            const Node& a = g_.node(nd.in[1]);
            const Node& b = g_.node(nd.in[2]);
            if (a.op == Op::Const && pow2_shift(a.ival, k)) x = nd.in[2];
            else if (b.op == Op::Const && pow2_shift(b.ival, k)) x = nd.in[1];
        } else if (op == BinOp::Shl) {
            const Node& cnt = g_.node(nd.in[2]);
            if (cnt.op == Op::Const && cnt.ival >= 0 && cnt.ival <= 63) {
                k = static_cast<u8>(cnt.ival);
                x = nd.in[1];
            }
        }
        if (x != kNoNode) {
            OpRef src = consume(x, n);
            ValCell c;
            c.form = ValCell::Form::ShlImm;
            c.a = src;
            c.shift = k;
            c.size = size;
            consider(c, opref_cost(src) + C_OP,
                     (src.k == OpRef::K::Chain ? 0 : 1) + 1, 0);
        }
    }

    // ---- rules: Lea2 (full SIB) / LeaRR (baseless) — 64-bit chains --------
    if (lea_width(nd.ty) && (op == BinOp::Add || op == BinOp::Mul)) {
        NodeId base = kNoNode, idx = kNoNode;
        bool has_base = false, has_idx = false;
        u8 scale = 1;
        i64 disp = 0;
        bool matched = false;
        NodeId absorb1 = kNoNode, absorb2 = kNoNode;

        if (op == BinOp::Add) {
            NodeId a = nd.in[1], b = nd.in[2];
            NodeId x = kNoNode;
            ConstVal cv{};
            if (const_of(g_, b, cv)) { x = a; disp = cv.iv; }
            else if (const_of(g_, a, cv)) { x = b; disp = cv.iv; }
            if (x == kNoNode) {
                // no displacement: [p + q] — SIB with disp 0
                if (index_term(g_, b, idx, scale)) {
                    base = a; has_base = has_idx = true; matched = true;
                    if (single_user_is(b, n)) absorb1 = b;
                } else if (index_term(g_, a, idx, scale)) {
                    base = b; has_base = has_idx = true; matched = true;
                    if (single_user_is(a, n)) absorb1 = a;
                }
            } else {
                // unwrap one nested const addend: [p + c2] + d — the
                // unwrapped Add is shape-absorbed
                const Node& xd = g_.node(x);
                if (xd.op == Op::Bin && static_cast<BinOp>(xd.sub) == BinOp::Add &&
                    lea_width(xd.ty)) {
                    ConstVal c2{};
                    if (const_of(g_, xd.in[2], c2)) {
                        disp += c2.iv;
                        if (single_user_is(x, n)) absorb2 = x;
                        x = xd.in[1];
                    } else if (const_of(g_, xd.in[1], c2)) {
                        disp += c2.iv;
                        if (single_user_is(x, n)) absorb2 = x;
                        x = xd.in[2];
                    }
                }
                const Node& xm = g_.node(x);
                if (xm.op == Op::Bin && static_cast<BinOp>(xm.sub) == BinOp::Add &&
                    lea_width(xm.ty)) {
                    // [p + q] + d: index term on either side, or base+idx
                    NodeId i2 = kNoNode;
                    u8 s2 = 1;
                    if (index_term(g_, xm.in[2], i2, s2)) {
                        base = xm.in[1]; idx = i2; scale = s2;
                        has_base = has_idx = true; matched = true;
                        if (single_user_is(xm.in[2], x)) absorb1 = xm.in[2];
                        if (single_user_is(x, n)) absorb2 = x;
                    } else if (index_term(g_, xm.in[1], i2, s2)) {
                        base = xm.in[2]; idx = i2; scale = s2;
                        has_base = has_idx = true; matched = true;
                        if (single_user_is(xm.in[1], x)) absorb1 = xm.in[1];
                        if (single_user_is(x, n)) absorb2 = x;
                    } else if (disp != 0) {
                        // base + idx + disp (scale 1): one lea covers what
                        // the default path emits as add + add-imm
                        base = xm.in[1]; idx = xm.in[2]; scale = 1;
                        has_base = has_idx = true; matched = true;
                        if (single_user_is(x, n)) absorb2 = x;
                    }
                }
                if (!matched) {
                    // [index-term + d]: baseless — idx*scale + disp
                    NodeId i2 = kNoNode;
                    u8 s2 = 1;
                    if (index_term(g_, x, i2, s2)) {
                        idx = i2; scale = s2; has_idx = true; matched = true;
                        if (single_user_is(x, n)) absorb2 = x;
                    }
                }
            }
        } else { // Mul by 3/5/9: lea x + x*scale
            const Node& a = g_.node(nd.in[1]);
            const Node& b = g_.node(nd.in[2]);
            auto odd = [&](i64 v) -> u8 {
                if (v == 3) return 2;
                if (v == 5) return 4;
                if (v == 9) return 8;
                return 0;
            };
            if (a.op == Op::Const && b.op != Op::Const && odd(a.ival)) {
                base = idx = nd.in[2]; scale = odd(a.ival);
                has_base = has_idx = true; matched = true;
            } else if (b.op == Op::Const && a.op != Op::Const && odd(b.ival)) {
                base = idx = nd.in[1]; scale = odd(b.ival);
                has_base = has_idx = true; matched = true;
            }
        }

        if (matched && (has_base || disp != 0) && fits32(disp)) {
            // Look through machine-identity ptrcasts of always-materialized
            // operands (params, constants): reading the underlying slot is
            // strictly better than the cast's own round trip (which then
            // goes dead).
            auto skip_simple_ptrcast = [&](NodeId& x) {
                const Node* d = &g_.node(x);
                int hops = 0;
                while (d->op == Op::Cast && static_cast<CastOp>(d->sub) == CastOp::Ptr &&
                       hops++ < 4) {
                    const Node& u = g_.node(d->in[1]);
                    if (u.op != Op::Param && u.op != Op::Const) break;
                    x = d->in[1];
                    d = &g_.node(x);
                }
            };
            if (has_base) {
                skip_simple_ptrcast(base);
                skip_simple_ptrcast(idx);
            } else {
                skip_simple_ptrcast(idx);
            }
            ValCell c;
            c.form = has_base ? ValCell::Form::Lea2 : ValCell::Form::LeaRR;
            c.absorb1 = absorb1;
            c.absorb2 = absorb2;
            c.scale = scale;
            c.disp = disp;
            c.size = 8;
            i32 cost = 0;
            int instrs = 1;
            if (has_base) {
                c.base = consume(base, n);
                c.idx = consume(idx, n);
                // scratch discipline: at most one Chain operand per lea —
                // a chain completes using both scratch regs, so the second
                // operand must be a plain leaf load
                if (c.base.k == OpRef::K::Chain && c.idx.k == OpRef::K::Chain)
                    c.idx.k = OpRef::K::Leaf;
                cost += opref_cost(c.base) + opref_cost(c.idx);
                instrs += (c.base.k == OpRef::K::Chain ? 0 : 1) +
                          (c.idx.k == OpRef::K::Chain ? 0 : 1);
            } else {
                c.idx = consume(idx, n);
                cost += opref_cost(c.idx);
                instrs += c.idx.k == OpRef::K::Chain ? 0 : 1;
            }
            cost += C_OP;
            consider(c, cost, instrs, c.form == ValCell::Form::Lea2 ? 3 : 2);
        }
    }

    // ---- rule: Magic — Div/Mod by a non-pow2 constant ----------------------
    // Root-only standalone form (dividend is a Leaf: the sequence uses the
    // rax:rdx pair on top of {dst, other}). Verified identities above.
    if (op == BinOp::Div || op == BinOp::Mod) {
        ConstVal cd{};
        if (const_of(g_, nd.in[2], cd) && cd.iv != 0) {
            const bool sgn = ty_is_signed(nd.ty);
            const i64 d = cd.iv;
            // |d| as u64 (INT64_MIN-safe); unsigned divisors use their bits.
            const u64 ad = !sgn ? (u64)d
                               : d < 0 ? (u64)(-(d + 1)) + 1 : (u64)d;
            Magic mg;
            if ((sgn ? (d > 1 || d < -1) : true) && derive_magic(ad, mg)) {
                ValCell c;
                c.form = ValCell::Form::Magic;
                c.a = OpRef{OpRef::K::Leaf, nd.in[1], 0};
                c.size = size;
                c.divisor = d;
                c.magic = mg.m;
                c.mshift = mg.s;
                c.form_b = mg.form_b;
                c.signed_div = sgn;
                c.is_mod = (op == BinOp::Mod);
                i32 cost = leaf_cost(nd.in[1]);       // load x
                int instrs = 1;                       // ...the load
                if (size == 4 && sgn) { cost += C_OP; ++instrs; } // SExt32
                if (sgn) { cost += 6 * C_OP; instrs += 6; }     // abs wrapper
                cost += C_IMM; ++instrs;                            // movabs M'
                cost += C_IMUL; ++instrs;                          // mul
                cost += C_OP; ++instrs;                             // mov hi
                if (mg.form_b) {
                    cost += 3 * C_OP; instrs += 3; // sub+shr1+add (x in other)
                } else if (mg.s > 0) {
                    cost += C_OP; ++instrs;         // shr s
                }
                if (sgn) { cost += 2 * C_OP; instrs += 2; } // xor+sub sign
                if (sgn && d < 0) { cost += C_OP; ++instrs; } // neg
                if (c.is_mod) {
                    // r = x - q*d: movabs d, imul, reload x, sub, mov
                    cost += C_IMM + C_IMUL + C_LD + 2 * C_OP;
                    instrs += 5;
                }
                consider(c, cost, instrs, 4);
            }
        }
    }

    // ---- rule: generic Arith (b-side may be an inline chain) ----------------
    // Div/Mod never take this form: the serializer's ArithRR has no idiv
    // encoding (the hand emitter uses the Cqo+IDiv pair), and the magic rule
    // above is strictly better anyway.
    if (op != BinOp::Shl && op != BinOp::Div && op != BinOp::Mod) {
        ValCell c;
        c.form = ValCell::Form::Arith;
        c.bin = op;
        c.size = size;
        NodeId a = nd.in[1], b = nd.in[2];
        // keep any single-use chain on the b-side (scratch discipline);
        // commute when it sits on the a-side and the op allows it
        if (chainable_here(a) && single_user_is(a, n) && op_commutes(op)) {
            NodeId t = a;
            a = b;
            b = t;
        }
        c.a = OpRef{OpRef::K::Leaf, a, 0};
        ConstVal cb{};
        if (const_of(g_, b, cb) && !(cb.iv == 0 && (op == BinOp::Add || op == BinOp::Sub))) {
            c.b = OpRef{OpRef::K::Imm, kNoNode, cb.iv};
            c.big_imm = !fits32(cb.iv);
        } else {
            c.b = consume(b, n);
        }
        i32 bcost = 0;
        int instrs = 2;
        if (c.b.k == OpRef::K::Imm) {
            if (c.big_imm) { bcost = C_IMM; ++instrs; }
        } else {
            bcost = opref_cost(c.b);
            if (c.b.k != OpRef::K::Chain) ++instrs;
        }
        consider(c, leaf_cost(a) + bcost + op_cost(op), instrs, 1);
    }

    if (best_cost >= kInf) return false;
    best.cost = best_cost;
    cells_.insert(n, best);
    return true;
}

// What the hand emitter (emit_int_bin) pays for this node: every operand is
// a flat slot-load or immediate, then one op, then a store.
i32 DpIsel::fallback_mat(NodeId n) const {
    const Node& nd = g_.node(n);
    BinOp op = static_cast<BinOp>(nd.sub);
    NodeId a = nd.in[1], b = nd.in[2];
    auto opnd = [&](NodeId x) { return g_.node(x).op == Op::Const ? C_IMM : C_LD; };
    if (op == BinOp::Div || op == BinOp::Mod) {
        // Cqo (signed) / xor edx (unsigned) + idiv; the remainder arrives in
        // rdx for Mod (+1 mov). Const divisor materializes via movabs.
        return opnd(a) + C_IMM + C_OP + C_DIV + (op == BinOp::Mod ? C_OP : 0) +
               C_ST;
    }
    if (op == BinOp::Shl || op == BinOp::Shr) {
        if (g_.node(b).op == Op::Const) return opnd(a) + C_OP + C_ST;
        return opnd(a) + C_LD + C_OP + C_ST; // ShiftCl
    }
    ConstVal cb{};
    if (const_of(g_, b, cb)) {
        if (cb.iv == 0 && (op == BinOp::Add || op == BinOp::Sub))
            return opnd(a) + C_ST; // identity fold: load + store
        if (fits32(cb.iv)) return opnd(a) + op_cost(op) + C_ST;
        return opnd(a) + C_IMM + C_OP + C_ST; // movabs + arith
    }
    return opnd(a) + opnd(b) + op_cost(op) + C_ST;
}

void DpIsel::claim_root(NodeId n) {
    act_.insert(n, static_cast<u8>(Act::Root));
    ++roots_;
}

// Mark the nodes a claim consumes inline (never emitted at their own
// position), following the committed cells' Chain edges.
void DpIsel::mark_consumed(NodeId n) {
    if (act(n) != Act::None) return;
    act_.insert(n, static_cast<u8>(Act::Suppressed));
    ++folds_;
    mark_children_consumed(n);
}

void DpIsel::mark_children_consumed(NodeId n) {
    const ValCell* c = cells_.find(n);
    if (!c) return;
    const OpRef* refs[3] = {&c->base, &c->idx, &c->b};
    for (const OpRef* r : refs)
        if (r->k == OpRef::K::Chain && r->node != kNoNode) mark_consumed(r->node);
    NodeId absorbed[2] = {c->absorb1, c->absorb2};
    for (NodeId a : absorbed)
        if (a != kNoNode) mark_consumed(a);
}

} // namespace jules
