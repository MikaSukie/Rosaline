#pragma once
#include "common.hpp"

enum class TK {
    IntLit, FloatLit, StringLit, CharLit, True, False,

    I8, I16, I32, I64,
    U8, U16, U32, U64,
    Bool, Char, Str, F32, F64,
    Void,

    Hash,

    Extern, Unsafe,
    Import, Opaque, Struct, Packed, Enum, Match,
    If, Else, While, Break, Continue, Return,
    Typeswitch, Typecase, Fallback,
    In,
    Nomd,
    Readonly, Take, Release, Crumble,
    For,

    Ident,

    Plus, Minus, Star, Slash, Percent,
    Amp, Pipe, Caret, Tilde,
    Lshift, Rshift,

    And, Or, Bang,

    Eq, Neq, Lt, Gt, Leq, Geq,

    Assign,
    PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
    AmpAssign, PipeAssign, CaretAssign, LshiftAssign, RshiftAssign,

    Lparen, Rparen, Lbrace, Rbrace, Lbracket, Rbracket,
    Semi, Comma, Dot, Arrow,
    Question, Colon,

    DirTDL,
    DirTTriple,
    DirNoMain,
    At,

    Bang2,

    Eof,
};

inline std::string tkName(TK k) {
    switch (k) {
    case TK::IntLit:     return "int-literal";
    case TK::FloatLit:   return "float-literal";
    case TK::StringLit:  return "string-literal";
    case TK::CharLit:    return "char-literal";
    case TK::True:       return "true";
    case TK::False:      return "false";
    case TK::I8:         return "i8";
    case TK::I16:        return "i16";
    case TK::I32:        return "i32";
    case TK::I64:        return "i64";
    case TK::U8:         return "u8";
    case TK::U16:        return "u16";
    case TK::U32:        return "u32";
    case TK::U64:        return "u64";
    case TK::Bool:       return "bool";
    case TK::Char:       return "char";
    case TK::Str:        return "str";
    case TK::F32:        return "f32";
    case TK::F64:        return "f64";
    case TK::Void:       return "void";
    case TK::Hash:       return "#";
    case TK::Import:     return "import";
    case TK::Opaque:     return "opaque";
    case TK::Struct:     return "struct";
    case TK::Packed:     return "packed";
    case TK::Enum:       return "enum";
    case TK::Match:      return "match";
    case TK::If:         return "if";
    case TK::Else:       return "else";
    case TK::While:      return "while";
    case TK::Break:      return "break";
    case TK::Continue:   return "continue";
    case TK::Extern:     return "extern";
    case TK::Unsafe:     return "unsafe";
    case TK::Return:     return "return";
    case TK::Typeswitch: return "typeswitch";
    case TK::Typecase:   return "typecase";
    case TK::Fallback:   return "fallback";
    case TK::In:         return "in";
    case TK::Nomd:       return "nomd";
    case TK::Readonly:   return "readonly";
    case TK::Take:       return "take";
    case TK::Release:    return "release";
    case TK::Crumble:    return "crumble";
    case TK::For:        return "for";
    case TK::Ident:      return "identifier";
    case TK::Plus:       return "+";
    case TK::Minus:      return "-";
    case TK::Star:       return "*";
    case TK::Slash:      return "/";
    case TK::Percent:    return "%";
    case TK::Amp:        return "&";
    case TK::Pipe:       return "|";
    case TK::Caret:      return "^";
    case TK::Tilde:      return "~";
    case TK::Lshift:     return "<<";
    case TK::Rshift:     return ">>";
    case TK::And:        return "&&";
    case TK::Or:         return "||";
    case TK::Bang:       return "!";
    case TK::Eq:         return "==";
    case TK::Neq:        return "!=";
    case TK::Lt:         return "<";
    case TK::Gt:         return ">";
    case TK::Leq:        return "<=";
    case TK::Geq:        return ">=";
    case TK::Assign:     return "=";
    case TK::PlusAssign: return "+=";
    case TK::MinusAssign:return "-=";
    case TK::StarAssign: return "*=";
    case TK::SlashAssign:return "/=";
    case TK::PercentAssign:return "%=";
    case TK::AmpAssign:  return "&=";
    case TK::PipeAssign: return "|=";
    case TK::CaretAssign:return "^=";
    case TK::LshiftAssign:return "<<=";
    case TK::RshiftAssign:return ">>=";
    case TK::Lparen:     return "(";
    case TK::Rparen:     return ")";
    case TK::Lbrace:     return "{";
    case TK::Rbrace:     return "}";
    case TK::Lbracket:   return "[";
    case TK::Rbracket:   return "]";
    case TK::Semi:       return ";";
    case TK::Comma:      return ",";
    case TK::Dot:        return ".";
    case TK::Arrow:      return "->";
    case TK::Question:   return "?";
    case TK::Colon:      return ":";
    case TK::DirTDL:     return "@tdl";
    case TK::DirTTriple: return "@ttriple";
    case TK::DirNoMain:  return "@nomain";
    case TK::At:         return "@";
    case TK::Eof:        return "<eof>";
    default:             return "?";
    }
}

struct Token {
    TK          kind;
    std::string text;
    SourceLoc   loc;

    int64_t     ival = 0;
    double      fval = 0.0;
};

class Lexer {
public:
    Lexer(std::string src, std::string filename, DiagEngine& diag);

    void lex();

    std::span<const Token> tokens() const { return tokens_; }

    void dump() const;

private:
    std::string     src_;
    std::string     file_;
    DiagEngine&     diag_;
    size_t          pos_  = 0;
    int             line_ = 1;
    int             col_  = 1;
    std::vector<Token> tokens_;

    char peek(int offset = 0) const;
    char advance();
    void skipWhitespaceAndComments();
    Token makeToken(TK k, std::string text, int startLine, int startCol);
    Token lexStringLit(int startLine, int startCol);
    Token lexBacktickString(int startLine, int startCol);
    Token lexCharLit(int startLine, int startCol);
    Token lexNumber(int startLine, int startCol);
    Token lexIdentOrKeyword(int startLine, int startCol);
    Token lexDirective(int startLine, int startCol);
    SourceLoc loc(int line, int col, int len = 1) const;
    std::string lineFor(int lineNum) const;
};
