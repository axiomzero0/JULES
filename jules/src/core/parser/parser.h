// Parser public API.
#pragma once

#include "core/lexer/token.h"
#include "core/parser/ast.h"

namespace jules {

bool parse_tokens(const std::vector<Token>& toks, ModuleAst& out, Diagnostics& diag,
                  SymbolTable& syms);

} // namespace jules
