// Pass 50 — TailRecursionElimination (Phase 4)
//
// Three transforms in the tail-call family:
//
// 1. Tail-call-to-LOOP conversion (the transform production compilers
//    actually apply): a direct self tail call `return f(A1..An)` becomes a
//    real loop — header Region, memory phi, per-parameter value phis — so
//    the entire loop machinery applies afterwards (SCCP/GVN on the phis,
//    register allocation with phi coalescing, machine-level rotation). The
//    old jump-to-entry TCO pays a full frame teardown + rebuild per spine
//    iteration (~19 instructions of pure churn on tak's 39.5M tail calls);
//    the loop form pays three phi-update movs and one branch. Memory is
//    threaded through a loop phi: past iterations' callee effects stay
//    visible to later iterations and to the base-case return (returning
//    the entry memory would revert them). Conservative gating: exactly one
//    convertible tail return is rewritten per function (the rest keep the
//    jump form); entry-merge Regions and calls pinned at the entry block
//    are rejected.
//
// 2. Self tail-call marking: any remaining Return value is a Call to the
//    same function, the call is the final effect in its block, and the
//    return sits in that same block. The x86-64 emitter honors
//    kFlagTailCall by jumping to the function entry instead of calling (no
//    stack growth) — the fallback for shapes transform 1 rejects.
//
// 3. Recursion unrolling via accumulator introduction (the transform GCC
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
#include "core/son/passes/inline.h"

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
// Tail-call-to-loop conversion
// ---------------------------------------------------------------------------
class TailCallLoopifier {
public:
    TailCallLoopifier(FunctionGraph& fg) : g_(fg.g), fg_(fg), fid_(fg.fid) {}

    bool run() {
        // Inlined afterwards, the loop's self-calls feed phi backedges — the
        // inliner's repin closure does not support that shape (same reason
        // the accumulator transform marks no_inline).
        if (fg_.always_inline) return false;
        if (!match()) return false;
        rebuild();
        fg_.no_inline = true;
        return true;
    }

private:
    // Nodes whose pin (in[0]) may need to move into the loop header.
    static bool repinnable(Op o) {
        return o == Op::Bin || o == Op::Cmp || o == Op::Un || o == Op::Cast ||
               o == Op::Select || o == Op::Load || o == Op::Store ||
               o == Op::Alloc || o == Op::Call;
    }

    bool match() {
        NodeId start = g_.start();

        // Parameters indexed by declaration order (aux). Duplicated or
        // sparse indices would break the arg<->phi correspondence.
        for (NodeId n = 0; n < g_.size(); ++n) {
            const Node& nd = g_.node(n);
            if (nd.op == Op::Dead) continue;
            if (nd.op == Op::Param) {
                u32 idx = nd.aux;
                if (params_.size() <= idx) params_.resize(idx + 1, kNoNode);
                if (params_[idx] != kNoNode) return false;
                params_[idx] = n;
                continue;
            }
            // Entry-merge Regions: the header rewiring is ambiguous there;
            // the jump fallback handles those functions.
            if (nd.op == Op::Region) {
                for (u8 i = 0; i < nd.n_in; ++i)
                    if (nd.in[i] == start) return false;
            }
        }
        if (params_.empty()) return false;
        for (NodeId p : params_)
            if (p == kNoNode) return false;

        // A Return whose value is the self call directly (pure tail
        // position), the call is the block's final effect, and control is
        // shared. Exactly one convertible tail return per run.
        for (NodeId r = 0; r < g_.size(); ++r) {
            const Node& rn = g_.node(r);
            if (rn.op != Op::Return || rn.n_in != 3) continue;
            NodeId call = rn.in[2];
            const Node& cn = g_.node(call);
            if (cn.op != Op::Call || cn.aux != fid_) continue;
            if (rn.in[1] != call) continue;        // an effect after the call
            if (rn.in[0] != cn.in[0]) continue;    // control drifted
            if (cn.in[0] == start) continue;       // degenerate: no base case
            if (cn.n_in != 2 + params_.size()) continue; // arity mismatch

            // The call's only user is the return (it reads the call BOTH as
            // its value and as its memory version — the uses list holds one
            // entry per input slot, so r appears twice): killing the call
            // strands nothing else.
            const SmallVec<NodeId, 4>& us = g_.uses_of(call);
            bool only_ret = !us.empty();
            for (NodeId u : us)
                if (u != r) { only_ret = false; break; }
            if (!only_ret) continue;

            r_ = r;
            call_ = call;
            return true;
        }
        return false;
    }

