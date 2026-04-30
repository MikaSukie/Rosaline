#include "lexer.hpp"
#include <cctype>
#include <stdexcept>
#include <unordered_map>

static const std::unordered_map<std::string, TK> keywords = {
    {"import",     TK::Import},
    {"opaque",     TK::Opaque},
    {"struct",     TK::Struct},
    {"packed",     TK::Packed},
    {"enum",       TK::Enum},
    {"match",      TK::Match},
    {"if",         TK::If},
    {"else",       TK::Else},
    {"while",      TK::While},
    {"break",      TK::Break},
    {"continue",   TK::Continue},
    {"return",     TK::Return},
    {"typeswitch", TK::Typeswitch},
    {"typecase",   TK::Typecase},
    {"fallback",   TK::Fallback},
    {"in",         TK::In},
    {"nomd",       TK::Nomd},
    {"readonly",   TK::Readonly},
    {"take",       TK::Take},
    {"release",    TK::Release},
    {"crumble",    TK::Crumble},
    {"for",        TK::For},
    {"true",       TK::True},
    {"false",      TK::False},
    {"i8",         TK::I8},
    {"i16",        TK::I16},
    {"i32",        TK::I32},
    {"i64",        TK::I64},
    {"u8",         TK::U8},
    {"u16",        TK::U16},
    {"u32",        TK::U32},
    {"u64",        TK::U64},
    {"bool",       TK::Bool},
    {"char",       TK::Char},
    {"str",        TK::Str},
    {"f32",        TK::F32},
    {"f64",        TK::F64},
    {"void",       TK::Void},
    {"extern",     TK::Extern},
    {"unsafe",     TK::Unsafe},
};

Lexer::Lexer(std::string src, std::string filename, DiagEngine& diag)
    : src_(std::move(src)), file_(std::move(filename)), diag_(diag) {}

char Lexer::peek(int offset) const {
    size_t idx = pos_ + offset;
    return (idx < src_.size()) ? src_[idx] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; }
    else           { ++col_; }
    return c;
}

SourceLoc Lexer::loc(int line, int col, int len) const {
    return {file_, line, col, len};
}

std::string Lexer::lineFor(int lineNum) const {
    int cur = 1;
    size_t i = 0;
    while (i < src_.size() && cur < lineNum) {
        if (src_[i] == '\n') ++cur;
        ++i;
    }
    size_t end = i;
    while (end < src_.size() && src_[end] != '\n') ++end;
    return src_.substr(i, end - i);
}

Token Lexer::makeToken(TK k, std::string text, int sL, int sC) {
    Token t;
    t.kind = k;
    t.text = std::move(text);
    t.loc  = loc(sL, sC, (int)t.text.size());
    return t;
}

void Lexer::skipWhitespaceAndComments() {
    while (pos_ < src_.size()) {
        char c = peek();
        if (std::isspace((unsigned char)c)) { advance(); continue; }
        if (c == '/' && peek(1) == '/') {
            while (pos_ < src_.size() && peek() != '\n') advance();
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            int commentLine = line_, commentCol = col_;
            advance(); advance();
            int depth = 1;
            while (pos_ < src_.size() && depth > 0) {
                if (peek() == '/' && peek(1) == '*') {
                    advance(); advance();
                    ++depth;
                } else if (peek() == '*' && peek(1) == '/') {
                    advance(); advance();
                    --depth;
                } else {
                    advance();
                }
            }
            if (depth > 0)
                diag_.error(loc(commentLine, commentCol, 2),
                    "unterminated block comment", lineFor(commentLine));
            continue;
        }
        break;
    }
}

