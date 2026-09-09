// Pass 43 — ProfileGuidedUnrolling (Phase 4)
//
// Profile-guided unrolling of DYNAMIC-trip counted loops (the family pass 42
// rejects: `while i < n` with a runtime bound). Two exclusive modes:
//
//   --pgo=instrument  Every counted-shape loop (const OR dynamic bound) gets
//                     two counter bumps — an ENTRY counter pinned at the
//                     loop's entry predecessor (fires once per time control
//                     reaches the loop: once per outer iteration for nested
//                     loops) and a HEADER counter pinned at the guard's body
//                     projection (fires once per iteration). The bumps are
//                     Call{kFnPgoBump} effects chained into the memory phi
//                     when one exists (entry input / latch input rewired — no
//                     version forks), or chained off the root memory version
//                     when the loop is memory-free. The isel lowers each bump
//                     to one `incq jules_pgo_counters+idx*8(%rip)`; an
//                     .init_array constructor registers an atexit dump that
//                     writes the raw counters to jules.prof.
//                     Counter pairs are indexed in deterministic (function,
//                     loop-discovery) order — the enumeration happens BEFORE
//                     any mutation, so the instrument and use builds of the
//                     same source derive identical indices (nothing before
//                     pass 43 reads the PGO mode).
//                     Counted soundness under later transforms: const-trip
//                     unrolling/peeling (42/44, also in the cleanup sweep)
//                     duplicates the bumps but the total still equals the
//                     trip count (F copies each executing once per unrolled
//                     iteration); every other loop transform (vectorization,
//                     fusion, fission, interchange, idiom expansion) rejects
//                     loops containing calls, so instrumented loops keep
//                     their original trip structure.
//
//   --pgo=use=<f>     The driver reads jules.prof into the pass options.
//                     Loop i's average trip is H/E (header / entry
//                     counters — multi-invocation functions divide out).
//                     Loops with E == 0 (never entered), H == 0, or average
//                     < 2 are skipped. The factor is the largest power of
//                     two <= min(average, level budget). The transform is
//                     the GUARDED-EPILOGUE two-loop form:
//                         [main: guard (iv + (F-1)*step <rel> bound)]
//                         [body x F chained, IV advances F*step]
//                         [epilogue: fresh scalar clone of the original
//                          loop entered from the main exit, same guard]
//                     The main guard is the LAST copy's guard test — the
//                     unrolled body only runs when all F scalar iterations
//                     would have — so the construction is sound for every
//                     actual trip count (the profile only picks the FACTOR;
//                     correctness never depends on the profile). The
//                     epilogue's phis take the main loop's phis as entry
//                     values; post-loop readers are retargeted to the
//                     epilogue's phis/exit.
//
// Both modes are AOT (the counter dump rides the ELF .init_array / atexit
// machinery). Const-trip loops stay pass 42's (their profile is recorded
// but unused for unrolling — 42's exact form is strictly better there).
#include "core/son/passes/loop_transforms.h"

#include <cstdlib>
#include <cstdio>

