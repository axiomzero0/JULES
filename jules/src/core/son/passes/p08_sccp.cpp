// Pass 8 — SparseConditionalConstantPropagation (Phase 1)
//
// Cliff Click-style lattice SCCP over the SoN graph:
//   * lattice per value node: TOP (unvisited) / CONST / BOTTOM (overdefined)
//   * control executability flows through block heads; only edges the
//     lattice proves taken become executable
//   * phis meet over inputs arriving from executable predecessors only
// Rewrite phase: constants replace pure ops, dead predecessors are removed
// from regions (phis realigned), constant branches lose their dead
// projection. Separate from ConstantFolding because this propagates through
// control edges.
#include "core/son/passes/pass_utils.h"

namespace jules {

namespace {

enum class Lat : u8 { Top, Const, Bottom };

struct LatVal {
    Lat kind = Lat::Top;
    ConstVal v;
};

class Sccp {
public:
    explicit Sccp(Graph& g) : g_(g) {}

    bool run() {
        // Seed every Const node's lattice value up front: constants are
        // block-independent facts. The graph builder/inliner pins Const nodes
        // to the block where they were materialized — including blocks that
        // are not (yet) executable. Without pre-seeding, a pure op in a live
        // block whose Const operand sits in a not-yet-exec block stalls at
        // TOP forever: the branch never resolves, neither projection is
        // marked executable, and the optimistic phi lattice values (entry
        // inputs only) get frozen and rewritten as constants — decapitating
        // loops. Pre-seeding matches classic Click-style SCCP where constants
        // are known globally.
        for (NodeId id = 0; id < g_.size(); ++id) {
            if (g_.node(id).op == Op::Const) {
                LatVal v;
                v.kind = Lat::Const;
                const_of(g_, id, v.v);
                set(id, v);
            }
        }
        mark_exec(g_.start());
        drain();
        // Safety net: an If whose condition is STILL TOP at fixpoint could not
        // be resolved (stalled operand chain, not a proof). Treat it as
        // BOTTOM — both projections executable — so downstream regions never
        // lose all predecessors to an evaluation artifact. Ifs with Const
        // conditions keep their single taken projection (dead-branch
        // elimination); BOTTOM conditions already marked both during drain.
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op != Op::If) continue;
            if (get(n.in[1]).kind != Lat::Top) continue;
            NodeId tproj = kNoNode, fproj = kNoNode;
            for (NodeId u : g_.uses_of(id)) {
                Op uo = g_.node(u).op;
                if (uo == Op::IfTrue) tproj = u;
                if (uo == Op::IfFalse) fproj = u;
            }
            if (tproj != kNoNode && fproj != kNoNode) {
                mark_exec(tproj);
                mark_exec(fproj);
            }
        }
        return rewrite();
    }

private:
    // ---- executability -----------------------------------------------------
    void mark_exec(NodeId b) {
        if (b == kNoNode || exec_.contains(b)) return;
        if (!is_block_head(g_.node(b).op)) return;
        exec_.insert(b, true);
        exec_work_.push_back(b);
    }

    void on_block_exec(NodeId b) {
        // schedule every node pinned to this block
        for (NodeId u : g_.uses_of(b)) {
            const Node& un = g_.node(u);
            if (un.n_in > 0 && un.in[0] == b) work_.push_back(u);
        }
        // regions having b as a predecessor become executable; their phis
        // re-meet; jumps out of b become executable
        for (NodeId u : g_.uses_of(b)) {
            const Node& un = g_.node(u);
            if (un.op == Op::Region) {
                mark_exec(u);
                for (NodeId w : g_.uses_of(u)) {
                    if (g_.node(w).op == Op::Phi) work_.push_back(w);
                }
            } else if (un.op == Op::Jump && un.in[0] == b) {
                mark_exec(u);
            }
        }
    }

