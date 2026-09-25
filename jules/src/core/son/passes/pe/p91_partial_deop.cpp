// Pass 91 — PartialDeoptimization (PE family; see pe/pe.h)
//
// PARTIAL DEOP as a first-class version ladder, driven by PGO argument
// sketches (the "profiles are a force multiplier" leg of the design):
//
//   --pgo=instrument  One sticky-value sketch per (function, integer
//                     parameter) at the function entry: counters
//                     [first, total, match]. The bumps live on the ENTRY
//                     MEMORY CHAIN (the first effects of the function), so
//                     inlining earlier in the pipeline already copied them
//                     per call site — each invocation bumps exactly once
//                     and counts aggregate across sites (unlike pass 43's
//                     loop bumps, no no_inline marking is needed).
//
//   --pgo=use=<f>     A parameter is hot when match/total >= 95% with
//                     total >= 64. For every call whose argument is
//                     dynamic, emit the guarded ladder (up to two rungs):
//
//                         guard p1 == V1            else
//                           guard p2 == V2          else  call V1 (rung 1)
//                             call V12  (rung 2)
//                           merge R2
//                         merge R1
//
//                     Guard failure transfers DOWN one rung, not to the
//                     generic floor: V12 fails p2 -> V1; V1 fails p1 ->
//                     V0 (a fresh copy of the original generic call in the
//                     outer false arm). Every guard sits at a
//                     call-boundary checkpoint BEFORE any specialized
//                     effect executes — no OSR, no compensation, no
//                     side-effect replay; exactly one rung runs, and each
//                     rung is the whole function.
//
// Correctness never depends on the profile: a wrong hot value only means
// the guard fails and the generic rung runs. The next instrument round
// observes the shifted distribution (the guard routes non-matching
// invocations to the outlined generic body) — the sketch is the guard
// profiling / widening feedback loop.
//
// Honesty: rungs are capped at two assumptions per site and single
// generation (variants are never themselves specialized). Loop-header
// deop, OSR and mid-region guards are future work.
#include "core/son/passes/pe/pe.h"

#include <algorithm>

namespace jules {

namespace {

constexpr u32 kMaxLadderRungs = 2;

struct LadderEnd {
    NodeId val = kNoNode;   // value result (kNoNode for void functions)
    NodeId mem = kNoNode;   // memory version result
    NodeId exit = kNoNode;  // merge block (or the arm block when no merge)
};

} // namespace

class PartialDeoptimizationPass : public Pass {
public:
    const char* name() const override { return "PartialDeoptimization"; }
    int order() const override { return 91; }
    const char* phase_name() const override { return "Phase 9: Partial Evaluation & Deopt"; }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo |
               AnalysisKind::AliasInfo | AnalysisKind::MemDep | AnalysisKind::CallGraph;
    }
    ModeMask modes() const override {
        // The ladder is profile-driven speculation: AOT (the profile was
        // dumped via .init_array / atexit, same as pass 43) AND the
        // optimizing JIT tier (compile-time latency budgeted by
        // cap_level_for_jit). Baseline JIT stays out: it must not pay for
        // cloning. This is also what lets the guard family (71/74/89)
        // actually meet: pass 89's manifest is JIT-emitted, so the
        // guarded ladder must be able to exist in JIT mode.
        return kModeAOT | kModeJitOptimizing;
    }
    bool run(PassContext& ctx) override {
        if (ctx.opts.pgo == PgoMode::Instrument) return instrument(ctx);
        if (ctx.opts.pgo == PgoMode::Use) return use(ctx);
        return false; // off / sample / live: honest no-op
    }

private:
    // ---- instrument: entry sketches --------------------------------------
    static bool instrument(PassContext& ctx) {
        if (ctx.opts.orig_fn_count == 0) return false;
        u64 base = pe_sketch_base(ctx.opts);
        u32 j = 0;
        bool changed = false;
        u32 upto = ctx.opts.orig_fn_count < ctx.mod.fns.size()
                       ? ctx.opts.orig_fn_count
                       : static_cast<u32>(ctx.mod.fns.size());
        for (u32 fi = 0; fi < upto; ++fi) {
            FunctionGraph& fg = ctx.mod.fns[fi];
            Graph& g = fg.g;

            // eligible params, in param order (the enumeration contract)
            std::vector<u32> params;
            for (u32 p = 0; p < fg.param_types.size(); ++p)
                if (ty_is_int(fg.param_types[p])) params.push_back(p);
            if (params.empty()) continue;

            // Param nodes by index
            FlatMap<u32, NodeId> param_node;
            for (NodeId id = 0; id < g.size(); ++id) {
                const Node& n = g.node(id);
                if (n.op == Op::Param) param_node.insert(n.aux, id);
            }

            // Snapshot memory-kind users of the entry version BEFORE the
            // bumps exist (rewriting after would catch bump[0]'s own mem
            // input and form a cycle).
            NodeId start = g.start();
            struct MemUse { NodeId u; u8 slot; };
            std::vector<MemUse> mem_uses;
            for (NodeId u : g.uses_of(start)) {
                Node& un = g.node(u);
                if (un.op == Op::Dead) continue;
                if (un.op == Op::Phi) {
                    for (u8 i = 1; i < un.n_in; ++i)
                        if (un.in[i] == start) mem_uses.push_back({u, i});
                } else if (un.n_in > 1 && un.in[1] == start) {
                    mem_uses.push_back({u, 1});
                }
            }

            // the bump chain: start -> sk0 -> sk1 -> ... -> skN
            NodeId prev = start;
            for (u32 p : params) {
                const NodeId* pn = param_node.find(p);
                if (!pn || *pn == kNoNode) continue;
                NodeId b = g.make(Op::Call, ty_mem(), {start, prev, *pn}, 0,
                                  kFnPgoSketch);
                g.node(b).ival = static_cast<i64>(base + kPeSketchSlots * j);
                ++j;
                prev = b;
            }
            if (prev == start) continue; // nothing inserted

            // original memory readers now observe the chain's tail
            for (const MemUse& mu : mem_uses) g.set_input(mu.u, mu.slot, prev);
            g.mark_uses_dirty();
            g.touch();
            changed = true;
        }
        return changed;
    }

