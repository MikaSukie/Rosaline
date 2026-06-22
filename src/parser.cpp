#include "parser.hpp"
#include <stdexcept>

static const Token EOF_TOKEN = {TK::Eof, "<eof>", {"", 0, 0, 1}};

Parser::Parser(std::span<const Token> tokens, DiagEngine& diag,
               std::shared_ptr<SourceFile> sf)
: toks_(tokens), diag_(diag), sf_(std::move(sf)) {}

const Token& Parser::peek(int offset) const {
    size_t idx = pos_ + offset;
    if (idx >= toks_.size()) return EOF_TOKEN;
    return toks_[idx];
}

const Token& Parser::consume() {
    if (pos_ < toks_.size()) return toks_[pos_++];
    return EOF_TOKEN;
}

bool Parser::match(TK k) {
    if (check(k)) { consume(); return true; }
    return false;
}

std::string Parser::lineFor(const SourceLoc& l) const {
    if (sf_) return sf_->getLine(l.line);
    return {};
}

const Token& Parser::expect(TK k, std::string_view msg) {
    if (!check(k)) {
        std::string m = msg.empty()
        ? std::format("expected '{}', got '{}'", tkName(k), cur().text)
        : std::string(msg);
        diag_.error(cur().loc, m, lineFor(cur().loc));
    }
    return consume();
}

std::unique_ptr<Program> Parser::parse() {
    auto prog = std::make_unique<Program>();
    while (!atEnd()) {
        parseTopLevel(*prog);
    }
    return prog;
}

void Parser::parseTopLevel(Program& prog) {
    if (atEnd()) return;

    TK k = cur().kind;

    if (k == TK::Opaque) {
        consume();
        prog.opaques.push_back(parseOpaqueDecl());
        return;
    }

    if (k == TK::Packed) {
        consume();
        expect(TK::Struct, "expected 'struct' after 'packed'");
        prog.structs.push_back(parseStructDecl(true));
        return;
    }

    if (k == TK::Unsafe) {
        consume();
        expect(TK::Extern, "expected 'extern' after 'unsafe'");
        auto ed = parseExternDecl();
        ed->isUnsafe = true;
        prog.externs.push_back(std::move(ed));
        return;
    }

    if (k == TK::Extern) {
        consume();
        prog.externs.push_back(parseExternDecl());
        return;
    }

    if (k == TK::Struct) {
        consume();
        prog.structs.push_back(parseStructDecl(false));
        return;
    }

    if (k == TK::Enum) {
        consume();
        prog.enums.push_back(parseEnumDecl());
        return;
    }

    if (k == TK::In) {
        consume();
        prog.insertions.push_back(parseInsertion());
        return;
    }

    if (k == TK::Import) {
        while (!atEnd() && !check(TK::Semi)) consume();
        match(TK::Semi);
        return;
    }

    if (k == TK::DirTDL || k == TK::DirTTriple || k == TK::DirNoMain) {
        prog.directives.push_back(parseDirective());
        return;
    }

    if (k == TK::Nomd) {
        consume();
        prog.globals.push_back(parseGlobalDecl());
        if (prog.globals.back()) prog.globals.back()->nomd = true;
        return;
    }

    if (k == TK::Ident && cur().text == "main" && check(TK::Lparen, 1)) {
        auto fd = parseFuncDecl();
        fd->isMain = true;
        prog.funcs.push_back(std::move(fd));
        return;
    }

    if (k == TK::Ident && check(TK::Lparen, 1)) {
        prog.funcs.push_back(parseFuncDecl());
        return;
    }

    if (k == TK::Ident && check(TK::Lt, 1)) {
        size_t scan = pos_ + 2;
        int depth = 1;
        bool looksLikeTypeParams = true;
        while (scan < toks_.size() && depth > 0) {
            TK tk = toks_[scan].kind;
            if (tk == TK::Lt)  { ++depth; ++scan; continue; }
            if (tk == TK::Gt)  { if (--depth == 0) { ++scan; break; } ++scan; continue; }
            if (tk == TK::Eof || tk == TK::Semi) { looksLikeTypeParams = false; break; }
            if (tk != TK::Ident && tk != TK::Comma) { looksLikeTypeParams = false; break; }
            ++scan;
        }
        if (looksLikeTypeParams && depth == 0 &&
            scan < toks_.size() && toks_[scan].kind == TK::Lparen) {
            prog.funcs.push_back(parseFuncDecl());
            return;
        }
    }

    if (isTypeStart()) {
        prog.globals.push_back(parseGlobalDecl());
        return;
    }

    diag_.error(cur().loc,
                std::format("unexpected token '{}' at top level", cur().text),
                lineFor(cur().loc));
    consume();
}

std::unique_ptr<OpaqueDecl> Parser::parseOpaqueDecl() {
    auto od = std::make_unique<OpaqueDecl>();
    od->loc = cur().loc;

    od->name = expect(TK::Ident, "expected opaque type name after 'opaque'").text;
    expect(TK::Semi, "expected ';' after opaque declaration");
    return od;
}

std::unique_ptr<ExternDecl> Parser::parseExternDecl() {
    auto ed  = std::make_unique<ExternDecl>();
    ed->loc  = cur().loc;

    ed->name = expect(TK::Ident, "expected name after 'extern'").text;

    if (check(TK::Lparen)) {
        consume();
        if (check(TK::Comma)) {
            consume();
            ed->isVariadic = true;
        } else {
            while (!check(TK::Rparen) && !atEnd()) {
                Param p;
                p.loc  = cur().loc;
                if (match(TK::Nomd))
                    p.nomd = true;
                p.type = parseType();
                p.name = expect(TK::Ident, "expected parameter name").text;
                ed->params.push_back(std::move(p));

                if (match(TK::Comma)) {
                    if (check(TK::Rparen)) {
                        ed->isVariadic = true;
                        break;
                    }
                } else {
                    break;
                }
            }
        }

        expect(TK::Rparen, "expected ')' after extern parameters");
        if (check(TK::Semi)) {
            auto vt = std::make_unique<TypeNode>();
            vt->v = PrimType{PrimKind::Void};
            ed->retType = std::move(vt);
        } else {
            ed->retType = parseType();
        }
    } else {
        ed->isGlobal = true;
        ed->globalType = parseType();
    }

    expect(TK::Semi, "expected ';' after extern declaration");
    return ed;
}

