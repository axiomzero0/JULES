// Pass 34 — AllocationSiteProfiling (Phase 3: Escape & Allocation Analysis)
//
// Instruments allocation sites for the escape-analysis feedback loop
// (consumed by pass 33 in --pgo=use mode). Per ORIGINAL-function Alloc
// site (deterministic (fn, node-id) enumeration — variants appended by
// the PE family never shift indices), a 7-slot counter block:
//
//   [0..4]  size sketch — the kFnPgoSketch quintuple [first, total,
//           match, min, max] on the SIZE argument: total = allocation
//           count, min/max = the runtime size hull (dynamic
//           alloc(T, n) sites), first/match unused (the shape is
//           shared with pass 91's sketches so the serializer needs no
//           new expansion).
//   [5]     escape bumps — one kFnPgoBump inserted at every STATICALLY
//           identified escape EFFECT of the site's pointer: stored as a
//           value, passed as a call argument (callee may retain), or
//           returned. The counter fires exactly when an escaping path
//           executes at runtime.
//   [6]     free bumps — one per free() resolving to the site.
//
// Every alloc site in the original functions gets its block (at minimum
// the size sketch): the block index is the site's position in the (fn,
// node-id) alloc walk, and pass 33's use-mode reader derives the same
// index from the same walk — sites without statically-identified effects
// count allocations with their escape/free counters at zero.
//
// Counter layout in jules.prof (appended after the PE sketches):
//   [pass-43 loop pairs (2 each)]
//   [argument sketches (5 each)]
//   [allocation-site blocks (7 each)]
//
// Honesty: "escape" here is the STATIC effect-site classification —
// pointer arithmetic (Cast/Phi data flow that never reaches an effect)
// is not instrumentable without scheduling changes and is left to the
// static analysis; the counters refine, they do not replace it. In
// particular an escape whose pointer reaches the effect through a VALUE
// PHI (SROA-promoted loop-carried pointers — t36's shape) does not bump:
// the counter under-reports escapes in that direction, which only
// de-prioritizes a site pass 33's static arm-check would gate anyway.
// The profile is PROFITABILITY/CONFIDENCE data for pass 33: promoting on
// "never observed escaping" alone would be unguarded speculation.
#include "core/son/passes/pe/pe.h"

#include <algorithm>

