// Pass 63 — AutoSOATransform (Phase 5)
//
// Opt-in whole-program AoS-to-SoA restructuring (--soa). The single-module
// compiler (LTO=Full visibility by default) makes the whole-program
// guarantee cheap: every use of a heap array's base pointer is visible in
// one graph.
//
// TARGET: a struct-array allocation whose every element access is a
// FIELD access — `arr[i].f` lowers to (base + i*E + O_f) with the field
// offset O_f a compile-time constant (the builder bakes it; whole-struct
// memory ops do not exist — struct copies lower field-by-field). The
// transform rewrites the array of E-byte structs into one allocation PER
// ACCESSED FIELD (SoA): arr[i].f becomes soa_f[i], the untouched fields
// never exist, and each access shrinks its traffic from E bytes of
// stride to sizeof(f) — the cache win of structure splitting.
//
// ELIGIBILITY (each rejects, never approximates):
//   * the allocation's size is a compile-time constant n * E;
//   * the only uses of the pointer are: the i64 address base of field
//     accesses (Load/Store of scalar type through the parsed address
//     tree), and free();
//   * every address parses as {base, idx scaled by exactly E, constant
//     offset} — any other shape (a stored address, a passed pointer, a
//     method receiver &mut self) is an aliasing hazard the transform
//     refuses;
//   * evidence of struct layout: at least two distinct field offsets,
//     or one non-zero offset (a plain scalar array restructures into
//     itself — no win);
//   * field offsets + sizes fit inside E.
//
// The restructure creates FRESH allocations (NoAlias vs everything
// else); free() fans out to one free per field array. Profitability is
// the user's opt-in (--soa) — access-pattern profiles are the roadmap's
// automatic gate.
#include "core/son/passes/pass_utils.h"

#include <algorithm>
#include <vector>