    void drain() {
        u32 steps = 0;
        while ((!exec_work_.empty() || !work_.empty()) && ++steps <= kStepLimit) {
            if (!exec_work_.empty()) {
                NodeId b = exec_work_.back();
                exec_work_.pop_back();
                on_block_exec(b);
                continue;
            }
            NodeId n = work_.back();
            work_.pop_back();
            process(n);
        }
    }

    // ---- lattice ------------------------------------------------------------
    LatVal get(NodeId n) {
        if (const LatVal* l = lat_.find(n)) return *l;
        return LatVal{}; // Top
    }

    void set(NodeId n, const LatVal& v) {
        LatVal old = get(n);
        bool changed = false;
        if (old.kind == Lat::Top) changed = (v.kind != Lat::Top);
        else if (old.kind == Lat::Const) {
            if (v.kind == Lat::Bottom) changed = true;
            else if (v.kind == Lat::Const) {
                changed = (old.v.is_fp != v.v.is_fp) || (v.v.is_fp ? old.v.fv != v.v.fv
                                                                    : old.v.iv != v.v.iv);
            }
        }
        if (!changed) return;
        lat_.insert(n, v);
        for (NodeId u : g_.uses_of(n)) work_.push_back(u);
    }

    static LatVal meet(const LatVal& a, const LatVal& b) {
        if (a.kind == Lat::Top) return b;
        if (b.kind == Lat::Top) return a;
        if (a.kind == Lat::Bottom || b.kind == Lat::Bottom) return LatVal{Lat::Bottom, {}};
        if (a.v.is_fp != b.v.is_fp) return LatVal{Lat::Bottom, {}};
        if (a.v.is_fp ? a.v.fv == b.v.fv : a.v.iv == b.v.iv) return a;
        return LatVal{Lat::Bottom, {}};
    }

