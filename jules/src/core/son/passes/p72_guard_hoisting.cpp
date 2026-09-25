// Pass 72 — GuardHoisting (Phase 6: Devirtualization & Speculation)
//
// "LICM over guard nodes": a deopt guard whose condition is
// loop-invariant is checked every iteration but can only ever take the
// same arm — the check belongs at the loop entry. The transform is LOOP
// VERSIONING (the full machinery below):
//
//     pre: [P] -> [H: loop guard][body ... latch]      (ladder guard G)
//     new: [P] -> [G2 = If(P, cond)] --true-->  [L' = clone of L]
//                        \----false----------->  [L  = original]
//          L' resolves its in-loop copy of G to Const(true)  (deepest rung)
//          L  resolves G to Const(false)                     (generic rung)
//          both exits merge at a new Region with phis for the live-outs.
//
// The const-resolved ladders are then pruned by the cleanup sweep's SCCP
// (kill the dead projection, trim the merge preds, realign the phis,
// cascade the dead rung) — the arm-split elimination. The versioner's
// wiring contract: the clone's pin slots deferral-remap exactly like
// value slots (a forward-referencing block clone must not stay wired
// into the original loop), and the exit merge keeps its OWN pred slots
// (the after-loop re-pin walk must skip it — re-pointing them created a
// self-loop). The live-out set includes the HEADER PHIS: they are the
// loop-carried state post-loop code reads (dominance forbids body defs
// from escaping), and every merge phi pairs the original's def with the
// clone's on the two exit edges.
//
// Both fixed-shape hazards found by the t48 round: the resolved-false
// guard's dead true-arm met the OPTIMISTIC loop-entry IV value through
// its range guards and turned them "executable" (SCCP now gates the If
// arm marking on the If's OWN block executability — the classic rule
// that instructions only evaluate in executable blocks).
#include "core/son/passes/pass_utils.h"

#include <algorithm>
#include <vector>
#include <cstdlib>

namespace jules {

namespace {

bool cone_pure_op(Op o) {
    switch (o) {
        case Op::Const: case Op::Param: case Op::Bin: case Op::Cmp:
        case Op::Un: case Op::Cast: case Op::Select:
            return true;
        default:
            return false;
    }
}

// Control cone of the guard (its ladder): every block reachable from the
// guard's projections WITHOUT passing through the loop header (the
// backedge). The guard sits inside the loop and a ladder has no early
// exits — every arm merges back — so the reachable set is exactly this
// iteration's ladder, and it must lie entirely inside the loop for
// versioning to clone it whole. Reaching any block outside the loop
// rejects the site.
bool ladder_cone_inside(Graph& g, NodeId guard_if,
                        const FlatMap<NodeId, bool>& in_loop,
                        NodeId header) {
    std::vector<NodeId> cone;
    FlatMap<NodeId, bool> seen;
    for (NodeId u : g.uses_of(guard_if)) {
        const Node& un = g.node(u);
        if (un.op == Op::IfTrue || un.op == Op::IfFalse) {
            cone.push_back(u);
            seen.insert(u, true);
        }
    }
    if (cone.size() != 2) return false;
    for (size_t i = 0; i < cone.size(); ++i) {
        NodeId b = cone[i];
        if (b == header) continue; // backedge to the header: stop path
        for (NodeId u : g.uses_of(b)) {
            const Node& un = g.node(u);
            if (un.op == Op::Dead) continue;
            if (un.op == Op::If) {
                // an If is not a block head: its PROJECTIONS are the
                // successors
                if (un.in[0] != b) continue;
                for (NodeId p : g.uses_of(u)) {
                    const Node& pn = g.node(p);
                    if (pn.op != Op::IfTrue && pn.op != Op::IfFalse) continue;
                    if (p == header) continue;
                    if (!in_loop.contains(p)) return false; // left the loop
                    if (seen.contains(p)) continue;
                    seen.insert(p, true);
                    cone.push_back(p);
                }
                continue;
            }
            if (un.op == Op::IfTrue || un.op == Op::IfFalse ||
                un.op == Op::Jump) {
                if (un.in[0] != b) continue;
                if (!in_loop.contains(u)) return false; // left the loop
                if (seen.contains(u)) continue;
                if (u == header) continue; // loop backedge: stop path
                seen.insert(u, true);
                cone.push_back(u);
                continue;
            }
            if (un.op == Op::Region) {
                bool mine = false;
                for (u8 k = 0; k < un.n_in; ++k)
                    if (un.in[k] == b) mine = true;
                if (!mine) continue;
                if (u == header) continue; // loop backedge: stop path
                if (!in_loop.contains(u)) return false; // left the loop
                if (seen.contains(u)) continue;
                seen.insert(u, true);
                cone.push_back(u); // merges expand: their preds/users may
                                   // still be ladder internals
                continue;
            }
            // data users (calls, stores pinned at b): not control
        }
    }
    return true;
}

// Whole-loop versioner: clones the loop's blocks and contents under an
// original-id-keyed map, with deferred patching for in-copy backedge
// references (a clone swept early may reference an original that a later
// sweep will clone — the same discipline loopx::Cloner applies).
struct LoopVersioner {
    Graph& g;
    FlatMap<NodeId, NodeId> vmap;   // value/content node -> clone
    FlatMap<NodeId, NodeId> blkmap; // block head -> clone head
    const FlatMap<NodeId, bool>* in_body = nullptr;