std::unique_ptr<StructDecl> Parser::parseStructDecl(bool packed) {
    auto sd = std::make_unique<StructDecl>();
    sd->packed = packed;
    sd->loc    = cur().loc;
    sd->name   = expect(TK::Ident, "expected struct name").text;

    if (match(TK::Lt)) {
        do {
            sd->typeParams.push_back(expect(TK::Ident, "expected type parameter").text);
        } while (match(TK::Comma));
        expect(TK::Gt, "expected '>' after struct type parameters");
    }

    expect(TK::Lbrace, "expected '{' after struct name");

    while (!check(TK::Rbrace) && !atEnd()) {
        StructField f;
        f.loc  = cur().loc;
        f.type = parseType();
        f.name = expect(TK::Ident, "expected field name").text;
        expect(TK::Semi, "expected ';' after field declaration");
        sd->fields.push_back(std::move(f));
    }
    expect(TK::Rbrace, "expected '}' to close struct");
    return sd;
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    auto ed = std::make_unique<EnumDecl>();
    ed->loc  = cur().loc;
    ed->name = expect(TK::Ident, "expected enum name").text;

    if (match(TK::Lt)) {
        do {
            ed->typeParams.push_back(expect(TK::Ident, "expected type parameter").text);
        } while (match(TK::Comma));
        expect(TK::Gt, "expected '>' after type parameters");
    }

    expect(TK::Lbrace, "expected '{' after enum name");

    while (!check(TK::Rbrace) && !atEnd()) {
        EnumVariant v;
        v.loc  = cur().loc;
        v.name = expect(TK::Ident, "expected variant name").text;

        if (match(TK::Lparen)) {
            do {
                v.payload.push_back(parseType());
            } while (match(TK::Comma));
            expect(TK::Rparen, "expected ')' after variant payload");
        }
        expect(TK::Semi, "expected ';' after variant");
        ed->variants.push_back(std::move(v));
    }
    expect(TK::Rbrace, "expected '}' to close enum");
    return ed;
}

std::unique_ptr<FuncDecl> Parser::parseFuncDecl() {
    auto fd = std::make_unique<FuncDecl>();
    fd->loc  = cur().loc;
    fd->name = consume().text;

    genericParamsInScope_.clear();

    if (check(TK::Lt)) {
        size_t saved = pos_;
        consume();
        bool isGenericList = true;
        std::vector<std::string> explicitParams;
        while (!atEnd() && !check(TK::Gt)) {
            if (!check(TK::Ident)) { isGenericList = false; break; }
            explicitParams.push_back(consume().text);
            if (!match(TK::Comma)) break;
        }
        if (isGenericList && check(TK::Gt)) {
            consume();
            for (auto& p : explicitParams) {
                fd->genericParams.push_back(p);
                genericParamsInScope_.insert(p);
            }
        } else {
            pos_ = saved;
        }
    }

    expect(TK::Lparen, "expected '(' after function name");
    fd->params = parseParamList();
    expect(TK::Rparen, "expected ')' after parameters");

    if (check(TK::Lbrace)) {
        auto vt = std::make_unique<TypeNode>();
        vt->v = PrimType{PrimKind::Void};
        fd->retType = std::move(vt);
    } else {
        fd->retType = parseType();
    }

    for (auto& p : fd->params) {
        if (p.type && p.type->isNamed()) {
            auto& nt = p.type->asNamed();
            if (nt.name.size() == 1 && std::isupper(nt.name[0])) {
                if (!genericParamsInScope_.count(nt.name)) {
                    fd->genericParams.push_back(nt.name);
                    genericParamsInScope_.insert(nt.name);
                }
            }
        }
    }
    std::ranges::sort(fd->genericParams);
    fd->genericParams.erase(std::unique(fd->genericParams.begin(), fd->genericParams.end()),
                            fd->genericParams.end());

    if (fd->retType && fd->retType->isHash())
        fd->genericParams.push_back("#ctx");

    expect(TK::Lbrace, "expected '{' to start function body");
    fd->body = parseBlock();
    expect(TK::Rbrace, "expected '}' to close function body");

    genericParamsInScope_.clear();
    return fd;
}

std::unique_ptr<GlobalDecl> Parser::parseGlobalDecl() {
    auto gd  = std::make_unique<GlobalDecl>();
    gd->loc  = cur().loc;
    gd->type = parseType();
    gd->name = expect(TK::Ident, "expected variable name").text;
    if (match(TK::Assign))
        gd->init = parseExpr();
    expect(TK::Semi, "expected ';' after global declaration");
    return gd;
}

Insertion Parser::parseInsertion() {
    Insertion ins;
    ins.loc        = cur().loc;
    ins.targetFunc = expect(TK::Ident, "expected function name after 'in'").text;
    expect(TK::Lbrace, "expected '{' after function name in 'in' block");

    expect(TK::Typecase, "expected 'typecase' inside 'in' block");
    expect(TK::Lparen, "expected '(' after 'typecase'");
    ins.arm.matchType = parseType();
    expect(TK::Rparen, "expected ')'");

    expect(TK::For, "expected 'for' after typecase type");
    expect(TK::Lparen, "expected '('");
    if (check(TK::Hash)) {
        ins.forParam = "#";
        consume();
    } else {
        ins.forParam = expect(TK::Ident, "expected parameter name or '#'").text;
    }
    expect(TK::Rparen, "expected ')'");

    expect(TK::Lbrace, "expected '{'");
    ins.arm.body = parseBlock();
    expect(TK::Rbrace, "expected '}'");

    expect(TK::Rbrace, "expected '}' to close 'in' block");

    return ins;
}