    // ---- use: the guarded ladder -------------------------------------------
    static bool use(PassContext& ctx) {
        const std::vector<u64>& cnt = ctx.opts.pgo_counters;
        if (cnt.empty() || ctx.opts.orig_fn_count == 0) return false;
        u64 base = pe_sketch_base(ctx.opts);
        PeBudgets b = pe_budgets(ctx.opts.level);
        if (b.max_variants_per_fn == 0) return false;

        // enumerate (fn, param) in the instrument build's order; collect
        // hot parameters (sticky Const, or a Range hull within budget)
        FlatMap<FnId, std::vector<PeAssumption>> hot;
        u32 upto = ctx.opts.orig_fn_count < ctx.mod.fns.size()
                       ? ctx.opts.orig_fn_count
                       : static_cast<u32>(ctx.mod.fns.size());
        u32 j = 0;
        for (u32 fi = 0; fi < upto; ++fi) {
            FunctionGraph& fg = ctx.mod.fns[fi];
            for (u32 p = 0; p < fg.param_types.size(); ++p) {
                if (!ty_is_int(fg.param_types[p])) continue;
                if (base + kPeSketchSlots * j + 4 < cnt.size()) {
                    u64 first = cnt[base + kPeSketchSlots * j];
                    u64 total = cnt[base + kPeSketchSlots * j + 1];
                    u64 match = cnt[base + kPeSketchSlots * j + 2];
                    u64 mn = cnt[base + kPeSketchSlots * j + 3];
                    u64 mx = cnt[base + kPeSketchSlots * j + 4];
                    if (total < kPeSketchMinSamples) { ++j; continue; }
                    if (match * 100ull >= kPeSketchMinMatchPct * total) {
                        // sticky value: assume param == first
                        PeAssumption a;
                        a.param = static_cast<u8>(p);
                        a.value.is_fp = false;
                        a.value.ty = fg.param_types[p];
                        a.value.iv = static_cast<i64>(first);
                        hot[fg.fid].push_back(a);
                    } else if (ty_bits(fg.param_types[p]) == 64) {
                        // not sticky, but a narrow hull: assume param in
                        // [min, max]. 64-bit params only — narrower widths
                        // sketch zero-extended and their recorded order is
                        // not the source domain's signed order.
                        i64 slo = static_cast<i64>(mn);
                        i64 shi = static_cast<i64>(mx);
                        if (slo <= shi) {
                            u64 span = static_cast<u64>(shi) -
                                       static_cast<u64>(slo); // exact: slo<=shi
                            if (span > 0 && span <= b.range_max_span) {
                                PeAssumption a;
                                a.kind = PeKind::Range;
                                a.param = static_cast<u8>(p);
                                a.value.ty = fg.param_types[p];
                                a.range_lo = slo;
                                a.range_hi = shi;
                                hot[fg.fid].push_back(a);
                            }
                        }
                    }
                }
                ++j;
            }
        }
        if (hot.empty()) return false;

        bool changed = false;

        for (u32 fi = 0; fi < ctx.mod.fns.size(); ++fi) {
            for (NodeId id = 0; id < ctx.mod.fns[fi].g.size(); ++id) {
                Node nc = ctx.mod.fns[fi].g.node(id); // snapshot: variant
                // creation below appends to mod.fns and may move it
                if (nc.op != Op::Call || nc.n_in <= 2) continue;
                FnId target = nc.aux;
                if (target == kFnPrint || target == kFnFree ||
                    target == kFnPgoBump || target == kFnPgoSketch) {
                    continue;
                }
                if (target >= ctx.opts.orig_fn_count) continue; // originals only
                const std::vector<PeAssumption>* hs = hot.find(target);
                if (!hs || hs->empty()) continue;

                // rung candidates: hot params with DYNAMIC call arguments
                // (constant arguments are pass 90's domain — a guard on a
                // const is dead weight)
                std::vector<PeAssumption> asms;
                for (const PeAssumption& a : *hs) {
                    if (asms.size() >= kMaxLadderRungs) break;
                    u32 slot = 2u + a.param;
                    if (slot >= nc.n_in) continue;
                    if (ctx.mod.fns[fi].g.node(nc.in[slot]).op == Op::Const) continue;
                    asms.push_back(a);
                }
                if (asms.empty()) continue;
                std::sort(asms.begin(), asms.end(),
                          [](const PeAssumption& x, const PeAssumption& y) {
                              return x.param < y.param;
                          });

                // rung variants: prefix assumption sets V[0..k); the
                // generic floor (k == 0) is a fresh copy of the original
                // call, not a variant.
                std::vector<FnId> rungs(asms.size() + 1, kNoFn);
                bool ok = true;
                for (size_t k = 1; k <= asms.size(); ++k) {
                    std::vector<PeAssumption> prefix(asms.begin(),
                                                     asms.begin() + static_cast<long>(k));
                    bool made = false;
                    rungs[k] = pe_make_variant(ctx.mod, ctx.syms, target, prefix, b,
                                               &made);
                    if (rungs[k] == kNoFn || !made) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) continue;

                // Re-acquire after pe_make_variant (mod.fns may have moved).
                Graph& g = ctx.mod.fns[fi].g;
                if (id >= g.size() || g.node(id).op != Op::Call) continue; // defensive
                emit_ladder(g, id, asms, rungs);
                changed = true;
            }
        }
        return changed;
    }

