// Pass 33 — PartialEscapeAnalysis (Phase 3: Escape & Allocation Analysis)
//
// Path-sensitive escape refinement: pass 32's classification is
// flow-insensitive (one verdict per allocation over the whole function);
// this pass refines along control paths and performs the transform the
// refinement enables — ALLOCATION SINKING:
//
//   An allocation whose EVERY user (value uses, memory-chain uses, and
//   effect uses alike) is pinned inside ONE arm of a branch diamond is
//   sunk into that arm. The allocation cost is then paid only when the
//   arm executes; the not-taken path never calls malloc.
//
// Soundness of the sink (no materialization needed): the sunk Alloc's
// pin becomes the arm's entry projection, so every user keeps a
// dominating definition; its memory input (a version produced BEFORE
// the branch — necessarily dominating the arm) is unchanged; and the
// memory-chain users (the stores through the pointer, its free) all
// live in the arm and keep reading the Alloc as their version. A user
// outside the arm would have blocked the sink — the check walks the
// user set exactly, through phis (a phi input counts as a use at the
// phi's pin).
//
// Profile integration (--pgo=use, pass 34's counters): allocation sites
// whose runtime counters show escapes == 0 with allocs above the sample
// threshold are reported as RUNTIME-CONFIRMED non-escaping and sink
// FIRST when the static shape allows — the profile prioritizes, it
// never licenses: promoting on "never observed escaping" alone would be
// unguarded speculation, so a statically-escaping site stays untouched
// no matter what the counters say.
//
// Honesty: the classic virtual-object PEA (objects kept scalar on hot
// paths, materialized at branch joins, with recipes in the deopt
// metadata) needs an IR extension this milestone does not have; pass 35
// carries the manifest side of that design for the sunk allocations
// this pass creates. The sinking here is the sound subset.
#include "core/son/passes/pe/pe.h"

#include <algorithm>