namespace jules {

namespace {

// Parse the i64 address tree rooted at `n` against a SET of base casts
// (LICM hoists the casts to the entry and GVN's re-run comes later in
// the cleanup sweep, so one allocation legitimately carries several
// structurally-identical casts at this point in the pipeline).
// Returns false on any shape the transform does not own.
bool parse_addr(Graph& g, NodeId n, const FlatMap<NodeId, bool>& base_set,
                NodeId& idx, i64& scale, i64& offset) {
    idx = kNoNode;
    scale = 0;
    offset = 0;
    bool base_ok = false;
    std::vector<NodeId> work{n};
    while (!work.empty()) {
        NodeId x = work.back();
        work.pop_back();
        Node xn = g.node(x); // snapshot
        if (xn.op == Op::Cast && static_cast<CastOp>(xn.sub) == CastOp::Ptr &&
            base_set.contains(x)) { // the tree leaf IS a base cast
            if (base_ok) return false; // base twice
            base_ok = true;
            continue;
        }
        if (xn.op == Op::Const) {
            offset += xn.ival;
            continue;
        }
        if (xn.op == Op::Bin && static_cast<BinOp>(xn.sub) == BinOp::Add) {
            work.push_back(xn.in[1]);
            work.push_back(xn.in[2]);
            continue;
        }
        if (xn.op == Op::Bin && static_cast<BinOp>(xn.sub) == BinOp::Shl &&
            g.node(xn.in[2]).op == Op::Const) {
            if (idx != kNoNode) return false; // two indices
            idx = xn.in[1];
            scale = 1ll << (g.node(xn.in[2]).ival & 63);
            continue;
        }
        if (xn.op == Op::Bin && static_cast<BinOp>(xn.sub) == BinOp::Mul &&
            g.node(xn.in[2]).op == Op::Const) {
            if (idx != kNoNode) return false;
            idx = xn.in[1];
            scale = g.node(xn.in[2]).ival;
            continue;
        }
        // scale-1 index term (byte arrays only) — at most one, opaque
        if (scale == 0 && idx == kNoNode) {
            idx = x;
            scale = 1;
            continue;
        }
        return false; // unknown address arithmetic
    }
    return base_ok && idx != kNoNode && scale > 0;
}
} // namespace

class AutoSOATransformPass : public Pass {
public:
    const char* name() const override { return "AutoSOATransform"; }
    int order() const override { return 63; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo |
               AnalysisKind::AliasInfo | AnalysisKind::MemDep |
               AnalysisKind::CallGraph;
    }
    bool run(PassContext& ctx) override {
       
        if (!ctx.opts.soa) return false; // opt-in even at -O3
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            for (NodeId a = 0; a < fg.g.size(); ++a) {
                Node ac = fg.g.node(a);
                if (ac.op != Op::Alloc) continue;
                if (soa_one(fg, a)) changed = true;
            }
        }
        return changed;
    }

private:
    static bool soa_one(FunctionGraph& fg, NodeId A) {
        Graph& g = fg.g;
        Node ac = g.node(A);
        if (g.node(ac.in[2]).op != Op::Const) return false; // dynamic size
        i64 total = g.node(ac.in[2]).ival;
        if (total <= 0) return false;

        // the address base casts (LICM may have hoisted several
        // structurally-identical ones; GVN's re-run is later) + free sites;
        // anything else rejects
        FlatMap<NodeId, bool> base_set;
        std::vector<NodeId> bases;
        std::vector<NodeId> frees;
        {
            // The use list is PER-SLOT (a free chained directly on the
            // alloc's version AND passing the pointer appears twice) —
            // dedupe: the free would otherwise fan out twice, the second
            // time over a killed node.
            std::vector<NodeId> users;
            {
                const SmallVec<NodeId, 4> raw = g.uses_of(A);
                users.assign(raw.begin(), raw.end());
            }
            std::sort(users.begin(), users.end());
            users.erase(std::unique(users.begin(), users.end()), users.end());
            for (NodeId u : users) {
                Node un = g.node(u);
                if (un.op == Op::Dead) continue;
                if (un.op == Op::Cast && static_cast<CastOp>(un.sub) == CastOp::Ptr) {
                    base_set.insert(u, true);
                    bases.push_back(u);
                } else if (un.op == Op::Call && un.aux == kFnFree &&
                           un.n_in >= 3 && un.in[2] == A) {
                    frees.push_back(u);
                } else if (un.op == Op::Phi && un.ty == ty_mem()) {
                    continue; // memory version merge (re-threaded below)
                } else if ((un.op == Op::Store || un.op == Op::Call ||
                             un.op == Op::Alloc) &&
                           un.n_in > 1 && un.in[1] == A) {
                    // memory-chain use ONLY when no other slot references A:
                    // a store THROUGH the raw pointer, or a call PASSING the
                    // pointer as an argument, is an aliasing hazard the
                    // transform refuses — the argument/address would name a
                    // killed node after g.kill(A) (regression-locked by
                    // t53_soa_call).
                    for (u8 k = 2; k < un.n_in; ++k)
                        if (un.in[k] == A) return false;
                    continue; // effects only: for value ops slot 1 is DATA,
                             // not a version
                } else if (un.op == Op::Load && un.in[2] == A) {
                   
                    return false; // pointer-sized whole reads
                } else {
                   
                    return false; // unknown use: aliasing hazard
                }
            }
        }
        if (bases.empty()) return false; // nothing dereferenced

        // ---- collect + validate every field access through b64 --------
        // Each access is: root Add tree -> Cast(*mut fty) -> Load/Store.
        // Rewriting the CAST's input fixes every consumer of the address
        // at once (GVN shares address computations freely).
        struct Acc {
            NodeId cast = kNoNode;
            NodeId idx = kNoNode;
            i64 offset = 0;
            TypeId fty = ty_none();
        };
        std::vector<Acc> accs;
        i64 esz = 0;
        {
            std::vector<NodeId> users;
            for (NodeId b : bases) {
                const SmallVec<NodeId, 4> bu = g.uses_of(b);
                users.insert(users.end(), bu.begin(), bu.end());
            }
            for (NodeId u : users) {
                Node un = g.node(u);
                if (un.op == Op::Dead) continue;
                if (un.op != Op::Bin || static_cast<BinOp>(un.sub) != BinOp::Add)
                    { return false; }
                // climb to the tree root (through unique Add parents)
                NodeId cur = u;
                for (int guard = 0; guard < 16; ++guard) {
                    NodeId parent = kNoNode;
                    for (NodeId w : g.uses_of(cur)) {
                        Node wn = g.node(w);
                        if (wn.op == Op::Dead) continue;
                        if (wn.op == Op::Bin &&
                            static_cast<BinOp>(wn.sub) == BinOp::Add &&
                            (wn.in[1] == cur || wn.in[2] == cur)) {
                            if (parent != kNoNode) return false; // forked tree
                            parent = w;
                        }
                    }
                    if (parent == kNoNode) break;
                    cur = parent;
                }
                // the root Cast(*mut fty) consumer
                NodeId cast = kNoNode;
                for (NodeId w : g.uses_of(cur)) {
                    Node wn = g.node(w);
                    if (wn.op == Op::Dead) continue;
                    if (wn.op == Op::Cast && static_cast<CastOp>(wn.sub) == CastOp::Ptr &&
                        wn.in[1] == cur) {
                        if (cast != kNoNode) return false;
                        cast = w;
                    }
                }
                if (cast == kNoNode) return false;
                // the cast's consumers: at least one Load/Store through it
                bool used_ok = false;
                for (NodeId w : g.uses_of(cast)) {
                    Node wn = g.node(w);
                    if (wn.op == Op::Dead) continue;
                    if ((wn.op == Op::Load || wn.op == Op::Store) &&
                        wn.in[2] == cast)
                        used_ok = true;
                    else
                        { return false; }
                }
                if (!used_ok) return false;
                // skip casts already recorded (shared subtrees)
                bool dup = false;
                for (const Acc& a : accs)
                    if (a.cast == cast) dup = true;
                if (dup) continue;

                Acc ai;
                ai.cast = cast;
                ai.fty = ty_pointee(g.node(cast).ty);
                i64 scale = 0;
                if (!parse_addr(g, cur, base_set, ai.idx, scale, ai.offset))
                    return false;
                if (esz == 0) esz = scale;
                else if (esz != scale) return false;
                accs.push_back(ai);
            }
        }
        if (accs.empty() || esz < 2) return false;

        // ---- struct-layout evidence + containment ----------------------
        {
            std::vector<i64> offs;
            for (const Acc& ai : accs) offs.push_back(ai.offset);
            std::sort(offs.begin(), offs.end());
            offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
            if (offs.size() < 2 && offs[0] == 0) { return false; }
            if (total % esz != 0) return false;
            for (const Acc& ai : accs) {
                u32 fsz = ty_store_bytes(ai.fty);
                if (ai.offset < 0 ||
                    static_cast<u64>(ai.offset) + fsz >
                        static_cast<u64>(esz))
                    { return false; }
            }
        }
        i64 n_elems = total / esz;

        // ---- the per-field arrays ---------------------------------------
        std::vector<std::pair<i64, NodeId>> field_arrays; // offset -> alloc
        NodeId prev_mem = ac.in[1]; // chain the new allocs off A's input
        NodeId last_alloc = kNoNode;
        {
            std::vector<i64> offs;
            for (const Acc& ai : accs) offs.push_back(ai.offset);
            std::sort(offs.begin(), offs.end());
            offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
            for (i64 o : offs) {
                TypeId fty = ty_none();
                for (const Acc& ai : accs)
                    if (ai.offset == o) {
                        fty = ai.fty;
                        break;
                    }
                i64 fsz = static_cast<i64>(ty_store_bytes(fty));
                NodeId size = g.make(Op::Const, ty_i64(), {ac.in[0]});
                g.node(size).ival = n_elems * fsz;
                NodeId al = g.make(Op::Alloc, ty_ptr(fty),
                                   {ac.in[0], prev_mem, size});
                prev_mem = al;
                last_alloc = al;
                field_arrays.push_back({o, al});
            }
        }

        // ---- rewrite the accesses (the cast's input: fixes every
        // consumer of the shared address computation) ---------------------
        for (const Acc& ai : accs) {
            NodeId al = kNoNode;
            for (const auto& fa : field_arrays)
                if (fa.first == ai.offset) al = fa.second;
            if (al == kNoNode) return false; // defensive
            // pin at the ACCESS's block (the cast's): the index expression
            // is loop-local and only dominates inside it — pinning at the
            // allocation's top would put the address before its inputs.
            NodeId apin = g.node(ai.cast).in[0];
            i64 fsz = static_cast<i64>(ty_store_bytes(ai.fty));
            NodeId b64f = g.make(Op::Cast, ty_i64(), {apin, al},
                                 static_cast<u8>(CastOp::Ptr));
            NodeId scaled = ai.idx;
            if (fsz > 1) {
                NodeId k;
                if ((fsz & (fsz - 1)) == 0) {
                    u8 sh = 0;
                    while ((1ll << sh) < fsz) ++sh;
                    k = g.make(Op::Const, ty_i64(), {apin});
                    g.node(k).ival = sh;
                    scaled = g.make(Op::Bin, ty_i64(),
                                    {apin, ai.idx, k},
                                    static_cast<u8>(BinOp::Shl));
                } else {
                    k = g.make(Op::Const, ty_i64(), {apin});
                    g.node(k).ival = fsz;
                    scaled = g.make(Op::Bin, ty_i64(),
                                    {apin, ai.idx, k},
                                    static_cast<u8>(BinOp::Mul));
                }
            }
            NodeId addr64 = g.make(Op::Bin, ty_i64(), {apin, b64f, scaled},
                                   static_cast<u8>(BinOp::Add));
            g.set_input(ai.cast, 1, addr64);
        }

        // ---- re-thread A's memory users onto the new chain ---------------
        {
            // A's mem users (nodes with in[1] == A) now read the LAST new
            // alloc; A itself exits the chain.
            std::vector<NodeId> mem_users;
            const SmallVec<NodeId, 4> users = g.uses_of(A);
            for (NodeId u : users) {
                if (u == last_alloc) continue;
                Node un = g.node(u);
                if (un.op == Op::Dead) continue;
                if ((un.op == Op::Store || un.op == Op::Call ||
                     un.op == Op::Alloc) &&
                    un.n_in > 1 && un.in[1] == A) {
                    mem_users.push_back(u);
                } else if (un.op == Op::Phi && un.ty == ty_mem()) {
                    for (u8 k = 1; k < un.n_in; ++k)
                        if (un.in[k] == A) mem_users.push_back(u);
                }
            }
            for (NodeId u : mem_users) {
                Node un = g.node(u);
                if (un.op == Op::Phi) {
                    for (u8 k = 1; k < un.n_in; ++k)
                        if (un.in[k] == A) g.set_input(u, k, last_alloc);
                } else {
                    g.set_input(u, 1, last_alloc);
                }
            }
        }

        // ---- fan the frees out -------------------------------------------
        for (NodeId fr : frees) {
            if (g.node(fr).op == Op::Dead) continue; // killed by an earlier
                // iteration of this loop (a stale duplicate record)
            NodeId prev = g.node(fr).in[1]; // the free's memory version
            NodeId pin = g.node(fr).in[0];
            for (const auto& fa : field_arrays) {
                NodeId nf = g.make(Op::Call, ty_mem(), {pin, prev, fa.second},
                                   0, kFnFree);
                prev = nf;
            }
            // the old free's users move onto the last new free
            g.replace_uses_as_memory(fr, prev);
            g.kill(fr);
        }

        g.kill(A);
        for (NodeId b : bases) g.kill(b);
        g.mark_uses_dirty();
        g.touch();
        return true;
    }
};

JULES_REGISTER_PASS(AutoSOATransformPass, 63, "Phase 5")

} // namespace jules