namespace jules {
namespace {

struct PgoLoop {
    NodeId header = kNoNode;     // Region (2 preds: entry, latch)
    u8 entry_slot = 0, latch_slot = 1;
    NodeId guard_if = kNoNode;   // If pinned at header
    NodeId body_proj = kNoNode;  // guard projection entering the body
    NodeId exit_proj = kNoNode;  // guard projection leaving the loop
    NodeId iv_phi = kNoNode;
    i64 iv_step = 1;             // positive constant step
    NodeId bound = kNoNode;      // may be non-const (dynamic trip)
    CmpOp rel = CmpOp::Lt;
    bool iv_on_lhs = true;       // Cmp operand side of the IV
    NodeId mem_phi = kNoNode;    // kNoNode when the loop has no memory phi
    std::vector<NodeId> blocks;  // body block heads (header excluded)
    u32 body_nodes = 0;
    std::vector<NodeId> phis;    // ALL header phis (incl. mem)
    bool single_block = false;   // one body block (epilogue family's scope)
};

bool phi_at_header(const Graph& g, NodeId n, NodeId header) {
    return n != kNoNode && !g.is_dead(n) && g.node(n).op == Op::Phi &&
           g.node(n).in[0] == header;
}

bool has_pgo_bump(Graph& g, const Loop& l) {
    for (NodeId blk : l.blocks) {
        for (NodeId u : g.uses_of(blk)) {
            if (g.is_dead(u) || g.node(u).in[0] != blk) continue;
            if (g.node(u).op == Op::Call && g.node(u).aux == kFnPgoBump) return true;
        }
    }
    return false;
}

// Counted-shape matcher with a DYNAMIC bound allowed: match_counted's twin
// minus the const-bound requirement (and Lt/Le only — the epilogue guard
// construction is specified for those relations). The IV INIT may also be
// non-const (clone seeds handle either).
bool match_pgo_loop(Graph& g, LoopInfo& li, const Loop& l, PgoLoop& out) {
    if (l.blocks.size() < 2) return false;
    NodeId header = l.header;
    const Node& h = g.node(header);
    if (h.op != Op::Region || h.n_in != 2) return false;

    // Packed (vectorized) loops are pass 56's (same guard as match_counted).
    for (NodeId u : g.uses_of(header)) {
        if (g.is_dead(u)) continue;
        if (ty_is_vector(g.node(u).ty)) return false;
    }
    for (NodeId blk : l.blocks) {
        for (NodeId u : g.uses_of(blk)) {
            if (g.is_dead(u) || g.node(u).in[0] != blk) continue;
            if (ty_is_vector(g.node(u).ty)) return false;
        }
    }

    u8 entry_slot = 2, latch_slot = 2;
    for (u8 i = 0; i < 2; ++i) {
        NodeId p = h.in[i];
        bool in_loop = li.block_in_loop(l, p);
        if (in_loop) latch_slot = i; else entry_slot = i;
    }
    if (entry_slot == 2 || latch_slot == 2 || entry_slot == latch_slot) return false;

    NodeId guard_if = kNoNode;
    for (NodeId u : g.uses_of(header)) {
        if (g.node(u).op == Op::If && g.node(u).in[0] == header) {
            if (guard_if != kNoNode) return false;
            guard_if = u;
        }
    }
    if (guard_if == kNoNode) return false;
    const Node& gi = g.node(guard_if);
    if (gi.in[1] == kNoNode || g.node(gi.in[1]).op != Op::Cmp) return false;
    const Node& cmp = g.node(gi.in[1]);

    NodeId body_proj = kNoNode, exit_proj = kNoNode;
    for (NodeId u : g.uses_of(guard_if)) {
        Op o = g.node(u).op;
        if (o != Op::IfTrue && o != Op::IfFalse) continue;
        if (li.block_in_loop(l, u)) {
            if (body_proj != kNoNode) return false;
            body_proj = u;
        } else {
            if (exit_proj != kNoNode) return false;
            exit_proj = u;
        }
    }
    if (body_proj == kNoNode || exit_proj == kNoNode) return false;

    NodeId iv = kNoNode, bound = kNoNode;
    bool iv_on_lhs = true;
    if (phi_at_header(g, cmp.in[1], header)) { iv = cmp.in[1]; bound = cmp.in[2]; }
    else if (phi_at_header(g, cmp.in[2], header)) {
        iv = cmp.in[2];
        bound = cmp.in[1];
        iv_on_lhs = false;
    }
    if (iv == kNoNode || bound == kNoNode) return false;

    // Canonicalize to Lt/Le with the IV on the LHS. The builder/earlier
    // passes may emit the reversed form (`Cmp gt(bound, iv)` for `i < n`),
    // which is the same predicate with the operands swapped. Countdown
    // loops (Gt/Ge with the IV on the lhs, or Lt/Le with it on the rhs)
    // stay out of this family — the guarded-epilogue construction is
    // specified for the ascending form.
    CmpOp rel = static_cast<CmpOp>(cmp.sub);
    CmpOp eff;
    if (iv_on_lhs) {
        if (rel == CmpOp::Lt || rel == CmpOp::Le) eff = rel;
        else return false;
    } else {
        if (rel == CmpOp::Gt) eff = CmpOp::Lt;      // bound > iv  ==  iv < bound
        else if (rel == CmpOp::Ge) eff = CmpOp::Le; // bound >= iv ==  iv <= bound
        else return false;
    }

    const Node& ivn = g.node(iv);
    NodeId upd = ivn.in[latch_slot + 1];
    if (upd == kNoNode || g.is_dead(upd)) return false;
    const Node& u = g.node(upd);
    if (u.op != Op::Bin || static_cast<BinOp>(u.sub) != BinOp::Add) return false;
    i64 step = 0;
    ConstVal sc;
    if (u.in[1] == iv && const_of(g, u.in[2], sc)) step = sc.iv;
    else if (u.in[2] == iv && const_of(g, u.in[1], sc)) step = sc.iv;
    else return false;
    if (step <= 0) return false;

    // no early exits (same scan as match_counted): every internal If's
    // projections stay inside the loop
    for (NodeId blk : l.blocks) {
        if (blk == header) continue;
        for (NodeId q : g.uses_of(blk)) {
            const Node& qn = g.node(q);
            if (qn.op != Op::If || qn.in[0] != blk) continue;
            if (q == guard_if) continue;
            for (NodeId p : g.uses_of(q)) {
                Op po = g.node(p).op;
                if (po != Op::IfTrue && po != Op::IfFalse) continue;
                if (!li.block_in_loop(l, p)) return false;
            }
        }
    }

    if (has_pgo_bump(g, l)) return false; // idempotence (defensive)

    out.header = header;
    out.entry_slot = entry_slot;
    out.latch_slot = latch_slot;
    out.guard_if = guard_if;
    out.body_proj = body_proj;
    out.exit_proj = exit_proj;
    out.iv_phi = iv;
    out.iv_step = step;
    out.bound = bound;
    out.rel = eff;        // canonical Lt/Le
    out.iv_on_lhs = true; // canonical: guards are built IV-side-left
    out.mem_phi = kNoNode;
    for (NodeId p : g.uses_of(header)) {
        if (g.is_dead(p) || g.node(p).op != Op::Phi || p == iv) continue;
        if (g.node(p).ty == ty_mem()) { out.mem_phi = p; break; }
    }
    out.blocks.clear();
    out.body_nodes = 0;
    for (NodeId blk : l.blocks) {
        if (blk == header) continue;
        out.blocks.push_back(blk);
        for (NodeId q : g.uses_of(blk)) {
            if (g.node(q).in[0] == blk && !is_control_op(g.node(q).op))
                ++out.body_nodes;
        }
    }
    out.single_block = out.blocks.size() == 1;
    out.phis.clear();
    for (NodeId p : g.uses_of(header))
        if (g.node(p).op == Op::Phi) out.phis.push_back(p);
    return true;
}

// Counter-bump effect: Call{kFnPgoBump}, in = {ctrl, mem}, ival = index.
// The isel lowers it to one incq; the output memory version is only used
// when the caller rewires a phi input to it.
NodeId make_bump(Graph& g, NodeId pin, NodeId mem, u64 idx) {
    NodeId c = g.make(Op::Call, ty_mem(), {pin, mem}, 0, kFnPgoBump);
    g.node(c).ival = static_cast<i64>(idx);
    return c;
}

// ---- instrument mode --------------------------------------------------------

u32 instrument_fn(Graph& g, LoopInfo& li, u32 pair_base) {
    // enumerate BEFORE mutating: the use-mode build must derive the same
    // (fn, loop) -> index assignment from the identical pre-43 graph.
    std::vector<PgoLoop> loops;
    for (const Loop& l : li.loops()) {
        PgoLoop pl;
        if (match_pgo_loop(g, li, l, pl)) loops.push_back(pl);
    }
    u32 pairs = 0;
    for (PgoLoop& pl : loops) {
        u64 idx_e = 2ull * (pair_base + pairs);
        u64 idx_h = idx_e + 1;

        // ENTRY bump: pinned at the loop's entry predecessor control node
        // (a Jump heads the shim block before the header; a projection
        // heads the if-arm block that flows into the loop). Fires exactly
        // once per loop entry (per outer iteration when nested).
        NodeId entry_pred = g.node(pl.header).in[pl.entry_slot];
        NodeId e_mem = pl.mem_phi != kNoNode
                           ? g.node(pl.mem_phi).in[pl.entry_slot + 1]
                           : g.start(); // memory-free loop: root version
        NodeId bump_e = make_bump(g, entry_pred, e_mem, idx_e);
        if (pl.mem_phi != kNoNode)
            g.set_input(pl.mem_phi, pl.entry_slot + 1, bump_e);

        // HEADER bump: pinned at the guard's body projection (the loop-body
        // entry block — rotation-stable, executes once per iteration).
        // Memory chaining: when the phi has a real latch input the bump
        // takes its place (the phi's backedge reads the bump — no forks).
        // A SELF-LATCHING phi (read-only loop, `Phi{h, entry, phi}`) must
        // NOT be rewired — the bump would read the phi while the phi read
        // the bump (a cycle); the bump then reads the phi's version and
        // its output dangles (verifier-legal: effects need no users).
        // Memory-free loops chain off the entry bump.
        NodeId h_mem;
        bool rewire_latch = false;
        if (pl.mem_phi == kNoNode) {
            h_mem = bump_e;
        } else {
            NodeId latch_val = g.node(pl.mem_phi).in[pl.latch_slot + 1];
            if (latch_val == pl.mem_phi) {
                h_mem = pl.mem_phi; // self-latching: read, do not rewire
            } else {
                h_mem = latch_val;
                rewire_latch = true;
            }
        }
        NodeId bump_h = make_bump(g, pl.body_proj, h_mem, idx_h);
        if (rewire_latch) g.set_input(pl.mem_phi, pl.latch_slot + 1, bump_h);

        ++pairs;
    }
    return pairs;
}

// ---- use mode: the guarded-epilogue transform --------------------------------

// Split `while iv <rel> bound { body }` (dynamic bound) into the F-unrolled
// main loop with the last-copy guard + a fresh scalar epilogue:
//     [main: (iv + (F-1)*step) <rel> bound] body x F
//     [epi: iv2 <rel> bound] body-clone
//     post-loop (retargeted from the original exit/phis to the epilogue)
// Sound for every runtime trip: the main body only runs when all F scalar
// iterations would have; the epilogue handles the 0..F-1 remainder.
bool unroll_dynamic(Graph& g, const PgoLoop& pl, u32 f) {
    if (!pl.single_block || f < 2) return false;
    NodeId body = pl.body_proj;

    // body contents (non-control, pinned at the single body block)
    std::vector<NodeId> pinned;
    for (NodeId u : g.uses_of(body)) {
        if (g.is_dead(u) || g.node(u).in[0] != body) continue;
        if (is_control_op(g.node(u).op)) continue;
        pinned.push_back(u);
    }

    // ---- (a) the epilogue loop, entered from the original exit ----
    NodeId h2 = g.make(Op::Region, ty_ctrl(), {pl.exit_proj});

    // fresh phis; entry input = the ORIGINAL phi (its value at main-exit:
    // the header dominates the exit, so the read is legal there)
    FlatMap<NodeId, NodeId> phi2; // orig header phi -> epilogue phi
    for (NodeId p : pl.phis) {
        NodeId np = g.make(Op::Phi, g.node(p).ty, {h2, p});
        phi2.insert(p, np);
    }
    NodeId iv2 = *phi2.find(pl.iv_phi);

    // guard: same relation (canonicalized Lt/Le) and bound node, the fresh
    // IV on the lhs. The bound node dominates the original header, hence
    // the exit, hence h2.
    NodeId cmp2 = g.make(Op::Cmp, ty_i1(), {h2, iv2, pl.bound},
                         static_cast<u8>(pl.rel));
    NodeId if2 = g.make(Op::If, ty_ctrl(), {h2, cmp2});
    NodeId body2 = g.make(g.node(pl.body_proj).op, ty_ctrl(), {if2});
    NodeId exit2 = g.make(g.node(pl.exit_proj).op, ty_ctrl(), {if2});
    g.append_input(h2, body2); // latch pred: entry slot 0, latch slot 1

    // clone the body subtree into body2 (dependency order; header phis
    // remap to the epilogue's phis, loop-external leaves are reused —
    // they dominate the original header and therefore the epilogue too)
    FlatMap<NodeId, NodeId> vmap; // orig body node -> clone
    auto remap = [&](NodeId n) -> NodeId {
        if (n == kNoNode) return kNoNode;
        if (const NodeId* m = phi2.find(n)) return *m;
        if (const NodeId* m = vmap.find(n)) return *m;
        return n; // external leaf: reuse
    };
    FlatMap<NodeId, bool> done;
    bool progress = true;
    while (progress && done.size() < pinned.size()) {
        progress = false;
        for (NodeId u : pinned) {
            if (done.contains(u)) continue;
            const Node& un = g.node(u);
            bool ready = true;
            for (u8 i = 1; i < un.n_in; ++i) {
                NodeId d = un.in[i];
                if (d == kNoNode || d == u) continue;
                if (g.node(d).in[0] == body && !phi2.contains(d) &&
                    !done.contains(d)) {
                    ready = false; // in-body input not cloned yet
                    break;
                }
            }
            if (!ready) continue;
            Node un_copy = un; // copy: make_arr grows the node vector
            NodeId ins[kMaxInputs];
            ins[0] = body2;
            for (u8 i = 1; i < un_copy.n_in; ++i) ins[i] = remap(un_copy.in[i]);
            NodeId c = g.make_arr(un_copy.op, un_copy.ty, ins, un_copy.n_in,
                                  un_copy.sub, un_copy.aux);
            g.node(c).ival = un_copy.ival;
            g.node(c).fval = un_copy.fval;
            g.node(c).flags = un_copy.flags;
            vmap.insert(u, c);
            done.insert(u, true);
            progress = true;
        }
    }
    if (done.size() < pinned.size()) return false; // dependency cycle: bail

    // phi backedges: the ORIGINAL latch input, remapped (in-body updates
    // resolve to their clones; external latch values reuse as-is)
    for (NodeId p : pl.phis) {
        NodeId latch_val = g.node(p).in[pl.latch_slot + 1];
        NodeId nv = remap(latch_val);
        if (nv == kNoNode) return false;
        g.append_input(*phi2.find(p), nv);
    }

    // post-loop control: everything after the original exit moves after the
    // EPILOGUE's exit (the epilogue runs first); merge regions retarget
    // their predecessor slot.
    for (NodeId u : g.uses_of(pl.exit_proj)) {
        if (g.is_dead(u) || u == h2) continue;
        Node& un = g.node(u);
        if (un.op == Op::Region) {
            for (u8 k = 0; k < un.n_in; ++k)
                if (un.in[k] == pl.exit_proj) g.set_input(u, k, exit2);
            continue;
        }
        if (un.in[0] == pl.exit_proj) g.set_input(u, 0, exit2);
    }

    // post-loop VALUE users of the original phis read the epilogue's phis
    // (they hold the entry value when the epilogue never iterates). In-loop
    // users (the main loop's guard/updates) and the epilogue's own phi
    // entry inputs keep the originals.
    for (NodeId p : pl.phis) {
        NodeId target = *phi2.find(p);
        const SmallVec<NodeId, 4> users = g.uses_of(p);
        for (NodeId u : users) {
            if (g.is_dead(u)) continue;
            const Node& un = g.node(u);
            if (un.op == Op::Phi && un.in[0] == h2) continue; // entry inputs
            NodeId pin = un.in[0];
            bool in_orig = pin == pl.header;
            if (!in_orig)
                for (NodeId blk : pl.blocks)
                    if (blk == pin) { in_orig = true; break; }
            if (in_orig) continue;
            for (u8 k = 1; k < un.n_in; ++k)
                if (un.in[k] == p) g.set_input(u, k, target);
        }
    }

    // ---- (b) unroll the original body f-1 copies (shared cloner) ----
    loopx::CountedLoop cl;
    cl.header = pl.header;
    cl.entry_slot = pl.entry_slot;
    cl.latch_slot = pl.latch_slot;
    cl.guard_if = pl.guard_if;
    cl.body_proj = pl.body_proj;
    cl.exit_proj = pl.exit_proj;
    cl.iv_phi = pl.iv_phi;
    cl.iv_step = pl.iv_step;
    cl.trip = -1; // dynamic
    cl.blocks = pl.blocks;
    cl.body_nodes = pl.body_nodes;
    cl.phis = pl.phis;
    if (loopx::clone_body_chain(g, cl, f - 1, false) == 0) return false;

    // ---- (c) the last-copy guard: (iv + (F-1)*step) <rel> bound ----
    // The F-th scalar iteration would test `iv + K <rel> bound`; running
    // the unrolled body exactly when that test passes keeps every original
    // iteration executing exactly once across main + epilogue.
    i64 K = static_cast<i64>(f - 1) * pl.iv_step;
    TypeId iv_ty = g.node(pl.iv_phi).ty;
    if (ty_bits(iv_ty) <= 32 && (K > (1 << 29) || K < -(1 << 29))) return false;
    if (K <= 0) return false;
    NodeId kconst = g.make(Op::Const, iv_ty, {pl.header});
    g.node(kconst).ival = K;
    NodeId addk = g.make(Op::Bin, iv_ty, {pl.header, pl.iv_phi, kconst},
                         static_cast<u8>(BinOp::Add));
    NodeId ncmp = g.make(Op::Cmp, ty_i1(), {pl.header, addk, pl.bound},
                         static_cast<u8>(pl.rel));
    g.set_input(pl.guard_if, 1, ncmp);

    g.touch();
    return true;
}

} // namespace

class ProfileGuidedUnrollingPass : public Pass {
public:
    const char* name() const override { return "ProfileGuidedUnrolling"; }
    int order() const override { return 43; }
    const char* phase_name() const override {
        return "Phase 4: Loop Analysis & Transforms";
    }
    ModeMask modes() const override { return kModeAOT; } // AOT: the counter
    // dump rides the ELF .init_array / atexit machinery
    AnalysisMask required() const override {
        return AnalysisKind::LoopInfo | AnalysisKind::Dominators;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::LoopInfo | AnalysisKind::Dominators |
               AnalysisKind::AliasInfo | AnalysisKind::MemDep;
    }
    bool run(PassContext& ctx) override {
        if (ctx.opts.pgo == PgoMode::Instrument) return instrument(ctx);
        if (ctx.opts.pgo == PgoMode::Use) return use(ctx);
        return false; // off / sample / live: honest no-op
    }

private:
    static bool instrument(PassContext& ctx) {
        bool changed = false;
        u32 pair = 0;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Graph& g = fg.g;
            LoopInfo& li = ctx.analysis.loops(fg);
            u32 pairs = instrument_fn(g, li, pair);
            pair += pairs;
            if (pairs > 0) {
                changed = true;
                // Instrumented functions stay OUTLINED: the inliner's clone
                // is demand-driven from the callee's Return (it clones the
                // control chain and the return's data/memory closure), so a
                // dangling counter bump would be DROPPED from the inlined
                // copy and the counts would silently under-report (the same
                // policy pass 56 applies to vectorized functions; outlined
                // bodies keep loop identity, and trip counts are inlining-
                // invariant). Counter indices are assigned at THIS slot —
                // before the inliner runs — so the use build (no bumps, no
                // marking) derives the identical enumeration.
                fg.no_inline = true;
            }
        }
        return changed;
    }

