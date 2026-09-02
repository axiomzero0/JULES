#include "core/lexer/token.h"

#include <cstdlib>

namespace jules {

namespace {
struct Key { const char* kw; Tok kind; };
constexpr Key kKeywords[] = {
    {"fn", Tok::KwFn}, {"comptime", Tok::KwComptime}, {"let", Tok::KwLet},
    {"var", Tok::KwVar}, {"const", Tok::KwConst}, {"return", Tok::KwReturn},
    {"if", Tok::KwIf}, {"else", Tok::KwElse}, {"while", Tok::KwWhile},
    {"for", Tok::KwFor}, {"in", Tok::KwIn}, {"break", Tok::KwBreak},
    {"continue", Tok::KwContinue}, {"true", Tok::KwTrue}, {"false", Tok::KwFalse},
    {"as", Tok::KwAs}, {"module", Tok::KwModule}, {"use", Tok::KwUse},
    {"struct", Tok::KwStruct}, {"class", Tok::KwClass}, {"enum", Tok::KwEnum},
    {"bitfield", Tok::KwBitfield}, {"bitmask", Tok::KwBitmask}, {"alias", Tok::KwAlias},
    {"trait", Tok::KwTrait}, {"impl", Tok::KwImpl}, {"dyn", Tok::KwDyn},
    {"defer", Tok::KwDefer}, {"extern", Tok::KwExtern},
};
} // namespace

const char* tok_name(Tok t) {
    switch (t) {
        case Tok::Eof: return "<eof>";
        case Tok::Ident: return "identifier";
        case Tok::IntLit: return "integer literal";
        case Tok::FloatLit: return "float literal";
        case Tok::KwFn: return "'fn'"; case Tok::KwComptime: return "'comptime'";
        case Tok::KwLet: return "'let'"; case Tok::KwVar: return "'var'";
        case Tok::KwConst: return "'const'"; case Tok::KwReturn: return "'return'";
        case Tok::KwIf: return "'if'"; case Tok::KwElse: return "'else'";
        case Tok::KwWhile: return "'while'"; case Tok::KwFor: return "'for'";
        case Tok::KwIn: return "'in'"; case Tok::KwBreak: return "'break'";
        case Tok::KwContinue: return "'continue'"; case Tok::KwTrue: return "'true'";
        case Tok::KwFalse: return "'false'"; case Tok::KwAs: return "'as'";
        case Tok::KwModule: return "'module'"; case Tok::KwUse: return "'use'";
        case Tok::KwStruct: return "'struct'"; case Tok::KwClass: return "'class'";
        case Tok::KwEnum: return "'enum'"; case Tok::KwBitfield: return "'bitfield'";
        case Tok::KwBitmask: return "'bitmask'"; case Tok::KwAlias: return "'alias'";
        case Tok::KwTrait: return "'trait'"; case Tok::KwImpl: return "'impl'";
        case Tok::KwDyn: return "'dyn'"; case Tok::KwDefer: return "'defer'";
        case Tok::KwExtern: return "'extern'";
        case Tok::LParen: return "'('"; case Tok::RParen: return "')'";
        case Tok::LBrace: return "'{'"; case Tok::RBrace: return "'}'";
        case Tok::LBracket: return "'['"; case Tok::RBracket: return "']'";
        case Tok::Comma: return "','"; case Tok::Semi: return "';'";
        case Tok::Colon: return "':'"; case Tok::Arrow: return "'->'";
        case Tok::DotDot: return "'..'"; case Tok::Attr: return "'#['";
        case Tok::Pound: return "'#'";
        case Tok::Plus: return "'+'"; case Tok::Minus: return "'-'";
        case Tok::Star: return "'*'"; case Tok::Slash: return "'/'";
        case Tok::Percent: return "'%'";
        case Tok::Assign: return "'='"; case Tok::Eq: return "'=='";
        case Tok::Ne: return "'!='"; case Tok::Lt: return "'<'";
        case Tok::Le: return "'<='"; case Tok::Gt: return "'>'";
        case Tok::Ge: return "'>='";
        case Tok::Not: return "'!'"; case Tok::AndAnd: return "'&&'";
        case Tok::OrOr: return "'||'"; case Tok::Amp: return "'&'";
        case Tok::Pipe: return "'|'"; case Tok::Caret: return "'^'";
        case Tok::Tilde: return "'~'"; case Tok::Shl: return "'<<'";
        case Tok::Shr: return "'>>'";
    }
    return "<unknown>";
}

namespace {
class Lexer {
public:
    Lexer(std::string_view src, std::vector<Token>& out, Diagnostics& diag)
        : src_(src), out_(out), diag_(diag) {}

    bool run() {
        bool ok = true;
        for (;;) {
            skip_ws_and_comments();
            if (at_end()) break;
            if (!lex_one()) ok = false;
            if (diag_.error_count() > 200) break; // named limit, prevents cascades
        }
        Token eof;
        eof.kind = Tok::Eof;
        eof.pos = pos_;
        eof.text = "<eof>";
        out_.push_back(eof);
        return ok;
    }

private:
    bool at_end() const { return i_ >= src_.size(); }
    char peek(size_t k = 0) const { return i_ + k < src_.size() ? src_[i_ + k] : '\0'; }
    char advance() {
        char c = src_[i_++];
        if (c == '\n') { ++pos_.line; pos_.col = 1; } else { ++pos_.col; }
        return c;
    }