Token Lexer::lexStringLit(int sL, int sC) {
    std::string val;
    bool closed = false;
    while (pos_ < src_.size()) {
        char c = peek();
        if (c == '"') { advance(); closed = true; break; }
        if (c == '\n') break;
        if (c == '\\') {
            advance();
            char esc = advance();
            switch (esc) {
            case 'n':  val += '\n'; break;
            case 't':  val += '\t'; break;
            case 'r':  val += '\r'; break;
            case '\\': val += '\\'; break;
            case '"':  val += '"';  break;
            case '0':  val += '\0'; break;
            case 'a':  val += '\a'; break;
            case 'b':  val += '\b'; break;
            case 'v':  val += '\v'; break;
            case 'f':  val += '\f'; break;
            case 'x': {
                int hi = -1, lo = -1;
                if (pos_ < src_.size() && std::isxdigit((unsigned char)peek())) {
                    char h = advance();
                    hi = (h >= '0' && h <= '9') ? h - '0'
                       : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                                 : h - 'A' + 10;
                }
                if (pos_ < src_.size() && std::isxdigit((unsigned char)peek())) {
                    char h = advance();
                    lo = (h >= '0' && h <= '9') ? h - '0'
                       : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                                 : h - 'A' + 10;
                }
                if (hi < 0 || lo < 0)
                    diag_.error(loc(sL, sC, 1),
                        "\\x escape requires exactly two hex digits", lineFor(sL));
                val += (char)((hi < 0 ? 0 : hi) * 16 + (lo < 0 ? 0 : lo));
                break;
            }
            case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                int oval = esc - '0';
                for (int i = 0; i < 2; ++i) {
                    if (pos_ < src_.size() && peek() >= '0' && peek() <= '7')
                        oval = oval * 8 + (advance() - '0');
                    else break;
                }
                val += (char)(oval & 0xFF);
                break;
            }
            default:   val += esc;  break;
            }
        } else {
            val += advance();
        }
    }
    if (!closed)
        diag_.error(loc(sL, sC, 1), "unterminated string literal", lineFor(sL));
    Token t;
    t.kind    = TK::StringLit;
    t.text    = val;
    t.loc     = loc(sL, sC, (int)val.size() + 2);
    return t;
}

Token Lexer::lexBacktickString(int sL, int sC) {
    std::string raw;
    bool closed = false;
    while (pos_ < src_.size()) {
        char c = peek();
        if (c == '`') { advance(); closed = true; break; }
        if (c == '\\' && peek(1) == '`') { advance(); raw += advance(); continue; }
        raw += advance();
    }
    if (!closed)
        diag_.error(loc(sL, sC, 1), "unterminated backtick string literal", lineFor(sL));

    std::vector<std::string> lines;
    {
        std::istringstream ss(raw);
        std::string ln;
        while (std::getline(ss, ln))
            lines.push_back(ln);
    }

    if (!lines.empty() && lines.front().find_first_not_of(" \t") == std::string::npos)
        lines.erase(lines.begin());

    if (!lines.empty() && lines.back().find_first_not_of(" \t") == std::string::npos)
        lines.pop_back();

    size_t minIndent = std::string::npos;
    for (auto& ln : lines) {
        size_t first = ln.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (first < minIndent) minIndent = first;
    }
    if (minIndent == std::string::npos) minIndent = 0;

    std::string val;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto& ln = lines[i];
        if (ln.find_first_not_of(" \t") == std::string::npos)
            val += "";
        else
            val += (minIndent <= ln.size() ? ln.substr(minIndent) : ln);
        if (i + 1 < lines.size()) val += '\n';
    }

    Token t;
    t.kind = TK::StringLit;
    t.text = val;
    t.loc  = loc(sL, sC, (int)raw.size() + 2);
    return t;
}

Token Lexer::lexCharLit(int sL, int sC) {
    char val = '\0';
    if (peek() == '\\') {
        advance();
        char esc = advance();
        switch (esc) {
        case 'n':  val = '\n'; break;
        case 't':  val = '\t'; break;
        case 'r':  val = '\r'; break;
        case '\\': val = '\\'; break;
        case '\'': val = '\''; break;
        case '0':  val = '\0'; break;
        case 'a':  val = '\a'; break;
        case 'b':  val = '\b'; break;
        case 'v':  val = '\v'; break;
        case 'f':  val = '\f'; break;
        case 'x': {
            int hi = -1, lo = -1;
            if (pos_ < src_.size() && std::isxdigit((unsigned char)peek())) {
                char h = advance();
                hi = (h >= '0' && h <= '9') ? h - '0'
                   : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                             : h - 'A' + 10;
            }
            if (pos_ < src_.size() && std::isxdigit((unsigned char)peek())) {
                char h = advance();
                lo = (h >= '0' && h <= '9') ? h - '0'
                   : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                             : h - 'A' + 10;
            }
            if (hi < 0 || lo < 0)
                diag_.error(loc(sL, sC, 1),
                    "\\x escape requires exactly two hex digits", lineFor(sL));
            val = (char)((hi < 0 ? 0 : hi) * 16 + (lo < 0 ? 0 : lo));
            break;
        }
        case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            int oval = esc - '0';
            for (int i = 0; i < 2; ++i) {
                if (pos_ < src_.size() && peek() >= '0' && peek() <= '7')
                    oval = oval * 8 + (advance() - '0');
                else break;
            }
            val = (char)(oval & 0xFF);
            break;
        }
        default:   val = esc;  break;
        }
    } else {
        val = advance();
    }
    if (peek() != '\'')
        diag_.error(loc(sL, sC, 1), "unterminated character literal", lineFor(sL));
    else
        advance();
    Token t;
    t.kind = TK::CharLit;
    t.text = std::string(1, val);
    t.ival = (int64_t)(unsigned char)val;
    t.loc  = loc(sL, sC, 3);
    return t;
}