namespace jules {

namespace {

constexpr u64 kAllocSiteSlots = 7; // size sketch (5) + escapes + frees

// Counter base of the allocation-site region (after loop pairs + arg
// sketches). Both the instrument and use builds derive this identically.
inline u64 alloc_site_base(const PassOptions& o, const Module& mod) {
    return pe_sketch_base(o) + 5ull * pe_sketch_slots(mod, o.orig_fn_count);
}

} // namespace

class AllocationSiteProfilingPass : public Pass {
public:
    const char* name() const override { return "AllocationSiteProfiling"; }
    int order() const override { return 34; }
    const char* phase_name() const override {
        return "Phase 3: Escape & Allocation Analysis";
    }
    AnalysisMask invalidated() const override {
        return static_cast<AnalysisMask>(AnalysisKind::MemDep); // chain edits
    }
    bool run(PassContext& ctx) override {
        if (ctx.opts.pgo != PgoMode::Instrument) return false; // use-mode is p33's
        if (ctx.opts.orig_fn_count == 0) return false;

        const u64 base = alloc_site_base(ctx.opts, ctx.mod);
        u32 j = 0;
        bool changed = false;
        u32 upto = ctx.opts.orig_fn_count < ctx.mod.fns.size()
                       ? ctx.opts.orig_fn_count
                       : static_cast<u32>(ctx.mod.fns.size());
        for (u32 fi = 0; fi < upto; ++fi) {
            Graph& g = ctx.mod.fns[fi].g;
            for (NodeId id = 0; id < g.size(); ++id) {
                Node nc = g.node(id); // snapshot: chain edits below
                if (nc.op != Op::Alloc) continue;

                // Snapshot the user list FIRST: every bump below changes
                // input slots, and the cached use list must not be walked
                // while it mutates (the same discipline pass 91's instrument
                // path documents). The list is PER-SLOT (a call that both
                // chains on the alloc's version and passes the pointer
                // appears twice) — dedupe: one bump per effect site, or the
                // escape counter counts the same escape twice.
                std::vector<NodeId> users;
                {
                    const SmallVec<NodeId, 4>& live = g.uses_of(id);
                    users.assign(live.begin(), live.end());
                }
                std::sort(users.begin(), users.end());
                users.erase(std::unique(users.begin(), users.end()), users.end());

                // escape/free bumps at the site's effect users, inserted
                // BEFORE the site's own sketch so the sketch insertion's
                // user re-pointing threads them into the chain.
                //
                // The 7-slot block is emitted for EVERY alloc site (the
                // sketch at minimum) so the site enumeration matches pass
                // 33's use-mode reader one-for-one: the reader walks every
                // alloc in the original fns and derives block j from the
                // position in that walk. Skipping unobservable sites here
                // shifted every later block down (site mixups — the
                // reviewer's t46 demonstration) and a site with no
                // statically-identified effect could never become
                // runtime-confirmed non-escaping, which is the feedback
                // loop's whole point. Sites with no identified effects
                // still count allocations; their escape/free counters
                // stay zero exactly because nothing bumps them.
                for (NodeId u : users) {
                    if (u == kNoNode) continue;
                    Node un = g.node(u); // by value: set_input may reorder
                    if (un.op == Op::Dead) continue;
                    bool escape = false, is_free = false;
                    if (un.op == Op::Store && un.n_in >= 4 && un.in[3] == id) {
                        escape = true;                 // stored as a value
                    } else if (un.op == Op::Call) {
                        if (un.aux == kFnFree && un.n_in >= 3 && un.in[2] == id) {
                            is_free = true;            // free(this site)
                        } else {
                            for (u8 k = 2; k < un.n_in; ++k)
                                if (un.in[k] == id) {
                                    escape = true;      // passed as an arg
                                    break;
                                }
                        }
                    } else if (un.op == Op::Return) {
                        if (un.n_in >= 3 && un.in[2] == id) escape = true;
                        else continue;                  // mem-chain use only
                    } else {
                        continue; // Load/Store addressing, casts: not effects
                    }
                    if (!escape && !is_free) continue;
                    u64 idx = base + kAllocSiteSlots * j + (is_free ? 6 : 5);
                    NodeId pin = un.in[0];
                    NodeId mem = un.in[1];
                    NodeId bump = g.make(Op::Call, ty_mem(), {pin, mem}, 0,
                                         kFnPgoBump);
                    g.node(bump).ival = static_cast<i64>(idx);
                    g.set_input(u, 1, bump); // chain: ... -> bump -> effect
                }

                // size sketch right after the allocation (the size argument
                // is available at the alloc's pin): alloc -> sketch ->
                // (re-point the alloc's memory users). Snapshot the memory
                // users NOW — after the bumps exist, so they thread onto
                // the sketch too (the isel's PgoBump reads only its own
                // counter; ordering is the counter's, not the value's).
                //
                // The re-pointing is TYPE-AWARE: only MEMORY phis and true
                // memory-chain consumers (in[1] is a version input only for
                // Load/Store/Call/Return/Alloc) thread onto the sketch. A
                // VALUE phi or cast holding the POINTER keeps reading the
                // alloc — the alloc node survives as the pointer's producer;
                // re-pointing its value users onto the sketch's <mem> result
                // is type corruption (the pointer graph would read the
                // memory-version token as an address — found by t36_pgo,
                // where SROA had promoted the loop-carried pointer into a
                // phi whose entry input is the alloc).
                std::vector<NodeId> mem_users;
                {
                    const SmallVec<NodeId, 4>& live = g.uses_of(id);
                    mem_users.assign(live.begin(), live.end());
                }
                NodeId sk = g.make(Op::Call, ty_mem(), {nc.in[0], id, nc.in[2]},
                                   0, kFnPgoSketch);
                g.node(sk).ival = static_cast<i64>(base + kAllocSiteSlots * j);
                for (NodeId mu : mem_users) {
                    Node& m = g.node(mu);
                    if (m.op == Op::Dead) continue;
                    if (mu == sk) continue; // self
                    if (m.op == Op::Phi) {
                        if (m.ty != ty_mem()) continue; // value phi: pointer
                        for (u8 s = 1; s < m.n_in; ++s)
                            if (m.in[s] == id) g.set_input(mu, s, sk);
                    } else if (m.n_in > 1 && m.in[1] == id &&
                               (m.op == Op::Load || m.op == Op::Store ||
                                m.op == Op::Call || m.op == Op::Return ||
                                m.op == Op::Alloc)) {
                        g.set_input(mu, 1, sk); // memory-chain consumer
                    }
                }
                g.mark_uses_dirty();
                g.touch();
                changed = true;
                ++j;
            }
        }
        return changed;
    }
};

JULES_REGISTER_PASS(AllocationSiteProfilingPass, 34, "Phase 3")

} // namespace jules
