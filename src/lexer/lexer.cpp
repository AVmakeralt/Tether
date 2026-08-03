// lexer/lexer.cpp — tokenizer implementation
//
// The lexer is a single-pass scanner. It produces a vector<Token> for
// the entire file. Comments and whitespace are discarded. Errors
// (unterminated strings, invalid escapes, bad numeric literals) are
// emitted as TokenKind::Error tokens AND reported through the
// DiagnosticEmitter; the parser can choose to skip error tokens or
// abort.

#include "lexer/lexer.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace tether {

namespace {

bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ident_continue(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

bool is_digit(char c)  { return c >= '0' && c <= '9'; }
bool is_hex(char c)    {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_bin(char c)    { return c == '0' || c == '1'; }
bool is_oct(char c)    { return c >= '0' && c <= '7'; }

} // namespace

Lexer::Lexer(InternTable& intern, DiagnosticEmitter& diag, const SourceFile& file)
    : intern_(intern), diag_(diag), file_(file),
      src_(file.content()), file_id_(file.file_id()) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        skip_whitespace_and_comments();
        if (pos_ >= src_.size()) {
            Token t;
            t.kind  = TokenKind::Eof;
            t.range.start = loc_here();
            t.range.end   = t.range.start;
            tokens.push_back(t);
            break;
        }
        tokens.push_back(lex_token());
    }
    return tokens;
}

void Lexer::skip_whitespace_and_comments() {
    while (pos_ < src_.size()) {
        char c = src_[pos_];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            ++pos_;
            continue;
        }
        // Line comment.
        if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
            pos_ += 2;
            while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            continue;
        }
        // Block comment. Nestable.
        if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
            std::size_t start = pos_;
            pos_ += 2;
            int depth = 1;
            while (pos_ < src_.size() && depth > 0) {
                if (src_[pos_] == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                    depth += 1;
                    pos_ += 2;
                } else if (src_[pos_] == '*' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                    depth -= 1;
                    pos_ += 2;
                } else {
                    ++pos_;
                }
            }
            if (depth > 0) {
                diag_.error(range_from(start),
                            "unterminated block comment");
            }
            continue;
        }
        break;
    }
}

Token Lexer::lex_token() {
    tok_start_ = pos_;
    char c = src_[pos_];

    if (is_ident_start(c)) return lex_ident_or_keyword();
    if (is_digit(c))       return lex_number();
    if (c == '"')          return lex_string();
    if (c == '\'')         return lex_char();
    return lex_operator();
}

Token Lexer::lex_ident_or_keyword() {
    std::size_t start = pos_;
    while (pos_ < src_.size() && is_ident_continue(src_[pos_])) ++pos_;
    std::string_view text = src_.substr(start, pos_ - start);

    TokenKind k = classify_keyword(text);
    if (k == TokenKind::Ident || k == TokenKind::SelfKw ||
        k == TokenKind::BoolLit) {
        StrId id = intern_.intern(text);
        Token t;
        t.kind   = k;
        t.range  = range_from(start);
        t.str_id = id;
        if (k == TokenKind::BoolLit) {
            // 'true' or 'false'
            t.int_val = (text == "true") ? 1 : 0;
        }
        return t;
    }
    return make(k, start, pos_);
}

Token Lexer::lex_number() {
    // Detect radix: 0x, 0b, 0o, else decimal (which may become float).
    if (src_[pos_] == '0' && pos_ + 1 < src_.size()) {
        char next = src_[pos_ + 1];
        if (next == 'x' || next == 'X' || next == 'b' || next == 'B' ||
            next == 'o' || next == 'O') {
            return lex_int_or_radix();
        }
    }

    // Decimal. Lex integer part.
    std::size_t start = pos_;
    while (pos_ < src_.size() && (is_digit(src_[pos_]) || src_[pos_] == '_')) ++pos_;
    std::size_t int_part_len = pos_ - start;

    // Float?
    if (pos_ < src_.size() && src_[pos_] == '.' &&
        pos_ + 1 < src_.size() && is_digit(src_[pos_ + 1])) {
        return lex_float_continuation(int_part_len);
    }
    // Exponent without dot? (1e10)
    if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
        return lex_float_continuation(int_part_len);
    }

    // Plain integer. Strip underscores and parse.
    uint64_t value = 0;
    for (std::size_t i = 0; i < int_part_len; ++i) {
        char d = src_[start + i];
        if (d == '_') continue;
        value = value * 10 + static_cast<uint64_t>(d - '0');
    }
    return make_int(TokenKind::IntLit, start, pos_, value);
}