    // ---- ladder emission -----------------------------------------------------

    // NOTE: the `Node nc` snapshots are passed BY VALUE everywhere below —
    // every g.make()/make_arr() can reallocate the graph's node vector,
    // which would dangle a Node reference.

    // One rung's call: the variant binding asms[0..bound_count) — Const
    // bindings DROP their call arguments, Range bindings KEEP them (the
    // value stays runtime inside the variant). rung == kNoFn falls back to
    // the original target (the generic floor).
    static NodeId make_rung_call(Graph& g, Node nc, NodeId cb, NodeId call_mem,
                                 FnId rung, const std::vector<PeAssumption>& asms,
                                 u32 bound_count) {
        NodeId ins[kMaxInputs];
        ins[0] = cb;
        ins[1] = call_mem;
        u8 k = 2;
        for (u8 i = 2; i < nc.n_in; ++i) {
            u32 pidx = static_cast<u32>(i - 2);
            bool is_bound = false;
            for (u32 z = 0; z < bound_count; ++z)
                if (asms[z].param == pidx &&
                    asms[z].kind == PeKind::Const) { is_bound = true; break; }
            if (is_bound) continue;
            if (k >= kMaxInputs) break;
            ins[k++] = nc.in[i];
        }
        FnId tgt = (rung != kNoFn) ? rung : nc.aux;
        return g.make_arr(Op::Call, nc.ty, ins, k, 0, tgt);
    }