    struct Deferred {
        NodeId node;
        u8 slot;
        NodeId orig;
    };
    std::vector<Deferred> deferred;

    NodeId remap(NodeId n) const {
        if (n == kNoNode) return kNoNode;
        if (const NodeId* m = vmap.find(n)) return *m;
        if (const NodeId* m = blkmap.find(n)) return *m;
        return n; // invariant/external: shared original
    }

    NodeId clone_node(const Node& un, NodeId pin) {
        NodeId ins[kMaxInputs];
        ins[0] = pin;
        size_t mark = deferred.size(); // records pushed by THIS clone
        // The pin slot follows the SAME deferral protocol as the value
        // slots (the pre-pass comment's contract, now actually true): the
        // pre-pass pins If clones at their ORIGINAL block, and a forward
        // block reference (a Jump/Region clone whose pred sweep has not
        // run yet) remaps to itself — both must patch onto the clone in
        // the post-sweep pass. Without this, every cloned control node
        // stays wired into the ORIGINAL loop (observed on t48: verifier
        // "dead predecessor" cascade + wrong-code at the default -O2).
        if (un.in[0] != kNoNode && ins[0] == un.in[0] &&
            in_body->contains(un.in[0]))
            deferred.push_back(Deferred{0, 0, un.in[0]});
        for (u8 i = 1; i < un.n_in; ++i) {
            NodeId o = un.in[i];
            ins[i] = remap(o);
            // an in-body original that remapped to itself is a forward or
            // backedge reference within the copy: defer, patch post-sweep
            if (o != kNoNode && ins[i] == o && in_body->contains(o))
                deferred.push_back(Deferred{0, i, o});
        }
        NodeId c = g.make_arr(un.op, un.ty, ins, un.n_in, un.sub, un.aux);
        Node& cn = g.node(c);
        cn.ival = un.ival;
        cn.fval = un.fval;
        cn.flags = un.flags;
        if (un.op == Op::Return) g.append_input(g.stop(), c);
        for (size_t d = mark; d < deferred.size(); ++d)
            deferred[d].node = c; // bind the records to the real clone
        return c;
    }

