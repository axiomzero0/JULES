// Pass 50 — TailRecursionElimination (Phase 4)
//
// Two transforms in the tail-call family:
//
// 1. Self tail-call marking: Return value is a Call to the same function,
//    the call is the final effect in its block, and the return sits in that
//    same block. The x86-64 emitter honors kFlagTailCall by jumping to the
//    function entry instead of calling (no stack growth).
//
// 2. Recursion unrolling via accumulator introduction (the transform GCC
//    applies to fib under the -foptimize-sibling-calls umbrella). A
//    self-recursive function of the shape
//        fn f(x) { if C(x) { return B(x); } return f(g1(x)) + f(g2(x)); }
//    (integer +, both calls to self, pure C/B/g1/g2) rewrites to
//        loop { if C(x) { return B(x) + acc; }
//               acc = acc + f(g1(x));  x = g2(x); }
//    with acc threaded through a loop phi. ONE recursive call executes per
//    level instead of two — the right spine of the call tree (half its
//    dynamic nodes) becomes the accumulator. Divergence behavior is
//    preserved exactly: the loop follows the g2-chain the original right
//    recursion took; where the original terminated, C eventually holds.
//    Integer-only: reassociating a float sum changes results.
//
// Runs before inlining/loop opts per the catalog ordering contract.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class TailCallMarker {
public:
    TailCallMarker(Graph& g, FnId fid) : g_(g), fid_(fid) {}

    bool run() {
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op != Op::Return || n.n_in != 3) continue;
            NodeId val = n.in[2];
            const Node& vn = g_.node(val);
            if (vn.op != Op::Call || vn.aux != fid_) continue;
            // the call must be the last memory effect and share the return's block
            if (n.in[1] != val) continue;         // something happened after the call
            if (n.in[0] != vn.in[0]) continue;    // control drifted
            g_.node(val).flags |= kFlagTailCall;
            changed_ = true;
        }
        return changed_;
    }

private:
    Graph& g_;
    FnId fid_;
    bool changed_ = false;
};

// ---------------------------------------------------------------------------
// Accumulator recursion unrolling
// ---------------------------------------------------------------------------
class AccumulatorUnroller {
public:
    AccumulatorUnroller(FunctionGraph& fg) : g_(fg.g), fg_(fg), fid_(fg.fid) {}

    bool run() {
        // Transformed functions must not be inlined afterwards: their shape
        // is a loop whose self-call result feeds the accumulator phi's
        // backedge — the inliner's repin closure does not support a call
        // inside a phi cycle (the inline drops the call and strands the
        // accumulator's value register; observed on fib @ -O3). An
        // always_inline function keeps its original shape instead.
        if (fg_.always_inline) return false;
        if (!match()) return false;
        rebuild();
        fg_.no_inline = true;
        return true;
    }

private:
    // Purity for transform inputs: no memory effects, no phis (a phi input
    // would make the expression loop-dependent in unsound ways).
    bool pure_subtree(NodeId root) {
        if (root == kNoNode) return false;
        std::vector<NodeId> stack{root};
        FlatMap<NodeId, bool> seen;
        while (!stack.empty()) {
            NodeId n = stack.back();
            stack.pop_back();
            if (n == kNoNode) return false;
            if (seen.contains(n)) continue;
            seen.insert(n, true);
            const Node& nd = g_.node(n);
            switch (nd.op) {
                case Op::Const:
                case Op::Param:
                    continue; // leaves (inputs not data)
                case Op::Bin:
                case Op::Cmp:
                case Op::Un:
                case Op::Cast:
                case Op::Select:
                    break;
                default:
                    return false; // Call/Load/Store/Alloc/Phi/control
            }
            for (u8 i = 1; i < nd.n_in; ++i) stack.push_back(nd.in[i]);
        }
        return true;
    }

    // Re-pin a value subtree's non-Const nodes to `blk`: post-RAUW they read
    // the loop phis, so they must execute inside the loop (or the exit
    // block), not once in the entry.
    void repin_subtree(NodeId root, NodeId blk) {
        std::vector<NodeId> stack{root};
        FlatMap<NodeId, bool> seen;
        while (!stack.empty()) {
            NodeId n = stack.back();
            stack.pop_back();
            if (n == kNoNode) continue;
            if (seen.contains(n)) continue;
            seen.insert(n, true);
            Node& nd = g_.node(n);
            if (nd.op != Op::Const && nd.op != Op::Param && nd.op != Op::Phi)
                g_.set_input(n, 0, blk);
            for (u8 i = 1; i < nd.n_in; ++i) stack.push_back(nd.in[i]);
        }
    }