    void rebuild() {
        NodeId start = g_.start();
        NodeId tail_blk = g_.node(call_).in[0];
        NodeId backedge_mem = g_.node(call_).in[1];

        // Header + memory phi. mphi starts with the region only: its entry
        // input is appended AFTER the memory rethread, otherwise
        // replace_uses_as_memory would loop mphi onto itself (its own start
        // input is a memory-phi input slot).
        NodeId hdr = g_.make(Op::Region, ty_ctrl(), {start});
        NodeId mphi = g_.make(Op::Phi, ty_mem(), {hdr});

        // Per-parameter phis; entry inputs appended after the RAUW so the
        // replacement cannot self-cycle.
        std::vector<NodeId> phis(params_.size(), kNoNode);
        for (size_t i = 0; i < params_.size(); ++i)
            phis[i] = g_.make(Op::Phi, g_.node(params_[i]).ty, {hdr});
        for (size_t i = 0; i < params_.size(); ++i)
            g_.replace_all_uses(params_[i], phis[i]);
        for (size_t i = 0; i < params_.size(); ++i)
            g_.append_input(phis[i], params_[i]);

        // Memory rethread: every memory-slot reader of the entry memory now
        // reads the loop phi. Past iterations' callee effects stay visible
        // to later iterations AND to the base-case returns (a base return
        // still reading the entry memory would revert those effects).
        g_.replace_uses_as_memory(start, mphi);
        g_.append_input(mphi, start);
        g_.append_input(mphi, backedge_mem);

        // Re-pin fixpoint. Control: the entry If executes under the header.
        // Values: any entry-pinned node that (transitively) reads a loop
        // phi or the loop memory must execute per iteration — computing it
        // once at the entry would freeze the first iteration's values.
        bool moved = true;
        while (moved) {
            moved = false;
            for (NodeId n = 0; n < g_.size(); ++n) {
                Node& nd = g_.node(n);
                if (nd.op == Op::Dead) continue;
                if (nd.in[0] != start) continue;
                bool is_entry_if = (nd.op == Op::If);
                if (!is_entry_if && !repinnable(nd.op)) continue;
                bool reads_loop = false;
                for (u8 i = 1; i < nd.n_in && !reads_loop; ++i) {
                    NodeId d = nd.in[i];
                    if (d == kNoNode) continue;
                    if (d == mphi) { reads_loop = true; break; }
                    for (NodeId phi : phis)
                        if (d == phi) { reads_loop = true; break; }
                    if (!reads_loop && d != start &&
                        g_.node(d).in[0] == hdr)
                        reads_loop = true;
                }
                if (is_entry_if || reads_loop) {
                    g_.set_input(n, 0, hdr);
                    moved = true;
                }
            }
        }

        // Backedge: the tail block becomes the header's second predecessor;
        // the phi backedge inputs are the tail call's arguments.
        g_.append_input(hdr, tail_blk);
        for (size_t i = 0; i < params_.size(); ++i)
            g_.append_input(phis[i], g_.node(call_).in[2 + i]);

        // Kill the tail call and its return; compact Stop.
        g_.kill(call_);
        g_.kill(r_);
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
    std::vector<NodeId> params_;
    NodeId r_ = kNoNode, call_ = kNoNode;
};

// ---------------------------------------------------------------------------
// Recursive self-inlining (gcc's -O3 recursive call expansion, bounded)
// ---------------------------------------------------------------------------
namespace {

// Deep-copy a function graph for use as an inline source. The copy is
// NEVER registered in the module — it exists only to be inlined back into
// `src`. Internal self-calls keep pointing at `src.fid` (the function
// being EXPANDED): after the inline, every physical call to the function
// executes one explicit level plus the inlined level, halving the dynamic
// call count of the recursion (the mechanism behind gcc's self-inlined
// tak/fib shapes). The fake fid only exists to pass inline_call's
// self-recursion guard.
FunctionGraph clone_inline_source(FunctionGraph& src) {
    FunctionGraph out;
    out.fid = src.fid ^ 0x40000000u;
    out.name = src.name;
    out.param_types = src.param_types;
    out.ret = src.ret;
    out.no_inline = true; // inline source only
    out.node_estimate = src.node_estimate;
    Graph& g = out.g;
    const Graph& sg = src.g;
    std::vector<NodeId> map(sg.size(), kNoNode);
    map[sg.start()] = g.start(); // the fresh graph already has a Start
    for (NodeId n = 0; n < sg.size(); ++n) {
        if (n == sg.start()) continue;
        const Node& nd = sg.node(n);
        NodeId shell = g.make_arr(nd.op, nd.ty, nullptr, 0, nd.sub, nd.aux);
        g.node(shell).ival = nd.ival;
        g.node(shell).fval = nd.fval;
        g.node(shell).flags = nd.flags;
        map[n] = shell;
    }
    for (NodeId n = 0; n < sg.size(); ++n) {
        if (n == sg.start()) continue;
        const Node& nd = sg.node(n);
        NodeId shell = map[n];
        for (u8 i = 0; i < nd.n_in; ++i) {
            NodeId in = nd.in[i];
            g.append_input(shell, in == kNoNode ? kNoNode : map[in]);
        }
    }
    g.set_stop(map[sg.stop()]);
    g.mark_uses_dirty();
    return out;
}

// Merge the clone's returns into ONE return through a Region + phis (the
// inliner requires a single-return callee). Returns false for shapes the
// merge cannot express (mixed void/value arities).
bool merge_clone_returns(FunctionGraph& fg) {
    Graph& g = fg.g;
    std::vector<NodeId> rets;
    for (u8 i = 0; i < g.node(g.stop()).n_in; ++i) {
        NodeId r = g.node(g.stop()).in[i];
        if (g.node(r).op == Op::Return) rets.push_back(r);
    }
    if (rets.size() <= 1) return rets.size() == 1;
    for (NodeId r : rets)
        if (g.node(r).n_in != 3) return false; // valued returns only

    NodeId region = g.make(Op::Region, ty_ctrl(), {g.node(rets[0]).in[0]});
    for (size_t k = 1; k < rets.size(); ++k)
        g.append_input(region, g.node(rets[k]).in[0]);
    NodeId mphi = g.make(Op::Phi, ty_mem(), {region});
    for (NodeId r : rets) g.append_input(mphi, g.node(r).in[1]);
    NodeId vphi = g.make(Op::Phi, g.node(g.node(rets[0]).in[2]).ty, {region});
    for (NodeId r : rets) g.append_input(vphi, g.node(r).in[2]);
    NodeId rnew = g.make(Op::Return, ty_void(), {region, mphi, vphi});
    NodeId stop = g.make(Op::Stop, ty_void(), {rnew});
    g.set_stop(stop);
    for (NodeId r : rets) g.kill(r);
    g.mark_uses_dirty();
    g.touch();
    return true;
}

} // namespace

class SelfInliner {
public:
    SelfInliner(FunctionGraph& fg, OptLevel lvl) : fg_(fg), lvl_(lvl) {}

