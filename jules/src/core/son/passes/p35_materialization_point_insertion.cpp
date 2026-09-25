// Pass 35 — MaterializationPointInsertion (Phase 3: Escape & Allocation
// Analysis)
//
// Emits the materialization recipes for pass 33's sunk allocations into
// the linear module's deopt metadata — the table pass 89's manifest
// serializes. A sunk allocation IS its own materialization point: the
// allocation executes exactly at the arm entry where pass 33 re-pinned
// it, so the recipe records [function, arm block, size] and the deopt
// runtime can reconstruct "object not yet materialized on the paths that
// bypass the arm".
//
// The full virtual-object design (objects kept scalar on hot paths with
// field-value recipes reconstructed at join blocks) needs an IR
// extension this milestone does not carry; the recipe format below is
// the manifest-side contract that lands now, exercised end-to-end by
// the sunk allocations that exist.
#include "core/codegen/linear.h"
#include "core/son/passes/pass_utils.h"

namespace jules {

class MaterializationPointInsertionPass : public Pass {
public:
    const char* name() const override { return "MaterializationPointInsertion"; }
    int order() const override { return 35; }
    const char* phase_name() const override {
        return "Phase 3: Escape & Allocation Analysis";
    }
    Stage stage() const override { return Stage::Linear; }
    bool run(PassContext& ctx) override {
        if (!ctx.lin) return false;
        u32 before = static_cast<u32>(ctx.lin->materializations.size());
        for (LFunction& lf : ctx.lin->fns) {
            const FunctionGraph* fg = ctx.mod.find_fn(lf.fid);
            if (!fg) continue;
            for (const LBlock& b : lf.blocks) {
                // The sunk Alloc is scheduled into the arm's block; find
                // it by walking the block's nodes (the flag survives the
                // passes between 33 and here — anything that consumed the
                // allocation simply leaves no recipe, which is the
                // honest degradation).
                for (NodeId n : b.nodes) {
                    const Node& nd = fg->g.node(n);
                    if (nd.op != Op::Alloc) continue;
                    if ((nd.flags & kFlagMaterialized) == 0) continue;
                    i64 size = -1; // dynamic-size recipe marker
                    if (fg->g.node(nd.in[2]).op == Op::Const)
                        size = fg->g.node(nd.in[2]).ival;
                    LinearModule::Materialization m;
                    m.fn = lf.fid;
                    m.block = b.index;
                    m.size = size;
                    m.how = "sunk";
                    ctx.lin->materializations.push_back(m);
                }
            }
        }
        return ctx.lin->materializations.size() != before;
    }
};

JULES_REGISTER_PASS(MaterializationPointInsertionPass, 35, "Phase 3")

} // namespace jules
