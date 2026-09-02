#include "core/son/passes/inline.h"

namespace jules {

namespace {
constexpr u32 kDefaultInlineBudget = 400; // cloned-node budget per function

class Inliner {
public:
    Inliner(FunctionGraph& caller, FunctionGraph& callee)
        : c_(caller), g_(callee.g) {}

    bool run(NodeId call) {
        // single-return requirement
        NodeId ret = kNoNode;
        u32 returns = 0;
        for (u8 i = 0; i < g_.node(g_.stop()).n_in; ++i) {
            NodeId r = g_.node(g_.stop()).in[i];
            if (g_.node(r).op == Op::Return) {
                ++returns;
                ret = r;
            }
        }
        if (returns != 1 || ret == kNoNode) return false;
        if (g_.node(ret).n_in < 2) return false;

        call_ = call;
        NodeId B = c_.g.node(call).in[0];       // call site block
        call_mem_ = c_.g.node(call).in[1];      // caller memory at the call

        j_entry_ = c_.g.make(Op::Jump, ty_ctrl(), {B});

        // params -> arguments
        for (NodeId id = 0; id < g_.size(); ++id) {
            const Node& n = g_.node(id);
            if (n.op == Op::Param) {
                u32 idx = n.aux;
                NodeId arg = (2 + idx < c_.g.node(call).n_in) ? c_.g.node(call).in[2 + idx]
                                                              : kNoNode;
                map_.insert(id, arg);
            }
        }

        NodeId exit_val = clone(g_.node(ret).n_in == 3 ? g_.node(ret).in[2] : kNoNode);
        NodeId exit_mem = clone(g_.node(ret).in[1]);
        NodeId exit_blk = g_.node(ret).in[0];
        NodeId exit_ctrl = clone_ctrl(exit_blk);

        // resume point
        j_exit_ = (exit_blk == g_.start()) ? j_entry_
                                           : c_.g.make(Op::Jump, ty_ctrl(), {exit_ctrl});

        // Rewire the caller.
        // 1) memory users of the call -> callee exit memory
        c_.g.replace_uses_as_memory(call, exit_mem);
        // 2) value users -> callee exit value
        if (exit_val != kNoNode) c_.g.replace_all_uses(call, exit_val);
        // 3) repin nodes pinned at B that are NOT part of the call's memory
        //    ancestry to the resume block
        FlatMap<NodeId, bool> ancestry;
        for (NodeId m = call_mem_; m != kNoNode && c_.g.node(m).op != Op::Dead;) {
            ancestry.insert(m, true);
            const Node& mn = c_.g.node(m);
            m = (mn.op == Op::Phi) ? kNoNode : (mn.n_in > 1 ? mn.in[1] : kNoNode);
        }
        const SmallVec<NodeId, 4> pinned = c_.g.uses_of(B);
        for (NodeId u : pinned) {
            if (u == call || u == j_entry_) continue;
            Node& un = c_.g.node(u);
            if (un.op == Op::Dead || un.n_in == 0 || un.in[0] != B) continue;
            if (un.op == Op::Jump && un.in[0] == B && u != j_entry_) {
                // a jump out of B stays (it precedes the call structurally)
                if (ancestry.contains(u)) continue;
            }
            if (ancestry.contains(u)) continue;
            c_.g.set_input(u, 0, j_exit_);
        }
        c_.g.kill(call);
        return true;
    }

private:
    // clone a data/memory/control node from the callee into the caller
    NodeId clone(NodeId n) {
        if (n == kNoNode) return kNoNode;
        if (const NodeId* m = map_.find(n)) return *m;
        const Node& src = g_.node(n);
        if (src.op == Op::Start) {
            // memory version entry
            map_.insert(n, call_mem_);
            return call_mem_;
        }
        if (src.op == Op::Param) {
            NodeId arg = kNoNode; // filled in run(); safety fallback here
            if (const NodeId* m = map_.find(n)) arg = *m;
            map_.insert(n, arg);
            return arg;
        }

        // shell first (cycle-safe), then fill inputs
        NodeId shell = c_.g.make_arr(src.op, src.ty, nullptr, 0, src.sub, src.aux);
        c_.g.node(shell).ival = src.ival;
        c_.g.node(shell).fval = src.fval;
        c_.g.node(shell).flags = src.flags;
        map_.insert(n, shell);

        for (u8 i = 0; i < src.n_in; ++i) {
            NodeId in = src.in[i];
            NodeId mapped;
            if (in == g_.start()) {
                mapped = (i == 0) ? j_entry_ : call_mem_;
            } else {
                mapped = clone(in);
            }
            if (i == 0) c_.g.set_input(shell, 0, mapped);
            else c_.g.append_input(shell, mapped);
        }
        return shell;
    }

    // block-head clone (control side); Start maps to the entry jump
    NodeId clone_ctrl(NodeId n) {
        if (n == g_.start()) return j_entry_;
        return clone(n);
    }

    FunctionGraph& c_;
    Graph& g_;
    NodeId call_ = kNoNode;
    NodeId call_mem_ = kNoNode;
    NodeId j_entry_ = kNoNode;
    NodeId j_exit_ = kNoNode;
    FlatMap<NodeId, NodeId> map_;
};
} // namespace

bool inline_call(FunctionGraph& caller, NodeId call, FunctionGraph& callee) {
    if (caller.fid == callee.fid) return false; // self-recursion: pass 79 policy
    Inliner in(caller, callee);
    return in.run(call);
}

bool inline_recursion_ok(FnId caller, FnId callee) { return caller != callee; }

u32 inline_budget_default() { return kDefaultInlineBudget; }

} // namespace jules
