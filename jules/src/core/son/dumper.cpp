// Deterministic graph dumpers: plain text (for --emit-ir / golden tests) and
// DOT (for --emit-dot, tools/ir_dump).
#include "core/son/son.h"

#include <cstdio>
#include <sstream>

namespace jules {

namespace {

std::string node_detail(const Graph& g, SymbolTable& syms, NodeId id,
                         const std::vector<SymbolId>* fn_syms) {
    const Node& n = g.node(id);
    std::ostringstream os;
    switch (n.op) {
        case Op::Const:
            if (ty_is_float(n.ty)) os << " value=" << n.fval;
            else os << " value=" << n.ival;
            break;
        case Op::Param:
            os << " idx=" << n.aux;
            break;
        case Op::Call: {
            os << " fn=";
            // aux is a FnId = index into the module function list. Resolve
            // through fn_syms (FnId -> SymbolId) when provided: symbol table
            // index 0 is reserved for "" and is NOT a function, so treating
            // aux as a raw symbol index mislabels every call by one.
            if (n.aux == kFnPrint) os << "print";
            else if (n.aux == kFnFree) os << "free";
            else if (n.aux == kFnPgoBump) os << "pgobump#" << n.ival;
            else if (fn_syms && n.aux < fn_syms->size() && (*fn_syms)[n.aux] != kNoSymbol)
                os << syms.name((*fn_syms)[n.aux]);
            else if (n.aux != kNoFn) os << "fn" << n.aux;
            break;
        }
        case Op::Bin:
            os << " op=" << bin_name(static_cast<BinOp>(n.sub));
            break;
        case Op::Cmp:
            os << " op=" << cmp_name(static_cast<CmpOp>(n.sub));
            break;
        case Op::Un:
            os << " op=" << un_name(static_cast<UnOp>(n.sub));
            break;
        case Op::Cast:
            os << " op=" << cast_name(static_cast<CastOp>(n.sub));
            if (static_cast<CastOp>(n.sub) == CastOp::Extract) os << " lane=" << n.aux;
            break;
        case Op::Alloc:
            if (n.flags & kFlagStackPromoted) os << " [stack-promoted]";
            break;
        default: break;
    }
    if (is_value_op(n.op) && n.ty != ty_none()) os << " : " << ty_name(n.ty);
    return os.str();
}

} // namespace

std::string dump_graph_text(const Graph& g, SymbolTable& syms,
                            const std::vector<SymbolId>* fn_syms) {
    std::ostringstream os;
    os << "graph " << g.live_count() << " live / " << g.size() << " nodes\n";
    for (NodeId id = 0; id < g.size(); ++id) {
        const Node& n = g.node(id);
        if (n.op == Op::Dead) continue;
        os << "n" << id << ": " << op_name(n.op) << node_detail(g, syms, id, fn_syms) << " in=[";
        for (u8 i = 0; i < n.n_in; ++i) {
            if (i) os << ", ";
            os << "n" << n.in[i];
        }
        os << "]\n";
    }
    return os.str();
}

std::string dump_graph_dot(const Graph& g, SymbolTable& syms, const char* fn_name,
                           const std::vector<SymbolId>* fn_syms) {
    std::ostringstream os;
    os << "digraph \"" << fn_name << "\" {\n";
    os << "  rankdir=BT;\n  node [shape=record, fontname=\"Helvetica\"];\n";
    for (NodeId id = 0; id < g.size(); ++id) {
        const Node& n = g.node(id);
        if (n.op == Op::Dead) continue;
        os << "  n" << id << " [label=\"{" << op_name(n.op) << "|#" << id
           << node_detail(g, syms, id, fn_syms) << "}\"];\n";
    }
    for (NodeId id = 0; id < g.size(); ++id) {
        const Node& n = g.node(id);
        if (n.op == Op::Dead) continue;
        for (u8 i = 0; i < n.n_in; ++i) {
            if (n.in[i] == kNoNode) continue;
            const char* style = "color=black";
            if (is_control_op(n.op) && i == 0 && is_control_op(g.node(n.in[i]).op))
                style = "color=blue";
            else if (n.op == Op::Load || n.op == Op::Store || n.op == Op::Call ||
                     n.op == Op::Alloc) {
                if (i == 1) style = "color=red, style=dashed";
            } else if (n.op == Op::If && i == 1) {
                style = "color=blue";
            }
            os << "  n" << id << " -> n" << n.in[i] << " [" << style << "];\n";
        }
    }
    os << "}\n";
    return os.str();
}

} // namespace jules