Token Lexer::lexNumber(int sL, int sC) {
    std::string num;
    bool isFloat = false;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        num += advance(); num += advance();
        while (pos_ < src_.size() && std::isxdigit((unsigned char)peek())) num += advance();
    } else {
        while (pos_ < src_.size() && std::isdigit((unsigned char)peek())) num += advance();
        if (peek() == '.' && std::isdigit((unsigned char)peek(1))) {
            isFloat = true;
            num += advance();
            while (pos_ < src_.size() && std::isdigit((unsigned char)peek())) num += advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            isFloat = true;
            num += advance();
            if (peek() == '+' || peek() == '-') num += advance();
            while (pos_ < src_.size() && std::isdigit((unsigned char)peek())) num += advance();
        }
    }

    Token t;
    t.loc = loc(sL, sC, (int)num.size());
    t.text = num;
    if (isFloat) {
        t.kind = TK::FloatLit;
        t.fval = std::stod(num);
    } else {
        t.kind = TK::IntLit;
        t.ival = std::stoll(num, nullptr, 0);
    }
    return t;
}

Token Lexer::lexIdentOrKeyword(int sL, int sC) {
    std::string ident;
    while (pos_ < src_.size() && (std::isalnum((unsigned char)peek()) || peek() == '_'))
        ident += advance();

    if (ident.starts_with("__ROSALINE__")) {
        diag_.error(loc(sL, sC, (int)ident.size()),
            std::format("'{}' uses reserved prefix __ROSALINE__", ident),
            lineFor(sL));
    }

    auto it = keywords.find(ident);
    TK kind = (it != keywords.end()) ? it->second : TK::Ident;

    Token t;
    t.kind = kind;
    t.text = ident;
    t.loc  = loc(sL, sC, (int)ident.size());
    if (kind == TK::True)  t.ival = 1;
    if (kind == TK::False) t.ival = 0;
    return t;
}

Token Lexer::lexDirective(int sL, int sC) {
    std::string name;
    while (pos_ < src_.size() && (std::isalnum((unsigned char)peek()) || peek() == '_'))
        name += advance();

    Token t;
    t.loc = loc(sL, sC, (int)name.size() + 1);
    if (name == "tdl") {
        t.kind = TK::DirTDL;
        t.text = "@tdl";
    } else if (name == "ttriple") {
        t.kind = TK::DirTTriple;
        t.text = "@ttriple";
    } else if (name == "nomain") {
        t.kind = TK::DirNoMain;
        t.text = "@nomain";
    } else {
        t.kind = TK::At;
        t.text = "@" + name;
        diag_.warning(loc(sL, sC, (int)name.size() + 1),
            std::format("unknown compiler directive '@{}'", name),
            lineFor(sL));
    }
    return t;
}

