// Pass 73 — GuardWeakening (Phase 6: Devirtualization & Speculation)
//
// "Equality guard -> range guard when ranges suffice": for a const-guard
// rung of a pass-91 ladder whose profile sketch shows a narrow observed
// HULL around the sticky value (invocations inside the hull currently
// fail the equality check and fall to the less-specialized rung), the
// negotiation is:
//
//   1. re-derive the (origin-fn, param) sketch exactly as pass 91
//      enumerated it, and read the hull [min, max] containing V;
//   2. verify the guarded assumption is the ladder's unique Const
//      binding with the guard's constant;
//   3. accept the widening when the hull fits the level's range budget;
//
// The REWRITE is gated to the LAST (deepest) assumption of the ladder:
// the weakened Eq guard is replaced by the two-hinge range guard
// (arg >= lo, arg <= hi), the protected rung call is re-emitted against
// the widened variant ([prefix..., Range(param, lo, hi)]) and the
// false-path join takes over the old fallback rung. Soundness: rungs
// BELOW the weakened guard carry the prefix asms[0..j) for j > k —
// every one of them includes the weakened Const binding and would run
// with param != V after the widening (the [Const x, Range c] shape: the
// c-fallback rungs still assume x == V — a miscompile). With k LAST,
// the only such rung is the protected call itself, which the range rung
// REPLACES; the false-arm fallback (the prefix rung) never assumed
// anything about the weakened param. Non-last sites are detected and
// counted (the nested restructure — re-emitting deeper fallbacks with
// the widened binding — is future work).
//
// The variant-cap contract: the widened rung REPLACES the protected
// call's ladder position, so pe_make_variant is granted one extra slot
// (extra_slots=1) — the per-SITE ladder width is unchanged and the
// retired variant's calls are killed in the same action (see pe.h).
#include "core/son/passes/pe/pe.h"

#include <algorithm>
#include <cstdio>

namespace jules {

namespace {

// The rung call this guard protects: walking the true side THROUGH the
// deeper ladder guards (they guard OTHER parameters), the first Call
// node found is A rung; the COUNT is the soundness gate — a guard whose
// true arm still carries a deeper sub-ladder has one rung call per
// deeper level (every level's fallback lands inside the true side),
// while the LAST binding's true arm hosts exactly ONE call (the
// innermost rung). The walk STOPS at the ladder's top merge — the
// Region carrying the guard's false projection as a pred — so it
// never leaks into the post-ladder loop body (the fallback call there
// is not this guard's rung).
//
// (Found the hard way: the first-call-only version returned a FALLBACK
// rung by stack order on the [Const x, Range c] shape — with the
// rewrite gated off it was harmless telemetry, un-gated it
// restructured around the wrong call: SIGSEGV.)
NodeId true_side_call(Graph& g, NodeId proj, NodeId false_proj,
                      u32* call_count) {
    std::vector<NodeId> work{proj};
    FlatMap<NodeId, bool> seen;
    NodeId found = kNoNode;
    u32 calls = 0;
    while (!work.empty()) {
        NodeId n = work.back();
        work.pop_back();
        if (seen.contains(n)) continue;
        seen.insert(n, true);
        const Node& nn = g.node(n);
        if (nn.op == Op::Dead) continue;
        bool at_merge = false;
        for (u8 k = 0; k < nn.n_in; ++k)
            if (nn.in[k] == false_proj) at_merge = true;
        if (at_merge) continue; // the ladder's top: users are post-ladder
        bool is_rung = nn.op == Op::Call && nn.aux != kFnPrint &&
                       nn.aux != kFnFree && nn.aux != kFnPgoBump &&
                       nn.aux != kFnPgoSketch;
        if (is_rung) {
            if (++calls > 1) break; // nested sub-ladder: not the last binding
            found = n;
        }
        if (is_rung || nn.op == Op::If || nn.op == Op::IfTrue ||
            nn.op == Op::IfFalse || nn.op == Op::Region || nn.op == Op::Jump)
            for (NodeId u : g.uses_of(n)) work.push_back(u);
    }
    *call_count = calls;
    // The first call feeds the negotiation (any rung's assumptions carry
    // the guarded binding); ONLY the count gates the rewrite.
    return found;
}

} // namespace

class GuardWeakeningPass : public Pass {
public:
    const char* name() const override { return "GuardWeakening"; }
    int order() const override { return 73; }
    const char* phase_name() const override {
        return "Phase 6: Devirtualization & Speculation";
    }
    AnalysisMask required() const override {
        return static_cast<AnalysisMask>(AnalysisKind::Dominators);
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo |
               AnalysisKind::AliasInfo | AnalysisKind::MemDep |
               AnalysisKind::CallGraph;
    }
    ModeMask modes() const override { return kModeAOT | kModeJitOptimizing; }
    u32 detected_ = 0; // telemetry: weakenable guard sites

