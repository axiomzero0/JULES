// Lexer tokens for the JULES frontend (MVP language subset).
#pragma once

#include "core/diagnostics/diag.h"
#include "core/support/common.h"

namespace jules {

enum class Tok : u16 {
    Eof, Ident, IntLit, FloatLit,
    // keywords
    KwFn, KwComptime, KwLet, KwVar, KwConst, KwReturn, KwIf, KwElse, KwWhile,
    KwFor, KwIn, KwBreak, KwContinue, KwTrue, KwFalse, KwAs, KwModule, KwUse,
    // reserved for future milestones (clear diagnostics when used)
    KwStruct, KwClass, KwEnum, KwBitfield, KwBitmask, KwAlias, KwTrait, KwImpl,
    KwDyn, KwDefer, KwExtern,
    // punctuation / operators
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semi, Colon, Arrow, DotDot, Attr, Pound,
    Plus, Minus, Star, Slash, Percent,
    Assign, Eq, Ne, Lt, Le, Gt, Ge,
    Not, AndAnd, OrOr, Amp, Pipe, Caret, Tilde, Shl, Shr,
};

struct Token {
    Tok kind = Tok::Eof;
    SourcePos pos;
    std::string text;   // identifier / raw literal text
    u64 int_value = 0;  // decoded integer literal
    f64 fp_value = 0.0; // decoded float literal
    bool is_signed_overflow = false;
};

const char* tok_name(Tok t);

// Tokenize whole source. Returns false (tokens still usable up to error) on failure.
bool lex_source(std::string_view source, std::vector<Token>& out, Diagnostics& diag);

} // namespace jules
