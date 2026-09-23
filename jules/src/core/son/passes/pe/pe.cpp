// PE/PD core: binding-time analysis, the specialized-graph fold engine,
// variant cloning/dedup/budgets, and call-user rewiring. See pe.h.
#include "core/son/passes/pe/pe.h"

#include "core/son/son.h"

#include <algorithm>
#include <bit>

namespace jules {

// ---- budgets -----------------------------------------------------------------

PeBudgets pe_budgets(OptLevel lvl) {
    PeBudgets b{};
    switch (lvl) {
        case OptLevel::O0:
        case OptLevel::Og:
        case OptLevel::O1:
            b.max_variants_per_fn = 0; // off (matrix also gates by level)
            b.growth_num = 1;
            b.growth_den = 1;
            b.total_nodes = 0;
            break;
        case OptLevel::O2:
            b.max_variants_per_fn = 2;
            b.growth_num = 5;  // 1.25x per variant (shared across sites)
            b.growth_den = 4;
            b.total_nodes = 1024;
            b.range_max_span = 256;  // range hulls up to +-256 wide
            break;
        case OptLevel::O3:
            b.max_variants_per_fn = 4;
            b.growth_num = 3;  // 1.5x
            b.growth_den = 2;
            b.total_nodes = 4096;
            b.range_max_span = 4096;
            break;
        case OptLevel::Os:
            b.max_variants_per_fn = 1; // size-first: only the top rung
            b.growth_num = 9;           // 1.125x
            b.growth_den = 8;
            b.total_nodes = 256;
            b.range_max_span = 16;      // only near-constant hulls pay
            break;
        case OptLevel::Oz:
            b.max_variants_per_fn = 0; // size-first: no PE
            b.growth_num = 1;
            b.growth_den = 1;
            b.total_nodes = 0;
            break;
    }
    return b;
}

// ---- shared internals ----------------------------------------------------------

namespace {

// Lattice: Top (optimistic) -> Const -> Range -> Bottom (dynamic).
// Range is the widening rung: a value known to lie in [lo, hi] (signed
// domain; single-point ranges lo == hi carry the same information as a
// Const but meet through the hull like any range). The propagation is
// monotone down this chain — inputs only descend, meets only widen hulls.
enum class Lat : u8 { Top, Const, Range, Bottom };
struct LatVal {
    Lat kind = Lat::Top;
    ConstVal v;
    i64 lo = 0, hi = 0; // Range
};

u32 lat_rank(Lat k) {
    switch (k) {
        case Lat::Top: return 0;
        case Lat::Const: return 1;
        case Lat::Range: return 2;
        case Lat::Bottom: return 3;
    }
    return 3;
}

LatVal range_of(i64 lo, i64 hi) {
    LatVal r;
    r.kind = Lat::Range;
    r.lo = lo;
    r.hi = hi;
    return r;
}

LatVal as_range(const LatVal& a) { // Const -> single-point range
    if (a.kind == Lat::Range) return a;
    if (a.kind == Lat::Const)
        return range_of(a.v.iv, a.v.iv);
    return a; // Top / Bottom pass through
}

// Cap on how wide a PROPAGATED hull may get before we give up and call
// the value dynamic. Soundness never depends on it (wider hulls are still
// sound); it only bounds the usefulness/precision trade.
constexpr u64 kRangePropSpanCap = 1ull << 20;

// Control reachability from Start. A Region is reachable when any
// predecessor is (the optimistic-but-sound direction for phi meets: a phi
// input arriving from an unreachable predecessor must not taint the meet).
// (Graph is non-const: uses_of lazily rebuilds its cache.)
FlatMap<NodeId, bool> control_reachable(Graph& g) {
    FlatMap<NodeId, bool> reach;
    SmallVec<NodeId, 32> work;
    reach.insert(g.start(), true);
    work.push_back(g.start());
    while (!work.empty()) {
        NodeId b = work.back();
        work.pop_back();
        for (NodeId u : g.uses_of(b)) {
            const Node& un = g.node(u);
            if (un.op == Op::Dead) continue;
            bool ctrl_edge = false;
            if (un.op == Op::Jump && un.in[0] == b) ctrl_edge = true;
            else if (un.op == Op::Region) {
                for (u8 i = 0; i < un.n_in; ++i)
                    if (un.in[i] == b) { ctrl_edge = true; break; }
            } else if (un.op == Op::If && un.in[0] == b) {
                // both projections are structurally reachable here; a
                // const condition prunes its dead side in the rewrite phase
                for (NodeId p : g.uses_of(u)) {
                    Op po = g.node(p).op;
                    if ((po == Op::IfTrue || po == Op::IfFalse) &&
                        !reach.contains(p)) {
                        reach.insert(p, true);
                        work.push_back(p);
                    }
                }
                continue;
            }
            if (ctrl_edge && !reach.contains(u)) {
                reach.insert(u, true);
                work.push_back(u);
            }
        }
    }
    return reach;
}

// ---- range transfers (signed integer domain) ---------------------------------
//
// Hull from i64 endpoints: ordered, and span (exact in u64 when lo <= hi
// as signed) within the propagation cap.
bool hull_ok(i64 lo, i64 hi) {
    if (lo > hi) return false;
    u64 span = static_cast<u64>(hi) - static_cast<u64>(lo);
    return span <= kRangePropSpanCap;
}

LatVal eval_bin_range(BinOp op, const LatVal& a, const LatVal& b) {
    // FP stays Const-only (no fp ranges in the MVP).
    if ((a.kind == Lat::Const && a.v.is_fp) || (b.kind == Lat::Const && b.v.is_fp))
        return LatVal{Lat::Bottom, {}};
    if (a.kind == Lat::Bottom || b.kind == Lat::Bottom) return LatVal{Lat::Bottom, {}};

    // both Const: exact (existing evaluator, includes wrap semantics)
    if (a.kind == Lat::Const && b.kind == Lat::Const) {
        ConstVal out;
        if (!eval_bin_const(op, a.v, b.v, out)) return LatVal{Lat::Bottom, {}};
        return LatVal{Lat::Const, out};
    }

    // interval cases (Add / Sub always; Mul only Const x Range). Overflow
    // in i64 gives up (Bottom) — the wrap semantics live in the exact
    // Const evaluator only; hulls stay in-range by construction.
    LatVal x = as_range(a), y = as_range(b);
    if (x.kind != Lat::Range || y.kind != Lat::Range) return LatVal{Lat::Bottom, {}};
    i64 lo, hi;
    switch (op) {
        case BinOp::Add: {
            if (__builtin_add_overflow(x.lo, y.lo, &lo)) break;
            if (__builtin_add_overflow(x.hi, y.hi, &hi)) break;
            if (!hull_ok(lo, hi)) break;
            return range_of(lo, hi);
        }
        case BinOp::Sub: {
            if (__builtin_sub_overflow(x.lo, y.hi, &lo)) break;
            if (__builtin_sub_overflow(x.hi, y.lo, &hi)) break;
            if (!hull_ok(lo, hi)) break;
            return range_of(lo, hi);
        }
        case BinOp::Mul: {
            i64 c;
            LatVal r;
            if (a.kind == Lat::Const && b.kind == Lat::Range) { c = a.v.iv; r = y; }
            else if (b.kind == Lat::Const && a.kind == Lat::Range) { c = b.v.iv; r = x; }
            else break; // Range x Range
            if (__builtin_mul_overflow(c, r.lo, &lo)) break;
            if (__builtin_mul_overflow(c, r.hi, &hi)) break;
            if (lo > hi) { i64 t = lo; lo = hi; hi = t; } // c < 0 flips
            if (!hull_ok(lo, hi)) break;
            return range_of(lo, hi);
        }
        default:
            break; // Div/Mod/bitwise/shifts/min/max: no interval model
    }
    return LatVal{Lat::Bottom, {}};
}

// Compare an interval against a constant (or another interval): the
// result is 0/1 when the relation holds for EVERY value of the hull(s)
// or for NO value — otherwise Bottom.
LatVal eval_cmp_range(CmpOp op, const LatVal& a, const LatVal& b) {
    if ((a.kind == Lat::Const && a.v.is_fp) || (b.kind == Lat::Const && b.v.is_fp))
        return LatVal{Lat::Bottom, {}};
    if (a.kind == Lat::Bottom || b.kind == Lat::Bottom) return LatVal{Lat::Bottom, {}};
    if (a.kind == Lat::Const && b.kind == Lat::Const) {
        ConstVal out;
        if (!eval_cmp_const(op, a.v, b.v, out)) return LatVal{Lat::Bottom, {}};
        return LatVal{Lat::Const, out};
    }

    // normalize: (left hull, right hull), both as intervals
    LatVal x = as_range(a), y = as_range(b);
    if (x.kind != Lat::Range || y.kind != Lat::Range) return LatVal{Lat::Bottom, {}};
    auto b01 = [&](bool v) {
        LatVal r;
        r.kind = Lat::Const;
        r.v.is_fp = false;
        r.v.ty = ty_i1();
        r.v.iv = v ? 1 : 0;
        return r;
    };
    // Relation on the endpoints (signed):
    //   definitely-true forms and definitely-false forms per CmpOp.
    switch (op) {
        case CmpOp::Lt: // x < y: true iff x.hi < y.lo; false iff x.lo >= y.hi
            if (x.hi < y.lo) return b01(true);
            if (x.lo >= y.hi) return b01(false);
            break;
        case CmpOp::Le: // true iff x.hi <= y.lo; false iff x.lo > y.hi
            if (x.hi <= y.lo) return b01(true);
            if (x.lo > y.hi) return b01(false);
            break;
        case CmpOp::Gt: // true iff x.lo > y.hi; false iff x.hi <= y.lo
            if (x.lo > y.hi) return b01(true);
            if (x.hi <= y.lo) return b01(false);
            break;
        case CmpOp::Ge: // true iff x.lo >= y.hi; false iff x.hi < y.lo
            if (x.lo >= y.hi) return b01(true);
            if (x.hi < y.lo) return b01(false);
            break;
        case CmpOp::Eq: // true only for identical single points; false on
            // disjoint hulls. Overlapping-but-not-identical hulls stay
            // dynamic (a meet may include values outside the overlap).
            if (x.lo == x.hi && y.lo == y.hi) return b01(x.lo == y.lo);
            if (x.hi < y.lo || y.hi < x.lo) return b01(false);
            break;
        case CmpOp::Ne:
            if (x.lo == x.hi && y.lo == y.hi) return b01(x.lo != y.lo);
            if (x.hi < y.lo || y.hi < x.lo) return b01(true);
            break;
    }
    return LatVal{Lat::Bottom, {}};
}

// Worklist constant propagation over the value lattice. Seeding is a
// separate phase so the BTA can override the (otherwise Bottom) params
// with binding constants BEFORE the drain, and the fold engine can
// classify every node (nothing stays Top: users of nothing would never be
// scheduled otherwise).
class Prop {
public:
    Prop(Graph& g, const FlatMap<NodeId, bool>& reach) : g_(g), reach_(reach) {}

