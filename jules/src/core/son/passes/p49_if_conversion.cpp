// Pass 49 — IfConversion (Phase 4)
//
// Converts effect-free if-diamonds into Select nodes (the specific
// predication pattern with its own legality checks):
//   * both branch arms carry no memory effects and no nested control
//   * each arm feeds exactly one merge region through value phis
//   * arm values are pure and their inputs are available before the branch
// The merge is dissolved; the phi becomes Select(cond, tval, fval) pinned
// to the branching block. Enables branchless code and feeds pass 17.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {
class IfConverter {
public:
    IfConverter(Graph& g, DomTree& dom) : g_(g), dom_(dom) {}

    bool run() {
        for (u32 round = 0; round < kMaxRounds; ++round) {
            bool this_round = false;
            for (NodeId id = 0; id < g_.size(); ++id) this_round |= convert(id);
            changed_ |= this_round;
            if (!this_round) break;
        }
        return changed_;
    }

private:
    static constexpr u32 kMaxRounds = 4;

    bool convert(NodeId if_id) {
        const Node& ifn = g_.node(if_id);
        if (ifn.op != Op::If) return false;
        NodeId h = ifn.in[0]; // branching block

        NodeId tproj = kNoNode, fproj = kNoNode;
        for (NodeId u : g_.uses_of(if_id)) {
            if (g_.node(u).op == Op::IfTrue) tproj = u;
            if (g_.node(u).op == Op::IfFalse) fproj = u;
        }
        if (tproj == kNoNode || fproj == kNoNode) return false;

        // both projections must fall into the SAME merge region
        NodeId region = common_merge(tproj, fproj);
        if (region == kNoNode) return false;
        const Node& r = g_.node(region);
        if (r.n_in != 2) return false;

        // arms must be effect-free and control-free
        if (!clean_arm(tproj, region) || !clean_arm(fproj, region)) return false;

        // find value phis at the merge (non-mem)
        std::vector<NodeId> vphis;
        for (NodeId u : g_.uses_of(region)) {
            const Node& un = g_.node(u);
            if (un.op == Op::Phi && un.ty != ty_mem()) vphis.push_back(u);
        }
        if (vphis.empty()) return false;

        // input index per projection
        u8 ti = 0, fi = 1;
        for (u8 i = 0; i < r.n_in; ++i) {
            if (r.in[i] == tproj) ti = i;
            if (r.in[i] == fproj) fi = i;
        }

        // convert every value phi into a Select; arm values must be pure and
        // available before the branch (inputs dominate the branching block)
        for (NodeId phi : vphis) {
            const Node& pn = g_.node(phi);
            NodeId tv = pn.in[ti + 1];
            NodeId fv = pn.in[fi + 1];
            if (!arm_value_ok(tv, tproj, h) || !arm_value_ok(fv, fproj, h)) return false;
            NodeId sel = g_.make(Op::Select, pn.ty, {h, ifn.in[1], tv, fv});
            g_.replace_all_uses(phi, sel);
            g_.kill(phi);
        }

        // trivial memphi: both inputs identical -> forward
        for (NodeId u : g_.uses_of(region)) {
            const Node& un = g_.node(u);
            if (un.op != Op::Phi || un.ty != ty_mem()) continue;
            if (un.in[1] == un.in[2]) {
                g_.replace_all_uses(u, un.in[1]);
                g_.kill(u);
            } else {
                return false; // memory merge of different versions: unsafe to dissolve
            }
        }

        // dissolve the merge: repin R's contents to H, repoint R's region uses
        const SmallVec<NodeId, 4> region_users = g_.uses_of(region);
        for (NodeId u : region_users) {
            Node& un = g_.node(u);
            if (un.op == Op::Region) {
                for (u8 i = 0; i < un.n_in; ++i)
                    if (un.in[i] == region) g_.set_input(u, i, h);
            } else if (un.in[0] == region) {
                g_.set_input(u, 0, h);
            }
        }
        g_.kill(region);
        g_.kill(tproj);
        g_.kill(fproj);
        g_.kill(if_id);
        return true;
    }

    NodeId common_merge(NodeId tproj, NodeId fproj) {
        NodeId rt = kNoNode, rf = kNoNode;
        for (NodeId u : g_.uses_of(tproj))
            if (g_.node(u).op == Op::Region) {
                if (rt != kNoNode) return kNoNode; // multiple merges
                rt = u;
            }
        for (NodeId u : g_.uses_of(fproj))
            if (g_.node(u).op == Op::Region) {
                if (rf != kNoNode) return kNoNode;
                rf = u;
            }
        if (rt != kNoNode && rf != kNoNode && rt == rf) return rt;
        return kNoNode;
    }

    bool clean_arm(NodeId proj, NodeId region) {
        for (NodeId u : g_.uses_of(proj)) {
            const Node& un = g_.node(u);
            if (un.in[0] != proj) continue; // used as region pred etc.
            switch (un.op) {
                case Op::Region:
                case Op::Jump:
                    return false; // nested control in the arm
                case Op::Store:
                case Op::Call:
                case Op::Alloc:
                case Op::Load:
                    return false; // effects (loads are memory reads: excluded)
                case Op::Return:
                    return false;
                default:
                    continue; // pure computation feeding the phi
            }
        }
        (void)region;
        return true;
    }

    // arm value must be pure with all inputs available before the branch
    bool arm_value_ok(NodeId v, NodeId arm_proj, NodeId h) {
        if (v == kNoNode || g_.node(v).op == Op::Dead) return false;
        const Node& vn = g_.node(v);
        if (!is_pure_op(vn.op)) return false;
        if (is_block_head(vn.op)) return false;
        NodeId vb = vn.in[0];
        if (vb == arm_proj) {
            // defined in the arm: its own inputs must dominate the branch
            for (u8 i = 1; i < vn.n_in; ++i) {
                NodeId d = vn.in[i];
                if (d == kNoNode || g_.node(d).op == Op::Dead) continue;
                if (is_block_head(g_.node(d).op)) {
                    if (!dom_.dominates(d, h)) return false;
                } else if (!dom_.dominates(g_.node(d).in[0], h)) {
                    return false;
                }
            }
            // move it out of the arm (it is pure)
            g_.set_input(v, 0, h);
            return true;
        }
        // defined before the branch already
        return dom_.dominates(vb, h) || vb == h;
    }

    Graph& g_;
    DomTree& dom_;
    bool changed_ = false;
};
} // namespace

class IfConversionPass : public Pass {
public:
    const char* name() const override { return "IfConversion"; }
    int order() const override { return 49; }
    const char* phase_name() const override { return "Phase 4: Loop Analysis & Transforms"; }
    AnalysisMask required() const override { return static_cast<AnalysisMask>(AnalysisKind::Dominators); }
    AnalysisMask invalidated() const override {
        return AnalysisKind::Dominators | AnalysisKind::LoopInfo;
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            IfConverter c(fg.g, ctx.analysis.doms(fg));
            changed |= c.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(IfConversionPass, 49, "Phase 4")

} // namespace jules
