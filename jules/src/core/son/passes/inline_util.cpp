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

        // Rewire the caller. The call is BOTH a value and a memory version,
        // and its users split by slot kind: effect nodes (and Returns/Loads)
        // read it in slot 1 as MEMORY, memory phis read it in any value
        // slot, everything else reads its VALUE. Rewriting with
        // replace_all_uses or replace_uses_as_memory alone clobbers the
        // other kind — Cast/Un/Bin keep their FIRST DATA OPERAND in slot 1,
        // the same index effect nodes use for memory. A blind slot-1
        // rewrite mapped a `Cast(trunc, call)`'s value read onto the
        // callee's exit ALLOC (printed the heap pointer instead of the
        // computed result; u64 `as i64` of a call result).
        {
            const SmallVec<NodeId, 4> users = c_.g.uses_of(call);
            for (NodeId u : users) {
                Node& un = c_.g.node(u);
                bool is_mem_phi = (un.op == Op::Phi && un.ty == ty_mem());
                bool mem_slot1_user = (un.op == Op::Call || un.op == Op::Store ||
                                       un.op == Op::Load || un.op == Op::Alloc ||
                                       un.op == Op::Return);
                for (u8 i = 0; i < un.n_in; ++i) {
                    if (un.in[i] != call) continue;
                    bool as_memory = is_mem_phi || (i == 1 && mem_slot1_user);
                    if (!as_memory && exit_val == kNoNode) continue; // void call
                    un.in[i] = as_memory ? exit_mem : exit_val;
                }
            }
            c_.g.mark_uses_dirty();
            c_.g.touch();
        }
        // 3) repin nodes pinned at B that execute AFTER the inlined body to
        //    the resume block.
        //
        //    The classification is a full backward closure over ALL inputs
        //    (control, memory, data) of the call: every node the call
        //    transitively depends on must stay at B (or above). The memory
        //    chain alone is NOT sufficient: reads (Load) carry no memory
        //    version, so a load pinned at B that feeds the call-site's own
        //    users was misclassified as "after the call" and stranded its
        //    consumers — a use-before-def miscompile at low levels where
        //    no post-inline folding repairs the graph (-O0/-Og).
        //
        //    Control nodes (Jump/If) keep the original always-move rule:
        //    they are ordered by control flow, not data dependencies, and
        //    moving them preserves the original block topology. Phis are
        //    structural (pinned to their region) and never move.
        FlatMap<NodeId, bool> before;
        {
            SmallVec<NodeId, 32> stack;
            for (u8 i = 0; i < c_.g.node(call).n_in; ++i)
                stack.push_back(c_.g.node(call).in[i]);
            while (!stack.empty()) {
                NodeId n = stack.back();
                stack.pop_back();
                if (n == kNoNode || before.contains(n)) continue;
                before.insert(n, true);
                const Node& nd = c_.g.node(n);
                for (u8 i = 0; i < nd.n_in; ++i)
                    if (nd.in[i] != kNoNode) stack.push_back(nd.in[i]);
            }
        }
        const SmallVec<NodeId, 4> pinned = c_.g.uses_of(B);
        for (NodeId u : pinned) {
            if (u == call || u == j_entry_) continue;
            Node& un = c_.g.node(u);
            if (un.op == Op::Dead || un.n_in == 0 || un.in[0] != B) continue;
            if (un.op == Op::Phi) continue;        // structural: block-owned
            bool controlish = un.op == Op::Jump || un.op == Op::If;
            if (!controlish && before.contains(u)) continue; // feeds the call
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