DirectiveDecl Parser::parseDirective() {
    DirectiveDecl d;
    d.loc = cur().loc;

    if (cur().kind == TK::DirNoMain) {
        d.key = "nomain";
        consume();
        expect(TK::Semi, "expected ';' after '@nomain'");
        return d;
    }

    d.key = (cur().kind == TK::DirTDL) ? "tdl" : "ttriple";
    consume();
    expect(TK::Assign, "expected '=' after directive name");
    if (!check(TK::StringLit))
        diag_.error(cur().loc, "expected string value for directive", lineFor(cur().loc));
    d.value = consume().text;
    expect(TK::Semi, "expected ';' after directive");
    return d;
}

std::vector<Param> Parser::parseParamList() {
    std::vector<Param> params;
    if (check(TK::Rparen)) return params;

    auto parseOneParam = [&]() -> Param {
        Param p;
        p.loc  = cur().loc;

        if (cur().kind == TK::Ident &&
            (cur().text == "argc" || cur().text == "argv")) {
            std::string pseudo = consume().text;
        p.name = expect(TK::Ident, "expected parameter name").text;
        auto t = std::make_unique<TypeNode>();
        if (pseudo == "argc") {
            t->v = PrimType{PrimKind::I32};
        } else {
            auto inner = std::make_unique<TypeNode>();
            inner->v = PrimType{PrimKind::Str};
            t->v = PtrType{std::move(inner)};
        }
        p.type = std::move(t);
        return p;
            }

            p.type = parseType();
            p.name = expect(TK::Ident, "expected parameter name").text;
            return p;
    };

    params.push_back(parseOneParam());
    while (match(TK::Comma)) {
        if (check(TK::Rparen)) break;
        params.push_back(parseOneParam());
    }
    return params;
}

bool Parser::isTypeStart(int offset) const {
    TK k = peek(offset).kind;
    return k == TK::I8 || k == TK::I16 || k == TK::I32 || k == TK::I64
    || k == TK::U8 || k == TK::U16 || k == TK::U32 || k == TK::U64
    || k == TK::IPtr || k == TK::UPtr || k == TK::ISize || k == TK::USize
    || k == TK::Bool || k == TK::Char || k == TK::Str
    || k == TK::F32 || k == TK::F64
    || k == TK::Void
    || k == TK::Hash
    || k == TK::Fn
    || (k == TK::Ident);
}

bool Parser::isGenericTypeName(const std::string& name) const {
    return genericParamsInScope_.count(name) > 0;
}

bool Parser::looksLikeGenericCall() const {
    size_t depth = 0;
    size_t scan  = pos_;
    if (scan >= toks_.size() || toks_[scan].kind != TK::Lt) return false;
    ++scan; ++depth;
    while (scan < toks_.size() && depth > 0) {
        TK k = toks_[scan].kind;
        switch (k) {
            case TK::Lt:  ++depth; break;
            case TK::Gt:  --depth; if (depth == 0) goto done; break;

            case TK::Eof:
            case TK::Semi:
            case TK::Lbrace:
            case TK::Rbrace:
            case TK::Lbracket:
                return false;
            case TK::Arrow:
            case TK::Assign:
            case TK::PlusAssign: case TK::MinusAssign:
            case TK::StarAssign:  case TK::SlashAssign: case TK::PercentAssign:
            case TK::AmpAssign:   case TK::PipeAssign:  case TK::CaretAssign:
            case TK::LshiftAssign: case TK::RshiftAssign:
            case TK::Or:
            case TK::And:
            case TK::Eq:
            case TK::Neq:
            case TK::Leq:
            case TK::Geq:
            case TK::Plus: case TK::Minus:
            case TK::Slash: case TK::Percent:
            case TK::IntLit: case TK::FloatLit:
            case TK::StringLit: case TK::CharLit:
            case TK::True: case TK::False:
                return false;
            default: break;
        }
        ++scan;
    }
    done:
    if (depth != 0) return false;
    ++scan;
    if (scan >= toks_.size()) return false;
    return toks_[scan].kind == TK::Lparen;
}