    bool run(PassContext& ctx) override {
        // The negotiation analysis runs in use mode; the REWRITE is gated
        // pending the variant-cap interaction (see the file header).
        if (ctx.opts.pgo != PgoMode::Use) return false;
        if (ctx.opts.orig_fn_count == 0) return false;
        PeBudgets b = pe_budgets(ctx.opts.level);
        if (b.max_variants_per_fn == 0 || b.range_max_span == 0) return false;

        bool changed = false;
        // INDEX iteration + PER-ITERATION re-binding: pe_make_variant
        // appends to mod.fns and may REALLOCATE it — any reference held
        // across the call dangles, INCLUDING the loop condition's g and
        // any g bound before the rewrite (31-a crash; 31-c's ASAN proof
        // showed the per-FUNCTION binding surviving into the next gid
        // iteration — the rest of the scan ran on freed storage). Node
        // ids and value snapshots survive; references never do.
        u32 fn_count = static_cast<u32>(ctx.mod.fns.size());
        for (u32 fi = 0; fi < fn_count; ++fi) {
            for (NodeId gid = 0; gid < ctx.mod.fns[fi].g.size(); ++gid) {
                Graph& g = ctx.mod.fns[fi].g; // fresh each iteration
                Node gc = g.node(gid); // snapshot
                if (gc.op != Op::If || (gc.flags & kFlagGuardSite) == 0)
                    continue;
                if (gc.n_in < 2) continue;
                NodeId cmp = gc.in[1];
                if (g.node(cmp).op != Op::Cmp ||
                    static_cast<CmpOp>(g.node(cmp).sub) != CmpOp::Eq)
                    continue;
                NodeId arg = g.node(cmp).in[1];
                NodeId vc = g.node(cmp).in[2];
                if (g.node(vc).op != Op::Const) continue;
                NodeId B = gc.in[0];
                NodeId t = kNoNode, f = kNoNode;
                for (NodeId u : g.uses_of(gid)) {
                    const Node& un = g.node(u);
                    if (un.op == Op::IfTrue && un.in[0] == gid) t = u;
                    if (un.op == Op::IfFalse && un.in[0] == gid) f = u;
                }
                if (t == kNoNode || f == kNoNode) continue;

                // the protected rung call: the negotiation reads A rung's
                // assumptions; the rewrite additionally requires the
                // TRUE side to host exactly that ONE call (the last-binding
                // shape — see true_side_call).
                u32 n_calls = 0;
                NodeId call = true_side_call(g, t, f, &n_calls);
                if (call == kNoNode) continue; // no rung under the true side
                Node cc = g.node(call);
                FnId origin = pe_variant_origin(cc.aux);
                if (origin == kNoFn || origin >= ctx.opts.orig_fn_count)
                    continue;
                const std::vector<PeAssumption>* asms =
                    pe_variant_assumptions(cc.aux);
                if (!asms || asms->empty()) continue;

                // the guarded parameter: this guard is the LAST binding's
                // guard — its argument node must be the one the ORIGINAL
                // call passed for asms->back().param. The const binding
                // DROPPED that argument from the variant call; the guard's
                // cond still carries the node, so the association is
                // (origin fn, back().param) with V == the sketch's sticky
                // value.
                // the guarded assumption: the unique Const binding whose
                // value is the guard's constant (Range bindings hinge on
                // runtime values and never form Eq guards)
                u8 P = 0;
                bool found_asm = false;
                for (const PeAssumption& a : *asms) {
                    if (a.kind != PeKind::Const) continue;
                    if (a.value.iv != g.node(vc).ival) continue;
                    if (found_asm) {
                        found_asm = false; // ambiguous: skip
                        break;
                    }
                    P = a.param;
                    found_asm = true;
                }
                if (!found_asm) continue;
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] guard n%u P=%u V=%lld origin=%u\n",
                             gid, P, (long long)g.node(vc).ival, origin);
#endif
                i64 V = g.node(vc).ival;
                FunctionGraph* tf = ctx.mod.find_fn(origin);
                if (!tf || P >= tf->param_types.size()) continue;
                if (ty_bits(tf->param_types[P]) != 64) continue;
                // Snapshot the param type BEFORE any variant creation — tf
                // points into mod.fns and dangles across pe_make_variant's
                // append (see the re-acquisition below).
                TypeId pty = tf->param_types[P];

                // ---- soundness gate: the weakened binding is LAST ------
                // (see the file header; non-last sites are detected only)
                if (asms->empty() || asms->back().param != P ||
                    asms->back().kind != PeKind::Const) {
                    detected_ += 1;
                    continue;
                }

                // ---- the hull, re-derived exactly as p91 enumerated ------
                // (sketch region starts right after the loop-pair counters;
                // the ALLOCATION-site blocks of pass 34 come later)
                const u64 sbase = pe_sketch_base(ctx.opts);
                u64 skidx = sbase;
                // (fn, param) enumeration: fn-major, param order
                u32 upto = ctx.opts.orig_fn_count < ctx.mod.fns.size()
                               ? ctx.opts.orig_fn_count
                               : static_cast<u32>(ctx.mod.fns.size());
                bool found = false;
                i64 lo = 0, hi = 0;
                for (u32 fi = 0; fi < upto && !found; ++fi) {
                    const FunctionGraph& q = ctx.mod.fns[fi];
                    for (u32 p = 0; p < q.param_types.size(); ++p) {
                        if (!ty_is_int(q.param_types[p])) continue;
                        if (q.fid == tf->fid && p == P) {
                            u64 i0 = skidx;
                            if (i0 + 4 < ctx.opts.pgo_counters.size()) {
                                lo = static_cast<i64>(ctx.opts.pgo_counters[i0 + 3]);
                                hi = static_cast<i64>(ctx.opts.pgo_counters[i0 + 4]);
                                found = true;
                            }
                            break;
                        }
                        skidx += 5;
                    }
                }
                if (!found) continue;
                if (lo > hi) continue;
                if (V < lo || V > hi) continue; // hull must contain V
                u64 span = static_cast<u64>(hi) - static_cast<u64>(lo);
                if (span == 0 || span > b.range_max_span) continue;

                // Accepted negotiation — count every site (rewrite or
                // not; nested shapes are future work).
                detected_ += 1;

                // ---- the sound-rewrite gate: the ONE-call true arm -----
                // (n_calls > 1 means the true side still carries a deeper
                // sub-ladder whose fallback rungs assume the weakened
                // Const binding — see the file header).
                if (n_calls != 1) continue;

                // ---- the range variant ----------------------------------
                std::vector<PeAssumption> rasms = *asms;
                for (PeAssumption& a : rasms) {
                    if (a.param != P || a.kind != PeKind::Const) continue;
                    a.kind = PeKind::Range;
                    a.range_lo = lo;
                    a.range_hi = hi;
                }
                // "ranges suffice": branch pruning must survive — checked
                // BEFORE creating the variant so a rejected widening
                // leaks nothing into the module or the variant table.
                {
                    PeBta bc = pe_binding_time_analysis(tf->g, *asms);
                    PeBta br = pe_binding_time_analysis(tf->g, rasms);
                    if (br.static_ifs < bc.static_ifs) continue;
                }
                bool made = false;
                // extra_slots=1: the widened rung REPLACES the protected
                // call's ladder position (its calls are killed below in
                // this same action) — the per-site width is unchanged.
                FnId range_rung =
                    pe_make_variant(ctx.mod, ctx.syms, origin, rasms, b, &made,
                                    /*extra_slots=*/1);
                if (!made || range_rung == kNoFn) continue;
                // Re-acquire after pe_make_variant (mod.fns may have
                // moved): the iteration's binding predates the append.
                // Node ids (B, arg, t, f, call, gid) and the cc/gc
                // snapshots are values — they stay valid; only the
                // reference must be rebuilt. (Same block scope as the
                // loop-top binding, hence the distinct name.)
                Graph& gr = ctx.mod.fns[fi].g;
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] REWRITE n%u rasms=%zu range=%u\n",
                             gid, rasms.size(), range_rung);
#endif