    // Emit the rung call with `depth` assumptions, pinned at `cb`.
    static LadderEnd arm_call(Graph& g, Node nc, NodeId cb,
                              const std::vector<PeAssumption>& asms,
                              const std::vector<FnId>& rungs, size_t depth) {
        u32 bound = static_cast<u32>(depth);
        if (bound > asms.size()) bound = static_cast<u32>(asms.size());
        FnId rung = (bound < rungs.size()) ? rungs[bound] : kNoFn;
        NodeId c = make_rung_call(g, nc, cb, nc.in[1], rung, asms, bound);
        LadderEnd e;
        e.val = (nc.ty != ty_void()) ? c : kNoNode;
        e.mem = c;
        e.exit = cb;
        return e;
    }

    // Merge a true-side end and a false-side end at their join: Region +
    // value/memory phis (the ladder's structural merge, shared by the
    // const-guard and range-guard shapes).
    static LadderEnd merge_ends(Graph& g, Node nc, const LadderEnd& T, NodeId f,
                                const LadderEnd& F) {
        LadderEnd out;
        out.exit = g.make(Op::Region, ty_ctrl(), {T.exit, f});
        out.mem = g.make(Op::Phi, ty_mem(), {out.exit, T.mem, F.mem});
        if (nc.ty != ty_void())
            out.val = g.make(Op::Phi, nc.ty, {out.exit, T.val, F.val});
        return out;
    }

    // Recursive rung builder. Level k guards asms[k] at `cb` on its true
    // side (descending to level k+1, or the innermost rung call when k+1
    // is past the end) and falls back to the rung with k bindings on its
    // false side (the generic floor at k == 0).
    static LadderEnd build_level(Graph& g, Node nc, NodeId cb, size_t k,
                                 const std::vector<PeAssumption>& asms,
                                 const std::vector<FnId>& rungs,
                                 std::vector<NodeId>& guards) {
        if (k >= asms.size()) return arm_call(g, nc, cb, asms, rungs, asms.size());

        const PeAssumption& a = asms[k];
        u32 slot = 2u + a.param;
        NodeId arg = (slot < nc.n_in) ? nc.in[slot] : kNoNode;
        if (arg == kNoNode) return arm_call(g, nc, cb, asms, rungs, k); // defensive

        if (a.kind == PeKind::Range) {
            // Range guard: (arg >= lo) and (arg <= hi) as two nested Ifs —
            // the IR's Cmp is signed-only and this reuses exactly the
            // control shapes the const path exercises. BOTH false arms
            // fall to the rung with k bindings (fresh calls — exactly one
            // of the three arms runs; each is the whole function).
            NodeId lo_c = g.make(Op::Const, a.value.ty, {cb});
            g.node(lo_c).ival = a.range_lo;
            NodeId ge = g.make(Op::Cmp, ty_i1(), {cb, arg, lo_c},
                               static_cast<u8>(CmpOp::Ge));
            NodeId gif1 = g.make(Op::If, ty_ctrl(), {cb, ge});
            g.node(gif1).flags |= kFlagGuardSite;
            guards.push_back(gif1);
            NodeId t1 = g.make(Op::IfTrue, ty_ctrl(), {gif1});
            NodeId f1 = g.make(Op::IfFalse, ty_ctrl(), {gif1});

            NodeId hi_c = g.make(Op::Const, a.value.ty, {t1});
            g.node(hi_c).ival = a.range_hi;
            NodeId le = g.make(Op::Cmp, ty_i1(), {t1, arg, hi_c},
                               static_cast<u8>(CmpOp::Le));
            NodeId gif2 = g.make(Op::If, ty_ctrl(), {t1, le});
            g.node(gif2).flags |= kFlagGuardSite;
            guards.push_back(gif2);
            NodeId t2 = g.make(Op::IfTrue, ty_ctrl(), {gif2});
            NodeId f2 = g.make(Op::IfFalse, ty_ctrl(), {gif2});

            LadderEnd T = build_level(g, nc, t2, k + 1, asms, rungs, guards);
            LadderEnd F2 = arm_call(g, nc, f2, asms, rungs, k);
            LadderEnd inner = merge_ends(g, nc, T, f2, F2);

            LadderEnd F1 = arm_call(g, nc, f1, asms, rungs, k);
            return merge_ends(g, nc, inner, f1, F1);
        }

        // const guard: arg == V (the hot constant, in the param's domain)
        NodeId v = g.make(Op::Const, a.value.ty, {cb});
        g.node(v).ival = a.value.iv;
        g.node(v).fval = a.value.fv;
        NodeId cmp = g.make(Op::Cmp, ty_i1(), {cb, arg, v}, static_cast<u8>(CmpOp::Eq));
        NodeId gif = g.make(Op::If, ty_ctrl(), {cb, cmp});
        // Mark the guard site: the scalar unroll/peel family must not clone
        // through a deopt ladder (its cloner re-threads merge-region phis
        // and would sever the fallback rung) — the reserved flag is exactly
        // this contract. Vectorized loops use the same class of family
        // ownership (see match_counted's packed-loop rejection).
        g.node(gif).flags |= kFlagGuardSite;
        guards.push_back(gif);
        NodeId t = g.make(Op::IfTrue, ty_ctrl(), {gif});
        NodeId f = g.make(Op::IfFalse, ty_ctrl(), {gif});

        // true side: deeper rungs (or the innermost variant call)
        LadderEnd T = build_level(g, nc, t, k + 1, asms, rungs, guards);

        // false side: the rung with k bindings (generic floor at k == 0)
        LadderEnd F = arm_call(g, nc, f, asms, rungs, k);

        return merge_ends(g, nc, T, f, F);
    }