Token Lexer::lex_int_or_radix() {
    std::size_t start = pos_;
    char prefix = src_[pos_ + 1];
    pos_ += 2;
    std::size_t digits_start = pos_;

    bool (*is_digit_in_radix)(char);
    int radix;
    if (prefix == 'x' || prefix == 'X') { radix = 16; is_digit_in_radix = is_hex; }
    else if (prefix == 'b' || prefix == 'B') { radix = 2;  is_digit_in_radix = is_bin; }
    else { radix = 8; is_digit_in_radix = is_oct; }

    while (pos_ < src_.size() && (is_digit_in_radix(src_[pos_]) || src_[pos_] == '_')) ++pos_;

    if (pos_ == digits_start) {
        return make_error(start, pos_, "numeric literal has no digits after radix prefix");
    }

    uint64_t value = 0;
    for (std::size_t i = digits_start; i < pos_; ++i) {
        char d = src_[i];
        if (d == '_') continue;
        int dv;
        if (d >= '0' && d <= '9')      dv = d - '0';
        else if (d >= 'a' && d <= 'f') dv = 10 + (d - 'a');
        else                            dv = 10 + (d - 'A');
        value = value * radix + static_cast<uint64_t>(dv);
    }
    return make_int(TokenKind::IntLit, start, pos_, value);
}

Token Lexer::lex_float_continuation(std::size_t int_part_len) {
    std::size_t start = tok_start_;
    // Consume fractional part if present.
    if (pos_ < src_.size() && src_[pos_] == '.') {
        ++pos_;
        while (pos_ < src_.size() && (is_digit(src_[pos_]) || src_[pos_] == '_')) ++pos_;
    }
    // Exponent.
    if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
        ++pos_;
        if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
        std::size_t exp_start = pos_;
        while (pos_ < src_.size() && (is_digit(src_[pos_]) || src_[pos_] == '_')) ++pos_;
        if (pos_ == exp_start) {
            return make_error(start, pos_, "float literal has no digits in exponent");
        }
    }

    // Parse with strtod. Build a sanitized buffer (strip underscores).
    std::string sanitized;
    sanitized.reserve(pos_ - start);
    for (std::size_t i = start; i < pos_; ++i) {
        char c = src_[i];
        if (c != '_') sanitized.push_back(c);
    }
    double val = std::strtod(sanitized.c_str(), nullptr);
    return make_float(start, pos_, val);
}

Token Lexer::lex_string() {
    std::size_t start = pos_;
    ++pos_; // consume opening quote
    std::string bytes;
    while (pos_ < src_.size() && src_[pos_] != '"') {
        char c = src_[pos_];
        if (c == '\n') {
            return make_error(start, pos_, "unterminated string literal (newline)");
        }
        if (c == '\\') {
            ++pos_;
            if (pos_ >= src_.size()) {
                return make_error(start, pos_, "unterminated string escape");
            }
            char esc = src_[pos_++];
            switch (esc) {
                case 'n':  bytes.push_back('\n'); break;
                case 't':  bytes.push_back('\t'); break;
                case 'r':  bytes.push_back('\r'); break;
                case '0':  bytes.push_back('\0'); break;
                case '"':  bytes.push_back('"');  break;
                case '\\': bytes.push_back('\\'); break;
                case 'x': {
                    if (pos_ + 1 >= src_.size() ||
                        !is_hex(src_[pos_]) || !is_hex(src_[pos_ + 1])) {
                        return make_error(start, pos_, "invalid \\x escape");
                    }
                    char h1 = src_[pos_];
                    char h2 = src_[pos_ + 1];
                    pos_ += 2;
                    auto hexval = [](char ch) -> int {
                        if (ch >= '0' && ch <= '9') return ch - '0';
                        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                        return 10 + (ch - 'A');
                    };
                    int v = (hexval(h1) << 4) | hexval(h2);
                    bytes.push_back(static_cast<char>(v));
                    break;
                }
                default:
                    return make_error(start, pos_, std::string("unknown escape \\") + esc);
            }
        } else {
            bytes.push_back(c);
            ++pos_;
        }
    }
    if (pos_ >= src_.size()) {
        return make_error(start, pos_, "unterminated string literal");
    }
    ++pos_; // consume closing quote
    StrId id = intern_.intern(std::move(bytes));
    return make_str(TokenKind::StringLit, start, pos_, id);
}

