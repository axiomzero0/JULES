// Pass 26 — ScalarReplacementOfAggregates (Phase 2: Memory Optimization)
//
// Graal-style allocation scalarization (mem2reg on SoN): a promotable
// allocation (uses are only load/store addresses and the memory chain —
// never escaping data) has every load replaced by the SSA value computed by
// a reaching-def walk over the memory chain, creating value phis at merges
// (loop-carried values become real loop phis, which is exactly what IV
// recognition and GVN want to see downstream). Stores to promoted objects
// and the allocation itself are bypassed out of the memory chain.
//
// Uninitialized reads resolve to a zero constant of the pointee type
// (documented in docs/son_spec.md: MVP deterministic behavior).
#include "core/son/passes/pass_utils.h"
#include <cstdio>
#include <cstdlib>

namespace jules {

namespace {
class Sroa {
public:
    explicit Sroa(Graph& g) : g_(g) {}

    bool run() {
        for (NodeId alloc = 0; alloc < g_.size(); ++alloc) {
            if (g_.node(alloc).op != Op::Alloc) continue;
            if (g_.node(alloc).flags & kFlagStackPromoted) continue; // already stack: still promotable
            if (!promotable(alloc)) continue;
            promote(alloc);
        }
        return changed_;
    }

private:
    // ---- promotability: no data-escaping uses ----------------------------------
    bool promotable(NodeId alloc) {
        for (NodeId u : g_.uses_of(alloc)) {
            const Node& un = g_.node(u);
            switch (un.op) {
                case Op::Load:
                case Op::Store:
                    if (un.in[2] == alloc || un.in[1] == alloc) continue;
                    return false; // used as a VALUE (e.g. stored pointer)
                case Op::Call:
                    if (un.in[1] == alloc) continue; // memory-chain use only
                    return false;                    // passed as an argument: escapes
                case Op::Alloc:
                    if (un.in[1] == alloc) continue; // chained allocation (mem use)
                    return false;
                default:
                    return false;                    // any other data use escapes
            }
        }
        return true;
    }

    // ---- promotion ---------------------------------------------------------------
    void promote(NodeId alloc) {
        TypeId pointee = pointee_of(alloc);
        memo_.clear();
        poison_ = false; // per-allocation reset
        if (getenv("JULES_DEBUG_SROA"))
            fprintf(stderr, "[sroa] promote n%u\n", alloc);

        // Phase 1: resolve every load of this allocation to its SSA value.
        // If ANY resolution fails, abort promotion (conservative: the
        // allocation stays memory-backed, which is always correct).
        struct Replacement { NodeId load; NodeId value; };
        std::vector<Replacement> repls;
        bool all_ok = true;
        const SmallVec<NodeId, 4> users = g_.uses_of(alloc);
        for (NodeId u : users) {
            Node& un = g_.node(u);
            if (un.op != Op::Load || un.in[2] != alloc) continue;
            NodeId val = resolve(un.in[1], alloc, pointee, un.in[0]);
            if (val == kNoNode || val == u) { all_ok = false; break; }
            repls.push_back({u, val});
        }
        if (!all_ok || poison_) return;
        for (const Replacement& r : repls) {
            g_.replace_all_uses(r.load, r.value);
            g_.kill(r.load);
            changed_ = true;
        }

        // Bypass dead stores (no loads remain) out of the memory chain.
        const SmallVec<NodeId, 4> users2 = g_.uses_of(alloc);
        for (NodeId u : users2) {
            Node& un = g_.node(u);
            if (un.op == Op::Store && un.in[2] == alloc) {
                g_.replace_uses_as_memory(u, un.in[1]);
                g_.kill(u);
                changed_ = true;
            }
        }
        // Remove the allocation from the chain. Memory phis carry the alloc
        // in ANY value slot (one per predecessor), so the bypass must use
        // the memory-aware replacement: phis get every input slot rewritten,
        // regular nodes get slot 1. A plain slot-1 replacement leaves mem
        // phis pointing at the killed alloc (verifier: uses a killed node).
        NodeId entry_mem = g_.node(alloc).in[1];
        g_.replace_uses_as_memory(alloc, entry_mem);
        g_.kill(alloc);
        changed_ = true;
    }

