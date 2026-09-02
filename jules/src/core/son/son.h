// Public entry points of the SoN layer.
#pragma once

#include "core/parser/ast.h"
#include "core/sema/sema.h"
#include "core/son/graph.h"

namespace jules {

// AST -> SoN graph for one function (see builder.cpp).
bool build_function(const FnDecl& fn, const SemaModule& sema, FunctionGraph& fg,
                    SymbolTable& syms, Diagnostics& diag);

// Structural verifier (docs/son_spec.md invariants). Returns false + reports
// through `diag` on violation.
bool verify_graph(Graph& g, SymbolTable& syms, Diagnostics& diag);

// Dumpers. `text` returns a deterministic per-node listing; `dot` returns
// graphviz source.
std::string dump_graph_text(const Graph& g, SymbolTable& syms);
std::string dump_graph_dot(const Graph& g, SymbolTable& syms, const char* fn_name);

} // namespace jules
