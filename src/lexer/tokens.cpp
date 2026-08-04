// lexer/tokens.cpp — token classification

#include "tokens.hpp"

#include <cstring>
#include <string_view>

namespace tether {

using sv = std::string_view;

namespace {

struct KwEntry {
    const char* spelling;
    TokenKind   kind;
};

// The single table of keywords. Adding a keyword is a one-line change
// here; nothing else in the compiler needs to know the spelling.
const KwEntry kKeywordTable[] = {
    {"module",  TokenKind::KwModule},
    {"import",  TokenKind::KwImport},
    {"export",  TokenKind::KwExport},
    {"fn",      TokenKind::KwFn},
    {"struct",  TokenKind::KwStruct},
    {"enum",    TokenKind::KwEnum},
    {"union",   TokenKind::KwUnion},
    {"trait",   TokenKind::KwTrait},
    {"impl",    TokenKind::KwImpl},
    {"type",    TokenKind::KwType},
    {"alias",   TokenKind::KwAlias},
    {"let",     TokenKind::KwLet},
    {"mut",     TokenKind::KwMut},
    {"const",   TokenKind::KwConst},
    {"static",  TokenKind::KwStatic},
    {"if",      TokenKind::KwIf},
    {"else",    TokenKind::KwElse},
    {"match",   TokenKind::KwMatch},
    {"while",   TokenKind::KwWhile},
    {"for",     TokenKind::KwFor},
    {"loop",    TokenKind::KwLoop},
    {"break",   TokenKind::KwBreak},
    {"continue",TokenKind::KwContinue},
    {"return",  TokenKind::KwReturn},
    {"defer",   TokenKind::KwDefer},
    {"alloc",   TokenKind::KwAlloc},
    {"move",    TokenKind::KwMove},
    {"borrow",  TokenKind::KwBorrow},
    {"unsafe",  TokenKind::KwUnsafe},
    {"extern",  TokenKind::KwExtern},
    {"ffi",     TokenKind::KwFfi},
    {"comptime",TokenKind::KwComptime},
    {"spawn",   TokenKind::KwSpawn},
    {"await",   TokenKind::KwAwait},
    // Contextual keyword. The lexer produces TokenKind::SelfKw only
    // when 'self' appears as a bare identifier; the parser decides
    // whether it is actually meaningful (e.g. inside an impl block).
    {"self",    TokenKind::SelfKw},
    // Bool literals are tokens, not keywords — but they are reserved
    // words. We lex them as BoolLit directly.
    {"true",    TokenKind::BoolLit},
    {"false",   TokenKind::BoolLit},
};

constexpr std::size_t kKeywordCount =
    sizeof(kKeywordTable) / sizeof(kKeywordTable[0]);

} // namespace

TokenKind classify_keyword(std::string_view ident) {
    // Linear scan. The keyword table is small (~37 entries); a hash
    // table would be faster but is not worth the dependency. If this
    // ever shows up in a profile, switch to a perfect hash.
    for (std::size_t i = 0; i < kKeywordCount; ++i) {
        const KwEntry& e = kKeywordTable[i];
        // Use size + memcmp to avoid constructing a std::string.
        std::size_t len = std::strlen(e.spelling);
        if (ident.size() == len &&
            std::memcmp(ident.data(), e.spelling, len) == 0) {
            return e.kind;
        }
    }
    return TokenKind::Ident;
}

namespace {

struct PunctName {
    TokenKind   kind;
    const char* name;
    const char* spelling; // nullptr for non-fixed-spelling tokens
};

const PunctName kPunctNames[] = {
    {TokenKind::Eof,           "eof",            nullptr},
    {TokenKind::Error,         "error",          nullptr},
    {TokenKind::Newline,       "newline",        nullptr},
    {TokenKind::Ident,         "ident",          nullptr},
    {TokenKind::IntLit,        "int_literal",    nullptr},
    {TokenKind::FloatLit,      "float_literal",  nullptr},
    {TokenKind::StringLit,     "string_literal", nullptr},
    {TokenKind::CharLit,       "char_literal",   nullptr},
    {TokenKind::BoolLit,       "bool_literal",   nullptr},
    {TokenKind::SelfKw,        "self",           "self"},

    {TokenKind::KwModule,      "kw_module",      "module"},
    {TokenKind::KwImport,      "kw_import",      "import"},
    {TokenKind::KwExport,      "kw_export",      "export"},
    {TokenKind::KwFn,          "kw_fn",          "fn"},
    {TokenKind::KwStruct,      "kw_struct",      "struct"},
    {TokenKind::KwEnum,        "kw_enum",        "enum"},
    {TokenKind::KwUnion,       "kw_union",       "union"},
    {TokenKind::KwTrait,       "kw_trait",       "trait"},
    {TokenKind::KwImpl,        "kw_impl",        "impl"},
    {TokenKind::KwType,        "kw_type",        "type"},
    {TokenKind::KwAlias,       "kw_alias",       "alias"},
    {TokenKind::KwLet,         "kw_let",         "let"},
    {TokenKind::KwMut,         "kw_mut",         "mut"},
    {TokenKind::KwConst,       "kw_const",       "const"},
    {TokenKind::KwStatic,      "kw_static",      "static"},
    {TokenKind::KwIf,          "kw_if",          "if"},
    {TokenKind::KwElse,        "kw_else",        "else"},
    {TokenKind::KwMatch,       "kw_match",       "match"},
    {TokenKind::KwWhile,       "kw_while",       "while"},
    {TokenKind::KwFor,         "kw_for",         "for"},
    {TokenKind::KwLoop,        "kw_loop",        "loop"},
    {TokenKind::KwBreak,       "kw_break",       "break"},
    {TokenKind::KwContinue,    "kw_continue",    "continue"},
    {TokenKind::KwReturn,      "kw_return",      "return"},
    {TokenKind::KwDefer,       "kw_defer",       "defer"},
    {TokenKind::KwAlloc,       "kw_alloc",       "alloc"},
    {TokenKind::KwMove,        "kw_move",        "move"},
    {TokenKind::KwBorrow,      "kw_borrow",      "borrow"},
    {TokenKind::KwUnsafe,      "kw_unsafe",      "unsafe"},
    {TokenKind::KwExtern,      "kw_extern",      "extern"},
    {TokenKind::KwFfi,         "kw_ffi",         "ffi"},
    {TokenKind::KwComptime,    "kw_comptime",    "comptime"},
    {TokenKind::KwSpawn,       "kw_spawn",       "spawn"},
    {TokenKind::KwAwait,       "kw_await",       "await"},

    {TokenKind::LParen,        "lparen",         "("},
    {TokenKind::RParen,        "rparen",         ")"},
    {TokenKind::LBrace,        "lbrace",         "{"},
    {TokenKind::RBrace,        "rbrace",         "}"},
    {TokenKind::LBracket,      "lbracket",       "["},
    {TokenKind::RBracket,      "rbracket",       "]"},
    {TokenKind::Comma,         "comma",          ","},
    {TokenKind::Semicolon,     "semicolon",      ";"},
    {TokenKind::Colon,         "colon",          ":"},
    {TokenKind::DoubleColon,   "double_colon",   "::"},
    {TokenKind::Dot,           "dot",            "."},
    {TokenKind::DotDot,        "dot_dot",        ".."},
    {TokenKind::DotDotEq,      "dot_dot_eq",     "..="},
    {TokenKind::Pipe,          "pipe",           "|"},
    {TokenKind::Arrow,         "arrow",          "->"},
    {TokenKind::FatArrow,      "fat_arrow",      "=>"},
    {TokenKind::At,            "at",             "@"},
    {TokenKind::Hash,          "hash",           "#"},
    {TokenKind::Question,      "question",       "?"},

    {TokenKind::Assign,        "assign",         "="},
    {TokenKind::PlusAssign,    "plus_assign",    "+="},
    {TokenKind::MinusAssign,   "minus_assign",   "-="},
    {TokenKind::StarAssign,    "star_assign",    "*="},
    {TokenKind::SlashAssign,   "slash_assign",   "/="},
    {TokenKind::PercentAssign, "percent_assign", "%="},
    {TokenKind::Plus,          "plus",           "+"},
    {TokenKind::Minus,         "minus",          "-"},
    {TokenKind::Star,          "star",           "*"},
    {TokenKind::Slash,         "slash",          "/"},
    {TokenKind::Percent,       "percent",        "%"},
    {TokenKind::Eq,            "eq",             "=="},
    {TokenKind::Neq,           "neq",            "!="},
    {TokenKind::Lt,            "lt",             "<"},
    {TokenKind::Gt,            "gt",             ">"},
    {TokenKind::Le,            "le",             "<="},
    {TokenKind::Ge,            "ge",             ">="},
    {TokenKind::And,           "and",            "&&"},
    {TokenKind::Or,            "or",             "||"},
    {TokenKind::Not,           "not",            "!"},
    {TokenKind::BitAnd,        "bit_and",        "&"},
    {TokenKind::BitOr,         "bit_or",         "|"},
    {TokenKind::BitXor,        "bit_xor",        "^"},
    {TokenKind::BitNot,        "bit_not",        "~"},
    {TokenKind::Shl,           "shl",            "<<"},
    {TokenKind::Shr,           "shr",            ">>"},
};

constexpr std::size_t kPunctNameCount =
    sizeof(kPunctNames) / sizeof(kPunctNames[0]);

} // namespace

const char* token_kind_name(TokenKind k) {
    for (std::size_t i = 0; i < kPunctNameCount; ++i) {
        if (kPunctNames[i].kind == k) {
            return kPunctNames[i].name;
        }
    }
    return "<unknown>";
}

const char* token_kind_spelling(TokenKind k) {
    for (std::size_t i = 0; i < kPunctNameCount; ++i) {
        if (kPunctNames[i].kind == k) {
            return kPunctNames[i].spelling;
        }
    }
    return nullptr;
}

} // namespace tether