    void seed_const(NodeId n, const LatVal& v) { set(n, v); }

    void seed_graph(const std::vector<PeAssumption>* bindings) {
        // Phase 1: constants are block-independent facts (pre-seeding
        // matches classic SCCP; without it a pure op in a live block whose
        // Const operand sits in a not-yet-visited block stalls at Top).
        for (NodeId id = 0; id < g_.size(); ++id) {
            if (g_.node(id).op == Op::Const) {
                LatVal v;
                v.kind = Lat::Const;
                const_of(g_, id, v.v);
                set(id, v);
            }
        }
        // Phase 2: parameters — bound ones carry the binding (Const value
        // or Range hull), the rest are dynamic by construction. Bound
        // params must be seeded BEFORE the Bottom sweep so the lattice
        // stays monotone.
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op != Op::Param) continue;
            const PeAssumption* a = nullptr;
            if (bindings)
                for (const PeAssumption& x : *bindings)
                    if (x.param == n.aux) { a = &x; break; }
            if (a != nullptr && a->kind == PeKind::Const) {
                LatVal v;
                v.kind = Lat::Const;
                v.v = a->value;
                set(id, v);
            } else if (a != nullptr && a->kind == PeKind::Range) {
                set(id, range_of(a->range_lo, a->range_hi));
            } else {
                set(id, LatVal{Lat::Bottom, {}});
            }
        }
    }

