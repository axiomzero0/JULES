// Graph verifier: structural invariants from docs/son_spec.md.
// Runs after every pass in --verify mode (and in debug builds).
#include "core/son/son.h"

namespace jules {

namespace {
struct VCheck {
    Graph& g;
    SymbolTable& syms;
    Diagnostics& diag;
    bool ok = true;

    void fail(NodeId n, const std::string& msg) {
        diag.error(SourcePos{}, "verifier: node n" + std::to_string(n) + " (" +
                    op_name(g.node(n).op) + "): " + msg);
        ok = false;
    }

    bool is_live(NodeId n) const { return n != kNoNode && g.node(n).op != Op::Dead; }

    void check_node(NodeId id) {
        const Node& n = g.node(id);
        if (n.op == Op::Dead) return;

        for (u8 i = 0; i < n.n_in; ++i)
            if (n.in[i] == kNoNode)
                fail(id, "input slot " + std::to_string(i) + " is unconnected");

        switch (n.op) {
            case Op::Start:
                if (n.n_in != 0) fail(id, "Start must have no inputs");
                break;
            case Op::Region: {
                if (n.n_in == 0) { fail(id, "Region with no predecessors"); break; }
                for (u8 i = 0; i < n.n_in; ++i) {
                    if (!is_live(n.in[i])) { fail(id, "dead predecessor"); break; }
                    if (!is_block_head(g.node(n.in[i]).op))
                        fail(id, "predecessor is not a block head");
                }
                // phis aligned with preds
                for (NodeId u : g.uses_of(id)) {
                    const Node& phi = g.node(u);
                    if (phi.op != Op::Phi) continue;
                    if (phi.n_in != n.n_in + 1)
                        fail(u, "phi input count does not match region predecessors");
                    if (phi.in[0] != id) fail(u, "phi input 0 must be its region");
                }
                break;
            }
            case Op::If: {
                if (n.n_in != 2) fail(id, "If must have {ctrl, cond}");
                else {
                    if (!is_block_head(g.node(n.in[0]).op)) fail(id, "If ctrl is not a block head");
                    if (g.node(n.in[1]).ty != ty_i1()) fail(id, "If condition must be bool");
                }
                break;
            }
            case Op::IfTrue:
            case Op::IfFalse: {
                if (n.n_in != 1 || g.node(n.in[0]).op != Op::If)
                    fail(id, "projection must have exactly one If input");
                break;
            }
            case Op::Jump: {
                if (n.n_in != 1 || !is_block_head(g.node(n.in[0]).op))
                    fail(id, "Jump must take one block-head input");
                break;
            }
            case Op::Return: {
                if (n.n_in < 2 || n.n_in > 3) fail(id, "Return must have {ctrl, mem[, value]}");
                else {
                    if (!is_block_head(g.node(n.in[0]).op)) fail(id, "Return ctrl is not a block head");
                    if (!is_mem_version(g.node(n.in[1]).op, g.node(n.in[1]).ty))
                        fail(id, "Return mem input is not a memory version");
                }
                break;
            }
            case Op::Stop: {
                for (u8 i = 0; i < n.n_in; ++i)
                    if (g.node(n.in[i]).op != Op::Return)
                        fail(id, "Stop input is not a Return");
                break;
            }
            case Op::Param: {
                if (n.n_in != 1 || g.node(n.in[0]).op != Op::Start)
                    fail(id, "Param must anchor to Start");
                break;
            }
            case Op::Phi: {
                if (n.n_in < 2) { fail(id, "Phi needs a region + values"); break; }
                if (g.node(n.in[0]).op != Op::Region)
                    fail(id, "Phi input 0 must be a Region");
                else if (n.n_in != g.node(n.in[0]).n_in + 1)
                    fail(id, "Phi input count does not match region predecessors");
                break;
            }
            case Op::Bin: {
                if (n.n_in != 3) fail(id, "Bin must have {ctrl, a, b}");
                else if (g.node(n.in[1]).ty != g.node(n.in[2]).ty ||
                         g.node(n.in[1]).ty != n.ty)
                    fail(id, "Bin operand/result type mismatch");
                break;
            }
            case Op::Cmp: {
                if (n.n_in != 3) fail(id, "Cmp must have {ctrl, a, b}");
                else if (n.ty != ty_i1()) fail(id, "Cmp result must be bool");
                break;
            }
            case Op::Un:
            case Op::Cast: {
                if (n.n_in != 2) fail(id, "Un/Cast must have {ctrl, x}");
                break;
            }
            case Op::Select: {
                if (n.n_in != 4) fail(id, "Select must have {ctrl, cond, t, f}");
                break;
            }
            case Op::Load: {
                if (n.n_in != 3) { fail(id, "Load must have {ctrl, mem, addr}"); break; }
                if (!is_block_head(g.node(n.in[0]).op)) fail(id, "Load ctrl is not a block head");
                if (!is_mem_version(g.node(n.in[1]).op, g.node(n.in[1]).ty))
                    fail(id, "Load mem input is not a memory version");
                if (!ty_is_ptr(g.node(n.in[2]).ty)) fail(id, "Load address is not a pointer");
                break;
            }
            case Op::Store: {
                if (n.n_in != 4) { fail(id, "Store must have {ctrl, mem, addr, value}"); break; }
                if (n.ty != ty_mem()) fail(id, "Store result must be a memory version");
                if (!is_mem_version(g.node(n.in[1]).op, g.node(n.in[1]).ty))
                    fail(id, "Store mem input is not a memory version");
                if (!ty_is_ptr(g.node(n.in[2]).ty)) fail(id, "Store address is not a pointer");
                break;
            }
            case Op::Alloc: {
                if (n.n_in != 3) { fail(id, "Alloc must have {ctrl, mem, size}"); break; }
                if (!ty_is_ptr(n.ty)) fail(id, "Alloc result must be a pointer");
                break;
            }
            case Op::Call: {
                if (n.n_in < 2) { fail(id, "Call must have {ctrl, mem, args...}"); break; }
                if (!is_block_head(g.node(n.in[0]).op)) fail(id, "Call ctrl is not a block head");
                if (!is_mem_version(g.node(n.in[1]).op, g.node(n.in[1]).ty))
                    fail(id, "Call mem input is not a memory version");
                break;
            }
            case Op::Const:
                break;
            case Op::Dead:
                break;
        }
    }

    void check_no_dead_uses() {
        for (NodeId id = 0; id < g.size(); ++id) {
            if (g.node(id).op == Op::Dead) continue;
            for (u8 i = 0; i < g.node(id).n_in; ++i)
                if (id != g.node(id).in[i] && g.node(id).in[i] != kNoNode &&
                    g.node(g.node(id).in[i]).op == Op::Dead)
                    fail(id, "uses a killed node");
        }
    }
};
} // namespace

bool verify_graph(Graph& g, SymbolTable& syms, Diagnostics& diag) {
    VCheck v{g, syms, diag, true};
    for (NodeId id = 0; id < g.size(); ++id) v.check_node(id);
    v.check_no_dead_uses();
    return v.ok;
}

} // namespace jules