void Lexer::lex() {
    tokens_.clear();

    while (true) {
        skipWhitespaceAndComments();
        if (pos_ >= src_.size()) {
            tokens_.push_back(makeToken(TK::Eof, "<eof>", line_, col_));
            break;
        }

        int sL = line_, sC = col_;
        char c = peek();

        if (std::isdigit((unsigned char)c)) {
            tokens_.push_back(lexNumber(sL, sC));
            continue;
        }

        if (std::isalpha((unsigned char)c) || c == '_') {
            tokens_.push_back(lexIdentOrKeyword(sL, sC));
            continue;
        }

        if (c == '"') {
            advance();
            tokens_.push_back(lexStringLit(sL, sC));
            continue;
        }

        if (c == '\'') {
            advance();
            tokens_.push_back(lexCharLit(sL, sC));
            continue;
        }

        if (c == '`') {
            advance();
            tokens_.push_back(lexBacktickString(sL, sC));
            continue;
        }

        if (c == '@') {
            advance();
            tokens_.push_back(lexDirective(sL, sC));
            continue;
        }

        if (c == '#') {
            advance();
            tokens_.push_back(makeToken(TK::Hash, "#", sL, sC));
            continue;
        }

        advance();

        switch (c) {
        case '+':
            if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::PlusAssign, "+=", sL, sC)); }
            else                tokens_.push_back(makeToken(TK::Plus, "+", sL, sC));
            break;
        case '-':
            if (peek() == '>') { advance(); tokens_.push_back(makeToken(TK::Arrow, "->", sL, sC)); }
            else if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::MinusAssign, "-=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Minus, "-", sL, sC));
            break;
        case '*':
            if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::StarAssign, "*=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Star, "*", sL, sC));
            break;
        case '/':
            if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::SlashAssign, "/=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Slash, "/", sL, sC));
            break;
        case '%':
            if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::PercentAssign, "%=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Percent, "%", sL, sC));
            break;
        case '&':
            if (peek() == '&') { advance(); tokens_.push_back(makeToken(TK::And, "&&", sL, sC)); }
            else if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::AmpAssign, "&=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Amp, "&", sL, sC));
            break;
        case '|':
            if (peek() == '|') { advance(); tokens_.push_back(makeToken(TK::Or, "||", sL, sC)); }
            else if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::PipeAssign, "|=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Pipe, "|", sL, sC));
            break;
        case '^':
            if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::CaretAssign, "^=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Caret, "^", sL, sC));
            break;
        case '~':
            tokens_.push_back(makeToken(TK::Tilde, "~", sL, sC));
            break;
        case '!':
            if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::Neq, "!=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Bang, "!", sL, sC));
            break;
        case '<':
            if (peek() == '<') {
                advance();
                if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::LshiftAssign, "<<=", sL, sC)); }
                else tokens_.push_back(makeToken(TK::Lshift, "<<", sL, sC));
            } else if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::Leq, "<=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Lt, "<", sL, sC));
            break;
        case '>':
            if (peek() == '>') {
                advance();
                if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::RshiftAssign, ">>=", sL, sC)); }
                else tokens_.push_back(makeToken(TK::Rshift, ">>", sL, sC));
            } else if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::Geq, ">=", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Gt, ">", sL, sC));
            break;
        case '=':
            if (peek() == '=') { advance(); tokens_.push_back(makeToken(TK::Eq, "==", sL, sC)); }
            else tokens_.push_back(makeToken(TK::Assign, "=", sL, sC));
            break;
        case '?':  tokens_.push_back(makeToken(TK::Question,  "?",  sL, sC)); break;
        case ':':  tokens_.push_back(makeToken(TK::Colon,     ":",  sL, sC)); break;
        case '(':  tokens_.push_back(makeToken(TK::Lparen,    "(",  sL, sC)); break;
        case ')':  tokens_.push_back(makeToken(TK::Rparen,    ")",  sL, sC)); break;
        case '{':  tokens_.push_back(makeToken(TK::Lbrace,    "{",  sL, sC)); break;
        case '}':  tokens_.push_back(makeToken(TK::Rbrace,    "}",  sL, sC)); break;
        case '[':  tokens_.push_back(makeToken(TK::Lbracket,  "[",  sL, sC)); break;
        case ']':  tokens_.push_back(makeToken(TK::Rbracket,  "]",  sL, sC)); break;
        case ';':  tokens_.push_back(makeToken(TK::Semi,      ";",  sL, sC)); break;
        case ',':  tokens_.push_back(makeToken(TK::Comma,     ",",  sL, sC)); break;
        case '.':  tokens_.push_back(makeToken(TK::Dot,       ".",  sL, sC)); break;
        default:
            diag_.error(loc(sL, sC, 1),
                std::format("unexpected character '{}'", c),
                lineFor(sL));
            break;
        }
    }
}

void Lexer::dump() const {
    for (auto& t : tokens_) {
        std::cout << std::format("{}:{}:{} {:>15}  '{}'\n",
            t.loc.file, t.loc.line, t.loc.col,
            tkName(t.kind), t.text);
    }
}