                // ---- rewrite ----------------------------------------------
                NodeId lo_c = gr.make(Op::Const, pty, {B});
                gr.node(lo_c).ival = lo;
                NodeId ge = gr.make(Op::Cmp, ty_i1(), {B, arg, lo_c},
                                   static_cast<u8>(CmpOp::Ge));
                NodeId g1 = gr.make(Op::If, ty_ctrl(), {B, ge});
                gr.node(g1).flags |= kFlagGuardSite;
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] made g1=%u\n", g1);
#endif
                NodeId t1 = gr.make(Op::IfTrue, ty_ctrl(), {g1});
                NodeId f1 = gr.make(Op::IfFalse, ty_ctrl(), {g1});

                NodeId hi_c = gr.make(Op::Const, pty, {t1});
                gr.node(hi_c).ival = hi;
                NodeId le = gr.make(Op::Cmp, ty_i1(), {t1, arg, hi_c},
                                   static_cast<u8>(CmpOp::Le));
                NodeId g2n = gr.make(Op::If, ty_ctrl(), {t1, le});
                gr.node(g2n).flags |= kFlagGuardSite;
                NodeId t2 = gr.make(Op::IfTrue, ty_ctrl(), {g2n});
                NodeId f2 = gr.make(Op::IfFalse, ty_ctrl(), {g2n});

                // the false-path join (edges only — the arm's content moves
                // onto it, never duplicated)
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] made g2n/t2/f2\n");
#endif
                NodeId rins[2] = {f1, f2};
                NodeId rfalse = gr.make_arr(Op::Region, ty_ctrl(), rins, 2);
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] made rfalse=%u\n", rfalse);
#endif

                // the new rung call: the range variant KEEPS the param, so
                // the call regains the argument the const binding dropped.
                // Its position among the variant's arguments: the origin
                // slot P minus the prefix Const bindings below it (asms is
                // param-sorted, so all n-1 others sit below P).
                NodeId new_call;
                {
                    // `asms` points into the variant TABLE, which
                    // pe_make_variant just REALLOCATED (the append) — read
                    // the local copy instead. Bindings with param < P are
                    // identical in rasms (the weakening only mutates the
                    // P binding itself), so the below-count is unchanged.
                    u8 below = 0;
                    for (const PeAssumption& a : rasms)
                        if (a.param < P && a.kind == PeKind::Const) ++below;
                    u8 arg_pos = static_cast<u8>(P - below);
                    NodeId ins[kMaxInputs];
                    // The rung call's structural slots: pinned on the
                    // in-hull arm (t2), same incoming memory version as
                    // the retired call (the ladder position's mem input).
                    // These two were never initialized in the gated-off
                    // code — an unexecuted rewrite rots (uninitialized
                    // stack read: the verifier saw a Call pinned on an
                    // If with a Cmp as its memory version).
                    ins[0] = t2;
                    ins[1] = cc.in[1];
                    u8 k = 2, a = 0;
                    for (u8 i = 2; i < cc.n_in; ++i) {
                        if (a == arg_pos && k < kMaxInputs) {
                            ins[k++] = arg;
                            ++a;
                        }
                        if (k >= kMaxInputs) break;
                        ins[k++] = cc.in[i];
                        ++a;
                    }
                    if (a == arg_pos && k < kMaxInputs) ins[k++] = arg; // tail
                    new_call = gr.make_arr(Op::Call, cc.ty, ins, k, cc.sub,
                                          range_rung);
                }
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] made new_call=%u\n", new_call);
#endif

                // rewire the old call's users (value + memory) and kill it
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] step A\n");
#endif
                pe_rewire_call_users(gr, call, cc.ty != ty_void() ? new_call
                                                                  : kNoNode,
                                     new_call);
                gr.kill(call);
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] step A done\n");
#endif

                // true-side content (the call's dependents, pinned at t)
                // follows the call onto t2 — emit_ladder's repin walk.
                {
                    FlatMap<NodeId, bool> visited;
                    std::vector<NodeId> stack{new_call};
                    for (NodeId u : gr.uses_of(new_call)) stack.push_back(u);
                    while (!stack.empty()) {
                        NodeId n = stack.back();
                        stack.pop_back();
                        if (n == kNoNode || visited.contains(n)) continue;
                        visited.insert(n, true);
                        const Node& nd = gr.node(n);
                        if (nd.op == Op::Dead) continue;
                        if (!is_control_op(nd.op) && nd.op != Op::Phi &&
                            nd.n_in > 0 && nd.in[0] == t)
                            gr.set_input(n, 0, t2);
                        for (NodeId u : gr.uses_of(n)) stack.push_back(u);
                    }
                }
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] step B done\n");
#endif
                // false-side content moves onto the join; merge regions
                // above the ladder re-point their arm-edge blocks.
                {
                    std::vector<NodeId> pinned_at_f;
                    for (NodeId u : gr.uses_of(f))
                        if (gr.node(u).in[0] == f) pinned_at_f.push_back(u);
                    for (NodeId u : pinned_at_f) gr.set_input(u, 0, rfalse);
                }
                // ladder merges / any control successor rewiring
                for (NodeId arm : {t, f}) {
                    NodeId repl = arm == t ? t2 : rfalse;
                    const SmallVec<NodeId, 4> users = gr.uses_of(arm);
                    for (NodeId u : users) {
                        Node un = gr.node(u);
                        if (un.op == Op::Dead) continue;
                        if (un.op == Op::Jump || un.op == Op::If ||
                            un.op == Op::Return) {
                            if (un.in[0] == arm) gr.set_input(u, 0, repl);
                        } else if (un.op == Op::Region) {
                            for (u8 k = 0; k < un.n_in; ++k)
                                if (un.in[k] == arm) gr.set_input(u, k, repl);
                        }
                    }
                }

#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] step C done\n");
#endif
                gr.kill(t);
                gr.kill(f);
                gr.kill(gid);
#ifdef JULES_DEBUG_GW
                std::fprintf(stderr, "[gw] step D done\n");
#endif
                gr.mark_uses_dirty();
                gr.touch();
               
                changed = true;
            }
        }
        (void)detected_;
        return changed || detected_ > 0;
    }
};

JULES_REGISTER_PASS(GuardWeakeningPass, 73, "Phase 6")

} // namespace jules