    bool run() {
        // Recursive inlining is the peak-level expansion: body size grows
        // by (1 + sites) — a deliberate -O3-only code-size trade.
        if (lvl_ != OptLevel::O3) return false;
        if (fg_.always_inline || fg_.no_inline) return false;

        // Self-call sites, split into inner sites (inline here) and the
        // tail-position call (the loopifier owns it — inlining the tail
        // would destroy the loop conversion).
        std::vector<NodeId> sites;
        for (NodeId n = 0; n < fg_.g.size(); ++n) {
            const Node& nd = fg_.g.node(n);
            if (nd.op != Op::Call || nd.aux != fg_.fid) continue;
            bool is_tail = false;
            for (NodeId u : fg_.g.uses_of(n)) {
                const Node& un = fg_.g.node(u);
                if (un.op == Op::Return && un.n_in == 3 &&
                    un.in[2] == n && un.in[1] == n) {
                    is_tail = true;
                    break;
                }
            }
            if (!is_tail) sites.push_back(n);
        }
        if (sites.empty()) return false;
        if (sites.size() > 4) return false;           // expansion cap
        if (fg_.g.live_count() > 120) return false;   // small bodies only

        FunctionGraph clone = clone_inline_source(fg_);
        if (!merge_clone_returns(clone)) return false;

        bool any = false;
        for (NodeId s : sites)
            any |= inline_call(fg_, s, clone);
        if (any) {
            fg_.no_inline = true; // expanded bodies are not further inlined
            fg_.g.mark_uses_dirty();
            fg_.g.touch();
        }
        return any;
    }

private:
    FunctionGraph& fg_;
    OptLevel lvl_;
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
            SelfInliner si(fg, ctx.opts.level);
            changed |= si.run();
            TailCallLoopifier lf(fg);
            changed |= lf.run();
            TailCallMarker m(fg.g, fg.fid);
            changed |= m.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(TailRecursionEliminationPass, 50, "Phase 4")

} // namespace jules
