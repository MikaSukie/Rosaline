#pragma once
#include "sema.hpp"
#include <sstream>
#include <unordered_map>
#include <unordered_set>

class CodeGen {
public:
    CodeGen(Program& prog, SemaPass& sema, DiagEngine& diag,
            std::shared_ptr<SourceFile> sf,
            const std::string& tdl   = "",
            const std::string& triple = "");

    std::string emit();

private:
    Program&    prog_;
    SemaPass&   sema_;
    DiagEngine& diag_;
    std::shared_ptr<SourceFile> sf_;
    std::string tdl_;
    std::string triple_;

    std::ostringstream  out_;
    std::ostringstream  decls_;

    std::unordered_map<std::string, std::string> strConsts_;
    int strConstIdx_ = 0;

    std::unordered_set<std::string> emittedTypes_;

    std::unordered_set<std::string> emittedFuncs_;
    bool hasGlobalInit_ = false;
    bool hasGlobalFini_ = false;

    struct MonoWork {
        MonoKey    key;
        SubstMap   subst;
        const FuncDecl* decl;
    };
    std::vector<MonoWork> monoQueue_;
    std::unordered_set<std::string> monoQueued_;

    using InsertionMap =
        std::unordered_map<std::string,
            std::unordered_map<std::string, std::vector<const TypecaseArm*>>>;
    InsertionMap insertions_;

    void buildInsertionMap();

    bool emitTypeswitchDCE(
        const std::string& funcName,
        const std::string& paramName,
        const std::vector<TypecaseArm>& baseArms,
        const ResolvedTyPtr& concreteType,
        const SourceLoc& loc);

    std::ostringstream*  fnOut_ = nullptr;
    int                  tmpIdx_  = 0;
    int                  labelIdx_= 0;

    std::unordered_map<std::string, std::string> varAlloca_;
    std::unordered_map<std::string, ResolvedTyPtr> varTypes_;

    std::vector<std::string> breakLabels_;
    std::vector<std::string> contLabels_;

    SubstMap currentSubst_;

    std::string newTmp()   { return "%t" + std::to_string(tmpIdx_++);   }
    std::string newLabel() { return "L" + std::to_string(labelIdx_++);  }

    void emit(const std::string& s) { *fnOut_ << s; }
    void line(const std::string& s);
    void gline(const std::string& s){ out_    << s << "\n"; }

    void emitTypeDecls();
    void emitOpaqueType(const std::string& name);
    void emitStructType(const std::string& name, bool force=false);
    void emitEnumType(const std::string& name,
                      const std::vector<ResolvedTyPtr>& typeArgs,
                      const SubstMap& subst);
    void ensureTypeEmitted(const ResolvedTyPtr& ty,
                           const SubstMap& subst);
    std::string enumTypeName(const std::string& baseName,
                             const std::vector<ResolvedTyPtr>& typeArgs) const;

    void emitGlobals(std::vector<GlobalDecl*>& runtimeInits);
    void emitBuiltins();
    bool isGlobalSymbol(const std::string& name) const;
    ResolvedTyPtr globalSymbolType(const std::string& name) const;
    void emitStringConst(const std::string& val, const std::string& symName);
    std::string emitOwnedStrCopy(const std::string& src);
    void emitFunc(const FuncDecl& fd, const SubstMap& subst,
                  const std::string& mangledName);
    void emitFuncSignature(std::ostringstream& os,
                           const std::string& name,
                           const FuncDecl& fd,
                           const SubstMap& subst);
    void processMonoQueue();

    void emitStmt(const Stmt& s);
    void emitVarDecl(const VarDeclStmt& vd, const SourceLoc& loc);
    void emitIf(const IfStmt& s);
    void emitWhile(const WhileStmt& s);
    void emitReturn(const ReturnStmt& s, const ResolvedTyPtr& retTy);

    std::string emitExpr(const Expr& e);
    std::string emitLValuePtr(const Expr& e, const SourceLoc& loc);
    std::string emitIntLit(int64_t v, const ResolvedTyPtr& ty = {});
    std::string tryFoldConstGlobal(const Expr& init, const ResolvedTyPtr& ty);