TypeNodePtr Parser::parseBaseType() {
    auto t = std::make_unique<TypeNode>();
    t->loc = cur().loc;

    auto makePrim = [&](PrimKind k) {
        consume();
        t->v = PrimType{k};
    };

    switch (cur().kind) {
        case TK::I8:   makePrim(PrimKind::I8);       break;
        case TK::I16:  makePrim(PrimKind::I16);      break;
        case TK::I32:  makePrim(PrimKind::I32);      break;
        case TK::I64:  makePrim(PrimKind::I64);      break;
        case TK::U8:   makePrim(PrimKind::U8);       break;
        case TK::U16:  makePrim(PrimKind::U16);      break;
        case TK::U32:  makePrim(PrimKind::U32);      break;
        case TK::U64:  makePrim(PrimKind::U64);      break;
        case TK::IPtr: makePrim(PrimKind::IPtr);     break;
        case TK::UPtr: makePrim(PrimKind::UPtr);     break;
        case TK::ISize:makePrim(PrimKind::ISize);    break;
        case TK::USize:makePrim(PrimKind::USize);    break;
        case TK::Bool: makePrim(PrimKind::Bool);     break;
        case TK::Char: makePrim(PrimKind::Char);     break;
        case TK::Str:  makePrim(PrimKind::Str);      break;
        case TK::F32:  makePrim(PrimKind::F32);      break;
        case TK::F64:  makePrim(PrimKind::F64);      break;
        case TK::Void: makePrim(PrimKind::Void);     break;
        case TK::Hash: consume(); t->v = HashType{}; break;
        case TK::Fn: {
            consume();
            expect(TK::Lparen, "expected '(' after 'fn' in function type");
            FuncType ft;
            if (!check(TK::Rparen)) {
                ft.paramTypes.push_back(parseType());
                while (match(TK::Comma)) {
                    if (check(TK::Rparen)) break;
                    ft.paramTypes.push_back(parseType());
                }
            }
            expect(TK::Rparen, "expected ')' after function type parameters");
            if (isTypeStart()) {
                ft.retType = parseType();
            } else {
                auto vt = std::make_unique<TypeNode>();
                vt->v = PrimType{PrimKind::Void};
                ft.retType = std::move(vt);
            }
            t->v = std::move(ft);
            break;
        }
        case TK::Ident: {
            NamedType nt;
            nt.name = consume().text;
            if (check(TK::Lt)) {
                consume();
                do {
                    nt.typeArgs.push_back(parseType());
                } while (match(TK::Comma));
                expect(TK::Gt, "expected '>' after type arguments");
            }
            t->v = std::move(nt);
            break;
        }
        default:
            diag_.error(cur().loc,
                        std::format("expected type, got '{}'", cur().text),
                        lineFor(cur().loc));
            t->v = PrimType{PrimKind::I32};
            break;
    }
    return t;
}

TypeNodePtr Parser::parseType() {
    auto base = parseBaseType();

    while (check(TK::Star)) {
        consume();
        auto pt = std::make_unique<TypeNode>();
        pt->loc = base->loc;
        pt->v   = PtrType{std::move(base)};
        base    = std::move(pt);
    }

    if (check(TK::Lbracket)) {
        consume();
        auto at  = std::make_unique<TypeNode>();
        at->loc  = base->loc;
        ArrayType arrTy;
        arrTy.elem = std::move(base);
        if (!check(TK::Rbracket))
            arrTy.size = parseExpr();
        expect(TK::Rbracket, "expected ']' in array type");
        at->v = std::move(arrTy);
        base  = std::move(at);
    }

    return base;
}

std::vector<StmtPtr> Parser::parseBlock() {
    std::vector<StmtPtr> stmts;
    while (!check(TK::Rbrace) && !atEnd()) {
        stmts.push_back(parseStmt());
    }
    return stmts;
}

StmtPtr Parser::parseStmt() {
    SourceLoc sLoc = cur().loc;

    if (check(TK::Nomd)) {
        consume();
        return parseVarDecl(true);
    }

    if (check(TK::If)) return parseIf();

    if (check(TK::While)) return parseWhile();

    if (check(TK::Return)) return parseReturn();

    if (check(TK::DirAsm)) {
        consume();
        expect(TK::Lbrace, "expected '{' after '@asm'");

        AsmBlockStmt ab;
        ab.isVolatile = true;

        if (check(TK::StringLit)) {
            ab.asmTemplate = cur().text;
            consume();
        } else {
            diag_.error(cur().loc,
                "expected string literal for asm template inside '@asm { ... }'",
                lineFor(cur().loc));
            ab.asmTemplate = "";
        }

        if (match(TK::Colon)) {
            if (check(TK::StringLit)) {
                ab.constraints = cur().text;
                consume();
            }
            if (match(TK::Colon)) {
                if (check(TK::StringLit)) {
                    ab.clobbers = cur().text;
                    consume();
                }
            }
        }

        expect(TK::Rbrace, "expected '}' to close '@asm' block");
        expect(TK::Semi,   "expected ';' after '@asm { ... }'");

        auto s = std::make_unique<Stmt>();
        s->loc = sLoc;
        s->v   = std::move(ab);
        return s;
    }

    if (check(TK::Break)) {
        consume();
        expect(TK::Semi, "expected ';' after 'break'");
        auto s = std::make_unique<Stmt>();
        s->loc = sLoc; s->v = BreakStmt{};
        return s;
    }

    if (check(TK::Continue)) {
        consume();
        expect(TK::Semi, "expected ';' after 'continue'");
        auto s = std::make_unique<Stmt>();
        s->loc = sLoc; s->v = ContinueStmt{};
        return s;
    }

    bool looksLikeDecl = false;
    if (isTypeStart() && !check(TK::Ident, 0)) {
        looksLikeDecl = true;
    } else if (check(TK::Ident)) {
        int off = 1;
        while (peek(off).kind == TK::Star) ++off;
        if (peek(off).kind == TK::Ident &&
            peek(off + 1).kind != TK::Lparen)
            looksLikeDecl = true;
        if (peek(off).kind == TK::Lt && !looksLikeGenericCall())
            looksLikeDecl = true;
    }

    if (looksLikeDecl) {
        return parseVarDecl(false);
    }

    if (check(TK::Match) || check(TK::Typeswitch)) {
        auto expr = parseExpr();
        match(TK::Semi);
        auto s = std::make_unique<Stmt>();
        s->loc = sLoc;
        s->v   = ExprStmt{std::move(expr)};
        return s;
    }

    auto expr = parseExpr();
    if (!check(TK::Semi)) {
        diag_.error(cur().loc, "expected ';' after expression", lineFor(cur().loc));
        synchronize({TK::Semi, TK::Rbrace});
    }
    match(TK::Semi);
    auto s = std::make_unique<Stmt>();
    s->loc = sLoc;
    s->v   = ExprStmt{std::move(expr)};
    return s;
}

StmtPtr Parser::parseVarDecl(bool nomd) {
    auto s = std::make_unique<Stmt>();
    s->loc = cur().loc;

    VarDeclStmt vd;
    vd.nomd = nomd;
    vd.type = parseType();
    vd.name = expect(TK::Ident, "expected variable name").text;
    if (match(TK::Assign))
        vd.init = parseExpr();
    expect(TK::Semi, "expected ';' after variable declaration");
    s->v = std::move(vd);
    return s;
}