    void drain() {
        u32 steps = 0;
        while (!work_.empty() && ++steps <= kStepLimit) {
            NodeId n = work_.back();
            work_.pop_back();
            process(n);
        }
    }

    const LatVal* find(NodeId n) const { return lat_.find(n); }

private:
    LatVal get(NodeId n) {
        if (const LatVal* l = lat_.find(n)) return *l;
        return LatVal{};
    }

    void set(NodeId n, const LatVal& v) {
        LatVal old = get(n);
        if (lat_rank(old.kind) > lat_rank(v.kind)) return; // never ascend

        if (old.kind == Lat::Range && v.kind == Lat::Range) {
            // Hull GROWTH is the dangerous direction: a loop-carried phi
            // (i = phi(0, i+1)) widens by 1 per visit and has no fixpoint
            // below the span cap; the step limit would truncate the drain
            // mid-widening and leave STALE Const values on its users
            // (observed: `i < 7` still Const(true) from the [0,1] era ->
            // stage 2 pruned the live exit — the t24 miscompile class).
            // Two growths prove the value is cyclic: collapse to Bottom,
            // the classic SCCP jump to overdefined. Stable meets (phi of
            // independent branch values, range-seeded params) never grow
            // and keep their hull.
            if (!(v.lo < old.lo || v.hi > old.hi)) return; // not growing
            u8 w = 0;
            if (const u8* pw = widened_.find(n)) w = *pw;
            if (w >= 2) {
                lat_.insert(n, LatVal{Lat::Bottom, {}});
                for (NodeId u : g_.uses_of(n)) work_.push_back(u);
                return;
            }
            widened_.insert(n, static_cast<u8>(w + 1));
            lat_.insert(n, v);
            for (NodeId u : g_.uses_of(n)) work_.push_back(u);
            return;
        }

        if (old.kind == v.kind) {
            if (old.kind == Lat::Top || old.kind == Lat::Bottom) return;
            // Const: a differing value is a lattice violation in a
            // monotone drain; treat as a change (defensive, never observed)
            bool diff = (old.v.is_fp != v.v.is_fp) ||
                        (v.v.is_fp ? old.v.fv != v.v.fv : old.v.iv != v.v.iv);
            if (!diff) return;
        }
        // rank descent (Top->Const, Const->Range, *->Bottom) or the
        // defensive Const re-set above
        lat_.insert(n, v);
        for (NodeId u : g_.uses_of(n)) work_.push_back(u);
    }