    bool match() {
        NodeId stop = g_.stop();
        // exactly two live Returns
        NodeId r_base = kNoNode, r_rec = kNoNode;
        u8 nret = 0;
        for (u8 i = 0; i < g_.node(stop).n_in; ++i) {
            NodeId r = g_.node(stop).in[i];
            if (g_.node(r).op != Op::Return) continue;
            ++nret;
            if (r_base == kNoNode) r_base = r;
            else if (r_rec == kNoNode) r_rec = r;
            else return false;
        }
        if (nret != 2) return false;

        // recursive return: value = Bin(Add, Call1, Call2)
        Node& rr = g_.node(r_rec);
        if (rr.n_in != 3) return false;
        NodeId addv = rr.in[2];
        Node& an = g_.node(addv);
        if (an.op != Op::Bin || an.sub != static_cast<u32>(BinOp::Add)) return false;
        if (ty_is_float(an.ty)) return false; // integer + only (reassociation)
        NodeId c1 = an.in[1], c2 = an.in[2];
        if (g_.node(c1).op != Op::Call || g_.node(c1).aux != fid_) return false;
        if (g_.node(c2).op != Op::Call || g_.node(c2).aux != fid_) return false;
        Node& k1 = g_.node(c1);
        Node& k2 = g_.node(c2);
        // memory order: call1 -> call2 -> return (last effect in the block)
        if (rr.in[1] != c2) return false;
        if (k2.in[1] != c1) return false;
        // both calls share the return's block (a projection of the entry If)
        if (rr.in[0] != k1.in[0] || rr.in[0] != k2.in[0]) return false;
        NodeId body_proj = rr.in[0];
        if (g_.node(body_proj).op != Op::IfTrue && g_.node(body_proj).op != Op::IfFalse)
            return false;
        ifn_ = g_.node(body_proj).in[0];
        if (g_.node(ifn_).op != Op::If) return false;
        if (g_.node(ifn_).in[0] != g_.start()) return false; // If pinned at entry

        // exactly two self-calls in the whole function (killed calls stay
        // in the node array as Dead — skip them)
        u32 self_calls = 0;
        for (NodeId n = 0; n < g_.size(); ++n) {
            const Node& nd = g_.node(n);
            if (nd.op == Op::Dead) continue;
            if (nd.op == Op::Call && nd.aux == fid_) ++self_calls;
        }
        if (self_calls != 2) return false;

        // no memory effects outside the two calls
        for (NodeId n = 0; n < g_.size(); ++n) {
            const Node& nd = g_.node(n);
            if (nd.op == Op::Dead) continue;
            if (nd.op == Op::Store || nd.op == Op::Alloc) return false;
            if (nd.op == Op::Call && n != c1 && n != c2) return false;
        }

        // base return: from the OTHER projection, no effects before it
        Node& br = g_.node(r_base);
        if (br.n_in != 3) return false;
        base_proj_ = br.in[0];
        if (base_proj_ == body_proj) return false;
        if (g_.node(base_proj_).op != Op::IfTrue && g_.node(base_proj_).op != Op::IfFalse)
            return false;
        if (g_.node(base_proj_).in[0] != ifn_) return false;
        if (br.in[1] != g_.start()) return false;
        if (ty_is_float(g_.node(br.in[2]).ty)) return false;
        if (g_.node(br.in[2]).ty != an.ty) return false;

        // arity: both calls pass exactly the parameter count
        u32 nparams = 0;
        for (NodeId n = 0; n < g_.size(); ++n) {
            const Node& nd = g_.node(n);
            if (nd.op == Op::Param) ++nparams;
        }
        if (nparams == 0) return false;
        if (k1.n_in != 2 + nparams || k2.n_in != 2 + nparams) return false;

        // purity of the transform inputs
        if (!pure_subtree(g_.node(ifn_).in[1])) return false; // C
        if (!pure_subtree(br.in[2])) return false;           // B
        for (u8 i = 2; i < k1.n_in; ++i)
            if (!pure_subtree(k1.in[i])) return false;       // g1 args
        for (u8 i = 2; i < k2.n_in; ++i)
            if (!pure_subtree(k2.in[i])) return false;       // g2 args

        r_base_ = r_base;
        r_rec_ = r_rec;
        addv_ = addv;
        c1_ = c1;
        c2_ = c2;
        body_proj_ = body_proj;
        acc_ty_ = an.ty;
        return true;
    }