    // Reaching-def resolution: which value does `mem` carry for `alloc`?
    NodeId resolve(NodeId mem, NodeId alloc, TypeId pointee, NodeId pin) {
        if (mem == kNoNode || g_.node(mem).op == Op::Dead) {
            poison_ = true;
            return kNoNode;
        }
        if (const NodeId* m = memo_.find(mem)) return *m == kNoNode ? kNoNode : *m;

        const Node& mv = g_.node(mem);
        switch (mv.op) {
            case Op::Alloc:
                if (mem == alloc) {
                    // initial value of the local: zero of the pointee type
                    ConstVal z;
                    z.ty = pointee;
                    z.is_fp = ty_is_float(pointee);
                    z.iv = 0;
                    z.fv = 0.0;
                    return make_const_node(g_, pin, z);
                }
                return resolve(mv.in[1], alloc, pointee, pin);
            case Op::Start:
                return zero_const(pointee, pin);
            case Op::Store: {
                if (same_alloc(mv.in[2], alloc))
                    return mv.in[3]; // the stored value
                return resolve(mv.in[1], alloc, pointee, pin);
            }
            case Op::Call:
                // the allocation never escapes: calls cannot write it
                return resolve(mv.in[1], alloc, pointee, pin);
            case Op::Phi: {
                // memory merge: create a value phi pinned to the REGION (the
                // mem phi's input 0); optimistic + memoized for cycles.
                NodeId region = mv.in[0];
                if (g_.node(region).op != Op::Region) return kNoNode;
                NodeId vphi = g_.make(Op::Phi, pointee, {region});
                memo_.insert(mem, vphi);
                for (u8 i = 1; i < mv.n_in; ++i) {
                    NodeId v = resolve(mv.in[i], alloc, pointee, pin);
                    if (v == kNoNode) { poison_ = true; }
                    g_.append_input(vphi, v == kNoNode ? zero_const(pointee, pin) : v);
                }
                return vphi;
            }
            default:
                poison_ = true;
                memo_.insert(mem, kNoNode);
                return kNoNode;
        }
    }

    bool same_alloc(NodeId addr, NodeId alloc) const {
        if (addr == alloc) return true;
        const Node& a = g_.node(addr);
        if (a.op == Op::Cast && a.in[1] == alloc) return true;
        return false;
    }

    NodeId zero_const(TypeId ty, NodeId pin) {
        ConstVal z;
        z.ty = ty;
        z.is_fp = ty_is_float(ty);
        z.iv = 0;
        z.fv = 0.0;
        return make_const_node(g_, pin, z);
    }

    TypeId pointee_of(NodeId alloc) const {
        TypeDesc d = type_desc(g_.node(alloc).ty);
        switch (d.pointee) {
            case Ty::I64: return ty_i64();
            case Ty::I32: return ty_i32();
            case Ty::U64: return ty_u64();
            case Ty::U32: return ty_u32();
            case Ty::F32: return ty_f32();
            case Ty::F64: return ty_f64();
            case Ty::I1:  return ty_i1();
            default: return ty_i64();
        }
    }

    Graph& g_;
    FlatMap<NodeId, NodeId> memo_; // mem version -> value node (per allocation)
    bool poison_ = false;  // any unresolved path aborts promotion
    bool changed_ = false;
};
} // namespace

class ScalarReplacementOfAggregatesPass : public Pass {
public:
    const char* name() const override { return "ScalarReplacementOfAggregates"; }
    int order() const override { return 26; }
    const char* phase_name() const override { return "Phase 2: Memory Optimization"; }
    AnalysisMask invalidated() const override {
        return AnalysisKind::AliasInfo | AnalysisKind::MemDep;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Sroa s(fg.g);
            changed |= s.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(ScalarReplacementOfAggregatesPass, 26, "Phase 2")

} // namespace jules