    static LatVal meet(const LatVal& a, const LatVal& b) {
        if (a.kind == Lat::Top) return b;
        if (b.kind == Lat::Top) return a;
        if (a.kind == Lat::Bottom || b.kind == Lat::Bottom)
            return LatVal{Lat::Bottom, {}};
        if (a.kind == Lat::Const && b.kind == Lat::Const) {
            if (a.v.is_fp != b.v.is_fp) return LatVal{Lat::Bottom, {}};
            if (a.v.is_fp ? a.v.fv == b.v.fv : a.v.iv == b.v.iv) return a;
            if (a.v.is_fp) return LatVal{Lat::Bottom, {}}; // fp has no hull
            return range_of(a.v.iv < b.v.iv ? a.v.iv : b.v.iv,
                            a.v.iv < b.v.iv ? b.v.iv : a.v.iv);
        }
        if (a.v.is_fp || b.v.is_fp) return LatVal{Lat::Bottom, {}};
        LatVal x = as_range(a), y = as_range(b); // Const -> single point
        if (x.kind != Lat::Range || y.kind != Lat::Range)
            return LatVal{Lat::Bottom, {}};
        i64 lo = x.lo < y.lo ? x.lo : y.lo;
        i64 hi = x.hi > y.hi ? x.hi : y.hi;
        if (!hull_ok(lo, hi)) return LatVal{Lat::Bottom, {}};
        return range_of(lo, hi);
    }