    static bool use(PassContext& ctx) {
        const std::vector<u64>& cnt = ctx.opts.pgo_counters;
        if (cnt.empty()) return false;
        u32 budget = level_budgets(ctx.opts.level).unroll_factor;
        if (budget < 2) return false; // -O1/-Os/-Oz: no unrolling budget
        bool changed = false;
        u32 pair = 0;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Graph& g = fg.g;
            LoopInfo& li = ctx.analysis.loops(fg);
            // enumerate once with the SAME pre-mutation order the
            // instrument build used, then transform eligible loops
            std::vector<PgoLoop> loops;
            for (const Loop& l : li.loops()) {
                PgoLoop pl;
                if (match_pgo_loop(g, li, l, pl)) loops.push_back(pl);
            }
            for (size_t i = 0; i < loops.size(); ++i) {
                u32 idx = pair + static_cast<u32>(i);
                if (2ull * idx + 1 >= cnt.size()) break; // profile mismatch
                u64 entries = cnt[2 * idx];
                u64 heads = cnt[2 * idx + 1];
                if (entries == 0 || heads == 0) continue;     // never ran
                if (heads < 2 * entries) continue;            // avg trip < 2
                if (!loops[i].single_block) continue;         // v1 family
                if (loops[i].iv_step > (1 << 27)) continue;   // K overflow guard
                u64 avg = heads / entries;
                u32 f = 2;
                while (static_cast<u64>(f) * 2 <= avg && f * 2 <= budget) f *= 2;
                if (loops[i].body_nodes * f > 240) continue;  // size guard
                if (unroll_dynamic(g, loops[i], f)) changed = true;
                // NOTE: no break — every enumerated loop gets its shot. The
                // epilogue loops created here are NOT re-enumerated (their
                // profile pairs do not exist; they run 0..F-1 times).
            }
            pair += static_cast<u32>(loops.size());
        }
        return changed;
    }
};

JULES_REGISTER_PASS(ProfileGuidedUnrollingPass, 43, "Phase 4")

} // namespace jules
