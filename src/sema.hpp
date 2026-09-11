#pragma once
#include "types.hpp"
#include <stack>

struct VarInfo {
    std::string    name;
    ResolvedTyPtr  type;
    bool           nomd = false;
    bool           isGlobal = false;
    SourceLoc      declLoc;

    enum class OwnerState { Readable, Mutable, Dropped };
    OwnerState ownerState = OwnerState::Readable;

    int generation = 0;
    std::string handleOf;
    int         handleGen = -1;

    uint64_t instanceId = 0;
};

struct HandleInfo {
    std::string  varName;
    bool         isMutable;
    int          generation;
    SourceLoc    createdAt;
    bool         invalid = false;
};

class Scope {
public:
    void push() { frames_.push_back({}); }
    void pop()  {
        assert(!frames_.empty());
        frames_.pop_back();
    }
    void declare(const std::string& name, VarInfo info) {
        assert(!frames_.empty());
        info.instanceId = ++nextInstanceId_;
        instanceNames_[info.instanceId] = name;
        frames_.back()[name] = std::move(info);
    }
    VarInfo* lookup(const std::string& name) {
        for (int i = (int)frames_.size() - 1; i >= 0; --i) {
            auto it = frames_[i].find(name);
            if (it != frames_[i].end()) return &it->second;
        }
        return nullptr;
    }
    bool declaredInCurrent(const std::string& name) const {
        if (frames_.empty()) return false;
        return frames_.back().count(name) > 0;
    }
    const std::string* nameOf(uint64_t instanceId) const {
        auto it = instanceNames_.find(instanceId);
        return it != instanceNames_.end() ? &it->second : nullptr;
    }

    struct OwnershipSnapshot {
        std::unordered_map<uint64_t, std::pair<VarInfo::OwnerState, int>> byInstance;
    };

    OwnershipSnapshot snapshotOwnership() const {
        OwnershipSnapshot snap;
        for (auto& frame : frames_)
            for (auto& [name, vi] : frame)
                snap.byInstance[vi.instanceId] = {vi.ownerState, vi.generation};
        return snap;
    }

    void restoreOwnership(const OwnershipSnapshot& snap) {
        for (auto& frame : frames_) {
            for (auto& [name, vi] : frame) {
                auto it = snap.byInstance.find(vi.instanceId);
                if (it != snap.byInstance.end()) {
                    vi.ownerState = it->second.first;
                    vi.generation = it->second.second;
                }
            }
        }
    }

private:
    std::vector<std::unordered_map<std::string, VarInfo>> frames_;
    std::unordered_map<uint64_t, std::string> instanceNames_;
    uint64_t nextInstanceId_ = 0;
};

struct FuncSig {
    std::string                name;
    std::vector<ResolvedTyPtr> paramTypes;
    ResolvedTyPtr              retType;
    bool                       isGeneric   = false;
    bool                       isVariadic  = false;
    bool                       isExtern    = false;
    bool                       isUnsafe    = false;
    std::vector<std::string>   genericParams;
    std::vector<bool>          paramNomd;
    const FuncDecl*            decl        = nullptr;
    const ExternDecl*          externDecl  = nullptr;
};

struct CrumbleState {
    int readLimit  = -1;
    int writeLimit = -1;
    int readCount  = 0;
    int writeCount = 0;
    SourceLoc declLoc;
};

class SemaPass {
public:
    SemaPass(Program& prog, DiagEngine& diag,
             std::shared_ptr<SourceFile> sf);
    bool run();

    const std::unordered_map<std::string, std::shared_ptr<OpaqueTypeInfo>>& opaques() const { return opaques_; }
    const std::unordered_map<std::string, std::shared_ptr<StructTypeInfo>>& structs() const { return structs_; }
    const std::unordered_map<std::string, std::shared_ptr<EnumTypeInfo>>&   enums()   const { return enums_; }
    const std::unordered_map<std::string, FuncSig>&                         funcs()   const { return funcs_; }

    ResolvedTyPtr resolveType(const TypeNode& tn, const SubstMap& subst = {}) const;

private:
    Program&    prog_;
    DiagEngine& diag_;
    std::shared_ptr<SourceFile> sf_;

    std::unordered_map<std::string, std::shared_ptr<OpaqueTypeInfo>> opaques_;
    std::unordered_map<std::string, std::shared_ptr<StructTypeInfo>> structs_;
    std::unordered_map<std::string, std::shared_ptr<EnumTypeInfo>>   enums_;
    std::unordered_map<std::string, FuncSig>                         funcs_;

    std::unique_ptr<TypeResolver> tyResolver_;

    std::unordered_map<std::string, CrumbleState> crumbleStates_;
    std::string lvalCtxVar_;
    std::string ownerOpCtxVar_;
    ResolvedTyPtr currentRetTy_;

    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> aliases_;
    std::unordered_map<std::string, int64_t> constIntGlobals_;
    int loopDepth_ = 0;

    void collectOpaques();
    void collectStructNames();
    void resolveStructFields();
    void collectEnumNames();
    void resolveEnumPayloads();
    void collectFuncs();
    void checkGlobals();
    void checkFuncs();
    void checkInsertions();

    void checkFunc(FuncDecl& fd, const SubstMap& subst = {});
    ResolvedTyPtr checkStmt(Stmt& s, Scope& scope,
                            const ResolvedTyPtr& retTy,
                            bool& hasReturn,
                            std::vector<HandleInfo>& handles,
                            const SubstMap& subst = {});
    ResolvedTyPtr checkExpr(Expr& e, Scope& scope,
                             std::vector<HandleInfo>& handles,
                             const SubstMap& subst = {});

    void unsafeInvalidate(const std::string& varName, Scope& scope,
                          std::vector<HandleInfo>& handles, const SourceLoc& loc);
    void ownerReadonly(const std::string& varName, Scope& scope,
                       std::vector<HandleInfo>& handles, const SourceLoc& loc);
    void ownerTake(const std::string& varName, Scope& scope,
                   std::vector<HandleInfo>& handles, const SourceLoc& loc);
    void ownerRelease(const std::string& varName, Scope& scope,
                      std::vector<HandleInfo>& handles, const SourceLoc& loc);
    bool isHandleValid(const HandleInfo& h, Scope& scope) const;
    void invalidateReadersOf(const std::string& varName, int generation,
                              std::vector<HandleInfo>& handles);

    void crumbleRead(const std::string& varName, const SourceLoc& loc);
    void crumbleWrite(const std::string& varName, const SourceLoc& loc);
    void crumbleCheckUnderuse(const SourceLoc& funcLoc);

    std::string lineFor(const SourceLoc& l) const {
        if (sf_) return sf_->getLine(l.line);
        return {};
    }
    bool isGenericTy(const ResolvedTyPtr& ty) const;
};
