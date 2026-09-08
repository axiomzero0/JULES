// Pass 53 — IdiomRecognition (Phase 5)
//
// Recognizes library-idiom loop shapes and lowers them to their optimal
// forms. First family: constant-trip FILL and COPY loops small enough
// that the loop machinery costs more than the body (the unroller skips
// trips < 6; the vectorizer keeps a guard + remainder loop):
//
//   while i < T { a[i] = <invariant v>; i++ }   ==>  T stores at the loop
//   while i < T { a[i] = b[i];     i++ }   ==>  T loads + T stores   entry
//
// The expansion replaces the loop's EFFECTS with a straight-line chain
// threaded off the entry memory, RAUs the memory phi to the last store
// (post-loop readers see the fully-written array), RAUs the IV phi to its
// final constant (init + T*step — exact, counted loop, no early exits),
// and pins the loop guard's condition to the constant that always takes
// the EXIT projection — the body becomes unreachable and the cleanup
// SCCP/DCE sweep reclaims it (at levels without cleanup rounds the shell
// survives as a never-taken branch; behavior is identical, code is just
// larger).
//
// COPY requires the two bases to be NoAlias: the rebuilt loads read the
// pre-loop memory version (the original read iteration k's version —
// identical for a source nothing in the loop writes). FILL requires the
// value to be loop-invariant AND available before the entry block (its
// defining block dominates the entry — otherwise the straight-line store
// would consume a use-before-def). SLP (54) then packs the copy's
// adjacent load/store pairs into movups; fill stores emit as single
// immediate stores. This is the inlined-memset/memcpy shape production
// compilers emit for small constant sizes.
#include "core/son/passes/loop_transforms.h"
#include "core/son/passes/vector_utils.h"

namespace jules {

namespace {

class IdiomRecognizer {
public:
    IdiomRecognizer(Graph& g, LoopInfo& li, DomTree& dom, AliasInfo& aa)
        : g_(g), li_(li), dom_(dom), aa_(aa) {}

    u32 run() {
        u32 hits = 0;
        for (int round = 0; round < 8; ++round) {
            bool any = false;
            for (const Loop& l : li_.loops()) {
                loopx::CountedLoop cl;
                if (!loopx::match_counted(g_, li_, dom_, l, cl)) continue;
                if (try_expand(cl, l)) {
                    ++hits;
                    any = true;
                    break; // structure changed: recompute loop info
                }
            }
            if (!any) break;
        }
        return hits;
    }

private:
    // Every input of the closure (and the node itself) must be defined
    // before the loop: no header phi, nothing pinned inside the loop,
    // and every value's pin block dominates the entry block.
    bool available_before(NodeId n, NodeId entry, NodeId header) {
        if (n == kNoNode || g_.is_dead(n)) return true;
        if (seen_.contains(n)) return true;
        seen_.insert(n, true);
        const Node& nd = g_.node(n);
        if (nd.op == Op::Phi && nd.in[0] == header) return false;
        if (is_control_op(nd.op) || is_block_head(nd.op)) {
            return n == entry || dom_.dominates(n, entry);
        }
        NodeId pin = nd.in[0];
        if (pin == kNoNode) return false;
        if (pin == entry || dom_.dominates(pin, entry)) {
            for (u8 i = 1; i < nd.n_in; ++i)
                if (!available_before(nd.in[i], entry, header)) return false;
            return true;
        }
        return false;
    }

    NodeId build_elem_addr(NodeId pin, NodeId base, i64 k, u32 esz, TypeId ptr_ty) {
        ConstVal kv;
        kv.is_fp = false;
        kv.ty = ty_i64();
        kv.iv = k;
        NodeId kconst = make_const_node(g_, pin, kv);
        NodeId base64 = g_.make(Op::Cast, ty_i64(), {pin, base},
                                static_cast<u8>(CastOp::Ptr));
        NodeId scaled = kconst;
        if (esz > 1) {
            ConstVal sv;
            sv.is_fp = false;
            sv.ty = ty_i64();
            u32 sh = 0;
            while ((1u << sh) < esz) ++sh;
            sv.iv = static_cast<i64>(sh);
            NodeId s = make_const_node(g_, pin, sv);
            scaled = g_.make(Op::Bin, ty_i64(), {pin, kconst, s},
                             static_cast<u8>(BinOp::Shl));
        }
        NodeId sum = g_.make(Op::Bin, ty_i64(), {pin, base64, scaled},
                             static_cast<u8>(BinOp::Add));
        return g_.make(Op::Cast, ptr_ty, {pin, sum}, static_cast<u8>(CastOp::Ptr));
    }