StmtPtr Parser::parseIf() {
    auto s = std::make_unique<Stmt>();
    s->loc = cur().loc;
    consume();

    IfStmt ifs;
    {
        bool savedNSI = noStructInit_;
        noStructInit_ = true;
        ifs.cond = parseExpr();
        noStructInit_ = savedNSI;
    }

    expect(TK::Lbrace, "expected '{' after if condition");
    ifs.thenBody = parseBlock();
    expect(TK::Rbrace, "expected '}'");

    if (match(TK::Else)) {
        if (check(TK::If)) {
            ifs.elseBody.push_back(parseIf());
        } else {
            expect(TK::Lbrace, "expected '{' after 'else'");
            ifs.elseBody = parseBlock();
            expect(TK::Rbrace, "expected '}'");
        }
    }

    s->v = std::move(ifs);
    return s;
}

StmtPtr Parser::parseWhile() {
    auto s = std::make_unique<Stmt>();
    s->loc = cur().loc;
    consume();

    WhileStmt ws;
    {
        bool savedNSI = noStructInit_;
        noStructInit_ = true;
        ws.cond = parseExpr();
        noStructInit_ = savedNSI;
    }

    expect(TK::Lbrace, "expected '{' after while condition");
    ws.body = parseBlock();
    expect(TK::Rbrace, "expected '}'");

    s->v = std::move(ws);
    return s;
}

StmtPtr Parser::parseReturn() {
    auto s = std::make_unique<Stmt>();
    s->loc = cur().loc;
    consume();

    ReturnStmt rs;
    if (!check(TK::Semi))
        rs.value = parseExpr();
    expect(TK::Semi, "expected ';' after return");
    s->v = std::move(rs);
    return s;
}

ExprPtr Parser::parseExpr() { return parseAssign(); }

static bool isAssignOp(TK k) {
    return k == TK::Assign || k == TK::PlusAssign  || k == TK::MinusAssign
    || k == TK::StarAssign || k == TK::SlashAssign || k == TK::PercentAssign
    || k == TK::AmpAssign  || k == TK::PipeAssign  || k == TK::CaretAssign
    || k == TK::LshiftAssign || k == TK::RshiftAssign;
}

ExprPtr Parser::parseTernary() {
    auto cond = parseLogicalOr();
    if (!check(TK::Question)) return cond;

    SourceLoc loc = cur().loc;
    consume();
    auto thenExpr = parseExpr();
    expect(TK::Colon, "expected ':' in ternary expression");
    auto elseExpr = parseTernary();

    TernaryExpr te;
    te.cond     = std::move(cond);
    te.thenExpr = std::move(thenExpr);
    te.elseExpr = std::move(elseExpr);

    auto e = std::make_unique<Expr>();
    e->loc = loc;
    e->v   = std::move(te);
    return e;
}

ExprPtr Parser::parseAssign() {
    auto lhs = parseTernary();
    if (isAssignOp(cur().kind)) {
        std::string op = cur().text;
        SourceLoc loc  = cur().loc;
        consume();
        auto rhs = parseAssign();
        auto e = std::make_unique<Expr>();
        e->loc = loc;
        e->v   = AssignExpr{op, std::move(lhs), std::move(rhs)};
        return e;
    }
    return lhs;
}

