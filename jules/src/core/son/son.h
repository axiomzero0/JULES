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
// graphviz source. `fn_syms` (FnId -> SymbolId, built from mod.fns[].name)
// resolves Call targets; without it user calls dump as fn<N>.
std::string dump_graph_text(const Graph& g, SymbolTable& syms,
                            const std::vector<SymbolId>* fn_syms = nullptr);
std::string dump_graph_dot(const Graph& g, SymbolTable& syms, const char* fn_name,
                           const std::vector<SymbolId>* fn_syms = nullptr);

} // namespace jules
