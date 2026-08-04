// lexer/lexer.hpp — tokenizer
//
// The lexer scans a SourceFile's bytes and produces a vector of Tokens.
// It performs no semantic analysis; ambiguous identifiers (e.g. `self`,
// `true`) are classified purely lexically.
//
// The lexer owns the token vector. Callers iterate it via the returned
// span; if they need to keep tokens alive past the lexer's lifetime,
// they must copy.

#pragma once

#include "lexer/tokens.hpp"
#include "diagnostics/diagnostics.hpp"
#include "support/intern.hpp"
#include "support/source.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tether {

class Lexer {
public:
    Lexer(InternTable& intern, DiagnosticEmitter& diag, const SourceFile& file);

    // Tokenize the entire file. Returns the tokens by value; the
    // vector is moved out so callers do not need to keep the Lexer
    // alive.
    std::vector<Token> tokenize();

private:
    InternTable&        intern_;
    DiagnosticEmitter&  diag_;
    const SourceFile&   file_;
    std::string_view    src_;

    std::size_t pos_ = 0;
    uint32_t    file_id_;

    // The current token's start offset, used to build SourceRange.
    std::size_t tok_start_ = 0;

    // Main loop helpers.
    void skip_whitespace_and_comments();
    Token lex_token();

    // Individual lexers.
    Token lex_ident_or_keyword();
    Token lex_number();
    Token lex_int_or_radix();
    Token lex_float_continuation(std::size_t int_part_len);
    Token lex_string();
    Token lex_char();
    Token lex_operator();

    // Utility.
    char  peek(std::size_t ahead = 0) const;
    bool  match(char c);
    bool  match2(char a, char b);
    SourceLoc loc_here() const;
    SourceRange range_from(std::size_t start) const;
    Token make(TokenKind kind, std::size_t start, std::size_t end);
    Token make_int(TokenKind kind, std::size_t start, std::size_t end,
                   uint64_t value);
    Token make_float(std::size_t start, std::size_t end, double value);
    Token make_str(TokenKind kind, std::size_t start, std::size_t end,
                   StrId str_id);
    Token make_error(std::size_t start, std::size_t end, std::string msg);
};

} // namespace tether