    void process(NodeId n) {
        Node& nd = g_.node(n);
        if (nd.op == Op::Dead) return;

        switch (nd.op) {
            case Op::Const: {
                LatVal v;
                v.kind = Lat::Const;
                const_of(g_, n, v.v);
                set(n, v);
                return;
            }
            case Op::Param: {
                set(n, LatVal{Lat::Bottom, {}});
                return;
            }
            case Op::Bin: {
                LatVal a = get(nd.in[1]), b = get(nd.in[2]);
                if (a.kind == Lat::Top || b.kind == Lat::Top) return;
                if (a.kind == Lat::Bottom || b.kind == Lat::Bottom) {
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                ConstVal out;
                if (!eval_bin_const(static_cast<BinOp>(nd.sub), a.v, b.v, out)) {
                    set(n, LatVal{Lat::Bottom, {}}); // e.g. runtime div-by-zero
                    return;
                }
                LatVal r;
                r.kind = Lat::Const;
                r.v = out;
                set(n, r);
                return;
            }
            case Op::Cmp: {
                LatVal a = get(nd.in[1]), b = get(nd.in[2]);
                if (a.kind == Lat::Top || b.kind == Lat::Top) return;
                if (a.kind == Lat::Bottom || b.kind == Lat::Bottom) {
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                ConstVal out;
                if (!eval_cmp_const(static_cast<CmpOp>(nd.sub), a.v, b.v, out)) {
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                LatVal r;
                r.kind = Lat::Const;
                r.v = out;
                set(n, r);
                return;
            }
            case Op::Un: {
                LatVal a = get(nd.in[1]);
                if (a.kind == Lat::Top) return;
                if (a.kind == Lat::Bottom) { set(n, LatVal{Lat::Bottom, {}}); return; }
                ConstVal out;
                if (!eval_un_const(static_cast<UnOp>(nd.sub), a.v, out)) {
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                LatVal r;
                r.kind = Lat::Const;
                r.v = out;
                set(n, r);
                return;
            }
            case Op::Cast: {
                LatVal a = get(nd.in[1]);
                if (a.kind == Lat::Top) return;
                if (a.kind == Lat::Bottom) { set(n, LatVal{Lat::Bottom, {}}); return; }
                ConstVal out;
                if (!eval_cast_const(static_cast<CastOp>(nd.sub), a.v, nd.ty, out)) {
                    set(n, LatVal{Lat::Bottom, {}});
                    return;
                }
                LatVal r;
                r.kind = Lat::Const;
                r.v = out;
                set(n, r);
                return;
            }
            case Op::Select: {
                LatVal c = get(nd.in[1]);
                if (c.kind == Lat::Const) {
                    set(n, get(c.v.iv != 0 ? nd.in[2] : nd.in[3]));
                    return;
                }
                if (c.kind == Lat::Bottom) {
                    set(n, meet(get(nd.in[2]), get(nd.in[3])));
                }
                return;
            }
            case Op::Phi: {
                NodeId region = nd.in[0];
                const Node& r = g_.node(region);
                LatVal acc;
                acc.kind = Lat::Top;
                bool any_exec = false;
                for (u8 i = 0; i < r.n_in; ++i) {
                    if (!exec_.contains(r.in[i])) continue;
                    any_exec = true;
                    acc = meet(acc, get(nd.in[i + 1]));
                    if (acc.kind == Lat::Bottom) break;
                }
                if (any_exec) set(n, acc);
                return;
            }
            case Op::If: {
                LatVal c = get(nd.in[1]);
                if (c.kind == Lat::Top) return; // wait for the condition
                // find this If's projections
                NodeId tproj = kNoNode, fproj = kNoNode;
                for (NodeId u : g_.uses_of(n)) {
                    if (g_.node(u).op == Op::IfTrue) tproj = u;
                    if (g_.node(u).op == Op::IfFalse) fproj = u;
                }
                if (tproj == kNoNode || fproj == kNoNode) return;
                if (c.kind == Lat::Const) {
                    mark_exec(c.v.iv != 0 ? tproj : fproj);
                } else {
                    mark_exec(tproj);
                    mark_exec(fproj);
                }
                return;
            }
            case Op::Load:
            case Op::Call:
            case Op::Alloc:
                set(n, LatVal{Lat::Bottom, {}});
                return;
            default:
                return; // control / memory nodes carry no lattice value
        }
    }

    // ---- rewrite ---------------------------------------------------------------
    bool rewrite() {
        bool changed = false;

        // 1) constant pure nodes -> Const nodes
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op == Op::Dead || n.op == Op::Const) continue;
            if (!is_pure_op(n.op) && n.op != Op::Phi) continue;
            const LatVal* l = lat_.find(id);
            if (!l || l->kind != Lat::Const) continue;
            // only rewrite nodes in executable blocks
            if (n.n_in > 0 && !exec_.contains(n.in[0]) && is_block_head(g_.node(n.in[0]).op))
                continue;
            NodeId c = make_const_node(g_, block_pin(g_, id), l->v);
            g_.replace_all_uses(id, c);
            g_.kill(id);
            changed = true;
        }

        // 2) remove non-executable predecessors from regions; realign phis
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op != Op::Region) continue;
            u8 keep = 0;
            for (u8 i = 0; i < n.n_in; ++i)
                if (exec_.contains(n.in[i])) n.in[keep++] = n.in[i];
            if (keep == n.n_in) continue; // nothing dead
            g_.touch(); // pred trim is a real change
            if (keep == 0) {
                // unreachable merge: kill region + phis; the pinned subgraph
                // dies in the phase-5 cascade below
                const SmallVec<NodeId, 4> users = g_.uses_of(id);
                for (NodeId u : users) {
                    if (g_.node(u).op == Op::Phi) {
                        g_.kill(u);
                        changed = true;
                    }
                }
                g_.kill(id);
                dead_ctrl_.insert(id, true);
                changed = true;
                continue;
            }
            u8 removed = static_cast<u8>(n.n_in - keep);
            // realign phis: drop input (pred_index + 1) for removed preds.
            // Recompute mapping on the ORIGINAL order, so do it before trimming.
            const SmallVec<NodeId, 4> users = g_.uses_of(id);
            for (NodeId u : users) {
                Node& phi = g_.node(u);
                if (phi.op != Op::Phi) continue;
                u8 pk = 1;
                for (u8 i = 0; i < n.n_in; ++i) {
                    if (exec_.contains(n.in[i])) phi.in[pk++] = phi.in[i + 1];
                }
                phi.n_in = pk;
            }
            n.n_in = keep;
            changed = true;
            (void)removed;
        }

        // 3) constant branches: kill the dead projection
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op != Op::If) continue;
            const LatVal* c = lat_.find(n.in[1]);
            if (!c || c->kind != Lat::Const) continue;
            for (NodeId u : g_.uses_of(id)) {
                Op uo = g_.node(u).op;
                bool taken = c->v.iv != 0;
                if ((uo == Op::IfTrue && !taken) || (uo == Op::IfFalse && taken)) {
                    if (!exec_.contains(u)) {
                        g_.kill(u);
                        dead_ctrl_.insert(u, true);
                        changed = true;
                    }
                }
            }
        }

        // 4) single-input phis produced by pred removal
        for (NodeId id = 0; id < g_.size(); ++id) {
            Node& n = g_.node(id);
            if (n.op != Op::Phi || n.n_in != 2) continue;
            g_.replace_all_uses(id, n.in[1]);
            g_.kill(id);
            changed = true;
        }

        // 5) cascade: kill the subgraph pinned to removed control. Phases 2-3
        // realigned phis and trimmed regions, so live nodes no longer
        // reference the dead subgraph as data; what remains pinned under
        // killed block heads (stores, loads, consts, jumps, unreachable
        // returns) must die with its control, otherwise the graph carries
        // uses-of-killed-nodes until the next DCE run — which never comes
        // within the same pipeline sweep.
        if (!dead_ctrl_.empty()) {
            bool again = true;
            while (again) {
                again = false;
                for (NodeId id = 0; id < g_.size(); ++id) {
                    Node& n = g_.node(id);
                    if (n.op == Op::Dead || n.op == Op::Stop) continue;
                    if (n.n_in == 0) continue;
                    if (!dead_ctrl_.contains(n.in[0])) continue;
                    g_.kill(id);
                    if (is_block_head(n.op)) dead_ctrl_.insert(id, true);
                    changed = true;
                    again = true;
                }
            }
            // Stop compaction: returns killed under dead control must leave
            // the stop list, or Stop keeps a dead input forever.
            Node& stop = g_.node(g_.stop());
            if (stop.op == Op::Stop) {
                u8 keep = 0;
                for (u8 i = 0; i < stop.n_in; ++i)
                    if (g_.node(stop.in[i]).op != Op::Dead) stop.in[keep++] = stop.in[i];
                if (keep != stop.n_in) {
                    stop.n_in = keep;
                    g_.touch();
                    changed = true;
                }
            }
        }
        (void)0;
        return changed;
    }

    static constexpr u32 kStepLimit = 100000; // named worklist bound

    Graph& g_;
    FlatMap<NodeId, LatVal> lat_;
    FlatMap<NodeId, bool> exec_;
    FlatMap<NodeId, bool> dead_ctrl_; // killed control nodes (cascade roots)
    std::vector<NodeId> work_;
    std::vector<NodeId> exec_work_;
};
} // namespace

class SparseConditionalConstantPropagationPass : public Pass {
public:
    const char* name() const override { return "SparseConditionalConstantPropagation"; }
    int order() const override { return 8; }
    const char* phase_name() const override { return "Phase 1: Value & Scalar Optimization"; }
    AnalysisMask invalidated() const override {
        return static_cast<AnalysisMask>(AnalysisKind::Dominators | AnalysisKind::LoopInfo);
    }
    bool run(PassContext& ctx) override {
        bool changed = false;
        for (FunctionGraph& fg : ctx.mod.fns) {
            Sccp s(fg.g);
            changed |= s.run();
        }
        return changed;
    }
};

JULES_REGISTER_PASS(SparseConditionalConstantPropagationPass, 8, "Phase 1")

} // namespace jules