    void clone_block_contents(NodeId old_blk, NodeId new_blk) {
        const SmallVec<NodeId, 4> users = g.uses_of(old_blk);
        for (NodeId u : users) {
            if (blkmap.contains(u) || vmap.contains(u)) continue;
            Node un = g.node(u); // copy: make_arr reallocs
            if (un.in[0] != old_blk) continue;
            // heads (Jump/projections) are cloned via blkmap in the sweep;
            // Ifs ARE block contents — but the PRE-PASS already cloned
            // every If pinned at a loop block (they sit in vmap as KEYS,
            // so originals are skipped above). The only Ifs reaching this
            // point are the PRE-PASS CLONES THEMSELVES, still pinned at
            // their original block until the deferred patch — cloning them
            // again produced kFlagGuardSite-carrying orphans that p74
            // would resolve pointlessly (31-a review finding F2).
            if (un.op == Op::Jump || un.op == Op::IfTrue ||
                un.op == Op::IfFalse || un.op == Op::Region ||
                un.op == Op::If)
                continue;
            // Phis included: a join phi inside the body remaps uniformly
            // (region input = the cloned block; value inputs remap with
            // the same deferral). Header phis were handled by the caller.
            NodeId c = clone_node(un, new_blk);
            vmap.insert(u, c);
        }
    }
};

} // namespace

class GuardHoistingPass : public Pass {
public:
    const char* name() const override { return "GuardHoisting"; }
    int order() const override { return 72; }
    const char* phase_name() const override {
        return "Phase 6: Devirtualization & Speculation";
    }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo |
               AnalysisKind::AliasInfo | AnalysisKind::MemDep |
               AnalysisKind::CallGraph;
    }
    ModeMask modes() const override {
        // The versioner duplicates the whole loop — the same cloning cost
        // class as the ladder creator (91) whose guards it hoists. The
        // baseline JIT tier must not pay for speculation machinery.
        return kModeAOT | kModeJitOptimizing;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            for (int round = 0; round < 4; ++round) {
                auto li = LoopInfo::compute(fg.g, ctx.analysis.doms(fg));
                bool this_round = false;
                for (const Loop& l : li->loops()) {
                    if (hoist_one(fg, *li, l)) {
                        this_round = true;
                        break; // CFG changed: recompute
                    }
                }
                changed |= this_round;
                if (!this_round) break;
            }
        }
        return changed;
    }

