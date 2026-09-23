// Phase 2 — the hybrid allocator driver: exact flow selection, bank-aware
// greedy assignment, iterated conservative coalescing.
//
// Staging per register class (two exact flow stages + a greedy repair):
//   Stage A: call-crossing ranges (eligible only for callee-saved banks)
//     are selected with capacity = |callee-saved registers| — the exact
//     optimum for the constrained sub-population.
//   Stage B: non-crossing ranges are selected with the time-varying
//     residual capacity K - x_p, where x_p is the stage-A occupancy — the
//     exact optimum given stage A. (The joint two-bank problem is a
//     two-commodity flow — NP-hard in general; the staged decomposition is
//     exact per stage, and the repair pass below recovers the common
//     ordering losses.)
//   Repair: deselected crossing ranges are added back greedily (weight
//     order) wherever both banks still have room.
//
// Assignment is a by-left-endpoint interval greedy over the bank lists
// (crossing ranges first — they are the picky ones), with the same
// handover-touch rule the occupancy model uses, so the flow's colorability
// guarantee carries over: at any point the selected set's overlap is
// within capacity, hence an eligible register is always free. A defensive
// demote valve (range stays in memory) covers unexpected shapes rather
// than ever producing an unsound assignment.
//
// Coalescing then unifies move-related ranges onto one register at
// sub-interval precision: a move (a, b) fires when every live
// sub-interval of the mover is disjoint from (or exactly hands over to)
// the destination's, and no third range already lives on that register
// within the mover's hull. This is the iterated conservative coalescing
// repair of George-Appel IRC, applied on top of a pre-solved coloring —
// merges can never make the assignment uncolorable, which is the property
// textbook IRC buys with its simplify/freeze stack (unnecessary here: the
// flow already guaranteed colorability of the selected set).
#include "core/codegen/ralloc.h"

#include <algorithm>
#include <vector>

namespace jules::ralloc {

u32 RaClass::total_regs() const {
    u32 n = 0;
    for (const RaBank& b : banks) n += static_cast<u32>(b.regs.size());
    return n;
}

u32 RaClass::callee_saved_regs() const {
    u32 n = 0;
    for (const RaBank& b : banks)
        if (b.callee_saved) n += static_cast<u32>(b.regs.size());
    return n;
}

namespace {

struct Ctx {
    const RaProblem& p;
    std::vector<RaAssign> out; // indexed like p.vregs
    FlatMap<i32, u32> index_of; // vreg id -> problem index

    explicit Ctx(const RaProblem& problem) : p(problem) {
        out.resize(problem.vregs.size());
        for (size_t i = 0; i < problem.vregs.size(); ++i) {
            out[i].id = problem.vregs[i].id;
            index_of.insert(problem.vregs[i].id, static_cast<u32>(i));
        }
    }

