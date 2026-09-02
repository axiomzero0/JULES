// Semantic analysis: name resolution, type checking (no implicit conversions),
// comptime evaluation of const decls and comptime-fn calls, builtin resolution
// (print / free / alloc<T>). Annotates the AST with result types.
#pragma once

#include "core/parser/ast.h"

namespace jules {

struct SemaFn {
    SymbolId name = kNoSymbol;
    TypeId ret = ty_void();
    std::vector<TypeId> params;
    bool is_comptime = false;
    bool always_inline = false;
    bool no_inline = false;
    size_t ast_index = 0;   // index into ModuleAst::fns
    u32 node_estimate = 0;  // rough size metric for the inlining cost model
};

struct SemaModule {
    std::vector<SemaFn> fns;
    FlatMap<SymbolId, size_t> fn_by_name;
    bool has_main = false;
};

bool run_sema(ModuleAst& ast, SemaModule& out, Diagnostics& diag, SymbolTable& syms);

} // namespace jules