    bool try_expand(const loopx::CountedLoop& cl, const Loop& l) {
        (void)l;
        if (cl.trip < 1 || cl.trip > 16) return false;
        if (cl.blocks.size() != 1) return false;          // single body block
        NodeId body = cl.blocks[0];

        // exactly one Store, no Calls in the body
        NodeId store = kNoNode;
        u32 nstores = 0, ncalls = 0;
        for (NodeId u : g_.uses_of(body)) {
            if (g_.is_dead(u) || g_.node(u).in[0] != body) continue;
            Op o = g_.node(u).op;
            if (o == Op::Store) { store = u; ++nstores; }
            if (o == Op::Call) ++ncalls;
        }
        if (nstores != 1 || ncalls != 0 || store == kNoNode) return false;
        const Node& sn = g_.node(store);
        NodeId val = sn.in[3];
        u32 esz = ty_store_bytes(g_.node(val).ty);
        if (esz != 4 && esz != 8) return false;

        NodeId entry = g_.node(cl.header).in[cl.entry_slot];

        // destination: base[iv] with a base available before the entry
        vecx::AddrPattern dst = vecx::match_addr(g_, sn.in[2], esz);
        if (!dst.ok || dst.idx != cl.iv_phi) return false;
        seen_.clear();
        if (!available_before(dst.base, entry, cl.header)) return false;

        // value: FILL (invariant + available) or COPY (single src[iv] load)
        bool is_copy = false;
        NodeId src_base = kNoNode;
        if (g_.node(val).op == Op::Load && g_.node(val).in[0] == body) {
            vecx::AddrPattern src = vecx::match_addr(g_, g_.node(val).in[2], esz);
            if (src.ok && src.idx == cl.iv_phi) {
                seen_.clear();
                if (available_before(src.base, entry, cl.header)) {
                    is_copy = true;
                    src_base = src.base;
                }
            }
        }
        if (!is_copy) {
            seen_.clear();
            if (!available_before(val, entry, cl.header)) return false;
        }
        if (is_copy && aa_.alias(dst.base, src_base) != AliasResult::NoAlias)
            return false; // overlapping copy: keep the loop's ordering

        // the memory phi at the header (entry input = pre-loop version)
        NodeId mem_phi = kNoNode;
        for (NodeId u : g_.uses_of(cl.header)) {
            const Node& un = g_.node(u);
            if (un.op == Op::Phi && un.ty == ty_mem() && un.in[0] == cl.header)
                mem_phi = u;
        }
        if (mem_phi == kNoNode) return false;
        NodeId m0 = g_.node(mem_phi).in[cl.entry_slot + 1];

        // the IV's post-loop value must fold to a constant (const init)
        ConstVal init_c;
        if (!const_of(g_, g_.node(cl.iv_phi).in[cl.entry_slot + 1], init_c))
            return false;

        // rebuild the straight-line chain at the entry predecessor
        NodeId mem = m0;
        for (i64 k = 0; k < cl.trip; ++k) {
            NodeId addr = build_elem_addr(entry, dst.base, k, esz,
                                          g_.node(sn.in[2]).ty);
            NodeId v;
            if (is_copy) {
                NodeId saddr = build_elem_addr(entry, src_base, k, esz,
                                               g_.node(g_.node(val).in[2]).ty);
                v = g_.make(Op::Load, g_.node(val).ty, {entry, m0, saddr});
            } else {
                v = val;
            }
            mem = g_.make(Op::Store, ty_mem(), {entry, mem, addr, v});
            g_.touch();
        }

        // post-loop memory readers see the final store
        g_.replace_uses_as_memory(mem_phi, mem);

        ConstVal final_iv;
        final_iv.is_fp = false;
        final_iv.ty = g_.node(cl.iv_phi).ty;
        final_iv.iv = init_c.iv + cl.trip * cl.iv_step;
        NodeId fiv = make_const_node(g_, entry, final_iv);
        g_.replace_all_uses(cl.iv_phi, fiv);

        // guard always exits: the body becomes unreachable
        ConstVal f;
        f.is_fp = false;
        f.ty = ty_i1();
        bool body_is_true = (g_.node(cl.body_proj).op == Op::IfTrue);
        f.iv = body_is_true ? 0 : 1; // select the EXIT projection
        NodeId fc = make_const_node(g_, cl.header, f);
        g_.set_input(cl.guard_if, 1, fc);

        return true;
    }

    Graph& g_;
    LoopInfo& li_;
    DomTree& dom_;
    AliasInfo& aa_;
    FlatMap<NodeId, bool> seen_;
};

} // namespace

class IdiomRecognitionPass : public Pass {
public:
    const char* name() const override { return "IdiomRecognition"; }
    int order() const override { return 53; }
    const char* phase_name() const override {
        return "Phase 5: Vectorization & Superword Parallelism";
    }
    AnalysisMask required() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo | AnalysisKind::AliasInfo;
    }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo | AnalysisKind::AliasInfo |
               AnalysisKind::MemDep;
    }
    ModeMask modes() const override { return kModeAll; }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Graph& g = fg.g;
            auto li = LoopInfo::compute(g, ctx.analysis.doms(fg));
            IdiomRecognizer r(g, *li, ctx.analysis.doms(fg), ctx.analysis.alias(fg));
            changed |= r.run() > 0;
        }
        return changed;
    }
};

JULES_REGISTER_PASS(IdiomRecognitionPass, 53, "Phase 5")

} // namespace jules
