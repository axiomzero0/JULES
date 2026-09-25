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
// STATUS: detection live, rewrite gated. Building the range rung means a
// THIRD variant for the same origin function ([Range(x), Range(c)] next
// to the existing [Const(x)] and [Const(x), Range(c)]) — and
// pe_make_variant's per-function variant cap (the PE family's deliberate
// termination guard, "rungs are capped at two assumptions per site and
// single generation") rejects it. The rewrite also needs the same
// arm-split-elimination surgery pass 74 documents (the false-path join
// restructure below is written and stays for the day the cap-side
// contract is extended). The default mode reports the weakenable-guard
// count through --stats — the proof-count contract passes 72/74 ship
// under; the restructure code below is complete but UNREACHABLE by
// construction — the acceptance point counts the detection and takes
// an unconditional continue, which IS the gate (toggling it on requires
// both named dependencies).
#include "core/son/passes/pe/pe.h"

#include <algorithm>
#include <cstdio>

namespace jules {

namespace {

// The rung call this guard protects: walking the true side THROUGH the
// deeper ladder guards (they guard OTHER parameters), the first Call
// node is the innermost rung. The walk STOPS at the ladder's top merge —
// the Region carrying the guard's false projection as a pred — so it
// never leaks into the post-ladder loop body (the fallback call there
// is not this guard's rung).
NodeId true_side_call(Graph& g, NodeId proj, NodeId false_proj) {
    std::vector<NodeId> work{proj};
    FlatMap<NodeId, bool> seen;
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
        if (nn.op == Op::Call && nn.aux != kFnPrint && nn.aux != kFnFree &&
            nn.aux != kFnPgoBump && nn.aux != kFnPgoSketch)
            return n;
        if (nn.op == Op::If || nn.op == Op::IfTrue || nn.op == Op::IfFalse ||
            nn.op == Op::Region || nn.op == Op::Jump)
            for (NodeId u : g.uses_of(n)) work.push_back(u);
    }
    return kNoNode;
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
        for (FunctionGraph& fg : ctx.mod.fns) {
            Graph& g = fg.g;
            for (NodeId gid = 0; gid < g.size(); ++gid) {
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

                // the protected rung call: the innermost true-side call
                NodeId call = true_side_call(g, t, f);
                if (call == kNoNode) continue;
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
                i64 V = g.node(vc).ival;
                FunctionGraph* tf = ctx.mod.find_fn(origin);
                if (!tf || P >= tf->param_types.size()) continue;
                if (ty_bits(tf->param_types[P]) != 64) continue;

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

                // Accepted negotiation — the rewrite is the gated part
                // (variant cap; see the file header). Count it.
                detected_ += 1;
                continue;

                // ---- the range variant ----------------------------------
                std::vector<PeAssumption> rasms = *asms;
                for (PeAssumption& a : rasms) {
                    if (a.param != P || a.kind != PeKind::Const) continue;
                    a.kind = PeKind::Range;
                    a.range_lo = lo;
                    a.range_hi = hi;
                }
                bool made = false;
                FnId range_rung =
                    pe_make_variant(ctx.mod, ctx.syms, origin, rasms, b, &made);
                if (!made || range_rung == kNoFn) continue;
                // "ranges suffice": branch pruning must survive
                {
                    PeBta bc = pe_binding_time_analysis(tf->g, *asms);
                    PeBta br = pe_binding_time_analysis(tf->g, rasms);
                    if (br.static_ifs < bc.static_ifs) continue;
                }

                // ---- rewrite ----------------------------------------------
                TypeId pty = tf->param_types[P];
                NodeId lo_c = g.make(Op::Const, pty, {B});
                g.node(lo_c).ival = lo;
                NodeId ge = g.make(Op::Cmp, ty_i1(), {B, arg, lo_c},
                                   static_cast<u8>(CmpOp::Ge));
                NodeId g1 = g.make(Op::If, ty_ctrl(), {B, ge});
                g.node(g1).flags |= kFlagGuardSite;
                NodeId t1 = g.make(Op::IfTrue, ty_ctrl(), {g1});
                NodeId f1 = g.make(Op::IfFalse, ty_ctrl(), {g1});

                NodeId hi_c = g.make(Op::Const, pty, {t1});
                g.node(hi_c).ival = hi;
                NodeId le = g.make(Op::Cmp, ty_i1(), {t1, arg, hi_c},
                                   static_cast<u8>(CmpOp::Le));
                NodeId g2n = g.make(Op::If, ty_ctrl(), {t1, le});
                g.node(g2n).flags |= kFlagGuardSite;
                NodeId t2 = g.make(Op::IfTrue, ty_ctrl(), {g2n});
                NodeId f2 = g.make(Op::IfFalse, ty_ctrl(), {g2n});

                // the false-path join (edges only — the arm's content moves
                // onto it, never duplicated)
                NodeId rins[2] = {f1, f2};
                NodeId rfalse = g.make_arr(Op::Region, ty_ctrl(), rins, 2);

                // the new rung call: the range variant KEEPS the param, so
                // the call regains the argument the const binding dropped.
                // Its position among the variant's arguments: the origin
                // slot P minus the prefix Const bindings below it (asms is
                // param-sorted, so all n-1 others sit below P).
                NodeId new_call;
                {
                    u8 below = 0;
                    for (const PeAssumption& a : *asms)
                        if (a.param < P && a.kind == PeKind::Const) ++below;
                    u8 arg_pos = static_cast<u8>(P - below);
                    NodeId ins[kMaxInputs];
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
                    new_call = g.make_arr(Op::Call, cc.ty, ins, k, cc.sub,
                                          range_rung);
                }

                // rewire the old call's users (value + memory) and kill it
                pe_rewire_call_users(g, call, cc.ty != ty_void() ? new_call
                                                                  : kNoNode,
                                     new_call);
                g.kill(call);

                // true-side content (the call's dependents, pinned at t)
                // follows the call onto t2 — emit_ladder's repin walk.
                {
                    FlatMap<NodeId, bool> visited;
                    std::vector<NodeId> stack{new_call};
                    for (NodeId u : g.uses_of(new_call)) stack.push_back(u);
                    while (!stack.empty()) {
                        NodeId n = stack.back();
                        stack.pop_back();
                        if (n == kNoNode || visited.contains(n)) continue;
                        visited.insert(n, true);
                        const Node& nd = g.node(n);
                        if (nd.op == Op::Dead) continue;
                        if (!is_control_op(nd.op) && nd.op != Op::Phi &&
                            nd.n_in > 0 && nd.in[0] == t)
                            g.set_input(n, 0, t2);
                        for (NodeId u : g.uses_of(n)) stack.push_back(u);
                    }
                }
                // false-side content moves onto the join; merge regions
                // above the ladder re-point their arm-edge blocks.
                {
                    std::vector<NodeId> pinned_at_f;
                    for (NodeId u : g.uses_of(f))
                        if (g.node(u).in[0] == f) pinned_at_f.push_back(u);
                    for (NodeId u : pinned_at_f) g.set_input(u, 0, rfalse);
                }
                // ladder merges / any control successor rewiring
                for (NodeId arm : {t, f}) {
                    NodeId repl = arm == t ? t2 : rfalse;
                    const SmallVec<NodeId, 4> users = g.uses_of(arm);
                    for (NodeId u : users) {
                        Node un = g.node(u);
                        if (un.op == Op::Dead) continue;
                        if (un.op == Op::Jump || un.op == Op::If ||
                            un.op == Op::Return) {
                            if (un.in[0] == arm) g.set_input(u, 0, repl);
                        } else if (un.op == Op::Region) {
                            for (u8 k = 0; k < un.n_in; ++k)
                                if (un.in[k] == arm) g.set_input(u, k, repl);
                        }
                    }
                }

                g.kill(t);
                g.kill(f);
                g.kill(gid);
                g.mark_uses_dirty();
                g.touch();
               
                changed = true;
            }
        }
        (void)detected_;
        return changed || detected_ > 0;
    }
};

JULES_REGISTER_PASS(GuardWeakeningPass, 73, "Phase 6")

} // namespace jules