    void rebuild() {
        NodeId start = g_.start();
        NodeId cond = g_.node(ifn_).in[1];

        // loop header + memory phi
        NodeId hdr = g_.make(Op::Region, ty_ctrl(), {start});
        NodeId mphi = g_.make(Op::Phi, ty_mem(), {hdr, start});

        // per-parameter value phis (entry inputs appended after RAUW)
        std::vector<NodeId> params;   // by Param index
        std::vector<NodeId> phis;
        for (NodeId n = 0; n < g_.size(); ++n) {
            const Node& nd = g_.node(n);
            if (nd.op != Op::Param) continue;
            size_t idx = params.size();
            params.push_back(n);
            phis.push_back(g_.make(Op::Phi, nd.ty, {hdr}));
            (void)idx;
        }
        // accumulator phi: acc = 0 entry
        NodeId zero = g_.make(Op::Const, acc_ty_, {start});
        g_.node(zero).ival = 0;
        NodeId accphi = g_.make(Op::Phi, acc_ty_, {hdr});

        // RAUW params -> phis (C/B/g1/g2 now read the loop phis)
        for (size_t i = 0; i < params.size(); ++i)
            g_.replace_all_uses(params[i], phis[i]);
        // entry inputs AFTER the RAUW (no self-cycle)
        for (size_t i = 0; i < params.size(); ++i)
            g_.append_input(phis[i], params[i]);
        g_.append_input(accphi, zero);

        // re-pin the condition subtree into the header (it reads the phis)
        repin_subtree(cond, hdr);
        // re-pin B into the base-exit block
        repin_subtree(g_.node(r_base_).in[2], base_proj_);

        // the If moves under the header
        g_.set_input(ifn_, 0, hdr);

        // base return: return B + acc, memory = loop phi
        NodeId b = g_.node(r_base_).in[2];
        NodeId baseadd = g_.make(Op::Bin, acc_ty_, {base_proj_, b, accphi},
                                 static_cast<u8>(BinOp::Add));
        g_.set_input(r_base_, 1, mphi);
        g_.set_input(r_base_, 2, baseadd);

        // recursive call: memory = loop phi (it used to read entry memory)
        g_.set_input(c1_, 1, mphi);

        // acc' = Call1 + acc   (replaces  Add(Call1, Call2))
        g_.set_input(addv_, 2, accphi);

        // backedges: header pred, mem version, param values from g2, acc
        g_.append_input(hdr, body_proj_);
        g_.append_input(mphi, c1_);
        for (size_t i = 0; i < params.size(); ++i)
            g_.append_input(phis[i], g_.node(c2_).in[2 + i]);
        g_.append_input(accphi, addv_);

        // kill the second call and the recursive return; compact Stop
        g_.kill(c2_);
        g_.kill(r_rec_);
        Node& stopn = g_.node(g_.stop());
        if (stopn.op == Op::Stop) {
            u8 keep = 0;
            for (u8 i = 0; i < stopn.n_in; ++i)
                if (g_.node(stopn.in[i]).op != Op::Dead) stopn.in[keep++] = stopn.in[i];
            stopn.n_in = keep;
        }
        g_.mark_uses_dirty();
        g_.touch();
    }

    Graph& g_;
    FunctionGraph& fg_;
    FnId fid_;
    NodeId ifn_ = kNoNode, base_proj_ = kNoNode, body_proj_ = kNoNode;
    NodeId r_base_ = kNoNode, r_rec_ = kNoNode, addv_ = kNoNode;
    NodeId c1_ = kNoNode, c2_ = kNoNode;
    TypeId acc_ty_ = ty_i64();
};
} // namespace

class TailRecursionEliminationPass : public Pass {
public:
    const char* name() const override { return "TailRecursionElimination"; }
    int order() const override { return 50; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::CallGraph); }
    bool run(PassContext& ctx) override {
        (void)ctx.analysis.callgraph();
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            AccumulatorUnroller au(fg);
            changed |= au.run();
            TailCallMarker m(fg.g, fg.fid);
            changed |= m.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(TailRecursionEliminationPass, 50, "Phase 4")

} // namespace jules