namespace jules {

namespace {

// Which single arm-subtree, if any, contains every user of `alloc`?
// Returns the arm's entry projection (IfTrue/IfFalse of some If), or
// kNoNode when the users straddle arms or live outside every arm.
// MEMORY-version users are handled by the caller (they re-thread onto
// the alloc's incoming version), so they are pre-filtered here.
NodeId single_arm_of(Graph& g, DomTree& dom, NodeId /*alloc*/,
                     const std::vector<NodeId>& value_users) {
    NodeId arm = kNoNode;
    for (NodeId u : value_users) {
        const Node& un = g.node(u);
        if (un.op == Op::Dead) continue;
        NodeId pin = un.in[0];
        if (un.op == Op::Phi) {
            // a phi input is a use at the phi's pin (the merge); the
            // merge is outside any single arm by construction
            return kNoNode;
        }
        if (pin == kNoNode) return kNoNode;
        // walk up the dominator chain: the first If-projection ancestor
        // tells which arm's subtree this user lives in (or the user is
        // above every branch — no sinkable arm)
        NodeId a = pin;
        NodeId found = kNoNode;
        while (a != kNoNode) {
            const Node& an = g.node(a);
            if (an.op == Op::IfTrue || an.op == Op::IfFalse) {
                found = a;
                break;
            }
            a = dom.idom(a);
        }
        if (found == kNoNode) return kNoNode; // user not inside any arm
        if (arm == kNoNode) arm = found;
        else if (arm != found) return kNoNode; // different arms
    }
    return arm;
}

} // namespace

class PartialEscapeAnalysisPass : public Pass {
public:
    const char* name() const override { return "PartialEscapeAnalysis"; }
    int order() const override { return 33; }
    const char* phase_name() const override {
        return "Phase 3: Escape & Allocation Analysis";
    }
    AnalysisMask required() const override {
        return static_cast<AnalysisMask>(AnalysisKind::Dominators);
    }
    AnalysisMask invalidated() const override {
        return static_cast<AnalysisMask>(AnalysisKind::AliasInfo |
                                         AnalysisKind::MemDep);
    }
    bool run(PassContext& ctx) override {
        // Profile priorities (pass 34's blocks): [.. size quintuple,
        // escapes, frees] per original-function alloc site. Sites are
        // keyed by (fn index, alloc node id) — re-derived the same way
        // the instrument build enumerated them (which now emits one
        // block per site).
        FlatMap<u64, u32> hot; // (fn idx << 32) | node id -> alloc count
        if (ctx.opts.pgo == PgoMode::Use && ctx.opts.orig_fn_count != 0) {
            const u64 base = pe_sketch_base(ctx.opts) +
                             5ull * pe_sketch_slots(ctx.mod, ctx.opts.orig_fn_count);
            const std::vector<u64>& cnt = ctx.opts.pgo_counters;
            // Missing counters read as zero: an absent slot is one the
            // instrument build never referenced (never fired), and the
            // .bss array is sized by the max referenced index — a site
            // without frees may legitimately have no slot-6 entry, and a
            // truncated profile degrades to "not hot", which is the
            // conservative direction (priorities only, never licensing).
            auto read_cnt = [&cnt](u64 idx) -> u64 {
                return idx < cnt.size() ? cnt[idx] : 0;
            };
            u32 j = 0;
            u32 upto = ctx.opts.orig_fn_count < ctx.mod.fns.size()
                           ? ctx.opts.orig_fn_count
                           : static_cast<u32>(ctx.mod.fns.size());
            for (u32 fi = 0; fi < upto; ++fi) {
                const Graph& g = ctx.mod.fns[fi].g;
                for (NodeId id = 0; id < g.size(); ++id) {
                    if (g.node(id).op != Op::Alloc) continue;
                    u64 i0 = base + 7ull * j;
                    u64 allocs = read_cnt(i0 + 1); // size-sketch total
                    u64 escapes = read_cnt(i0 + 5);
                    if (escapes == 0 && allocs >= kPeSketchMinSamples)
                        hot.insert((static_cast<u64>(fi) << 32) | id,
                                   static_cast<u32>(
                                       allocs > 0xFFFF ? 0xFFFF : allocs));
                    ++j;
                }
            }
        }

        bool changed = false;
        for (u32 fi = 0; fi < ctx.mod.fns.size(); ++fi) {
            FunctionGraph& fg = ctx.mod.fns[fi];
            Graph& g = fg.g;
            DomTree& dom = ctx.analysis.doms(fg);
            const u64 hot_key_base = static_cast<u64>(fi) << 32;

            // Sunk candidates: hot-profiled sites first (stable order:
            // count desc, then node id), then the rest.
            std::vector<NodeId> allocs;
            for (NodeId id = 0; id < g.size(); ++id)
                if (g.node(id).op == Op::Alloc) allocs.push_back(id);
            std::stable_sort(allocs.begin(), allocs.end(),
                             [&](NodeId a, NodeId b) {
                                 const u32* ha = hot.find(hot_key_base | a);
                                 const u32* hb = hot.find(hot_key_base | b);
                                 u32 va = ha ? *ha : 0, vb = hb ? *hb : 0;
                                 if (va != vb) return va > vb;
                                 return a < b;
                             });

            for (NodeId id : allocs) {
                Node nc = g.node(id);
                if (nc.op != Op::Alloc) continue; // sunk/merged in a prior iter
                if (nc.flags & kFlagStackPromoted) continue; // already frame-local

                // Split the users: VALUE uses (the pointer as data —
                // these decide the arm) and MEMORY-version uses (the
                // chain — re-threaded onto the incoming version below).
                // A VALUE phi merge is a real escape: reject.
                std::vector<NodeId> value_users;
                std::vector<std::pair<NodeId, u8>> mem_users;
                {
                    // The use list is PER-SLOT (a node referencing the alloc
                    // as version AND as address appears twice) — dedupe so
                    // each user is classified once; the mem-phi branch
                    // below records EVERY matching slot (a version can
                    // reach one merge on several predecessor edges, and
                    // each must re-thread).
                    std::vector<NodeId> live;
                    {
                        const SmallVec<NodeId, 4>& raw = g.uses_of(id);
                        live.assign(raw.begin(), raw.end());
                    }
                    std::sort(live.begin(), live.end());
                    live.erase(std::unique(live.begin(), live.end()), live.end());
                    for (NodeId u : live) {
                        Node un = g.node(u);
                        if (un.op == Op::Dead) continue;
                        if (un.op == Op::Phi) {
                            if (un.ty == ty_mem()) {
                                for (u8 k = 1; k < un.n_in; ++k)
                                    if (un.in[k] == id) mem_users.push_back({u, k});
                            } else {
                                // value phi: a real escape — rejected by
                                // single_arm_of's phi rule below
                                value_users.push_back(u);
                            }
                            continue;
                        }
                        // in[1] is a memory-version input ONLY for the
                        // memory-consuming ops (Load/Store/Call/Return/
                        // Alloc); everywhere else — and in every slot >= 2 —
                        // a reference to the alloc is the POINTER AS DATA
                        // (store address, free/call argument, returned
                        // value, cast/base). A node that BOTH chains on the
                        // version AND consumes the pointer (Store: mem+addr;
                        // Call: mem+args) is classified by its DATA use:
                        // re-threading its version slot while its address
                        // still names the sunk alloc leaves a
                        // non-dominating pointer (wild store / wild free —
                        // regression-locked by t52_pea_store). The re-thread
                        // below then only ever rewrites the version slot of
                        // PURE chain users.
                        const bool mem_in1 = un.n_in > 1 && un.in[1] == id &&
                            (un.op == Op::Load || un.op == Op::Store ||
                             un.op == Op::Call || un.op == Op::Return ||
                             un.op == Op::Alloc);
                        bool data_use = !mem_in1;
                        for (u8 k = 2; k < un.n_in && !data_use; ++k)
                            if (un.in[k] == id) data_use = true;
                        if (data_use) {
                            value_users.push_back(u);
                        } else {
                            mem_users.push_back({u, 1}); // pure effect chaining
                        }
                    }
                }

                NodeId arm = single_arm_of(g, dom, id, value_users);
                if (arm == kNoNode) continue;
                // the arm's projection must be strictly dominated by the
                // alloc's CURRENT pin — sinking only ever moves deeper.
                if (!dom.dominates(nc.in[0], arm) || nc.in[0] == arm) continue;

                g.set_input(id, 0, arm); // re-pin into the arm
                g.node(id).flags |= kFlagMaterialized; // pass 35's recipe
                // memory-version users OUTSIDE the arm read the alloc as a
                // chain version: the sunk allocation's writes all happen
                // through the pointer inside the arm, so the version
                // flowing past the branch is the alloc's own INCOMING
                // version (nc.in[1]) — it dominates the original pin,
                // hence every outside reader. IN-ARM users keep the (now
                // sunk) alloc: their effect order (allocation before any
                // write through the pointer) is only preserved through it.
                for (const auto& mu : mem_users) {
                    Node& mn = g.node(mu.first);
                    if (mn.op == Op::Dead) continue;
                    if (dom.dominates(arm, mn.in[0])) continue; // in-arm
                    g.set_input(mu.first, mu.second, nc.in[1]);
                }
                g.mark_uses_dirty();
                g.touch();
                changed = true;
            }
        }
        return changed;
    }
};

JULES_REGISTER_PASS(PartialEscapeAnalysisPass, 33, "Phase 3")

} // namespace jules