    void emitGlobalInitBody(const std::vector<GlobalDecl*>& runtimeInits);
    void emitGlobalFiniBody();
    std::string emitBinOp(const BinOpExpr& b, const SourceLoc& loc);
    std::string emitCall(const CallExpr& c, const SourceLoc& loc);
    std::string emitStringConcat(const std::string& lv, const std::string& rv);
    std::string emitCast(const CastExpr& c);
    std::string emitFieldAccess(const FieldAccessExpr& fa, const SourceLoc& loc);
    std::string emitIndex(const IndexExpr& ix, const SourceLoc& loc);
    std::string emitMatch(const MatchExpr& mx, const SourceLoc& loc);
    std::string emitTypeswitch(const TypeswitchExpr& ts, const SourceLoc& loc);
    std::string emitAssign(const AssignExpr& a, const SourceLoc& loc);

    std::string emitReadonly(const ReadonlyExpr& r);
    std::string emitTake(const TakeExpr& t);
    std::string emitRelease(const ReleaseExpr& r);
    std::string emitTernary(const TernaryExpr& t, const SourceLoc& loc);

    ResolvedTyPtr exprType(const Expr& e) const;
    ResolvedTyPtr resolveType(const TypeNode& tn) const;
    std::string llvmTy(const ResolvedTyPtr& t) const;
    std::string zeroVal(const ResolvedTyPtr& ty) const;
    std::string internString(const std::string& s);
    std::string loadVar(const std::string& name);
    void        storeVar(const std::string& name, const std::string& val,
                         const ResolvedTyPtr& ty, bool ownedVal = false);
    std::string allocaFor(const std::string& name, const ResolvedTyPtr& ty);

    std::string mangleCall(const std::string& funcName,
                           const std::vector<ResolvedTyPtr>& typeArgs) const;
    std::string materializeValue(const std::string& v,
                                const ResolvedTyPtr& ty);
    void schedMono(const std::string& funcName,
                   const std::vector<ResolvedTyPtr>& typeArgs);

    std::string   curFuncName_;
    std::string   curFuncOrigName_;
    ResolvedTyPtr curRetTy_;
    std::string   curSretPtr_;
    bool          blockTerminated_ = false;

    mutable std::unordered_map<std::string, bool> strAllocCache_;
    bool funcAlwaysAllocatesStr(const std::string& fname) const;

    enum class DropKind { OwnedStr, EnumWithStrPayload };

    struct DropEntry {
        std::string   varName;
        ResolvedTyPtr ty;
        DropKind      kind;
    };

    std::vector<std::vector<DropEntry>> dropScopes_;
    std::vector<int> loopDropDepths_;

    std::vector<DropEntry> globalDrops_;

    void pushDropScope();
    void popAndEmitDrops();
    void popNoDrops();
    void addToDropScope(const std::string& name, const ResolvedTyPtr& ty,
                        DropKind kind);
    void emitDropsForFrame(const std::vector<DropEntry>& frame,
                           const std::string& skipVar = {});
    void emitDropEntry(const DropEntry& de);
    void emitStrFree(const std::string& llvmPtr);

    void emitAllDropsExcept(const std::string& skipVarName = {});

    void emitDropsFromDepth(int fromDepth, const std::string& skipVar = {});

    void emitEnumPayloadDrop(const std::string& varName,
                             const ResolvedTyPtr& ty);

    bool isOwnedInit(const Expr* init) const;

    bool enumHasDroppablePayload(const std::string& enumName,
                                  const std::vector<ResolvedTyPtr>& typeArgs) const;

    bool isAggTy(const ResolvedTyPtr& ty) const {
        return ty && (ty->isStruct() || ty->isEnum() || ty->isArray());
    }
    void emitAggrCopy(const std::string& dst, const std::string& src,
                      const ResolvedTyPtr& ty);
    int llvmByteSize(const ResolvedTyPtr& ty) const;
    int enumPayloadPad(const std::string& name,
                       const std::vector<ResolvedTyPtr>& typeArgs,
                       const SubstMap& subst) const;
    struct AbiChunk {
        enum class Class { SSE, INTEGER };
        Class       cls;
        std::string llvmType;
        int         byteOffset;
        int         byteSize;
    };
    std::vector<AbiChunk> x86_64AbiChunks(const ResolvedTyPtr& ty) const;
    std::string emitLoadAbiChunk(const std::string& basePtr,
                                 const AbiChunk& chunk);

    std::string lineFor(const SourceLoc& l) const {
        if (sf_) return sf_->getLine(l.line);
        return {};
    }
};
