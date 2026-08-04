// lexer/tokens.hpp — token kinds and the Token structure
//
// This file defines the lexical vocabulary of Tether: all 34 keywords,
// all operators and punctuation, and the literal-bearing token kinds.
// The lexer produces a stream of these Tokens; the parser consumes
// them.

#pragma once

#include "support/intern.hpp"
#include "support/source.hpp"

#include <cstdint>

namespace tether {

enum class TokenKind : uint16_t {
    // ---- Special ----------------------------------------------------------
    Eof,            // end of file
    Error,          // lexer error; text in str_id
    Newline,        // used only when the lexer is in whitespace-significant
                    // mode (currently unused; Tether is not indent-sensitive)

    // ---- Literals ---------------------------------------------------------
    Ident,          // identifier; name in str_id
    IntLit,         // integer literal; value in int_val
    FloatLit,       // float literal; value in float_val
    StringLit,      // string literal; bytes in str_id
    CharLit,        // character literal; value in int_val (codepoint)
    BoolLit,        // true / false; value in int_val (0 or 1)
    SelfKw,         // 'self' (contextual keyword — handled here for simplicity)

    // ---- Keywords: declarations ------------------------------------------
    KwModule,
    KwImport,
    KwExport,
    KwFn,
    KwStruct,
    KwEnum,
    KwUnion,
    KwTrait,
    KwImpl,
    KwType,
    KwAlias,

    // ---- Keywords: bindings ----------------------------------------------
    KwLet,
    KwMut,
    KwConst,
    KwStatic,

    // ---- Keywords: control flow ------------------------------------------
    KwIf,
    KwElse,
    KwMatch,
    KwWhile,
    KwFor,
    KwLoop,
    KwBreak,
    KwContinue,
    KwReturn,
    KwDefer,

    // ---- Keywords: memory ------------------------------------------------
    KwAlloc,
    KwMove,
    KwBorrow,
    KwUnsafe,

    // ---- Keywords: FFI / compile-time ------------------------------------
    KwExtern,
    KwFfi,
    KwComptime,

    // ---- Keywords: concurrency -------------------------------------------
    KwSpawn,
    KwAwait,

    // ---- Punctuation -----------------------------------------------------
    LParen,     // (
    RParen,     // )
    LBrace,     // {
    RBrace,     // }
    LBracket,   // [
    RBracket,   // ]
    Comma,      // ,
    Semicolon,  // ;
    Colon,      // :
    DoubleColon,// ::
    Dot,        // .
    DotDot,     // ..
    DotDotEq,   // ..=
    Pipe,       // |
    Arrow,      // ->
    FatArrow,   // =>
    At,         // @
    Hash,       // #
    Question,   // ?

    // ---- Operators --------------------------------------------------------
    Assign,     // =
    PlusAssign, // +=
    MinusAssign,// -=
    StarAssign, // *=
    SlashAssign,// /=
    PercentAssign, // %=
    Plus,       // +
    Minus,      // -
    Star,       // *
    Slash,      // /
    Percent,    // %
    Eq,         // ==
    Neq,        // !=
    Lt,         // <
    Gt,         // >
    Le,         // <=
    Ge,         // >=
    And,        // &&
    Or,         // ||
    Not,        // !
    BitAnd,     // &
    BitOr,      // |
    BitXor,     // ^
    BitNot,     // ~
    Shl,        // <<
    Shr,        // >>

    // ---- Reserved for future use (lexer treats as identifier for now) ----
    // async, yield, macro, operator, reflect
    // These are reserved in the spec but not yet token kinds; they will
    // be promoted to keywords when implemented.

    LastPunct,
};

// Returns the keyword TokenKind for an identifier, or TokenKind::Ident
// if the identifier is not a keyword. This is the single source of
// truth for the keyword set — do not duplicate keyword strings in the
// lexer.
TokenKind classify_keyword(std::string_view ident);

// Returns the string spelling of a TokenKind, for diagnostics and
// debug printing. Returns "<unknown>" for unknown kinds.
const char* token_kind_name(TokenKind k);

// Returns the fixed spelling of a punctuation/keyword token, or
// nullptr for tokens that don't have a fixed spelling (idents,
// literals, EOF, error).
const char* token_kind_spelling(TokenKind k);

// A single token produced by the Lexer.
struct Token {
    TokenKind   kind   = TokenKind::Eof;
    SourceRange range  = {};
    StrId       str_id = kInvalidStrId;  // for Ident, StringLit, Error
    uint64_t    int_val  = 0;             // for IntLit, CharLit, BoolLit
    double      float_val = 0.0;          // for FloatLit

    bool is(TokenKind k) const { return kind == k; }
    bool is_not(TokenKind k) const { return kind != k; }
};

} // namespace tether