Token Lexer::lex_char() {
    std::size_t start = pos_;
    ++pos_; // consume opening quote
    if (pos_ >= src_.size() || src_[pos_] == '\'') {
        return make_error(start, pos_, "empty character literal");
    }
    uint64_t value = 0;
    char c = src_[pos_];
    if (c == '\\') {
        ++pos_;
        if (pos_ >= src_.size()) {
            return make_error(start, pos_, "unterminated character escape");
        }
        char esc = src_[pos_++];
        switch (esc) {
            case 'n':  value = '\n'; break;
            case 't':  value = '\t'; break;
            case 'r':  value = '\r'; break;
            case '0':  value = '\0'; break;
            case '\'': value = '\''; break;
            case '\\': value = '\\'; break;
            case 'x': {
                if (pos_ + 1 >= src_.size() ||
                    !is_hex(src_[pos_]) || !is_hex(src_[pos_ + 1])) {
                    return make_error(start, pos_, "invalid \\x escape in char");
                }
                char h1 = src_[pos_];
                char h2 = src_[pos_ + 1];
                pos_ += 2;
                auto hexval = [](char ch) -> int {
                    if (ch >= '0' && ch <= '9') return ch - '0';
                    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                    return 10 + (ch - 'A');
                };
                value = static_cast<uint64_t>((hexval(h1) << 4) | hexval(h2));
                break;
            }
            default:
                return make_error(start, pos_, std::string("unknown char escape \\") + esc);
        }
    } else {
        // UTF-8: collect raw bytes. We accept up to 4 bytes and pack
        // them into the low 32 bits of int_val.
        unsigned char lead = static_cast<unsigned char>(c);
        int extra = 0;
        if      ((lead & 0x80) == 0x00) extra = 0;
        else if ((lead & 0xE0) == 0xC0) extra = 1;
        else if ((lead & 0xF0) == 0xE0) extra = 2;
        else if ((lead & 0xF8) == 0xF0) extra = 3;
        else {
            return make_error(start, pos_, "invalid UTF-8 lead byte in char literal");
        }
        if (pos_ + 1 + extra > src_.size()) {
            return make_error(start, pos_, "truncated UTF-8 in char literal");
        }
        value = lead;
        ++pos_;
        for (int i = 0; i < extra; ++i) {
            value = (value << 8) | static_cast<uint64_t>(static_cast<unsigned char>(src_[pos_++]));
        }
    }
    if (pos_ >= src_.size() || src_[pos_] != '\'') {
        return make_error(start, pos_, "unterminated character literal");
    }
    ++pos_; // consume closing quote
    return make_int(TokenKind::CharLit, start, pos_, value);
}