    void process(NodeId n) {
        const Node& nd = g_.node(n);
        if (nd.op == Op::Dead) return;
        switch (nd.op) {
            case Op::Const: {
                LatVal v;
                v.kind = Lat::Const;
                const_of(g_, n, v.v);
                set(n, v);
                return;
            }
            case Op::Param:
                return; // seeded (Const or Bottom) in the seed phase
            case Op::Bin: {
                LatVal a = get(nd.in[1]), b = get(nd.in[2]);
                if (a.kind == Lat::Top || b.kind == Lat::Top) return;
                set(n, eval_bin_range(static_cast<BinOp>(nd.sub), a, b));
                return;
            }
            case Op::Cmp: {
                LatVal a = get(nd.in[1]), b = get(nd.in[2]);
                if (a.kind == Lat::Top || b.kind == Lat::Top) return;
                set(n, eval_cmp_range(static_cast<CmpOp>(nd.sub), a, b));
                return;
            }
            case Op::Un: {
                LatVal a = get(nd.in[1]);
                if (a.kind == Lat::Top) return;
                if (a.kind == Lat::Bottom) {
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                if (a.kind == Lat::Range &&
                    static_cast<UnOp>(nd.sub) == UnOp::Neg) {
                    i64 lo, hi;
                    if (!__builtin_sub_overflow(0, a.hi, &lo) &&
                        !__builtin_sub_overflow(0, a.lo, &hi) &&
                        hull_ok(lo, hi)) {
                        set(n, range_of(lo, hi));
                    } else {
                        set(n, LatVal{Lat::Bottom, {}});
                    }
                    return;
                }
                if (a.kind == Lat::Range) { // Not/BNot over an interval:
                    set(n, LatVal{Lat::Bottom, {}}); // no interval model
                    return;
                }
                ConstVal out;
                if (!eval_un_const(static_cast<UnOp>(nd.sub), a.v, out)) {
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                set(n, LatVal{Lat::Const, out});
                return;
            }
            case Op::Cast: {
                LatVal a = get(nd.in[1]);
                if (a.kind == Lat::Top) return;
                if (a.kind == Lat::Bottom) {
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                if (a.kind == Lat::Range) {
                    // width-changing casts need domain-crossing hulls
                    // (truncation wraps); out of MVP scope
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                ConstVal out;
                if (!eval_cast_const(static_cast<CastOp>(nd.sub), a.v, nd.ty, out)) {
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                set(n, LatVal{Lat::Const, out});
                return;
            }
            case Op::Select: {
                LatVal c = get(nd.in[1]);
                if (c.kind == Lat::Const) {
                    set(n, get(c.v.iv != 0 ? nd.in[2] : nd.in[3]));
                    return;
                }
                // dynamic condition (Bottom) or an unknown boolean hull
                // (Range, e.g. a meet of 0 and 1): either arm may run
                if (c.kind == Lat::Bottom || c.kind == Lat::Range)
                    set(n, meet(get(nd.in[2]), get(nd.in[3])));
                return;
            }
            case Op::Phi: {
                NodeId region = nd.in[0];
                const Node& r = g_.node(region);
                LatVal acc;
                bool any = false;
                for (u8 i = 0; i < r.n_in; ++i) {
                    if (!reach_.contains(r.in[i])) continue;
                    any = true;
                    acc = meet(acc, get(nd.in[i + 1]));
                    if (acc.kind == Lat::Bottom) break;
                }
                if (any) set(n, acc);
                return;
            }
            case Op::Load:
            case Op::Call:
            case Op::Alloc:
                set(n, LatVal{Lat::Bottom, {}}); // effect-correct: dynamic
                return;
            default:
                return; // control / memory nodes carry no lattice value
        }
    }

    static constexpr u32 kStepLimit = 100000;

    Graph& g_;
    const FlatMap<NodeId, bool>& reach_;
    FlatMap<NodeId, LatVal> lat_;
    FlatMap<NodeId, u8> widened_; // Range growth count (loop-widening cap)
    std::vector<NodeId> work_;
};

} // namespace

// ---- binding-time analysis ------------------------------------------------------

PeBta pe_binding_time_analysis(Graph& g, const std::vector<PeAssumption>& bindings) {
    FlatMap<NodeId, bool> reach = control_reachable(g);
    Prop p(g, reach);
    p.seed_graph(&bindings);
    p.drain();

    PeBta out;
    for (NodeId id = 0; id < g.size(); ++id) {
        const Node& n = g.node(id);
        if (n.op == Op::Dead) continue;
        if (is_pure_op(n.op) || n.op == Op::Phi) {
            const LatVal* l = p.find(id);
            if (l && l->kind == Lat::Const) ++out.static_values;
            else if (l && l->kind == Lat::Bottom) ++out.dynamic_values;
        } else if (n.op == Op::If) {
            const LatVal* l = p.find(n.in[1]);
            if (l && l->kind == Lat::Const) ++out.static_ifs;
        }
    }
    return out;
}

// ---- fold engine -----------------------------------------------------------------

namespace {

bool fold_round(Graph& g, const std::vector<PeAssumption>* bindings) {
    bool changed = false;
    FlatMap<NodeId, bool> reach = control_reachable(g);
    Prop prop(g, reach);
    prop.seed_graph(bindings);
    prop.drain();
    FlatMap<NodeId, bool> dead_ctrl;

    // 1) constant pure nodes / phis -> Const nodes (reachable blocks only)
    for (NodeId id = 0; id < g.size(); ++id) {
        Node& n = g.node(id);
        if (n.op == Op::Dead || n.op == Op::Const) continue;
        if (!is_pure_op(n.op) && n.op != Op::Phi) continue;
        const LatVal* l = prop.find(id);
        if (!l || l->kind != Lat::Const) continue;
        if (n.n_in > 0 && is_block_head(g.node(n.in[0]).op) && !reach.contains(n.in[0]))
            continue;
        NodeId c = make_const_node(g, block_pin(g, id), l->v);
        g.replace_all_uses(id, c);
        g.kill(id);
        changed = true;
    }

    // 2) constant branches: kill the dead projection
    for (NodeId id = 0; id < g.size(); ++id) {
        Node& n = g.node(id);
        if (n.op != Op::If) continue;
        const LatVal* c = prop.find(n.in[1]);
        if (!c || c->kind != Lat::Const) continue;
        for (NodeId u : g.uses_of(id)) {
            Op uo = g.node(u).op;
            bool taken = c->v.iv != 0;
            if ((uo == Op::IfTrue && !taken) || (uo == Op::IfFalse && taken)) {
                if (!reach.contains(u)) {
                    g.kill(u);
                    dead_ctrl.insert(u, true);
                    changed = true;
                }
            }
        }
    }

    // 3) remove unreachable/dead predecessors from regions; realign phis.
    //    The keep-mask is snapshotted BEFORE compaction (compacting first
    //    leaves stale live pred ids at positions >= keep and the phi
    //    realignment would re-copy them — phis ended up with MORE inputs
    //    than the region has predecessors; regression class from pass 8).
    for (NodeId id = 0; id < g.size(); ++id) {
        Node& n = g.node(id);
        if (n.op != Op::Region) continue;
        bool keep_mask[kMaxInputs];
        u8 keep = 0;
        for (u8 i = 0; i < n.n_in; ++i) {
            keep_mask[i] = reach.contains(n.in[i]) && !g.is_dead(n.in[i]);
            if (keep_mask[i]) ++keep;
        }
        if (keep == n.n_in) continue;
        g.touch();
        if (keep == 0) {
            const SmallVec<NodeId, 4> users = g.uses_of(id);
            for (NodeId u : users) {
                if (g.node(u).op == Op::Phi) {
                    g.kill(u);
                    changed = true;
                }
            }
            g.kill(id);
            dead_ctrl.insert(id, true);
            changed = true;
            continue;
        }
        const SmallVec<NodeId, 4> users = g.uses_of(id);
        for (NodeId u : users) {
            Node& phi = g.node(u);
            if (phi.op != Op::Phi) continue;
            u8 pk = 1;
            for (u8 i = 0; i < n.n_in; ++i)
                if (keep_mask[i]) phi.in[pk++] = phi.in[i + 1];
            phi.n_in = pk;
        }
        u8 k2 = 0;
        for (u8 i = 0; i < n.n_in; ++i)
            if (keep_mask[i]) n.in[k2++] = n.in[i];
        n.n_in = keep;
        changed = true;
    }

    // 4) single-input phis produced by pred removal
    for (NodeId id = 0; id < g.size(); ++id) {
        Node& n = g.node(id);
        if (n.op != Op::Phi || n.n_in != 2) continue;
        g.replace_all_uses(id, n.in[1]);
        g.kill(id);
        changed = true;
    }

    // 5) cascade: kill the subgraph pinned to removed control. An If is
    //    NOT a block head, so killing it through the cascade would strand
    //    its projections (their in[0] would be a Dead If — the verifier
    //    rejects that); killed Ifs take their projections with them, and
    //    the projections join dead_ctrl so their pinned subgraphs die too.
    if (!dead_ctrl.empty()) {
        bool again = true;
        while (again) {
            again = false;
            for (NodeId id = 0; id < g.size(); ++id) {
                Node n = g.node(id); // copy: kill/uses_of mutate around us
                if (n.op == Op::Dead || n.op == Op::Stop) continue;
                if (n.n_in == 0) continue;
                if (!dead_ctrl.contains(n.in[0])) continue;
                g.kill(id);
                if (is_block_head(n.op)) dead_ctrl.insert(id, true);
                if (n.op == Op::If) {
                    const SmallVec<NodeId, 4> projs = g.uses_of(id);
                    for (NodeId p : projs) {
                        Op po = g.node(p).op;
                        if (po == Op::IfTrue || po == Op::IfFalse) {
                            if (!g.is_dead(p)) {
                                g.kill(p);
                                dead_ctrl.insert(p, true);
                            }
                        }
                    }
                }
                changed = true;
                again = true;
            }
        }
        Node& stop = g.node(g.stop());
        if (stop.op == Op::Stop) {
            u8 keep = 0;
            for (u8 i = 0; i < stop.n_in; ++i)
                if (g.node(stop.in[i]).op != Op::Dead) stop.in[keep++] = stop.in[i];
            if (keep != stop.n_in) {
                stop.n_in = keep;
                g.touch();
                changed = true;
            }
        }
    }
    return changed;
}

} // namespace

bool pe_fold(Graph& g, u32 round_limit,
            const std::vector<PeAssumption>* bindings) {
    bool any = false;
    for (u32 r = 0; r < round_limit; ++r) {
        if (!fold_round(g, bindings)) break;
        any = true;
    }
    return any;
}

// ---- variant table -----------------------------------------------------------------

namespace {
struct VariantEntry {
    FnId fid = kNoFn;
    FnId origin = kNoFn;
    std::vector<PeAssumption> assumptions;
    u64 key = 0;
};

// Process-lifetime table. julesc is a one-shot compiler (one module per
// process); a resident-JIT host would need to key this by module and clear
// it between compiles (documented limitation, same class as the inliner's
// module assumption).
std::vector<VariantEntry>& variant_table() {
    static std::vector<VariantEntry> t;
    return t;
}

u64 mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

u64 assumptions_key(FnId origin, const std::vector<PeAssumption>& as) {
    std::vector<PeAssumption> s = as;
    std::sort(s.begin(), s.end(),
              [](const PeAssumption& a, const PeAssumption& b) { return a.param < b.param; });
    u64 h = mix(0x51ed270b, static_cast<u64>(origin));
    for (const PeAssumption& a : s) {
        h = mix(h, a.param);
        h = mix(h, static_cast<u64>(a.kind));
        if (a.kind == PeKind::Range) {
            h = mix(h, static_cast<u64>(a.range_lo));
            h = mix(h, static_cast<u64>(a.range_hi));
            continue;
        }
        h = mix(h, a.value.is_fp ? 1u : 0u);
        h = mix(h, a.value.is_fp ? std::bit_cast<u64>(a.value.fv)
                                 : static_cast<u64>(a.value.iv));
    }
    return h;
}
} // namespace

const std::vector<PeAssumption>* pe_variant_assumptions(FnId fid) {
    for (const VariantEntry& e : variant_table())
        if (e.fid == fid) return &e.assumptions;
    return nullptr;
}

namespace {

// Deep-clone `src` into a fresh graph. Const-bound parameters become
// entry Const nodes (dropped from the signature); Range-bound parameters
// are KEPT as runtime parameters (re-indexed contiguously with the other
// kept ones — `new_index` maps ORIGINAL param index -> clone index, and
// is also returned so the fold can seed ranges by the CLONE's indices).
FunctionGraph clone_bound(const FunctionGraph& src,
                          const std::vector<PeAssumption>& bindings,
                          std::vector<TypeId>& kept_params,
                          std::vector<u32>& new_index) {
    FunctionGraph out;
    out.name = kNoSymbol;
    out.always_inline = src.always_inline;
    out.is_comptime = src.is_comptime;
    // no_inline is deliberately NOT inherited: it is an outline policy for
    // the origin body (e.g. PGO instrumentation marks), and the variant is
    // a new, smaller function.
    out.ret = src.ret;

    Graph& g = out.g;
    const Graph& sg = src.g;
    FlatMap<NodeId, NodeId> map;

    // param bookkeeping: Const-bound -> dropped (entry Const); Range-
    // bound and unbound -> kept, compact index
    for (u32 p = 0; p < src.param_types.size(); ++p) {
        const PeAssumption* a = nullptr;
        for (const PeAssumption& x : bindings)
            if (x.param == p && x.kind == PeKind::Const) { a = &x; break; }
        if (a != nullptr) continue;
        new_index[p] = static_cast<u32>(kept_params.size());
        kept_params.push_back(src.param_types[p]);
    }
    out.param_types = kept_params;

    // pass 1: shells (id order; cycle-safe). Start maps to the fresh
    // graph's own start for BOTH its roles (initial control, initial
    // memory version) — the variant is a standalone function.
    for (NodeId id = 0; id < sg.size(); ++id) {
        const Node& n = sg.node(id);
        if (n.op == Op::Dead || n.op == Op::Start) continue;
        if (n.op == Op::Param) {
            const PeAssumption* a = nullptr;
            for (const PeAssumption& x : bindings)
                if (x.param == n.aux) { a = &x; break; }
            if (a != nullptr && a->kind == PeKind::Const) {
                ConstVal v = a->value;
                if (n.aux < src.param_types.size()) v.ty = src.param_types[n.aux];
                NodeId c = g.make(Op::Const, v.ty, {g.start()});
                g.node(c).ival = v.iv;
                g.node(c).fval = v.fv;
                map.insert(id, c);
            } else {
                // kept (unbound, or Range-bound: the value stays runtime,
                // the fold seeds the hull onto this node)
                u32 idx = (n.aux < new_index.size()) ? new_index[n.aux]
                                                    : 0xFFFFFFFFu;
                if (idx == 0xFFFFFFFFu) idx = 0; // defensive: stray param
                NodeId p = g.make(Op::Param, n.ty, {g.start()}, 0, idx);
                map.insert(id, p);
            }
            continue;
        }
        NodeId shell = g.make_arr(n.op, n.ty, nullptr, 0, n.sub, n.aux);
        g.node(shell).ival = n.ival;
        g.node(shell).fval = n.fval;
        g.node(shell).flags = n.flags & static_cast<u8>(~kFlagTailCall);
        map.insert(id, shell);
    }

    // pass 2: fill inputs (append-only; slot 0 via set_input)
    for (NodeId id = 0; id < sg.size(); ++id) {
        const Node& n = sg.node(id);
        if (n.op == Op::Dead || n.op == Op::Start || n.op == Op::Param) continue;
        const NodeId* shell = map.find(id);
        if (!shell) continue;
        for (u8 i = 0; i < n.n_in; ++i) {
            NodeId in = n.in[i];
            NodeId mapped;
            if (in == kNoNode) mapped = kNoNode;
            else if (in == sg.start()) mapped = g.start();
            else if (const NodeId* m = map.find(in)) mapped = *m;
            else mapped = kNoNode; // defensive: dead-tombstone input
            if (i == 0) g.set_input(*shell, 0, mapped);
            else g.append_input(*shell, mapped);
        }
        if (n.op == Op::Stop) g.set_stop(*shell);
    }
    return out;
}

} // namespace

FnId pe_make_variant(Module& mod, SymbolTable& syms, FnId origin,
                     const std::vector<PeAssumption>& bindings, const PeBudgets& b,
                     bool* changed) {
    if (changed) *changed = false;
    FunctionGraph* src = mod.find_fn(origin);
    if (!src || src->param_types.empty() || bindings.empty()) return kNoFn;
    // Single generation: variants of variants are rejected (termination).
    if (pe_variant_assumptions(origin) != nullptr) return kNoFn;
    for (const PeAssumption& a : bindings)
        if (a.param >= src->param_types.size()) return kNoFn;

    // dedup (exact assumption sets share one variant)
    u64 key = assumptions_key(origin, bindings);
    for (const VariantEntry& e : variant_table())
        if (e.key == key) return e.fid;

    if (b.max_variants_per_fn == 0) return kNoFn;
    u32 per_fn = 0;
    u64 total_nodes = 0;
    for (const VariantEntry& e : variant_table()) {
        if (e.origin == origin) ++per_fn;
        const FunctionGraph* v = mod.find_fn(e.fid);
        if (v) total_nodes += v->g.live_count();
    }
    if (per_fn >= b.max_variants_per_fn) return kNoFn;
    if (total_nodes >= b.total_nodes) return kNoFn;

    // benefit gate: something must become compile-time
    PeBta bta = pe_binding_time_analysis(src->g, bindings);
    if (bta.static_values == 0 && bta.static_ifs == 0) return kNoFn;

    std::vector<TypeId> kept;
    std::vector<u32> new_index(src->param_types.size(), 0xFFFFFFFFu);
    FunctionGraph v = clone_bound(*src, bindings, kept, new_index);

    // fold seeding uses the CLONE's compact param indices (Range params
    // are kept there; Const params are already entry Consts)
    std::vector<PeAssumption> seed = bindings;
    for (PeAssumption& a : seed)
        if (a.param < new_index.size()) a.param = static_cast<u8>(new_index[a.param]);

    bool folded = pe_fold(v.g, 8, &seed);
    u64 live = v.g.live_count();
    u64 origin_live = src->g.live_count();
    // growth budget: variant nodes <= origin * growth factor
    if (live * b.growth_den > origin_live * b.growth_num) return kNoFn;
    // no-benefit rejection: nothing folded and nothing shrank
    if (!folded && live >= origin_live) return kNoFn;

    // accept
    static u32 seq = 0;
    ++seq;
    std::string vname(syms.name(src->name).data());
    vname += "#pe" + std::to_string(seq);
    v.name = syms.intern(vname);
    v.fid = static_cast<FnId>(mod.fns.size());
    v.node_estimate = v.g.live_count();
    mod.fns.push_back(std::move(v));

    VariantEntry e;
    e.fid = v.fid;
    e.origin = origin;
    e.assumptions = bindings;
    e.key = key;
    variant_table().push_back(e);
    if (changed) *changed = true;
    return e.fid;
}

// ---- sketches ---------------------------------------------------------------------

u32 pe_sketch_slots(const Module& mod, u32 orig_fn_count) {
    u32 n = 0;
    u32 upto = mod.fns.size() < orig_fn_count ? static_cast<u32>(mod.fns.size())
                                              : orig_fn_count;
    for (u32 i = 0; i < upto; ++i)
        for (TypeId t : mod.fns[i].param_types)
            if (ty_is_int(t)) ++n;
    return n;
}

// ---- call rewiring ------------------------------------------------------------------

void pe_rewire_call_users(Graph& g, NodeId old_call, NodeId new_val, NodeId new_mem) {
    // Calls are BOTH a value and a memory version; users split by slot
    // kind exactly like the inliner's exit rewire (a blind slot-1 rewrite
    // maps a value read onto the memory node — see inline_util.cpp).
    const SmallVec<NodeId, 4> users = g.uses_of(old_call);
    for (NodeId u : users) {
        Node& un = g.node(u);
        bool is_mem_phi = (un.op == Op::Phi && un.ty == ty_mem());
        bool mem_slot1_user = (un.op == Op::Call || un.op == Op::Store ||
                               un.op == Op::Load || un.op == Op::Alloc ||
                               un.op == Op::Return);
        for (u8 i = 0; i < un.n_in; ++i) {
            if (un.in[i] != old_call) continue;
            bool as_memory = is_mem_phi || (i == 1 && mem_slot1_user);
            if (!as_memory && new_val == kNoNode) continue; // void call
            un.in[i] = as_memory ? new_mem : new_val;
        }
    }
    g.mark_uses_dirty();
    g.touch();
}

} // namespace jules