ExprPtr Parser::parseLogicalOr() {
    auto lhs = parseLogicalAnd();
    while (check(TK::Or)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseLogicalAnd();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseLogicalAnd() {
    auto lhs = parseBitOr();
    while (check(TK::And)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseBitOr();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseBitOr() {
    auto lhs = parseBitXor();
    while (check(TK::Pipe)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseBitXor();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseBitXor() {
    auto lhs = parseBitAnd();
    while (check(TK::Caret)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseBitAnd();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseBitAnd() {
    auto lhs = parseEquality();
    while (check(TK::Amp)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseEquality();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseEquality() {
    auto lhs = parseRelational();
    while (check(TK::Eq) || check(TK::Neq)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseRelational();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseRelational() {
    auto lhs = parseShift();
    while (check(TK::Lt) || check(TK::Gt) || check(TK::Leq) || check(TK::Geq)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseShift();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseShift() {
    auto lhs = parseAddSub();
    while (check(TK::Lshift) || check(TK::Rshift)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseAddSub();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseAddSub() {
    auto lhs = parseMulDiv();
    while (check(TK::Plus) || check(TK::Minus)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseMulDiv();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseMulDiv() {
    auto lhs = parseUnary();
    while (check(TK::Star) || check(TK::Slash) || check(TK::Percent)) {
        std::string op = cur().text; SourceLoc loc = cur().loc; consume();
        auto rhs = parseUnary();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BinOpExpr{op, std::move(lhs), std::move(rhs)};
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseUnary() {
    SourceLoc loc = cur().loc;

    if (check(TK::Lparen) && isTypeStart(1)) {
        size_t saved          = pos_;
        size_t savedDiagCount = diag_.diags.size();
        bool   savedHasErrors = diag_.hasErrors;
        consume();
        if (isTypeStart()) {
            TypeNodePtr tp;
            try { tp = parseType(); } catch(...) {
                pos_ = saved;
                diag_.diags.resize(savedDiagCount);
                diag_.hasErrors = savedHasErrors;
                goto not_cast;
            }
            if (diag_.diags.size() == savedDiagCount && check(TK::Rparen)) {
                consume();
                auto operand = parseUnary();
                auto e = std::make_unique<Expr>();
                e->loc = loc; e->v = CastExpr{std::move(tp), std::move(operand)};
                return e;
            }
        }
        diag_.diags.resize(savedDiagCount);
        diag_.hasErrors = savedHasErrors;
        pos_ = saved;
    }
    not_cast:

    if (check(TK::Bang)) {
        consume();
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = UnaryOpExpr{"!", std::move(operand)};
        return e;
    }
    if (check(TK::Tilde)) {
        consume();
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = UnaryOpExpr{"~", std::move(operand)};
        return e;
    }
    if (check(TK::Minus)) {
        consume();
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = UnaryOpExpr{"-", std::move(operand)};
        return e;
    }
    if (check(TK::Amp)) {
        consume();
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = AddressOfExpr{std::move(operand)};
        return e;
    }
    if (check(TK::Star)) {
        consume();
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = DerefExpr{std::move(operand)};
        return e;
    }

    if (check(TK::Readonly)) {
        consume();
        expect(TK::Lparen, "expected '('");
        auto op = parseExpr();
        expect(TK::Rparen, "expected ')'");
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = ReadonlyExpr{std::move(op)};
        return e;
    }
    if (check(TK::Take)) {
        consume();
        expect(TK::Lparen, "expected '('");
        auto op = parseExpr();
        expect(TK::Rparen, "expected ')'");
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = TakeExpr{std::move(op)};
        return e;
    }
    if (check(TK::Release)) {
        consume();
        expect(TK::Lparen, "expected '('");
        auto op = parseExpr();
        expect(TK::Rparen, "expected ')'");
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = ReleaseExpr{std::move(op)};
        return e;
    }
    if (check(TK::Crumble)) {
        consume();
        expect(TK::Lparen, "expected '('");
        auto op = parseExpr();
        expect(TK::Rparen, "expected ')'");
        return parseCrumbleConstraints(std::move(op));
    }

    auto base = parsePrimary();
    return parsePostfix(std::move(base));
}

ExprPtr Parser::parsePostfix(ExprPtr base) {
    while (true) {
        SourceLoc loc = cur().loc;

        if (check(TK::Dot)) {
            consume();
            std::string field = expect(TK::Ident, "expected field name").text;
            auto e = std::make_unique<Expr>();
            e->loc = loc;
            e->v   = FieldAccessExpr{std::move(base), field};
            base   = std::move(e);
            continue;
        }

        if (check(TK::Lbracket)) {
            consume();
            auto idx = parseExpr();
            expect(TK::Rbracket, "expected ']'");
            auto e = std::make_unique<Expr>();
            e->loc = loc;
            e->v   = IndexExpr{std::move(base), std::move(idx)};
            base   = std::move(e);
            continue;
        }

        if (check(TK::Lparen) && !base->is<IdentExpr>()) {
            consume();
            CallExpr call;
            call.callee = std::move(base);
            if (!check(TK::Rparen)) {
                call.args.push_back(parseExpr());
                while (match(TK::Comma)) {
                    if (check(TK::Rparen)) break;
                    call.args.push_back(parseExpr());
                }
            }
            expect(TK::Rparen, "expected ')'");
            auto e = std::make_unique<Expr>();
            e->loc = loc;
            e->v   = std::move(call);
            base   = std::move(e);
            continue;
        }

        break;
    }
    return base;
}

ExprPtr Parser::parsePrimary() {
    SourceLoc loc = cur().loc;

    if (check(TK::IntLit)) {
        int64_t v = cur().ival;
        consume();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = IntLitExpr{v};
        return e;
    }

    if (check(TK::FloatLit)) {
        double v = cur().fval;
        consume();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = FloatLitExpr{v};
        return e;
    }

    if (check(TK::StringLit)) {
        std::string v = cur().text;
        consume();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = StringLitExpr{v};
        return e;
    }

    if (check(TK::CharLit)) {
        char v = cur().text.empty() ? '\0' : cur().text[0];
        consume();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = CharLitExpr{v};
        return e;
    }

    if (check(TK::True) || check(TK::False)) {
        bool v = cur().kind == TK::True;
        consume();
        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = BoolLitExpr{v};
        return e;
    }

    if (check(TK::Lparen)) {
        consume();
        auto inner = parseExpr();
        expect(TK::Rparen, "expected ')'");
        return inner;
    }

    if (check(TK::Lbracket)) {
        consume();
        auto e = std::make_unique<Expr>();
        e->loc = loc;
        ArrayLitExpr alit;
        if (!check(TK::Rbracket)) {
            alit.elems.push_back(parseExpr());
            while (match(TK::Comma)) {
                if (check(TK::Rbracket)) break;
                alit.elems.push_back(parseExpr());
            }
        }
        expect(TK::Rbracket, "expected ']' after array literal");
        e->v = std::move(alit);
        return e;
    }

    if (check(TK::Typeswitch)) {
        return parseTypeswitchExpr();
    }

    if (check(TK::Match)) {
        return parseMatchExpr();
    }

    if (check(TK::Ident)) {
        std::string name = consume().text;

        if (check(TK::Arrow)) {
            consume();
            std::string variant = expect(TK::Ident, "expected variant name").text;
            EnumAccessExpr ea;
            ea.enumName = name;
            ea.variant  = variant;
            if (check(TK::Lparen)) {
                consume();
                if (!check(TK::Rparen)) {
                    ea.args.push_back(parseExpr());
                    while (match(TK::Comma)) {
                        if (check(TK::Rparen)) break;
                        ea.args.push_back(parseExpr());
                    }
                }
                expect(TK::Rparen, "expected ')' after enum variant payload");
            }
            auto e = std::make_unique<Expr>();
            e->loc = loc;
            e->v   = std::move(ea);
            return e;
        }

        if (check(TK::Lbrace) && !noStructInit_) {
            return parseStructInit(std::move(name), loc);
        }

        if (check(TK::Lt) && !noStructInit_) {
            size_t depth = 0, scan = pos_;
            bool isGenericStructInit = false;
            if (toks_[scan].kind == TK::Lt) {
                ++scan; ++depth;
                while (scan < toks_.size() && depth > 0) {
                    TK k = toks_[scan].kind;
                    if      (k == TK::Lt)  ++depth;
                    else if (k == TK::Gt)  { --depth; if (depth == 0) { ++scan; break; } }
                    else if (k == TK::Eof || k == TK::Semi) break;
                    ++scan;
                }
                if (depth == 0 && scan < toks_.size() && toks_[scan].kind == TK::Lbrace)
                    isGenericStructInit = true;
            }
            if (isGenericStructInit) {
                return parseStructInit(std::move(name), loc);
            }
        }

        bool isCall       = check(TK::Lparen);
        bool isGenericCall = check(TK::Lt) && looksLikeGenericCall();
        if (isCall || isGenericCall) {
            CallExpr call;
            auto callee = std::make_unique<Expr>();
            callee->loc = loc; callee->v = IdentExpr{name};
            call.callee = std::move(callee);

            if (isGenericCall) {
                consume();
                do {
                    call.typeArgs.push_back(parseType());
                } while (match(TK::Comma));
                expect(TK::Gt, "expected '>'");
            }

            expect(TK::Lparen, "expected '('");
            if (!check(TK::Rparen)) {
                call.args.push_back(parseExpr());
                while (match(TK::Comma)) {
                    if (check(TK::Rparen)) break;
                    call.args.push_back(parseExpr());
                }
            }
            expect(TK::Rparen, "expected ')'");

            auto e = std::make_unique<Expr>();
            e->loc = loc; e->v = std::move(call);
            return e;
        }

        auto e = std::make_unique<Expr>();
        e->loc = loc; e->v = IdentExpr{name};
        return e;
    }

    diag_.error(cur().loc,
                std::format("expected expression, got '{}'", cur().text),
                lineFor(cur().loc));
    if (!atEnd()) consume();
    auto e = std::make_unique<Expr>();
    e->loc = loc; e->v = IntLitExpr{0};
    return e;
}

void Parser::synchronize(std::initializer_list<TK> stops) {
    while (!atEnd()) {
        for (TK s : stops) {
            if (check(s)) return;
        }
        consume();
    }
}

ExprPtr Parser::parseStructInit(std::string name, SourceLoc loc) {
    auto makeType = [&](const std::string& n) -> TypeNodePtr {
        auto t = std::make_unique<TypeNode>();
        t->loc = loc;
        NamedType nt;
        nt.name = n;
        if (check(TK::Lt)) {
            consume();
            do {
                nt.typeArgs.push_back(parseType());
            } while (match(TK::Comma));
            expect(TK::Gt, "expected '>' after type arguments");
        }
        t->v = std::move(nt);
        return t;
    };

    StructInitExpr si;
    si.type = makeType(name);

    expect(TK::Lbrace, "expected '{' after struct name");
    while (!check(TK::Rbrace) && !atEnd()) {
        StructInitField f;
        f.name = expect(TK::Ident, "expected field name").text;

        if (match(TK::Colon)) {
            f.value = parseExpr();
        } else if (match(TK::Lparen)) {
            f.value = parseExpr();
            expect(TK::Rparen, "expected ')' after field value");
        } else {
            diag_.error(cur().loc,
                        "expected ':' or '(' after field name",
                        lineFor(cur().loc));
            while (!atEnd() && !check(TK::Rbrace) &&
                !check(TK::Semi) && !check(TK::Comma))
                consume();
        }

        si.fields.push_back(std::move(f));

        match(TK::Semi);
        match(TK::Comma);
    }
    expect(TK::Rbrace, "expected '}' after struct literal");

    auto e = std::make_unique<Expr>();
    e->loc = loc;
    e->v   = std::move(si);
    return e;
}

ExprPtr Parser::parseTypeswitchExpr() {
    SourceLoc loc = cur().loc;
    consume();
    expect(TK::Lparen, "expected '(' after typeswitch");

    std::string param;
    if (check(TK::Hash)) { consume(); param = "#"; }
    else param = expect(TK::Ident, "expected parameter name or '#'").text;

    expect(TK::Rparen, "expected ')'");
    expect(TK::Lbrace, "expected '{'");

    TypeswitchExpr ts;
    ts.param = param;
    ts.arms  = parseTypecaseArms();

    expect(TK::Rbrace, "expected '}'");

    auto e = std::make_unique<Expr>();
    e->loc = loc; e->v = std::move(ts);
    return e;
}

std::vector<TypecaseArm> Parser::parseTypecaseArms() {
    std::vector<TypecaseArm> arms;
    while (!check(TK::Rbrace) && !atEnd()) {
        TypecaseArm arm;
        if (check(TK::Fallback)) {
            consume();
            arm.isFallback = true;
        } else {
            expect(TK::Typecase, "expected 'typecase' or 'fallback'");
            expect(TK::Lparen,   "expected '('");
            arm.matchType = parseType();
            expect(TK::Rparen,   "expected ')'");
        }
        expect(TK::Lbrace, "expected '{'");
        arm.body = parseBlock();
        expect(TK::Rbrace, "expected '}'");
        arms.push_back(std::move(arm));
    }
    return arms;
}

ExprPtr Parser::parseMatchExpr() {
    SourceLoc loc = cur().loc;
    consume();

    bool hasParen = match(TK::Lparen);
    bool savedNSI = noStructInit_;
    noStructInit_ = true;
    auto subject = parseExpr();
    noStructInit_ = savedNSI;
    if (hasParen) expect(TK::Rparen, "expected ')'");

    expect(TK::Lbrace, "expected '{'");

    MatchExpr mx;
    mx.subject = std::move(subject);

    while (!check(TK::Rbrace) && !atEnd()) {
        MatchArm arm;
        arm.loc = cur().loc;
        arm.variantName = expect(TK::Ident, "expected variant name").text;
        if (arm.variantName == "_") {
            arm.isWildcard = true;
        }

        if (check(TK::Lparen)) {
            consume();
            do {
                arm.bindNames.push_back(expect(TK::Ident, "expected bind name").text);
            } while (match(TK::Comma));
            expect(TK::Rparen, "expected ')'");
        }

        expect(TK::Lbrace, "expected '{'");
        arm.body = parseBlock();
        expect(TK::Rbrace, "expected '}'");
        mx.arms.push_back(std::move(arm));
    }
    expect(TK::Rbrace, "expected '}'");

    auto e = std::make_unique<Expr>();
    e->loc = loc; e->v = std::move(mx);
    return e;
}

ExprPtr Parser::parseCrumbleConstraints(ExprPtr operand) {
    CrumbleExpr ce;
    ce.operand = std::move(operand);
    while (check(TK::Bang)) {
        consume();
        std::string which = expect(TK::Ident, "expected 'r' or 'w'").text;
        expect(TK::Assign, "expected '='");
        int64_t val = expect(TK::IntLit, "expected integer").ival;
        if (which == "r") ce.readLimit  = (int)val;
        else if (which == "w") ce.writeLimit = (int)val;
        else diag_.error(cur().loc, "crumble constraint must be 'r' or 'w'");
    }
    auto e = std::make_unique<Expr>();
    e->v = std::move(ce);
    return e;
}

static std::string indent(int n) { return std::string(n * 2, ' '); }

static std::string escapeForDump(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '\0': out += "\\0";  break;
            default:
                if (c < 32 || c == 127) {
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\x%02X", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

void dumpExpr(const Expr& e, int d) {
    std::visit([&](auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, IntLitExpr>)
            std::cout << indent(d) << "IntLit(" << x.val << ")\n";
        else if constexpr (std::is_same_v<T, FloatLitExpr>)
            std::cout << indent(d) << "FloatLit(" << x.val << ")\n";
        else if constexpr (std::is_same_v<T, StringLitExpr>)
            std::cout << indent(d) << "StringLit(\"" << escapeForDump(x.val) << "\")\n";
        else if constexpr (std::is_same_v<T, IdentExpr>)
            std::cout << indent(d) << "Ident(" << x.name << ")\n";
        else if constexpr (std::is_same_v<T, BinOpExpr>) {
            std::cout << indent(d) << "BinOp(" << x.op << ")\n";
            dumpExpr(*x.lhs, d+1); dumpExpr(*x.rhs, d+1);
        } else if constexpr (std::is_same_v<T, CallExpr>) {
            std::cout << indent(d) << "Call\n";
            dumpExpr(*x.callee, d+1);
            for (auto& a : x.args) dumpExpr(*a, d+1);
        } else if constexpr (std::is_same_v<T, AssignExpr>) {
            std::cout << indent(d) << "Assign(" << x.op << ")\n";
            dumpExpr(*x.lhs, d+1); dumpExpr(*x.rhs, d+1);
        } else {
            std::cout << indent(d) << "Expr(...)\n";
        }
    }, e.v);
}

void dumpStmt(const Stmt& s, int d) {
    std::visit([&](auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, ExprStmt>) {
            std::cout << indent(d) << "ExprStmt\n";
            dumpExpr(*x.expr, d+1);
        } else if constexpr (std::is_same_v<T, VarDeclStmt>) {
            std::cout << indent(d) << "VarDecl " << (x.nomd?"nomd ":"")
            << x.type->str() << " " << x.name << "\n";
            if (x.init) dumpExpr(*x.init, d+1);
        } else if constexpr (std::is_same_v<T, IfStmt>) {
            std::cout << indent(d) << "If\n";
            dumpExpr(*x.cond, d+1);
            for (auto& s2 : x.thenBody) dumpStmt(*s2, d+1);
            if (!x.elseBody.empty()) {
                std::cout << indent(d) << "Else\n";
                for (auto& s2 : x.elseBody) dumpStmt(*s2, d+1);
            }
        } else if constexpr (std::is_same_v<T, WhileStmt>) {
            std::cout << indent(d) << "While\n";
            dumpExpr(*x.cond, d+1);
            for (auto& s2 : x.body) dumpStmt(*s2, d+1);
        } else if constexpr (std::is_same_v<T, ReturnStmt>) {
            std::cout << indent(d) << "Return\n";
            if (x.value) dumpExpr(*x.value, d+1);
        } else {
            std::cout << indent(d) << "Stmt(...)\n";
        }
    }, s.v);
}

void dumpFuncDecl(const FuncDecl& f, int d) {
    std::cout << indent(d) << "FuncDecl " << f.name << "(";
    for (size_t i = 0; i < f.params.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << f.params[i].type->str() << " " << f.params[i].name;
    }
    std::cout << ") -> " << (f.retType ? f.retType->str() : "#") << "\n";
    for (auto& s : f.body) dumpStmt(*s, d+1);
}

void dumpProgram(const Program& prog, int d) {
    std::cout << "Program\n";
    for (auto& o : prog.opaques) {
        std::cout << indent(d+1) << "Opaque " << o->name << "\n";
    }
    for (auto& s : prog.structs) {
        std::cout << indent(d+1) << "Struct " << s->name << (s->packed?" [packed]":"") << "\n";
        for (auto& f : s->fields)
            std::cout << indent(d+2) << f.type->str() << " " << f.name << "\n";
    }
    for (auto& e : prog.enums) {
        std::cout << indent(d+1) << "Enum " << e->name << "\n";
    }
    for (auto& g : prog.globals) {
        std::cout << indent(d+1) << "Global " << g->type->str() << " " << g->name << "\n";
    }
    for (auto& f : prog.funcs) {
        dumpFuncDecl(*f, d+1);
    }
}