Token Lexer::lex_operator() {
    std::size_t start = pos_;
    char c = src_[pos_++];
    switch (c) {
        case '(': return make(TokenKind::LParen,    start, pos_);
        case ')': return make(TokenKind::RParen,    start, pos_);
        case '{': return make(TokenKind::LBrace,    start, pos_);
        case '}': return make(TokenKind::RBrace,    start, pos_);
        case '[': return make(TokenKind::LBracket,  start, pos_);
        case ']': return make(TokenKind::RBracket,  start, pos_);
        case ',': return make(TokenKind::Comma,     start, pos_);
        case ';': return make(TokenKind::Semicolon, start, pos_);
        case '@': return make(TokenKind::At,        start, pos_);
        case '#': return make(TokenKind::Hash,      start, pos_);
        case '?': return make(TokenKind::Question,  start, pos_);
        case '~': return make(TokenKind::BitNot,    start, pos_);
        case '.':
            if (match('.')) {
                // We don't have a range token in v0.1; emit two dots.
                --pos_;
                return make(TokenKind::Dot, start, start + 1);
            }
            return make(TokenKind::Dot, start, pos_);
        case ':':
            if (match(':')) return make(TokenKind::DoubleColon, start, pos_);
            return make(TokenKind::Colon, start, pos_);
        case '-':
            if (match('>')) return make(TokenKind::Arrow, start, pos_);
            if (match('=')) return make(TokenKind::MinusAssign, start, pos_);
            return make(TokenKind::Minus, start, pos_);
        case '=':
            if (match('>')) return make(TokenKind::FatArrow, start, pos_);
            if (match('=')) return make(TokenKind::Eq, start, pos_);
            return make(TokenKind::Assign, start, pos_);
        case '+':
            if (match('=')) return make(TokenKind::PlusAssign, start, pos_);
            return make(TokenKind::Plus, start, pos_);
        case '*':
            if (match('=')) return make(TokenKind::StarAssign, start, pos_);
            return make(TokenKind::Star, start, pos_);
        case '/':
            if (match('=')) return make(TokenKind::SlashAssign, start, pos_);
            return make(TokenKind::Slash, start, pos_);
        case '%':
            if (match('=')) return make(TokenKind::PercentAssign, start, pos_);
            return make(TokenKind::Percent, start, pos_);
        case '!':
            if (match('=')) return make(TokenKind::Neq, start, pos_);
            return make(TokenKind::Not, start, pos_);
        case '<':
            if (match('<')) return make(TokenKind::Shl, start, pos_);
            if (match('=')) return make(TokenKind::Le, start, pos_);
            return make(TokenKind::Lt, start, pos_);
        case '>':
            if (match('>')) return make(TokenKind::Shr, start, pos_);
            if (match('=')) return make(TokenKind::Ge, start, pos_);
            return make(TokenKind::Gt, start, pos_);
        case '&':
            if (match('&')) return make(TokenKind::And, start, pos_);
            return make(TokenKind::BitAnd, start, pos_);
        case '|':
            if (match('|')) return make(TokenKind::Or, start, pos_);
            return make(TokenKind::BitOr, start, pos_);
        case '^':
            return make(TokenKind::BitXor, start, pos_);
        default:
            return make_error(start, pos_,
                              std::string("unexpected character '") + c + "'");
    }
}

// ---- Utility ----

char Lexer::peek(std::size_t ahead) const {
    if (pos_ + ahead >= src_.size()) return '\0';
    return src_[pos_ + ahead];
}

bool Lexer::match(char c) {
    if (pos_ < src_.size() && src_[pos_] == c) {
        ++pos_;
        return true;
    }
    return false;
}

bool Lexer::match2(char a, char b) {
    if (pos_ + 1 < src_.size() && src_[pos_] == a && src_[pos_ + 1] == b) {
        pos_ += 2;
        return true;
    }
    return false;
}

SourceLoc Lexer::loc_here() const {
    SourceLoc l;
    l.file_id = file_id_;
    l.offset  = static_cast<uint32_t>(pos_);
    return l;
}

SourceRange Lexer::range_from(std::size_t start) const {
    SourceRange r;
    r.start.file_id = file_id_;
    r.start.offset  = static_cast<uint32_t>(start);
    r.end.file_id   = file_id_;
    r.end.offset    = static_cast<uint32_t>(pos_);
    return r;
}

Token Lexer::make(TokenKind kind, std::size_t start, std::size_t end) {
    Token t;
    t.kind  = kind;
    t.range.start.file_id = file_id_;
    t.range.start.offset  = static_cast<uint32_t>(start);
    t.range.end.file_id   = file_id_;
    t.range.end.offset    = static_cast<uint32_t>(end);
    return t;
}

Token Lexer::make_int(TokenKind kind, std::size_t start, std::size_t end,
                      uint64_t value) {
    Token t = make(kind, start, end);
    t.int_val = value;
    return t;
}

Token Lexer::make_float(std::size_t start, std::size_t end, double value) {
    Token t = make(TokenKind::FloatLit, start, end);
    t.float_val = value;
    return t;
}

Token Lexer::make_str(TokenKind kind, std::size_t start, std::size_t end,
                      StrId str_id) {
    Token t = make(kind, start, end);
    t.str_id = str_id;
    return t;
}

Token Lexer::make_error(std::size_t start, std::size_t end, std::string msg) {
    Token t = make(TokenKind::Error, start, end);
    t.str_id = intern_.intern(std::move(msg));
    diag_.error(t.range, intern_.get_owned(t.str_id));
    return t;
}

} // namespace tether