    // Build the guarded ladder over the (still live) call node `call`,
    // then replace it: its users are rewired onto the top merge's phis and
    // the original call is killed (the outer false arm gets a FRESH
    // generic call — re-pinning the original would tangle its external
    // users with the ladder's own phi inputs).
    static void emit_ladder(Graph& g, NodeId call, const std::vector<PeAssumption>& asms,
                            const std::vector<FnId>& rungs) {
        Node nc = g.node(call);
        NodeId B = nc.in[0];
        std::vector<NodeId> guards; // this ladder's guard Ifs (stay at B)

        LadderEnd top = build_level(g, nc, B, 0, asms, rungs, guards);

        // rewire the original call's users onto the merge phis, then kill it
        pe_rewire_call_users(g, call, top.val, top.mem);
        g.kill(call);

        // ---- repin: nodes pinned at B that (transitively) read the ladder
        // result must move to the merge block; B's original control users
        // (minus the ladder's guards) move with it; phis are structural
        // and never move. Everything that stays at B dominates the merge,
        // so non-dependent nodes keep valid dominance. (Every original
        // user of the call was dominated by B; the old terminator now sits
        // at the merge, so blocks after it remain dominated by the merge.)
        FlatMap<NodeId, bool> visited;
        SmallVec<NodeId, 32> stack;
        if (top.val != kNoNode) stack.push_back(top.val);
        if (top.mem != kNoNode) stack.push_back(top.mem);
        std::vector<NodeId> moved;
        while (!stack.empty()) {
            NodeId n = stack.back();
            stack.pop_back();
            if (n == kNoNode || visited.contains(n)) continue;
            visited.insert(n, true);
            const Node& nd = g.node(n);
            if (nd.op == Op::Dead) continue;
            // Phis and control nodes never move, but their USERS must
            // still be examined: a data node pinned at B reading the merge
            // phi (e.g. the loop's accumulator update) has to reach the
            // merge block or it executes BEFORE the guard — the observed
            // miscompile scheduled `acc += result` ahead of the calls.
            if (!is_control_op(nd.op) && nd.op != Op::Phi && nd.n_in > 0 &&
                nd.in[0] == B) {
                moved.push_back(n);
            }
            for (NodeId u : g.uses_of(n)) stack.push_back(u);
        }
        for (NodeId n : moved) g.set_input(n, 0, top.exit);

        const SmallVec<NodeId, 4> users = g.uses_of(B);
        for (NodeId u : users) {
            Node& un = g.node(u);
            if (un.op == Op::Dead) continue;
            if (un.op == Op::Jump || un.op == Op::If || un.op == Op::Return) {
                if (un.in[0] != B) continue;
                bool is_guard = false;
                for (NodeId gd : guards)
                    if (gd == u) { is_guard = true; break; }
                if (!is_guard) g.set_input(u, 0, top.exit);
            } else if (un.op == Op::Region) {
                for (u8 k = 0; k < un.n_in; ++k)
                    if (un.in[k] == B) g.set_input(u, k, top.exit);
            }
        }
        g.mark_uses_dirty();
        g.touch();
    }
};

JULES_REGISTER_PASS(PartialDeoptimizationPass, 91, "Phase 9")

} // namespace jules