private:
    static bool hoist_one(FunctionGraph& fg, LoopInfo& li, const Loop& l) {
        Graph& g = fg.g;
        (void)fg;

        FlatMap<NodeId, bool> in_loop;
        for (NodeId blk : l.blocks) in_loop.insert(blk, true);

        // ---- find a hoistable guard: a Cmp guard whose condition cone
        // is loop-invariant. Deeper guards (Range hinges on per-iteration
        // values) legitimately re-check every iteration and stay put.
        // Several invariant guards hoist one per round (the outer loop
        // recomputes).
        NodeId guard = kNoNode, cmp = kNoNode, B = kNoNode;
        std::vector<NodeId> probe;
        for (NodeId blk : l.blocks) {
            if (guard != kNoNode) break;
            for (NodeId u : g.uses_of(blk)) {
                Node un = g.node(u);
                if (un.op == Op::Dead) continue;
                if (un.op != Op::If || (un.flags & kFlagGuardSite) == 0 ||
                    un.in[0] != blk)
                    continue;
                Node uc = g.node(u);
                if (uc.n_in < 2 || g.node(uc.in[1]).op != Op::Cmp) continue;
                // invariance pre-test: every cone member pure + leaves out
                probe.clear();
                bool inv = true;
                {
                    std::vector<NodeId> work{uc.in[1]};
                    while (!work.empty()) {
                        NodeId n = work.back();
                        work.pop_back();
                        const Node& nn = g.node(n);
                        NodeId pin = nn.n_in > 0 ? nn.in[0] : kNoNode;
                        // invariance is about the node's POSITION: a node
                        // pinned outside the loop is a leaf (invariant by
                        // dominance), whatever op it is; a node pinned
                        // INSIDE must be pure so it can be repinned to the
                        // preheader. (The block SET holds block heads —
                        // data nodes are tested through their pin.)
                        if (pin == kNoNode || !in_loop.contains(pin))
                            continue;
                        if (!cone_pure_op(nn.op)) {
                            inv = false;
                            break;
                        }
                        probe.push_back(n);
                        for (u8 k = 1; k < nn.n_in; ++k)
                            if (nn.in[k] != kNoNode) work.push_back(nn.in[k]);
                    }
                }
                if (!inv || probe.empty()) { continue; }
                guard = u;
                cmp = uc.in[1];
                B = blk;
                break;
            }
        }
        if (guard == kNoNode) return false;

        // ---- condition cone (from the probe above; same set) ----------
        std::vector<NodeId> cone = probe;

        // ---- ladder containment ----------------------------------------
        if (!ladder_cone_inside(g, guard, in_loop, l.header)) {
 return false; }

        // ---- header shape: 2-pred Region, unique entry, preheader ------
        Node hc = g.node(l.header);
        if (hc.op != Op::Region || hc.n_in != 2) return false;
        u8 entry_slot = 255;
        {
            u32 outside = 0;
            for (u8 k = 0; k < 2; ++k)
                if (!in_loop.contains(hc.in[k])) {
                    ++outside;
                    entry_slot = k;
                }
            if (outside != 1) {
 return false; }
        }
        NodeId P = li.preheader(l.header);
        if (P == kNoNode) {
 return false; }

        // ---- the loop's own guard If and its single exit projection ----
        NodeId loop_if = kNoNode;
        for (NodeId u : g.uses_of(l.header)) {
            Node un = g.node(u);
            if (un.op == Op::If && un.in[0] == l.header &&
                (un.flags & kFlagGuardSite) == 0) {
                if (loop_if != kNoNode) {
 return false; }
                loop_if = u;
            }
        }
        if (loop_if == kNoNode) {
 return false; }
        NodeId exit_proj = kNoNode;
        for (NodeId u : g.uses_of(loop_if)) {
            Node un = g.node(u);
            if (un.op == Op::IfTrue || un.op == Op::IfFalse) {
                if (!in_loop.contains(u)) {
                    if (exit_proj != kNoNode) {
 return false; }
                    exit_proj = u;
                }
            }
        }
        if (exit_proj == kNoNode) {
 return false; }

        // no early exits: no other block's control leaves the loop, and no
        // return/terminator fires inside the body (each is an exit path)
        for (NodeId blk : l.blocks) {
            for (NodeId u : g.uses_of(blk)) {
                Node un = g.node(u);
                if (un.op == Op::Dead) continue;
                if (un.op == Op::Return && un.in[0] == blk) {
 return false; }
                if ((un.op == Op::IfTrue || un.op == Op::IfFalse ||
                     un.op == Op::Jump) &&
                    !in_loop.contains(u) && u != exit_proj) {
                   
                    return false;
                }
                if (un.op == Op::Region && !in_loop.contains(u)) {
                    for (u8 k = 0; k < un.n_in; ++k)
                        if (un.in[k] == blk) {
 return false; }
                }
            }
        }

        // size guard: versioning duplicates the whole loop
        u32 body_nodes = 0;
        for (NodeId blk : l.blocks)
            for (NodeId u : g.uses_of(blk))
                if (g.node(u).in[0] == blk && !is_control_op(g.node(u).op))
                    ++body_nodes;
        if (body_nodes == 0 || body_nodes > 200) {
 return false; }

        // ---- header phis + live-outs (collected before any edit) -------
        std::vector<NodeId> phis;
        for (NodeId u : g.uses_of(l.header)) {
            Node un = g.node(u);
            if (un.op == Op::Phi && un.in[0] == l.header) {
                if (un.n_in != 3) {
 return false; }
                phis.push_back(u);
            }
        }
        std::vector<NodeId> live_out;
        for (NodeId blk : l.blocks) {
            for (NodeId u : g.uses_of(blk)) {
                Node un = g.node(u);
                if (un.in[0] != blk) continue;
                if (is_control_op(un.op)) continue;
                // Phis ARE live-out candidates — the HEADER phis are the
                // loop-carried state, and they are exactly what post-loop
                // code reads (dominance forbids reading body defs after
                // the loop: the exit path skips the body). Excluding them
                // left `print(s)` reading the ORIGINAL loop's phi on the
                // clone path — stale stack state, wrong output (t48).
                for (NodeId w : g.uses_of(u)) {
                    Node wn = g.node(w);
                    if (wn.op == Op::Dead) continue;
                    NodeId wpin = wn.in[0];
                    if (wn.op == Op::Phi) wpin = wn.in[0]; // its Region
                    if (!in_loop.contains(wpin) && wpin != l.header) {
                        live_out.push_back(u);
                        break;
                    }
                }
            }
        }

        // ================= the versioning (edits begin) ==================

        // repin the condition cone to the preheader (pure nodes)
        for (NodeId n : cone) g.set_input(n, 0, P);

        NodeId g2 = g.make(Op::If, ty_ctrl(), {P, cmp});
        g.node(g2).flags |= kFlagGuardSite;
        NodeId t2 = g.make(Op::IfTrue, ty_ctrl(), {g2});
        NodeId f2 = g.make(Op::IfFalse, ty_ctrl(), {g2});

        LoopVersioner v{g, {}, {}, nullptr, {}};
        FlatMap<NodeId, bool> in_body;
        for (NodeId blk : l.blocks) {
            in_body.insert(blk, true);
            for (NodeId u : g.uses_of(blk))
                if (g.node(u).in[0] == blk) in_body.insert(u, true);
        }
        v.in_body = &in_body;

        // Pre-pass: clone every If pinned inside the loop (block CONTENT —
        // the loop guard, the ladder guards, internal branches). Their
        // projections then remap onto the clones during the block sweep.
        // The pin slot is deferred: the clone's block may not exist yet.
        for (NodeId blk : l.blocks) {
            const SmallVec<NodeId, 4> users = g.uses_of(blk);
            for (NodeId u : users) {
                if (v.vmap.contains(u)) continue;
                Node un = g.node(u);
                if (un.in[0] != blk || un.op != Op::If) continue;
                NodeId c = v.clone_node(un, blk);
                v.vmap.insert(u, c);
            }
        }
        // Invariant proven by the pre-pass above: loop_if is an If pinned
        // at a loop block (checked at discovery), so its clone exists NOW —
        // no structural edit has happened yet. The exit merge below depends
        // on it; bailing after the sweep would strand a half-versioned graph.
        if (!v.vmap.contains(loop_if)) return false;

        // header clone: Region entered from T2; latch pred patched after
        u8 latch_slot = entry_slot == 0 ? 1 : 0;
        NodeId h2;
        {
            NodeId hins[2] = {t2, hc.in[latch_slot]};
            h2 = g.make_arr(Op::Region, ty_ctrl(), hins, 2);
            v.blkmap.insert(l.header, h2);
            v.deferred.push_back(
                LoopVersioner::Deferred{h2, latch_slot, hc.in[latch_slot]});
        }

        // header phi clones: entry values shared, latch values deferred
        for (NodeId phi : phis) {
            Node pc = g.node(phi);
            NodeId entry_val = pc.in[entry_slot + 1];
            NodeId latch_val = pc.in[latch_slot + 1];
            NodeId pins[3] = {*v.blkmap.find(l.header), entry_val, latch_val};
            NodeId p2 = g.make_arr(Op::Phi, pc.ty, pins, 3);
            v.vmap.insert(phi, p2);
            v.deferred.push_back(
                LoopVersioner::Deferred{p2, static_cast<u8>(latch_slot + 1),
                                        latch_val});
        }

        // body blocks: heads then contents (deferral covers cross refs)
        for (NodeId blk : l.blocks) {
            if (blk == l.header) {
                v.clone_block_contents(blk, h2);
                continue;
            }
            Node bn = g.node(blk);
            if (bn.op == Op::IfTrue || bn.op == Op::IfFalse) {
                NodeId cif = v.remap(bn.in[0]);
                NodeId nb = g.make(bn.op, ty_ctrl(), {cif});
                v.blkmap.insert(blk, nb);
            } else {
                NodeId pin = v.remap(bn.in[0]);
                NodeId nb = v.clone_node(bn, pin);
                v.blkmap.insert(blk, nb);
            }
            v.clone_block_contents(blk, *v.blkmap.find(blk));
        }

        // patch deferred in-copy references onto the clones
        for (const LoopVersioner::Deferred& d : v.deferred) {
            NodeId m = v.remap(d.orig);
            if (m != d.orig) g.set_input(d.node, d.slot, m);
        }

        // ---- resolve the two guards ------------------------------------
        {
            NodeId fc = g.make(Op::Const, ty_i1(), {B});
            g.node(fc).ival = 0;
            g.set_input(guard, 1, fc); // original: generic rung
        }
        {
            const NodeId* gc = v.vmap.find(guard);
            const NodeId* b2 = v.blkmap.find(B);
            if (gc && b2) {
                NodeId tc = g.make(Op::Const, ty_i1(), {*b2});
                g.node(tc).ival = 1;
                g.set_input(*gc, 1, tc); // clone: deepest rung
            }
        }

        // ---- the original loop now enters from F2 ----------------------
        g.set_input(l.header, entry_slot, f2);

        // ---- exit merge: Region + live-out phis + after-loop re-pins ----
        const NodeId* lif_c = v.vmap.find(loop_if);
        if (!lif_c) {
            return false; // unreachable: the invariant check above
        }
        NodeId exit2 = g.make(g.node(exit_proj).op, ty_ctrl(), {*lif_c});
        NodeId r;
        {
            NodeId rins[2] = {exit_proj, exit2};
            r = g.make_arr(Op::Region, ty_ctrl(), rins, 2);
        }

        for (NodeId n : live_out) {
            Node nc = g.node(n);
            const NodeId* nc2 = v.vmap.find(n);
            if (!nc2) continue;
            NodeId pins[3] = {r, n, *nc2};
            NodeId pv = g.make_arr(Op::Phi, nc.ty, pins, 3);
            const SmallVec<NodeId, 4> nusers = g.uses_of(n);
            for (NodeId w : nusers) {
                if (w == pv) continue;
                Node wn = g.node(w);
                if (wn.op == Op::Dead) continue;
                NodeId wpin = wn.in[0];
                if (wn.op == Op::Phi) wpin = wn.in[0]; // its Region
                if (!in_loop.contains(wpin) && wpin != l.header) {
                    for (u8 k = 0; k < wn.n_in; ++k)
                        if (wn.in[k] == n) g.set_input(w, k, pv);
                }
            }
        }
        // after-loop code pinned at exit_proj moves to R (control first:
        // data re-pins must not orphan the use list being walked). R ITSELF
        // is a Region user of exit_proj — skipping it keeps its own pred
        // slots intact (re-pointing them to R created a self-loop,
        // observed as n201: Region in=[n201, n200]).
        {
            const SmallVec<NodeId, 4> users = g.uses_of(exit_proj);
            for (NodeId u : users) {
                if (u == r) continue; // R's own pred slots are structural
                Node un = g.node(u);
                if (un.op == Op::Dead) continue;
                if (un.op == Op::Jump || un.op == Op::If || un.op == Op::Return) {
                    if (un.in[0] == exit_proj) g.set_input(u, 0, r);
                } else if (un.op == Op::Region) {
                    for (u8 k = 0; k < un.n_in; ++k)
                        if (un.in[k] == exit_proj) g.set_input(u, k, r);
                }
            }
            const SmallVec<NodeId, 4> users2 = g.uses_of(exit_proj);
            for (NodeId u : users2) {
                Node un = g.node(u);
                if (un.op == Op::Dead) continue;
                if (!is_control_op(un.op) && un.op != Op::Phi &&
                    un.in[0] == exit_proj)
                    g.set_input(u, 0, r);
            }
        }

        g.mark_uses_dirty();
        g.touch();
       
        return true;
    }
};

JULES_REGISTER_PASS(GuardHoistingPass, 72, "Phase 6")

} // namespace jules