    void skip_ws_and_comments() {
        for (;;) {
            while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n')) advance();
            if (peek() == '/' && peek(1) == '/') {
                while (!at_end() && peek() != '\n') advance();
                continue;
            }
            if (peek() == '/' && peek(1) == '*') {
                SourcePos start = pos_;
                advance(); advance();
                int depth = 1;
                while (!at_end() && depth > 0) {
                    if (peek() == '/' && peek(1) == '*') { advance(); advance(); ++depth; }
                    else if (peek() == '*' && peek(1) == '/') { advance(); advance(); --depth; }
                    else advance();
                }
                if (depth > 0) diag_.error(start, "unterminated block comment");
                continue;
            }
            break;
        }
    }

    bool lex_one() {
        tok_start_ = pos_;
        char c = peek();
        if (is_ident_start(c)) return lex_ident();
        if (is_digit(c)) return lex_number();
        return lex_punct();
    }

    static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    bool emit(Tok k, std::string text) {
        Token t;
        t.kind = k; t.pos = tok_start_; t.text = std::move(text);
        out_.push_back(std::move(t));
        return true;
    }

    bool lex_ident() {
        std::string text;
        while (!at_end() && is_ident_char(peek())) text.push_back(advance());
        for (const Key& k : kKeywords)
            if (text == k.kw) return emit(k.kind, std::move(text));
        return emit(Tok::Ident, std::move(text));
    }

    bool lex_number() {
        std::string text;
        bool is_float = false;
        while (!at_end() && is_digit(peek())) text.push_back(advance());
        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            text.push_back(advance());
            while (!at_end() && is_digit(peek())) text.push_back(advance());
        }
        if (peek() == 'e' || peek() == 'E') {
            size_t save = i_;
            std::string exp;
            exp.push_back(advance());
            if (peek() == '+' || peek() == '-') exp.push_back(advance());
            if (is_digit(peek())) {
                is_float = true;
                while (!at_end() && is_digit(peek())) exp.push_back(advance());
                text += exp;
            } else {
                i_ = save; // not an exponent; 'e' begins an identifier-like suffix we reject below
            }
        }
        if (!at_end() && is_ident_start(peek())) {
            diag_.error(pos_, "invalid numeric suffix (type is chosen by literal range, e.g. `123 as u8`)");
            return false;
        }
        if (is_float) {
            Token t;
            t.kind = Tok::FloatLit; t.pos = tok_start_; t.text = text;
            t.fp_value = std::strtod(text.c_str(), nullptr);
            out_.push_back(std::move(t));
            return true;
        }
        Token t;
        t.kind = Tok::IntLit; t.pos = tok_start_; t.text = text;
        // Range-based typing: value decides i32 (default) vs i64.
        errno = 0;
        char* endp = nullptr;
        unsigned long long v = std::strtoull(text.c_str(), &endp, 10);
        t.int_value = v;
        t.is_signed_overflow = (v > static_cast<unsigned long long>(INT64_MAX));
        out_.push_back(std::move(t));
        return true;
    }

    bool lex_punct() {
        char c = advance();
        auto two = [&](char c2, Tok k2, Tok k1) -> bool {
            if (peek() == c2) { advance(); return emit(k2, {}); }
            return emit(k1, {});
        };
        switch (c) {
            case '(': return emit(Tok::LParen, {});
            case ')': return emit(Tok::RParen, {});
            case '{': return emit(Tok::LBrace, {});
            case '}': return emit(Tok::RBrace, {});
            case '[': return emit(Tok::LBracket, {});
            case ']': return emit(Tok::RBracket, {});
            case ',': return emit(Tok::Comma, {});
            case ';': return emit(Tok::Semi, {});
            case ':': return emit(Tok::Colon, {});
            case '+': return emit(Tok::Plus, {});
            case '-': return two('>', Tok::Arrow, Tok::Minus);
            case '*': return emit(Tok::Star, {});
            case '/': return emit(Tok::Slash, {});
            case '%': return emit(Tok::Percent, {});
            case '=': return two('=', Tok::Eq, Tok::Assign);
            case '!': return two('=', Tok::Ne, Tok::Not);
            case '<': {
                if (peek() == '=') { advance(); return emit(Tok::Le, {}); }
                if (peek() == '<') { advance(); return emit(Tok::Shl, {}); }
                return emit(Tok::Lt, {});
            }
            case '>': {
                if (peek() == '=') { advance(); return emit(Tok::Ge, {}); }
                if (peek() == '>') { advance(); return emit(Tok::Shr, {}); }
                return emit(Tok::Gt, {});
            }
            case '&': return two('&', Tok::AndAnd, Tok::Amp);
            case '|': return two('|', Tok::OrOr, Tok::Pipe);
            case '^': return emit(Tok::Caret, {});
            case '~': return emit(Tok::Tilde, {});
            case '.': {
                if (peek() == '.') { advance(); return emit(Tok::DotDot, {}); }
                diag_.error(pos_, "unexpected '.' (field access is not part of the MVP subset)");
                return false;
            }
            case '#': {
                if (peek() == '[') { advance(); return emit(Tok::Attr, {}); }
                return emit(Tok::Pound, {});
            }
            default: break;
        }
        diag_.error(pos_, std::string("unexpected character '") + c + "' in source");
        return false;
    }

    std::string_view src_;
    size_t i_ = 0;
    SourcePos pos_;
    SourcePos tok_start_;
    std::vector<Token>& out_;
    Diagnostics& diag_;
};
} // namespace

bool lex_source(std::string_view source, std::vector<Token>& out, Diagnostics& diag) {
    Lexer lex(source, out, diag);
    return lex.run();
}

} // namespace jules
