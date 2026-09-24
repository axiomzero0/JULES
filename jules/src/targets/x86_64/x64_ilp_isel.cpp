// x86-64 ILP tier, solver side (see x64_ilp_isel.h for the model).
//
// The solve is an exact enumeration over the region's decision set, seeded
// with the DP cover (a feasible assignment): death groups commit first
// (strict improvements by construction), then the absorb-flip subsets are
// enumerated under the objective  latency + lambda * peak-live.  Region
// sizes are bounded by kBudget (50 SoN nodes) and the flip candidate count
// by kMaxFlips (10), so the worst case is ~2^10 cheap interval sweeps per
// region — sub-millisecond, and deterministic (fixed iteration orders,
// ties broken toward fewer flips, then lower node ids).
#include "x64_ilp_isel.h"

#include "x64_dp_isel.h"
#include "core/son/passes/pass_utils.h"
#include "core/son/son.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace jules {
namespace {

// Same shape predicate as the DP's index_term (duplicated: it lives in
// x64_dp_isel.cpp's anonymous namespace).
bool index_term(const Graph& g, NodeId t, NodeId& i, u8& scale) {
    const Node& td = g.node(t);
    if (td.op != Op::Bin) return false;
    BinOp op = static_cast<BinOp>(td.sub);
    auto pow2 = [](i64 v, u8& k) {
        if (v == 1) { k = 0; return true; }
        if (v == 2) { k = 1; return true; }
        if (v == 4) { k = 2; return true; }
        if (v == 8) { k = 3; return true; }
        return false;
    };
    if (op == BinOp::Mul) {
        const Node& a = g.node(td.in[1]);
        const Node& b = g.node(td.in[2]);
        u8 k = 0;
        if (a.op == Op::Const && pow2(a.ival, k)) {
            i = td.in[2]; scale = static_cast<u8>(1 << k); return true;
        }
        if (b.op == Op::Const && pow2(b.ival, k)) {
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

bool env_flag(const char* name, bool def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    return *v != '0';
}

i32 env_lambda() {
    const char* v = std::getenv("JULES_ILP_LAMBDA");
    if (!v || !*v) return 1; // calibrated default (conservative)
    i32 l = std::atoi(v);
    return l < 0 ? 0 : l;
}

} // namespace

IlpIsel::IlpIsel(LFunction& lf, FunctionGraph& fg) : lf_(lf), g_(fg.g) {
    block_of_.assign(g_.size(), -1);
    for (const LBlock& b : lf.blocks)
        for (NodeId n : b.nodes)
            if (n < block_of_.size()) block_of_[n] = b.index;
    compute_loops();
}

bool IlpIsel::enabled() const { return env_flag("JULES_ILP_ISEL", true); }

// Natural-loop nesting depth per block (iterative dominators over the LBlock
// CFG, which is laid out in RPO; backedge p->h with h dominating p, loop
// body = backward reachability from the latch stopping at the header).
void IlpIsel::compute_loops() {
    const size_t n = lf_.blocks.size();
    loop_depth_.assign(n, 0);
    if (n == 0) return;

    // reachability from entry
    std::vector<u8> reach(n, 0);
    {
        std::vector<int> work{0};
        reach[0] = 1;
        while (!work.empty()) {
            int b = work.back();
            work.pop_back();
            for (int s : lf_.blocks[static_cast<size_t>(b)].succs)
                if (!reach[static_cast<size_t>(s)]) {
                    reach[static_cast<size_t>(s)] = 1;
                    work.push_back(s);
                }
        }
    }

    // dominators: dom[b] = {b} u (intersection over reachable preds)
    std::vector<std::vector<u8>> dom(n, std::vector<u8>(n, 1));
    for (size_t b = 0; b < n; ++b) {
        if (!reach[b]) continue;
        if (b == 0) {
            std::fill(dom[b].begin(), dom[b].end(), 0);
            dom[b][0] = 1;
            continue;
        }
        std::fill(dom[b].begin(), dom[b].end(), 1);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t b = 1; b < n; ++b) {
            if (!reach[b]) continue;
            std::vector<u8> nd(n, 1);
            bool first = true;
            for (int p : lf_.blocks[b].preds) {
                size_t pu = static_cast<size_t>(p);
                if (!reach[pu]) continue;
                if (first) {
                    nd = dom[pu];
                    first = false;
                } else {
                    for (size_t k = 0; k < n; ++k) nd[k] &= dom[pu][k];
                }
            }
            if (first) continue; // no reachable preds: unreachable in practice
            nd[b] = 1;
            if (nd != dom[b]) {
                dom[b] = nd;
                changed = true;
            }
        }
    }

    // backedges and loop bodies
    for (size_t p = 0; p < n; ++p) {
        if (!reach[p]) continue;
        for (int h : lf_.blocks[p].succs) {
            size_t hu = static_cast<size_t>(h);
            if (hu >= n || !dom[p][hu]) continue; // p -> h, h dominates p
            std::vector<u8> in_loop(n, 0);
            in_loop[hu] = 1;
            std::vector<int> work{static_cast<int>(p)};
            in_loop[p] = 1;
            while (!work.empty()) {
                int b = work.back();
                work.pop_back();
                for (int q : lf_.blocks[static_cast<size_t>(b)].preds) {
                    size_t qu = static_cast<size_t>(q);
                    if (qu < n && !in_loop[qu]) {
                        in_loop[qu] = 1;
                        work.push_back(q);
                    }
                }
            }
            for (size_t b = 0; b < n; ++b)
                if (in_loop[b]) ++loop_depth_[b];
        }
    }
}

// ---------------------------------------------------------------------------
// refine_block
// ---------------------------------------------------------------------------

void IlpIsel::refine_block(const LBlock& b, const FlatMap<NodeId, bool>& sup,
                           DpIsel& dp) {
    if (!enabled()) return;
    if (b.index < 0 || static_cast<size_t>(b.index) >= loop_depth_.size())
        return;
    if (std::getenv("JULES_ILP_TRACE"))
        std::fprintf(stderr, "[ilp] blk=%d depth=%d nodes=%zu\n", b.index,
                     loop_depth_[static_cast<size_t>(b.index)], b.nodes.size());
    if (b.nodes.size() >= IlpIsel::kBudget) return;
    if (loop_depth_[static_cast<size_t>(b.index)] < 1) return;

    const bool trace = env_flag("JULES_ILP_TRACE", false);
    const i32 lambda = env_lambda();
    const i32 n = static_cast<i32>(b.nodes.size());

    // node -> position in the block (scheduled order)
    FlatMap<NodeId, i32> pos;
    for (i32 p = 0; p < n; ++p) pos.insert(b.nodes[static_cast<size_t>(p)], p);

    // ======================= 1) death groups ==============================
    // THE joint decision the per-root DP cannot make: a multi-use index
    // term (Mul(i,2^k)/Shl(i,k)) whose every user is an in-block two-term
    // address Add. Per-root, the DP reads the mul's materialized slot
    // (Arith ties Lea2 on cost and wins the rank tie) — but if EVERY user
    // instead takes the Lea2 form (raw index + SIB scale), the mul's
    // materialization is dead: strictly better jointly, invisible
    // per-root. Commit = rewrite each user's cell to the Lea2 form +
    // suppress the mul. (Users whose cell already scale-extracted —
    // single-use shape-absorbs — keep their cell as is.)
    u32 groups_here = 0;
    for (NodeId x : b.nodes) {
        if (sup.find(x)) continue;
        const DpIsel::Act ax = dp.act(x);
        if (ax != DpIsel::Act::Root && ax != DpIsel::Act::None) continue;
        NodeId ii = kNoNode;
        u8 sc = 0;
        if (!index_term(g_, x, ii, sc)) continue;
        const SmallVec<NodeId, 4>& users = g_.uses_of(x);
        if (users.size() < 2) continue; // single-use is the DP's own rule

        // classify every user; build the rewrites as we go
        struct UserRewrite {
            NodeId u;                 // the address Add
            NodeId base;              // base operand (ptrcast-unwrapped)
            bool needs_cell;          // rewrite the cell (Arith form today)
        };
        SmallVec<UserRewrite, 4> rewrites;
        bool all = true;
        for (NodeId u : users) {
            if (u >= block_of_.size() || block_of_[u] != b.index) {
                all = false;
                break;
            }
            const DpIsel::Act au = dp.act(u);
            if (au != DpIsel::Act::Suppressed && au != DpIsel::Act::Root) {
                all = false; // hand-emitted user reads x's slot: x must live
                break;
            }
            const Node& un = g_.node(u);
            const DpIsel::ValCell* cu = dp.cell(u);
            if (!cu) {
                all = false;
                break;
            }
            if (cu->form == DpIsel::ValCell::Form::Lea2 ||
                cu->form == DpIsel::ValCell::Form::LeaRR) {
                // already a scale claim; it must NOT read x (it extracted)
                auto ref_is_x = [&](const DpIsel::OpRef& r) {
                    return r.node == x;
                };
                if (ref_is_x(cu->base) || ref_is_x(cu->idx) ||
                    cu->absorb1 == x || cu->absorb2 == x) {
                    all = false;
                    break;
                }
                rewrites.push_back({u, kNoNode, false});
                continue;
            }
            // Arith form reading x as a plain Leaf on either side, with
            // the other side a plain Leaf too (chains/imms keep the group
            // out — v1 scope; the joint win is not there anyway)
            if (un.op != Op::Bin ||
                static_cast<BinOp>(un.sub) != BinOp::Add) {
                all = false;
                break;
            }
            // the Lea2 form is 64-bit only (the DP's own lea_width gate)
            if (ty_in_xmm(un.ty) || ty_is_vector(un.ty) ||
                ty_bits(un.ty) != 64) {
                all = false;
                break;
            }
            if (cu->form != DpIsel::ValCell::Form::Arith) {
                all = false;
                break;
            }
            bool a_is_x = cu->a.k == DpIsel::OpRef::K::Leaf && cu->a.node == x;
            bool b_is_x = cu->b.k == DpIsel::OpRef::K::Leaf && cu->b.node == x;
            if (!a_is_x && !b_is_x) {
                all = false; // does not read x at all: not this group's shape
                break;
            }
            const DpIsel::OpRef& other = a_is_x ? cu->b : cu->a;
            if (other.k != DpIsel::OpRef::K::Leaf) {
                all = false; // chain/imm shapes: v1 keeps the materialized form
                break;
            }
            NodeId base = other.node;
            // machine-identity ptrcasts of always-materialized operands
            // (mirror the DP's skip_simple_ptrcast)
            int hops = 0;
            while (g_.node(base).op == Op::Cast &&
                   static_cast<CastOp>(g_.node(base).sub) == CastOp::Ptr &&
                   hops++ < 4) {
                const Node& bu = g_.node(g_.node(base).in[1]);
                if (bu.op != Op::Param && bu.op != Op::Const) break;
                base = g_.node(base).in[1];
            }
            rewrites.push_back({u, base, true});
        }
        if (!all || rewrites.empty()) continue;

        // joint objective (latency units, the DP's own numbers): commit
        // only when strictly better than the materialized-reader baseline
        i32 old_cost = 0, new_cost = 0;
        for (const UserRewrite& rw : rewrites) {
            const DpIsel::ValCell* cu = dp.cell(rw.u);
            old_cost += cu ? cu->cost : 0;
            if (rw.needs_cell)
                new_cost += (g_.node(rw.base).op == Op::Const
                                 ? DpIsel::kCImm
                                 : DpIsel::kCLd) +
                            (g_.node(ii).op == Op::Const ? DpIsel::kCImm
                                                          : DpIsel::kCLd) +
                            DpIsel::kCOp;
        }
        // x's materialization (the dead emission when the group commits)
        const DpIsel::ValCell* cx = dp.cell(x);
        i32 x_mat = (ax == DpIsel::Act::Root && cx)
                        ? cx->cost + DpIsel::kCSt
                        : dp.fallback_mat(x);
        if (new_cost >= old_cost + x_mat) continue; // not a strict joint win

        // commit: rewrite the cells, then kill x
        for (const UserRewrite& rw : rewrites) {
            if (!rw.needs_cell) continue;
            DpIsel::ValCell nc;
            nc.form = DpIsel::ValCell::Form::Lea2;
            nc.base = DpIsel::OpRef{DpIsel::OpRef::K::Leaf, rw.base, 0};
            nc.idx = DpIsel::OpRef{DpIsel::OpRef::K::Leaf, ii, 0};
            nc.scale = sc;
            nc.disp = 0;
            nc.size = 8;
            nc.absorb1 = x; // provenance: the death group
            nc.cost = (g_.node(rw.base).op == Op::Const ? DpIsel::kCImm
                                                       : DpIsel::kCLd) +
                      (g_.node(ii).op == Op::Const ? DpIsel::kCImm
                                                   : DpIsel::kCLd) +
                      DpIsel::kCOp;
            dp.cells_.insert(rw.u, nc);
        }
        if (ax == DpIsel::Act::Root) --dp.roots_;
        dp.act_.insert(x, static_cast<u8>(DpIsel::Act::Suppressed));
        ++dp.folds_;
        ++groups_;
        ++groups_here;
        if (trace)
            std::fprintf(stderr,
                         "[ilp] blk=%d death group n%u (%zu users, index n%u "
                         "scale %u; dcost %d)\n",
                         b.index, x, users.size(), ii, sc,
                         new_cost - old_cost - x_mat);
    }

    // ======================= 2) absorb-flips ===============================
    // Build the owner map (consumed node -> claiming root) by walking every
    // root's cell chain tree, then offer each chain-consumed node the
    // "materialize instead" alternative under the pressure objective.
    struct Flip {
        NodeId y;      // the absorbed node
        NodeId root;   // the root whose cell consumes it
        i32 dcost;     // latency delta of flipping (always > 0)
        std::vector<NodeId> subtree; // y + everything that must emit with it
    };
    std::vector<Flip> cands;

    FlatMap<NodeId, NodeId> owner; // suppressed-by-chain -> root
    for (NodeId r : b.nodes) {
        if (dp.act(r) != DpIsel::Act::Root) continue;
        std::vector<NodeId> stack;
        const DpIsel::ValCell* cr = dp.cell(r);
        if (!cr) continue;
        if (cr->base.k == DpIsel::OpRef::K::Chain) stack.push_back(cr->base.node);
        if (cr->idx.k == DpIsel::OpRef::K::Chain) stack.push_back(cr->idx.node);
        if (cr->a.k == DpIsel::OpRef::K::Chain) stack.push_back(cr->a.node);
        if (cr->b.k == DpIsel::OpRef::K::Chain) stack.push_back(cr->b.node);
        while (!stack.empty()) {
            NodeId v = stack.back();
            stack.pop_back();
            if (owner.find(v)) continue; // single-use: first walk is the owner
            owner.insert(v, r);
            const DpIsel::ValCell* cv = dp.cell(v);
            if (!cv) continue;
            if (cv->base.k == DpIsel::OpRef::K::Chain) stack.push_back(cv->base.node);
            if (cv->idx.k == DpIsel::OpRef::K::Chain) stack.push_back(cv->idx.node);
            if (cv->a.k == DpIsel::OpRef::K::Chain) stack.push_back(cv->a.node);
            if (cv->b.k == DpIsel::OpRef::K::Chain) stack.push_back(cv->b.node);
        }
    }

    if (lambda > 0 && !owner.empty()) {
        for (const auto& e : owner.entries()) {
            NodeId y = e.first;
            NodeId r = e.second;
            const DpIsel::ValCell* cy = dp.cell(y);
            if (!cy || cy->cost >= std::numeric_limits<i32>::max() / 4) continue;
            // subtree: y + chain children (recursively) + absorbs (the
            // shape nodes a hand emission reads from slots)
            std::vector<NodeId> sub;
            std::vector<NodeId> stack{y};
            FlatMap<NodeId, u8> seen;
            while (!stack.empty()) {
                NodeId v = stack.back();
                stack.pop_back();
                if (seen.find(v)) continue;
                seen.insert(v, 1);
                if (v != y) sub.push_back(v);
                const DpIsel::ValCell* cv = dp.cell(v);
                if (!cv) continue;
                if (cv->base.k == DpIsel::OpRef::K::Chain) stack.push_back(cv->base.node);
                if (cv->idx.k == DpIsel::OpRef::K::Chain) stack.push_back(cv->idx.node);
                if (cv->a.k == DpIsel::OpRef::K::Chain) stack.push_back(cv->a.node);
                if (cv->b.k == DpIsel::OpRef::K::Chain) stack.push_back(cv->b.node);
                if (cv->absorb1 != kNoNode) stack.push_back(cv->absorb1);
                if (cv->absorb2 != kNoNode) stack.push_back(cv->absorb2);
            }
            // latency delta: the root loads y's slot (+C_LD) instead of the
            // inline compute (-cell), and the subtree materializes
            i32 dcost = DpIsel::kCLd - cy->cost;
            for (NodeId v : sub) dcost += dp.fallback_mat(v);
            Flip f;
            f.y = y;
            f.root = r;
            f.dcost = dcost;
            f.subtree = std::move(sub);
            cands.push_back(std::move(f));
        }
    }

    // ---- objective machinery ---------------------------------------------
    // assignment = a subset of `cands` (flip set). Cost deltas are additive
    // per flip; only peak-live couples them, so enumerate subsets and sweep.
    const size_t kMaxFlips = 10;
    if (cands.size() > kMaxFlips) {
        std::stable_sort(cands.begin(), cands.end(),
                         [](const Flip& a, const Flip& b) {
                             if (a.dcost != b.dcost) return a.dcost < b.dcost;
                             return a.y < b.y;
                         });
        cands.resize(kMaxFlips);
        std::stable_sort(cands.begin(), cands.end(),
                         [](const Flip& a, const Flip& b) { return a.y < b.y; });
    }

    if (cands.empty()) {
        if (groups_here > 0) ++regions_;
        return;
    }

    // flipped membership lookup
    auto in_flip = [&](const std::vector<u8>& chosen, NodeId v) -> bool {
        for (size_t i = 0; i < cands.size(); ++i)
            if (chosen[i] && cands[i].y == v) return true;
        for (size_t i = 0; i < cands.size(); ++i)
            if (chosen[i])
                for (NodeId w : cands[i].subtree)
                    if (w == v) return true;
        return false;
    };

    // peak live over the block, for a given flip set. Values are live from
    // their def position (params/consts: block entry) to their last use
    // (suppressed consumers read at their owning root's position).
    auto peak_live = [&](const std::vector<u8>& chosen) -> i32 {
        std::vector<i32> events(static_cast<size_t>(n) + 1, 0);
        for (NodeId v : b.nodes) {
            const Node& vn = g_.node(v);
            if (vn.op == Op::Const) continue; // materialized at use
            bool flipped = in_flip(chosen, v);
            if (dp.act(v) == DpIsel::Act::Suppressed && !flipped) continue;
            const i32* pv = pos.find(v);
            i32 def = pv ? *pv : 0;
            // last use: max over users
            i32 use = -1;
            for (NodeId u : g_.uses_of(v)) {
                const i32* pu = pos.find(u);
                if (!pu) {
                    use = n; // user outside the block's node list (phi copy,
                             // terminator, other block): live to the end
                    continue;
                }
                i32 at = *pu;
                if (dp.act(u) == DpIsel::Act::Suppressed && !in_flip(chosen, u)) {
                    const NodeId* ow = owner.find(u);
                    if (ow) {
                        const i32* po = pos.find(*ow);
                        if (po) at = *po; // read when the owning root emits
                    }
                }
                use = std::max(use, at);
            }
            if (use < 0) use = def; // no users in evidence: dead value
            if (use < def) use = def;
            events[static_cast<size_t>(def)] += 1;
            events[static_cast<size_t>(use) + 1 <= static_cast<size_t>(n)
                     ? static_cast<size_t>(use) + 1
                     : static_cast<size_t>(n)] -= 1;
        }
        i32 live = 0, peak = 0;
        for (i32 p = 0; p < n; ++p) {
            live += events[static_cast<size_t>(p)];
            peak = std::max(peak, live);
        }
        return peak;
    };

    // enumerate subsets: seed = empty set; objective = dcost + lambda * peak
    const size_t m = cands.size();
    std::vector<u8> best(m, 0);
    i32 best_peak = peak_live(best);
    i64 best_obj = static_cast<i64>(lambda) * best_peak;
    bool improved = false;
    for (u32 mask = 1; mask < (1u << m); ++mask) {
        std::vector<u8> chosen(m, 0);
        i32 dcost = 0;
        for (size_t i = 0; i < m; ++i)
            if (mask & (1u << i)) {
                chosen[i] = 1;
                dcost += cands[i].dcost;
            }
        if (static_cast<i64>(dcost) >= best_obj) continue; // prune
        const i32 peak = peak_live(chosen);
        const i64 obj = static_cast<i64>(dcost) +
                        static_cast<i64>(lambda) * peak;
        if (obj < best_obj) {
            best_obj = obj;
            best_peak = peak;
            best = chosen;
            improved = true;
        }
    }

    if (improved) {
        for (size_t i = 0; i < m; ++i) {
            if (!best[i]) continue;
            const Flip& f = cands[i];
            // un-suppress the subtree (they hand-emit now)
            dp.act_.erase(f.y);
            for (NodeId v : f.subtree) dp.act_.erase(v);
            // patch the root's cell: the Chain ref to y becomes a Leaf load
            if (DpIsel::ValCell* cr = dp.cells_.find(f.root)) {
                DpIsel::OpRef* refs[4] = {&cr->base, &cr->idx, &cr->a, &cr->b};
                for (DpIsel::OpRef* r : refs)
                    if (r->k == DpIsel::OpRef::K::Chain && r->node == f.y)
                        r->k = DpIsel::OpRef::K::Leaf;
                cr->cost += DpIsel::kCLd -
                            (dp.cell(f.y) ? dp.cell(f.y)->cost : 0);
            }
            ++flips_;
            if (trace)
                std::fprintf(stderr,
                             "[ilp] blk=%d flip n%u -> materialize (root n%u, "
                             "dcost %d)\n",
                             b.index, f.y, f.root, f.dcost);
        }
    }
    ++regions_;
}

} // namespace jules
