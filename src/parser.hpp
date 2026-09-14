#pragma once
#include "ast.hpp"
#include "lexer.hpp"

class Parser {
public:
    Parser(std::span<const Token> tokens,
           DiagEngine&            diag,
           std::shared_ptr<SourceFile> sf);

    std::unique_ptr<Program> parse();

private:
    std::span<const Token>      toks_;
    size_t                      pos_  = 0;
    DiagEngine&                 diag_;
    std::shared_ptr<SourceFile> sf_;

    const Token& peek(int offset = 0) const;
    const Token& cur()                const { return peek(0); }
    const Token& consume();
    bool  check(TK k, int offset = 0) const { return peek(offset).kind == k; }
    bool  match(TK k);
    const Token& expect(TK k, std::string_view msg = {});
    const Token& expectSemi(std::string_view msg = {});
    std::string  lineFor(const SourceLoc& l) const;
    bool  atEnd()                     const { return cur().kind == TK::Eof; }

    void           parseTopLevel(Program& prog);
    void           parseImport(Program& prog);
    std::unique_ptr<FuncDecl>   parseFuncDecl();
    std::unique_ptr<GlobalDecl> parseGlobalDecl();
    std::unique_ptr<OpaqueDecl> parseOpaqueDecl();
    std::unique_ptr<StructDecl> parseStructDecl(bool packed);
    std::unique_ptr<EnumDecl>   parseEnumDecl();
    Insertion                   parseInsertion();
    DirectiveDecl               parseDirective();

    TypeNodePtr parseType();
    TypeNodePtr parseBaseType();

    StmtPtr parseStmt();
    StmtPtr parseVarDecl(bool nomd);
    StmtPtr parseIf();
    StmtPtr parseWhile();
    StmtPtr parseReturn();
    std::vector<StmtPtr> parseBlock();

    ExprPtr parseExpr();
    ExprPtr parseAssign();
    ExprPtr parseTernary();
    ExprPtr parseLogicalOr();
    ExprPtr parseLogicalAnd();
    ExprPtr parseBitOr();
    ExprPtr parseBitXor();
    ExprPtr parseBitAnd();
    ExprPtr parseEquality();
    ExprPtr parseRelational();
    ExprPtr parseShift();
    ExprPtr parseAddSub();
    ExprPtr parseMulDiv();
    ExprPtr parseUnary();
    ExprPtr parsePostfix(ExprPtr base);
    ExprPtr parsePrimary();
    ExprPtr parseTypeswitchExpr();
    ExprPtr parseMatchExpr();
    ExprPtr parseCheckConstraints(ExprPtr operand);
    ExprPtr parseClaimConstraints(ExprPtr operand);
    ExprPtr parseStructInit(std::string name, SourceLoc loc);

    std::vector<Param> parseParamList();
    std::vector<TypecaseArm> parseTypecaseArms();
    std::unique_ptr<ExternDecl> parseExternDecl();
    bool isTypeStart(int offset = 0) const;
    bool isGenericTypeName(const std::string& name) const;
    bool looksLikeGenericCall() const;

    std::unordered_set<std::string> genericParamsInScope_;
    bool noStructInit_ = false;
    void synchronize(std::initializer_list<TK> stops);
};