    RaAssign* find(i32 id) {
        const u32* i = index_of.find(id);
        return i ? &out[*i] : nullptr;
    }
    const RaVReg* vreg_of(i32 id) const {
        const u32* i = index_of.find(id);
        return i ? &p.vregs[*i] : nullptr;
    }
};

// Occupancy span of a vreg (the segment model — see RaSpan docs).
RaSpan span_of(const RaVReg& v) {
    RaSpan s;
    s.vreg = v.id;
    s.weight = v.weight;
    s.start = v.starts_at_def ? v.first : (v.first > 0 ? v.first - 1 : 0);
    s.end = v.last > s.start ? v.last : s.start + 1; // defensive: non-empty
    return s;
}

// Occupancy profile (per segment) of a set of selected vregs over [lo, hi].
std::vector<u32> occupancy(const std::vector<const RaVReg*>& sel, u32 lo, u32 hi) {
    std::vector<u32> occ(static_cast<size_t>(hi - lo) + 1, 0);
    for (const RaVReg* v : sel) {
        RaSpan s = span_of(*v);
        for (u32 p = s.start; p < s.end; ++p)
            if (p >= lo && p <= hi) occ[p - lo]++;
    }
    return occ;
}

bool ranges_overlap(u32 a_lo, u32 a_hi, u32 b_lo, u32 b_hi) {
    return a_lo <= b_hi && b_lo <= a_hi;
}

} // namespace

RaSolution ralloc_hybrid(const RaProblem& p) {
    Ctx ctx(p);
    RaSolution sol;

    for (size_t ci = 0; ci < p.classes.size(); ++ci) {
        const RaClass& cls = p.classes[ci];
        const u32 total = cls.total_regs();
        const u32 callee = cls.callee_saved_regs();
        if (total == 0) continue;

        std::vector<const RaVReg*> crossing, free_going;
        for (const RaVReg& v : p.vregs) {
            if (v.cls != ci) continue;
            if (v.crosses_call && callee == 0) continue; // no eligible bank
            (v.crosses_call ? crossing : free_going).push_back(&v);
        }

        // Segment range of the class model.
        u32 lo = ~0u, hi = 0;
        auto note = [&](const RaVReg* v) {
            RaSpan s = span_of(*v);
            lo = std::min(lo, s.start);
            hi = std::max(hi, s.end - 1);
        };
        for (const RaVReg* v : crossing) note(v);
        for (const RaVReg* v : free_going) note(v);
        if (lo > hi) continue; // empty class
        const size_t span_n = static_cast<size_t>(hi - lo) + 1;

        // ---- Stage A: crossing ranges, capacity = callee bank size ----
        std::vector<const RaVReg*> sel_a_v;
        if (!crossing.empty()) {
            std::vector<RaSpan> spans;
            for (const RaVReg* v : crossing) {
                RaSpan s = span_of(*v);
                s.start -= lo;
                s.end -= lo;
                spans.push_back(s);
            }
            std::vector<u32> caps(span_n, callee);
            FlatMap<i32, bool> in_a;
            for (i32 id : ralloc_flow_select(spans, caps)) in_a.insert(id, true);
            for (const RaVReg* v : crossing)
                if (in_a.contains(v->id)) sel_a_v.push_back(v);
        }

        // ---- Stage B: non-crossing, residual capacity K - x_p ---------
        std::vector<const RaVReg*> sel_b_v;
        if (!free_going.empty()) {
            std::vector<u32> x_p = occupancy(sel_a_v, lo, hi);
            std::vector<u32> caps(span_n);
            for (size_t i = 0; i < span_n; ++i)
                caps[i] = total - std::min(x_p[i], total);
            std::vector<RaSpan> spans;
            for (const RaVReg* v : free_going) {
                RaSpan s = span_of(*v);
                s.start -= lo;
                s.end -= lo;
                spans.push_back(s);
            }
            FlatMap<i32, bool> in_b;
            for (i32 id : ralloc_flow_select(spans, caps)) in_b.insert(id, true);
            for (const RaVReg* v : free_going)
                if (in_b.contains(v->id)) sel_b_v.push_back(v);
        }

        // ---- Repair: add deselected crossing ranges back where both
        //      banks still have room (weight order) ----------------------
        {
            std::vector<u32> xa = occupancy(sel_a_v, lo, hi);
            std::vector<u32> xb = occupancy(sel_b_v, lo, hi);
            std::vector<const RaVReg*> rejected;
            for (const RaVReg* v : crossing)
                if (std::find(sel_a_v.begin(), sel_a_v.end(), v) == sel_a_v.end())
                    rejected.push_back(v);
            std::sort(rejected.begin(), rejected.end(),
                      [](const RaVReg* a, const RaVReg* b) {
                          if (a->weight != b->weight) return a->weight > b->weight;
                          return a->id < b->id;
                      });
            for (const RaVReg* v : rejected) {
                RaSpan s = span_of(*v);
                bool fits = true;
                for (u32 q = s.start; q < s.end; ++q) {
                    size_t idx = q - lo;
                    if (xa[idx] >= callee || xa[idx] + xb[idx] >= total) {
                        fits = false;
                        break;
                    }
                }
                if (!fits) continue;
                for (u32 q = s.start; q < s.end; ++q) xa[q - lo]++;
                sel_a_v.push_back(v);
            }
        }

        // ---- Assignment: by left endpoint, crossing first -------------
        // Bank preference for non-crossing ranges: caller-first (no
        // prologue cost), callee-first under the size-biased profile.
        std::vector<size_t> bank_order;
        for (size_t bi = 0; bi < cls.banks.size(); ++bi) bank_order.push_back(bi);
        if (p.prefer_callee_saved)
            std::stable_sort(bank_order.begin(), bank_order.end(),
                             [&](size_t a, size_t b) {
                                 bool ca = cls.banks[a].callee_saved;
                                 bool cb = cls.banks[b].callee_saved;
                                 return ca && !cb;
                             });

        struct RegUse {
            u16 reg;
            u32 last;   // hull end of the current holder
            bool active;
        };
        std::vector<std::vector<RegUse>> bank_slots(cls.banks.size());
        for (size_t bi = 0; bi < cls.banks.size(); ++bi)
            for (u16 r : cls.banks[bi].regs)
                bank_slots[bi].push_back(RegUse{r, 0, false});

        std::vector<const RaVReg*> ordered;
        for (const RaVReg* v : sel_a_v) ordered.push_back(v);
        for (const RaVReg* v : sel_b_v) ordered.push_back(v);
        std::sort(ordered.begin(), ordered.end(),
                  [](const RaVReg* a, const RaVReg* b) {
                      if (a->first != b->first) return a->first < b->first;
                      return a->id < b->id;
                  });

        for (const RaVReg* v : ordered) {
            // expire holders whose range ended before this one begins;
            // a holder ending exactly where a def-starting range begins
            // is a two-operand handover, not an interference
            for (auto& slots : bank_slots)
                for (RegUse& u : slots) {
                    if (!u.active) continue;
                    if (u.last < v->first || (u.last == v->first && v->starts_at_def))
                        u.active = false;
                }
            // eligible banks in preference order
            bool placed = false;
            for (size_t bi = 0; bi < cls.banks.size() && !placed; ++bi) {
                if (v->crosses_call && !cls.banks[bi].callee_saved) continue;
                size_t choice = v->crosses_call ? bi : bank_order[bi];
                for (RegUse& u : bank_slots[choice]) {
                    if (u.active) continue;
                    u.active = true;
                    u.last = v->last;
                    RaAssign* a = ctx.find(v->id);
                    a->promoted = true;
                    a->reg = u.reg;
                    placed = true;
                    break;
                }
            }
            // demote valve: no eligible register -> stay in memory
        }
    }

    // ---------------------------------------------------------------
    // Iterated conservative coalescing of move pairs
    // ---------------------------------------------------------------
    for (int round = 0; round < 6; ++round) {
        bool any = false;
        for (const RaMove& mv : p.moves) {
            const RaVReg *va = ctx.vreg_of(mv.a), *vb = ctx.vreg_of(mv.b);
            if (!va || !vb || va->cls != vb->cls) continue;
            RaAssign *aa = ctx.find(mv.a), *ab = ctx.find(mv.b);
            if (!aa || !ab) continue;
            if (!aa->promoted || !ab->promoted) continue;
            if (aa->reg == ab->reg) continue;

            const RaClass& cls = p.classes[static_cast<size_t>(va->cls)];
            // exact sub-intervals, falling back to the hull when the
            // target did not refine them (a one-element list == the hull
            // keeps the coalescer honest)
            auto gens_of = [](const RaVReg* v) -> std::vector<RaGen> {
                if (!v->gens.empty()) return v->gens;
                return std::vector<RaGen>{RaGen{v->first, v->last}};
            };
            std::vector<RaGen> ga = gens_of(va), gb = gens_of(vb);
            auto attempt = [&](const std::vector<RaGen>& gm,
                               const std::vector<RaGen>& go, bool crosses,
                               u32 first, u32 last, i32 m_id, i32 o_id,
                               u16 reg) -> bool {
                bool reg_callee = false;
                for (const RaBank& b : cls.banks)
                    for (u16 r : b.regs)
                        if (r == reg) reg_callee = b.callee_saved;
                if (crosses && !reg_callee) return false;
                // exact live sub-intervals must be disjoint; a boundary
                // where one ends exactly as the other begins is a
                // two-operand handover in a single instruction, not an
                // interference
                auto touch = [](const RaGen& x, const RaGen& y) {
                    return x.start <= y.end && y.start <= x.end;
                };
                for (const RaGen& x : gm)
                    for (const RaGen& y : go) {
                        if (!touch(x, y)) continue;
                        if (x.end == y.start || y.end == x.start) continue;
                        return false;
                    }
                // no third promoted range on that register may overlap
                // the mover's hull
                for (const RaVReg& u : p.vregs) {
                    if (u.id == m_id || u.id == o_id || u.cls != va->cls) continue;
                    const RaAssign* ua = ctx.find(u.id);
                    if (!ua || !ua->promoted || ua->reg != reg) continue;
                    if (ranges_overlap(u.first, u.last, first, last))
                        return false;
                }
                return true;
            };
            if (attempt(ga, gb, va->crosses_call, va->first, va->last,
                        va->id, vb->id, ab->reg)) {
                aa->reg = ab->reg;
                ++sol.coalesced_moves;
                any = true;
            } else if (attempt(gb, ga, vb->crosses_call, vb->first, vb->last,
                                vb->id, va->id, aa->reg)) {
                ab->reg = aa->reg;
                ++sol.coalesced_moves;
                any = true;
            }
        }
        if (!any) break;
    }

    for (const RaAssign& a : ctx.out) {
        if (a.promoted) ++sol.promoted_count;
        else ++sol.spilled_count;
    }
    sol.vregs = std::move(ctx.out);
    return sol;
}

} // namespace jules::ralloc
