// Pass 24 — StoreMerging (Phase 2)
//
// Combine adjacent narrow stores into one wide store:
//   a[k]   = <4-byte const c1>
//   a[k+1] = <4-byte const c2>      ==>  one 8-byte store of the combined
//   (adjacent in the memory chain)       bit pattern at &a[k]
//
// Profitability gate (x86-64): `movq $imm, disp(%r)` encodes only a
// SIGN-EXTENDED imm32 — the merged 64-bit pattern must be reproducible
// from its low 32 bits, else the store needs a movabs materialization
// that costs more than the two movl's it replaces. The zero-initialize
// pair (0,0) — the unrolled form of every clearing loop — is the
// canonical profitable case: one `movq $0, disp(%r)` for two elements.
//
// Soundness: the pair must be adjacent in the memory CHAIN (s2's memory
// version is s1: no intervening effect), and no load reading either
// element may consume s1's version between the two stores (it would
// observe the pre-store value of element k+1; after the merge the wide
// store would have already written it). Loads of other addresses are
// unaffected — they are rethreaded to the merged version and read the
// same bytes.
//
// Where it fires: straight-line initializers and fully-unrolled constant
// fill loops (the cleanup re-run visits pass 24 after pass 42's unrolls).
// SLP (54) handles the load/store-isomorphic pairs; this pass owns the
// constant-value pairs SLP cannot pack.
#include "core/son/passes/vector_utils.h"

#include <cstring>

namespace jules {

namespace {

bool bits32_of(const ConstVal& c, u32& out) {
    if (c.is_fp) {
        if (c.ty != ty_f32()) return false;
        f32 f = static_cast<f32>(c.fv);
        std::memcpy(&out, &f, sizeof(f));
        return true;
    }
    if (c.ty != ty_i32() && c.ty != ty_u32()) return false;
    out = static_cast<u32>(static_cast<i64>(c.iv) & 0xFFFFFFFFull);
    return true;
}

class StoreMerger {
public:
    explicit StoreMerger(Graph& g) : g_(g) {}

    u32 run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            if (g_.node(id).op != Op::Store) continue;
            NodeId s2 = kNoNode;
            for (NodeId u : g_.uses_of(id)) {
                if (g_.is_dead(u)) continue;
                const Node& un = g_.node(u);
                if (un.op == Op::Store && un.in[1] == id) { s2 = u; break; }
            }
            if (s2 == kNoNode) continue;
            try_merge(id, s2);
        }
        return merged_;
    }

private:
    void try_merge(NodeId s1, NodeId s2) {
        const Node& n1 = g_.node(s1);
        const Node& n2 = g_.node(s2);
        if (n1.in[0] != n2.in[0]) return; // pinned apart: keep local reasoning

        // 4-byte element types only (i32/u32/f32): the pair must combine
        // into one 8-byte pattern
        ConstVal c1, c2;
        if (!const_of(g_, n1.in[3], c1) || !const_of(g_, n2.in[3], c2)) return;
        u32 b1 = 0, b2 = 0;
        if (!bits32_of(c1, b1) || !bits32_of(c2, b2)) return;

        // addresses: same base, consecutive const element indices
        vecx::AddrPattern a1 = vecx::match_addr(g_, n1.in[2], 4);
        vecx::AddrPattern a2 = vecx::match_addr(g_, n2.in[2], 4);
        if (!a1.ok || !a2.ok || a1.base != a2.base) return;
        i64 k1 = 0, k2 = 0;
        if (a1.idx != kNoNode) {
            ConstVal ci;
            if (!const_of(g_, a1.idx, ci)) return;
            k1 = ci.iv;
        }
        if (a2.idx != kNoNode) {
            ConstVal ci;
            if (!const_of(g_, a2.idx, ci)) return;
            k2 = ci.iv;
        }
        if (k2 != k1 + 1) return;

        // no load of either element may sit between the stores (it would
        // observe the pre-store value of element k+1)
        for (NodeId u : g_.uses_of(s1)) {
            if (g_.is_dead(u)) continue;
            const Node& un = g_.node(u);
            if (un.op != Op::Load || un.in[1] != s1) continue;
            vecx::AddrPattern la = vecx::match_addr(g_, un.in[2], 4);
            if (!la.ok || la.base != a1.base) continue;
            i64 lk = 0;
            if (la.idx != kNoNode) {
                ConstVal ci;
                if (!const_of(g_, la.idx, ci)) return; // unknown index: reject
                lk = ci.iv;
            }
            if (lk == k1 || lk == k2) return;
        }

        u64 combined = static_cast<u64>(b1) | (static_cast<u64>(b2) << 32);
        // movq $imm, m64 needs sign-extended imm32 to reproduce the pattern
        i64 pattern = static_cast<i64>(combined);
        if (static_cast<i64>(static_cast<i32>(static_cast<u32>(combined))) != pattern)
            return;

        ConstVal merged_const;
        merged_const.is_fp = false;
        merged_const.ty = ty_i64();
        merged_const.iv = pattern;
        NodeId cv = make_const_node(g_, n1.in[0], merged_const);
        NodeId wide = g_.make(Op::Store, ty_mem(),
                              {n1.in[0], n1.in[1], n1.in[2], cv});
        g_.replace_uses_as_memory(s1, wide);
        g_.replace_uses_as_memory(s2, wide);
        g_.kill(s1);
        g_.kill(s2);
        g_.touch();
        ++merged_;
    }

    Graph& g_;
    u32 merged_ = 0;
};

} // namespace

class StoreMergingPass : public Pass {
public:
    const char* name() const override { return "StoreMerging"; }
    int order() const override { return 24; }
    const char* phase_name() const override { return "Phase 2"; }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            StoreMerger m(fg.g);
            changed |= m.run() > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(StoreMergingPass, 24, "Phase 2")

} // namespace jules
