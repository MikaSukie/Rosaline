#include "sema.hpp"

SemaPass::SemaPass(Program& prog, DiagEngine& diag,
                   std::shared_ptr<SourceFile> sf)
    : prog_(prog), diag_(diag), sf_(std::move(sf)) {}

bool SemaPass::run() {
    collectOpaques();
    collectStructNames();
    collectEnumNames();
    constIntGlobals_.clear();
    for (auto& g : prog_.globals) {
        if (g->nomd && g->init && g->init->is<IntLitExpr>())
            constIntGlobals_[g->name] = g->init->as<IntLitExpr>().val;
    }
    resolveStructFields();
    resolveEnumPayloads();
    tyResolver_ = std::make_unique<TypeResolver>(opaques_, structs_, enums_, diag_, sf_, [this](const std::string& name) -> std::optional<int64_t> {
        auto it = constIntGlobals_.find(name);
        if (it != constIntGlobals_.end()) return it->second;
        return std::nullopt;
    });
    collectFuncs();
    if (diag_.hasErrors) return false;
    checkGlobals();
    if (diag_.hasErrors) return false;
    checkInsertions();
    if (diag_.hasErrors) return false;
    checkFuncs();

    bool hasNoMain = false;
    for (auto& d : prog_.directives)
        if (d.key == "nomain") { hasNoMain = true; break; }

    if (!hasNoMain) {
        bool foundMain = false;
        for (auto& fd : prog_.funcs)
            if (fd->isMain) { foundMain = true; break; }
        if (!foundMain) {
            SourceLoc fileLoc;
            fileLoc.file = sf_ ? sf_->path : "<unknown>";
            fileLoc.line = 1;
            fileLoc.col  = 1;
            fileLoc.len  = 1;
            diag_.warning(fileLoc,
                "no 'main' function defined; the output will not be a "
                "standalone executable. Use '@nomain;' to silence this warning.");
        }
    }

    return !diag_.hasErrors;
}

ResolvedTyPtr SemaPass::resolveType(const TypeNode& tn, const SubstMap& subst) const {
    if (!tyResolver_) {
        TypeResolver tr(opaques_, structs_, enums_, diag_, sf_, [this](const std::string& name) -> std::optional<int64_t> {
            auto it = constIntGlobals_.find(name);
            if (it != constIntGlobals_.end()) return it->second;
            return std::nullopt;
        });
        return tr.resolve(tn, subst);
    }
    return tyResolver_->resolve(tn, subst);
}

bool SemaPass::isGenericTy(const ResolvedTyPtr& ty) const {
    if (!ty) return false;
    return ty->isGeneric() ||
           (ty->isPtr() && isGenericTy(ty->inner)) ||
           (ty->isArray() && isGenericTy(ty->elemTy));
}

static bool containsIllegalStorageType(const ResolvedTyPtr& ty) {
    if (!ty) return false;
    if (ty->isVoid() || ty->isOpaque()) return true;
    if (ty->isPtr())  return false;
    if (ty->isArray()) return containsIllegalStorageType(ty->elemTy);
    return false;
}

static bool containsIllegalReturnType(const ResolvedTyPtr& ty) {
    if (!ty) return false;
    if (ty->isOpaque()) return true;
    if (ty->isPtr())  return false;
    if (ty->isArray()) return containsIllegalReturnType(ty->elemTy);
    return false;
}

static bool isPointerLike(const ResolvedTyPtr& ty) {
    return ty && (ty->isPtr() || ty->isStr());
}

static bool ptrTypesCompat(const ResolvedTyPtr& a, const ResolvedTyPtr& b) {
    if (!a || !b) return false;
    if (!isPointerLike(a) || !isPointerLike(b)) return false;
    if (a->isStr() != b->isStr()) return false;
    return true;
}

void SemaPass::collectOpaques() {
    for (auto& od : prog_.opaques) {
        if (opaques_.count(od->name) || structs_.count(od->name) || enums_.count(od->name)) {
            diag_.error(od->loc,
                std::format("duplicate type declaration '{}'", od->name),
                lineFor(od->loc));
            continue;
        }
        auto info = std::make_shared<OpaqueTypeInfo>();
        info->name = od->name;
        opaques_[od->name] = info;
    }
}

void SemaPass::collectStructNames() {
    for (auto& sd : prog_.structs) {
        if (opaques_.count(sd->name) || structs_.count(sd->name) || enums_.count(sd->name)) {
            diag_.error(sd->loc,
                std::format("duplicate struct declaration '{}'", sd->name),
                lineFor(sd->loc));
            continue;
        }
        auto info = std::make_shared<StructTypeInfo>();
        info->name   = sd->name;
        info->packed = sd->packed;
        info->typeParams = sd->typeParams;
        for (auto& f : sd->fields)
            info->fieldNames.push_back(f.name);
        structs_[sd->name] = info;
    }
}

void SemaPass::resolveStructFields() {
    for (auto& sd : prog_.structs) {
        auto it = structs_.find(sd->name);
        if (it == structs_.end()) continue;
        auto& info = it->second;
        info->fieldTypes.clear();
        TypeResolver tmpRes(opaques_, structs_, enums_, diag_, sf_, [this](const std::string& name) -> std::optional<int64_t> {
            auto it = constIntGlobals_.find(name);
            if (it != constIntGlobals_.end()) return it->second;
            return std::nullopt;
        });
        SubstMap genericSubst;
        for (auto& tp : sd->typeParams)
            genericSubst[tp] = Ty::Generic(tp);
        for (auto& f : sd->fields) {
            auto ft = tmpRes.resolve(*f.type, genericSubst);
            if (containsIllegalStorageType(ft)) {
                diag_.error(f.loc,
                    std::format("field '{}' of struct '{}' has an illegal storage type '{}'; "
                                "void and opaque types cannot be stored directly - use a pointer",
                                f.name, sd->name, ft ? ft->str() : "?"),
                    lineFor(f.loc));
            }
            info->fieldTypes.push_back(ft);
        }
    }
}

void SemaPass::collectEnumNames() {
    for (auto& ed : prog_.enums) {
        if (opaques_.count(ed->name) || structs_.count(ed->name) || enums_.count(ed->name)) {
            diag_.error(ed->loc,
                std::format("duplicate enum declaration '{}'", ed->name),
                lineFor(ed->loc));
            continue;
        }
        auto info = std::make_shared<EnumTypeInfo>();
        info->name       = ed->name;
        info->typeParams = ed->typeParams;
        int tag = 0;
        for (auto& v : ed->variants) {
            EnumVariantInfo vi;
            vi.name = v.name;
            vi.tag  = tag++;
            info->variants.push_back(std::move(vi));
        }
        enums_[ed->name] = info;
    }
}

void SemaPass::resolveEnumPayloads() {
    for (auto& ed : prog_.enums) {
        auto eit = enums_.find(ed->name);
        if (eit == enums_.end()) continue;
        auto& info = eit->second;

        SubstMap genericSubst;
        for (auto& tp : ed->typeParams)
            genericSubst[tp] = Ty::Generic(tp);

        TypeResolver tmpRes(opaques_, structs_, enums_, diag_, sf_, [this](const std::string& name) -> std::optional<int64_t> {
            auto it = constIntGlobals_.find(name);
            if (it != constIntGlobals_.end()) return it->second;
            return std::nullopt;
        });

        for (int i = 0; i < (int)ed->variants.size(); ++i) {
            auto& v  = ed->variants[i];
            auto& vi = info->variants[i];
            vi.payloadTypes.clear();
            for (auto& pt : v.payload) {
                auto rty = tmpRes.resolve(*pt, genericSubst);
                if (containsIllegalStorageType(rty)) {
                    diag_.error(pt->loc,
                        std::format("payload type '{}' in variant '{}::{}' is illegal; "
                                    "void and opaque types cannot be stored directly - use a pointer",
                                    rty ? rty->str() : "?", ed->name, v.name),
                        lineFor(pt->loc));
                }
                vi.payloadTypes.push_back(rty);
            }
        }
    }
}

void SemaPass::collectFuncs() {
    static const std::unordered_set<std::string> kBuiltinNames = {
        "strlen", "malloc", "free", "strcat", "strcpy", "memcpy"
    };

    auto addBuiltin = [&](const std::string& name,
                          std::vector<ResolvedTyPtr> paramTys,
                          ResolvedTyPtr retTy,
                          bool isVariadic = false) {
        FuncSig sig;
        sig.name       = name;
        sig.paramTypes = std::move(paramTys);
        sig.retType    = std::move(retTy);
        sig.isExtern   = true;
        sig.isVariadic = isVariadic;
        sig.decl       = nullptr;
        funcs_[name]   = std::move(sig);
    };

    addBuiltin("strlen", {Ty::Str()},              Ty::I64());
    addBuiltin("malloc", {Ty::I64()},              Ty::Ptr(Ty::Void()));
    addBuiltin("free",   {Ty::Ptr(Ty::Void())},    Ty::Void());
    addBuiltin("strcat", {Ty::Str(), Ty::Str()},   Ty::Str());
    addBuiltin("strcpy", {Ty::Str(), Ty::Str()},   Ty::Str());
    addBuiltin("memcpy", {Ty::Ptr(Ty::Void()), Ty::Ptr(Ty::Void()), Ty::I64()}, Ty::Ptr(Ty::Void()));

    std::unordered_map<std::string, SourceLoc> externDeclLocs;

    for (auto& ed : prog_.externs) {
        if (ed->isGlobal) continue;
        if (funcs_.count(ed->name)) {
            if (kBuiltinNames.count(ed->name)) {
                diag_.error(ed->loc,
                    std::format("'{}' is a compiler builtin and cannot be redeclared", ed->name),
                    lineFor(ed->loc));
            } else {
                diag_.error(ed->loc,
                    std::format("'{}' has already been declared", ed->name),
                    lineFor(ed->loc));
                auto it = externDeclLocs.find(ed->name);
                if (it != externDeclLocs.end())
                    diag_.note(it->second,
                        std::format("'{}' first declared here", ed->name),
                        lineFor(it->second));
            }
            continue;
        }
        externDeclLocs[ed->name] = ed->loc;
        FuncSig sig;
        sig.name       = ed->name;
        sig.isExtern   = true;
        sig.isVariadic = ed->isVariadic;
        sig.externDecl = ed.get();

        for (auto& p : ed->params) {
            auto t = resolveType(*p.type);
            if (containsIllegalStorageType(t)) {
                diag_.error(p.loc,
                    "void and opaque types are only allowed as pointer targets; use a pointer for an opaque type",
                    lineFor(p.loc));
            }
            sig.paramTypes.push_back(t);
            sig.paramNomd.push_back(p.nomd);
        }

        sig.retType = ed->retType ? resolveType(*ed->retType) : Ty::Void();
        if (containsIllegalReturnType(sig.retType)) {
            diag_.error(ed->loc,
                "opaque types are only allowed as pointer targets; use a pointer for an opaque type",
                lineFor(ed->loc));
        }
        funcs_[ed->name] = std::move(sig);
    }

    for (auto& fd : prog_.funcs) {
        if (fd->name.starts_with("__ROSALINE__")) {
            diag_.error(fd->loc,
                std::format("function name '{}' uses the reserved prefix "
                            "'__ROSALINE__'; this prefix is reserved for "
                            "compiler-generated symbols", fd->name),
                lineFor(fd->loc));
        }

        FuncSig sig;
        sig.name          = fd->name;
        sig.isGeneric     = !fd->genericParams.empty();
        sig.genericParams = fd->genericParams;
        sig.decl          = fd.get();

        for (auto& p : fd->params) {
            auto t = resolveType(*p.type);
            if (containsIllegalStorageType(t)) {
                diag_.error(p.loc,
                    "void and opaque types are only allowed as pointer targets; use a pointer for an opaque type",
                    lineFor(p.loc));
            }
            sig.paramTypes.push_back(t);
        }

        if (fd->retType)
            sig.retType = resolveType(*fd->retType);
        else
            sig.retType = Ty::Void();
        if (containsIllegalReturnType(sig.retType)) {
            diag_.error(fd->loc,
                "opaque types are only allowed as pointer targets; use a pointer for an opaque type",
                lineFor(fd->loc));
        }

        if (funcs_.count(fd->name)) {
            diag_.error(fd->loc,
                std::format("duplicate function '{}'", fd->name),
                lineFor(fd->loc));
            continue;
        }
        funcs_[fd->name] = std::move(sig);
    }
}

void SemaPass::checkGlobals() {
    std::unordered_set<std::string> seen;

    auto checkOne = [&](const std::string& name, const TypeNode& type, const SourceLoc& loc) {
        if (seen.count(name)) {
            diag_.error(loc,
                std::format("duplicate global variable '{}'", name),
                lineFor(loc));
        }
        seen.insert(name);
        if (funcs_.count(name)) {
            diag_.error(loc,
                std::format("global variable '{}' conflicts with function '{}'", name, name),
                lineFor(loc));
        }
        auto gty = resolveType(type);
        if (containsIllegalStorageType(gty)) {
            diag_.error(loc,
                "void and opaque types are only allowed as pointer targets; use a pointer for an opaque type",
                lineFor(loc));
        }
    };

    for (auto& g : prog_.globals) {
        checkOne(g->name, *g->type, g->loc);
    }
    for (auto& ed : prog_.externs) {
        if (!ed->isGlobal) continue;
        if (!ed->globalType) {
            diag_.error(ed->loc,
                std::format("extern global '{}' is missing a type", ed->name),
                lineFor(ed->loc));
            continue;
        }
        checkOne(ed->name, *ed->globalType, ed->loc);
    }

    if (diag_.hasErrors) return;

    Scope globalScope;
    globalScope.push();
    for (auto& g : prog_.globals) {
        VarInfo vi;
        vi.name     = g->name;
        vi.type     = resolveType(*g->type);
        vi.nomd     = g->nomd;
        vi.isGlobal = true;
        vi.declLoc  = g->loc;
        if (!globalScope.declaredInCurrent(g->name))
            globalScope.declare(g->name, vi);
    }
    for (auto& ed : prog_.externs) {
        if (!ed->isGlobal || !ed->globalType) continue;
        VarInfo vi;
        vi.name     = ed->name;
        vi.type     = resolveType(*ed->globalType);
        vi.isGlobal = true;
        vi.declLoc  = ed->loc;
        if (!globalScope.declaredInCurrent(ed->name))
            globalScope.declare(ed->name, vi);
    }

    auto isConstantFoldable = [](const Expr& e) -> bool {
        if (e.is<IntLitExpr>() || e.is<FloatLitExpr>() ||
            e.is<BoolLitExpr>() || e.is<CharLitExpr>())
            return true;
        if (e.is<CastExpr>()) {
            auto& c = e.as<CastExpr>();
            return c.operand && c.operand->is<IntLitExpr>() &&
                   c.operand->as<IntLitExpr>().val == 0;
        }
        
        return false;
    };

    for (auto& g : prog_.globals) {
        if (!g->init) continue;

        auto declTy = resolveType(*g->type);
        if (!declTy || declTy->isError()) continue;

        if (g->nomd && !isConstantFoldable(*g->init) && !g->init->is<StringLitExpr>()) {
            diag_.warning(g->loc,
                std::format("'nomd' global '{}' has a non-constant initialiser; "
                            "it will be treated as a mutable global at runtime",
                            g->name),
                lineFor(g->loc));
        }

        std::vector<HandleInfo> handles;
        SubstMap initSubst;
        if (!declTy->isGeneric())
            initSubst["#ctx"] = declTy;

        auto initTy = checkExpr(*g->init, globalScope, handles, initSubst);

        if (!initTy || initTy->isError() || initTy->isVoid()) continue;
        if (declTy->isGeneric() || initTy->isGeneric())       continue;

        bool ptrCompat = ptrTypesCompat(initTy, declTy);
        bool intCompat = initTy->isInteger() && declTy->isInteger();
        bool fltCompat = initTy->isFloat()   && declTy->isFloat();

        if (!ptrCompat && !intCompat && !fltCompat && !initTy->equals(*declTy)) {
            diag_.error(g->loc,
                std::format("cannot initialise global '{}' of type '{}' "
                            "with a value of type '{}'",
                            g->name, declTy->str(), initTy->str()),
                lineFor(g->loc));
        }
    }
    globalScope.pop();
}

void SemaPass::checkInsertions() {
    std::unordered_map<std::string, std::unordered_set<std::string>> seen;
    for (auto& ins : prog_.insertions) {
        auto* targetFd = prog_.findFunc(ins.targetFunc);
        if (!targetFd) {
            diag_.error(ins.loc,
                std::format("'in' targets unknown function '{}'", ins.targetFunc),
                lineFor(ins.loc));
            continue;
        }

        if (ins.forParam != "#") {
            bool found = false;
            for (auto& p : targetFd->params)
                if (p.name == ins.forParam) { found = true; break; }
            if (!found) {
                diag_.error(ins.loc,
                    std::format(
                        "'in' block uses 'for ({})' but '{}' is not a "
                        "parameter of function '{}'",
                        ins.forParam, ins.forParam, ins.targetFunc),
                    lineFor(ins.loc));
                std::string avail;
                for (auto& p : targetFd->params)
                    avail += (avail.empty() ? "" : ", ") + p.name;
                if (!avail.empty())
                    diag_.note(targetFd->loc,
                        std::format("parameters of '{}' are: {}",
                            ins.targetFunc, avail),
                        lineFor(targetFd->loc));
                continue;
            }
        }

        if (ins.arm.matchType) {
            auto resolvedMatchTy = resolveType(*ins.arm.matchType);
            if (resolvedMatchTy && resolvedMatchTy->isError()) {
                continue;
            }
            if (containsIllegalStorageType(resolvedMatchTy)) {
                diag_.error(ins.arm.matchType->loc,
                    std::format("typecase type '{}' is not a legal concrete type; "
                                "void and opaque types cannot appear as typecase match targets",
                                ins.arm.matchType->str()),
                    lineFor(ins.arm.matchType->loc));
                continue;
            }
        }

        std::string typeName = ins.arm.matchType ? ins.arm.matchType->str() : "fallback";
        auto key = ins.targetFunc + "::" + ins.forParam;
        if (seen[key].count(typeName)) {
            diag_.error(ins.loc,
                std::format("duplicate 'in' typecase({}) for '{}' in '{}'",
                    typeName, ins.forParam, ins.targetFunc),
                lineFor(ins.loc));
        }
        seen[key].insert(typeName);
    }
}

void SemaPass::checkFuncs() {
    for (auto& fd : prog_.funcs) {
        checkFunc(*fd);
    }
}

void SemaPass::checkFunc(FuncDecl& fd, const SubstMap& subst) {
    Scope scope;
    scope.push();

    for (size_t pi = 0; pi < fd.params.size(); ++pi) {
        auto& p = fd.params[pi];
        if (scope.declaredInCurrent(p.name)) {
            diag_.error(p.loc,
                std::format("duplicate parameter name '{}'", p.name),
                lineFor(p.loc));
        }
        VarInfo vi;
        vi.name = p.name;
        if (fd.isMain) {
            if (pi == 1)
                vi.type = Ty::Ptr(Ty::Str());
            else
                vi.type = Ty::I32();
        } else {
            vi.type = resolveType(*p.type, subst);
        }
        vi.declLoc = p.loc;
        scope.declare(p.name, std::move(vi));
    }

    auto declareGlobal = [&](const std::string& name, const TypeNode& type, bool nomd, const SourceLoc& loc) {
        if (scope.declaredInCurrent(name)) {
            SourceLoc paramLoc = fd.loc;
            for (auto& p : fd.params)
                if (p.name == name) { paramLoc = p.loc; break; }
            diag_.error(loc,
                std::format("global variable '{}' conflicts with parameter of the same name in '{}'",
                    name, fd.name),
                lineFor(loc));
            diag_.note(paramLoc,
                std::format("parameter '{}' declared here", name),
                lineFor(paramLoc));
            return;
        }
        VarInfo vi;
        vi.name     = name;
        vi.type     = resolveType(type);
        vi.nomd     = nomd;
        vi.isGlobal = true;
        vi.declLoc  = loc;
        scope.declare(name, vi);
    };

    for (auto& g : prog_.globals) {
        declareGlobal(g->name, *g->type, g->nomd, g->loc);
    }
    for (auto& ed : prog_.externs) {
        if (!ed->isGlobal || !ed->globalType) continue;
        declareGlobal(ed->name, *ed->globalType, false, ed->loc);
    }

    ResolvedTyPtr retTy;
    if (fd.isMain) {
        retTy = Ty::I32();
    } else if (fd.retType) {
        retTy = resolveType(*fd.retType, subst);
    } else {
        retTy = Ty::Void();
    }

    currentRetTy_ = retTy;

    bool hasReturn = false;
    std::vector<HandleInfo> handles;

    crumbleStates_.clear();
    aliases_.clear();
    lvalCtxVar_.clear();

    for (auto& s : fd.body) {
        checkStmt(*s, scope, retTy, hasReturn, handles, subst);
    }

    crumbleCheckUnderuse(fd.loc);

    if (!retTy->isVoid() && !hasReturn && !fd.genericParams.empty() == false) {
        diag_.warning(fd.loc,
            std::format("function '{}' returns '{}' but has no guaranteed return statement; "
                        "falling through will produce a zero/null value",
                        fd.name, retTy->str()),
            lineFor(fd.loc));
    }

    scope.pop();
}

ResolvedTyPtr SemaPass::checkStmt(Stmt& s, Scope& scope,
                                   const ResolvedTyPtr& retTy,
                                   bool& hasReturn,
                                   std::vector<HandleInfo>& handles,
                                   const SubstMap& subst) {
    return std::visit([&](auto& x) -> ResolvedTyPtr {
        using T = std::decay_t<decltype(x)>;

        if constexpr (std::is_same_v<T, VarDeclStmt>) {
            bool isDiscard = (x.name == "_");

            if (!isDiscard && scope.declaredInCurrent(x.name)) {
                diag_.error(s.loc,
                    std::format("variable '{}' already declared in this scope", x.name),
                    lineFor(s.loc));
            }
            auto declTy = resolveType(*x.type);
            if (containsIllegalStorageType(declTy)) {
                diag_.error(s.loc,
                    "void and opaque types are only allowed as pointer targets; use a pointer for an opaque type",
                    lineFor(s.loc));
            }
            if (x.init) {
                SubstMap initSubst = subst;
                if (declTy && !declTy->isGeneric())
                    initSubst["#ctx"] = declTy;
                auto initTy = checkExpr(*x.init, scope, handles, initSubst);

                if (declTy && initTy &&
                    !declTy->isGeneric() && !isGenericTy(initTy) &&
                    !initTy->isVoid() && !initTy->isError() &&
                    !initTy->equals(*declTy)) {
                    bool ptrCompat = ptrTypesCompat(initTy, declTy);
                    if (!ptrCompat)
                        diag_.error(s.loc,
                            std::format(
                                "cannot initialise '{}' of type '{}' with a value of type '{}'",
                                x.name, declTy->str(), initTy->str()),
                            lineFor(s.loc));
                }
            }

            if (!isDiscard) {
                VarInfo vi;
                vi.name     = x.name;
                vi.type     = declTy;
                vi.nomd     = x.nomd;
                vi.declLoc  = s.loc;

                if (x.init) {
                    auto detectHandle = [&](const Expr& initExpr,
                                            std::string& srcName,
                                            int& srcGen) {
                        auto tryIdent = [&](const Expr& operand) {
                            if (!operand.template is<IdentExpr>()) return;
                            const std::string& idn =
                                operand.template as<IdentExpr>().name;
                            auto* orig = scope.lookup(idn);
                            if (orig) { srcName = idn; srcGen = orig->generation; }
                        };
                        if (initExpr.template is<ReadonlyExpr>())
                            tryIdent(*initExpr.template as<ReadonlyExpr>().operand);
                        else if (initExpr.template is<TakeExpr>())
                            tryIdent(*initExpr.template as<TakeExpr>().operand);
                        else if (initExpr.template is<ReleaseExpr>())
                            tryIdent(*initExpr.template as<ReleaseExpr>().operand);
                    };
                    detectHandle(*x.init, vi.handleOf, vi.handleGen);

                    if (x.init->template is<IdentExpr>()) {
                        const std::string& srcName =
                            x.init->template as<IdentExpr>().name;
                        aliases_[srcName].insert(x.name);
                        aliases_[x.name].insert(srcName);
                    }
                }

                scope.declare(x.name, std::move(vi));
            }
            return Ty::Void();
        }

        if constexpr (std::is_same_v<T, ExprStmt>) {
            return checkExpr(*x.expr, scope, handles, subst);
        }

        if constexpr (std::is_same_v<T, ReturnStmt>) {
            hasReturn = true;
            if (x.value) {
                SubstMap retSubst = subst;
                if (retTy && !retTy->isGeneric())
                    retSubst["#ctx"] = retTy;
                auto t = checkExpr(*x.value, scope, handles, retSubst);
                if (retTy && !retTy->isVoid() && t && !isGenericTy(t) && !isGenericTy(retTy)) {
                    bool ptrCompat = ptrTypesCompat(t, retTy);
                    if (!t->isError() && !t->equals(*retTy) && !ptrCompat) {
                        diag_.error(s.loc,
                            std::format("return type mismatch: returning '{}' but function returns '{}'",
                                t->str(), retTy->str()),
                            lineFor(s.loc));
                    }
                }
            } else if (retTy && !retTy->isVoid()) {
                diag_.error(s.loc,
                    std::format("missing return value: function returns '{}'", retTy->str()),
                    lineFor(s.loc));
            }
            return Ty::Void();
        }

        if constexpr (std::is_same_v<T, IfStmt>) {
            checkExpr(*x.cond, scope, handles);
            scope.push();
            bool tb = false, eb = false;
            std::vector<HandleInfo> thenH = handles;
            std::vector<HandleInfo> elseH = handles;
            for (auto& st : x.thenBody) checkStmt(*st, scope, retTy, tb, thenH, subst);
            scope.pop();
            scope.push();
            for (auto& st : x.elseBody) checkStmt(*st, scope, retTy, eb, elseH, subst);
            scope.pop();
            for ([[maybe_unused]] auto& h : handles) {
                bool inv1 = false, inv2 = false;
                for (auto& h2 : thenH) if (h2.varName == h.varName && h2.generation == h.generation) { inv1 = h2.invalid; break; }
                for (auto& h2 : elseH) if (h2.varName == h.varName && h2.generation == h.generation) { inv2 = h2.invalid; break; }
                if (inv1 || inv2) h.invalid = true;
            }
            if (tb && eb) hasReturn = true;
            return Ty::Void();
        }

        if constexpr (std::is_same_v<T, WhileStmt>) {
            checkExpr(*x.cond, scope, handles);
            scope.push();
            bool wb = false;
            ++loopDepth_;
            for (auto& st : x.body) checkStmt(*st, scope, retTy, wb, handles, subst);
            --loopDepth_;
            scope.pop();
            return Ty::Void();
        }

        if constexpr (std::is_same_v<T, BreakStmt> || std::is_same_v<T, ContinueStmt>) {
            if (loopDepth_ == 0) {
                const char* kw = std::is_same_v<T, BreakStmt> ? "break" : "continue";
                diag_.error(s.loc,
                    std::format("'{}' used outside of a loop", kw),
                    lineFor(s.loc));
            }
            return Ty::Void();
        }

        if constexpr (std::is_same_v<T, AsmBlockStmt>) {
            diag_.warning(s.loc,
                "inline assembly block (@asm) bypasses all type safety and ownership checks",
                lineFor(s.loc));
            return Ty::Void();
        }

        return Ty::Void();
    }, s.v);
}

ResolvedTyPtr SemaPass::checkExpr(Expr& e, Scope& scope,
                                   std::vector<HandleInfo>& handles,
                                   const SubstMap& subst) {
    return std::visit([&](auto& x) -> ResolvedTyPtr {
        using T = std::decay_t<decltype(x)>;

        if constexpr (std::is_same_v<T, IntLitExpr>) {
            auto ctxIt = subst.find("#ctx");
            if (ctxIt != subst.end() && ctxIt->second) {
                if (ctxIt->second->isInteger())
                    return ctxIt->second;
                if (ctxIt->second->isFloat())
                    return ctxIt->second;
            }
            return Ty::I32();
        }
        if constexpr (std::is_same_v<T, FloatLitExpr>) {
            auto ctxIt = subst.find("#ctx");
            if (ctxIt != subst.end() && ctxIt->second && ctxIt->second->isFloat())
                return ctxIt->second;
            return Ty::F64();
        }
        if constexpr (std::is_same_v<T, StringLitExpr>) return Ty::Str();
        if constexpr (std::is_same_v<T, CharLitExpr>)   return Ty::Char();
        if constexpr (std::is_same_v<T, BoolLitExpr>)   return Ty::Bool();

        if constexpr (std::is_same_v<T, IdentExpr>) {
            auto* v = scope.lookup(x.name);
            if (!v) {
                if (funcs_.count(x.name)) {
                    diag_.error(e.loc,
                        std::format("'{}' is a function, not a variable; "
                                    "did you mean '{}(...)'?", x.name, x.name),
                        lineFor(e.loc));
                } else {
                    diag_.error(e.loc,
                        std::format("undeclared identifier '{}'", x.name),
                        lineFor(e.loc));
                }
                return Ty::Error();
            }

            if (!v->handleOf.empty()) {
                auto* orig = scope.lookup(v->handleOf);
                if (orig && orig->generation != v->handleGen) {
                    diag_.error(e.loc,
                        std::format(
                            "use of invalidated handle '{}': underlying variable "
                            "'{}' was mutated (via take) after this handle was created",
                            x.name, v->handleOf),
                        lineFor(e.loc));
                }
            }

            for (auto& h : handles) {
                if (h.varName == x.name && h.invalid) {
                    diag_.error(e.loc,
                        std::format("use of invalidated handle '{}'", x.name),
                        lineFor(e.loc));
                    break;
                }
            }

            {
                auto ait = aliases_.find(x.name);
                if (ait != aliases_.end()) {
                    for (const auto& aliasName : ait->second) {
                        auto* av = scope.lookup(aliasName);
                        if (av && av->ownerState == VarInfo::OwnerState::Mutable) {
                            diag_.error(e.loc,
                                std::format(
                                    "use of '{}' while alias '{}' holds exclusive mutable access",
                                    x.name, aliasName),
                                lineFor(e.loc));
                            break;
                        }
                    }
                }
            }

            if (v->ownerState == VarInfo::OwnerState::Dropped) {
                diag_.error(e.loc,
                    std::format("use of dropped variable '{}'", x.name),
                    lineFor(e.loc));
            } else if (v->ownerState == VarInfo::OwnerState::Mutable &&
                       v->handleOf.empty()) {
            }

            if (x.name != lvalCtxVar_)
                crumbleRead(x.name, e.loc);
            return v->type;
        }

        if constexpr (std::is_same_v<T, AssignExpr>) {
            ResolvedTyPtr lhsCtxTy;
            if (x.lhs->template is<IdentExpr>()) {
                auto* v = scope.lookup(x.lhs->template as<IdentExpr>().name);
                if (v) lhsCtxTy = v->type;
            }
            SubstMap rhsSubst = subst;
            if (lhsCtxTy && (lhsCtxTy->isInteger() || lhsCtxTy->isFloat()))
                rhsSubst["#ctx"] = lhsCtxTy;
            auto rTy = checkExpr(*x.rhs, scope, handles, rhsSubst);
            if (x.lhs->template is<IdentExpr>()) {
                auto& id = x.lhs->template as<IdentExpr>();
                auto* v  = scope.lookup(id.name);
                if (v && v->nomd) {
                    diag_.error(e.loc,
                        std::format("cannot assign to 'nomd' variable '{}'", id.name),
                        lineFor(e.loc));
                }
                if (v && rTy && !isGenericTy(rTy) && !v->type->isGeneric() &&
                    !rTy->isVoid() && !rTy->isError() && !rTy->equals(*v->type)) {
                    bool ptrCompat = ptrTypesCompat(rTy, v->type);
                    if (!ptrCompat)
                        diag_.error(e.loc,
                            std::format("cannot assign value of type '{}' to variable '{}' of type '{}'",
                                rTy->str(), id.name, v->type->str()),
                            lineFor(e.loc));
                }
                lvalCtxVar_ = id.name;
                checkExpr(*x.lhs, scope, handles, subst);
                lvalCtxVar_.clear();
                crumbleWrite(id.name, e.loc);
            } else {
                checkExpr(*x.lhs, scope, handles, subst);
            }
            return rTy;
        }

        if constexpr (std::is_same_v<T, BinOpExpr>) {
            auto lt = checkExpr(*x.lhs, scope, handles, subst);
            SubstMap binSubst = subst;
            if (lt && (lt->isInteger() || lt->isFloat()))
                binSubst["#ctx"] = lt;
            {
                auto outerCtx = subst.find("#ctx");
                if (outerCtx != subst.end() && outerCtx->second &&
                    outerCtx->second->isFloat() && lt && !lt->isFloat()) {
                    binSubst["#ctx"] = outerCtx->second;
                }
            }
            auto rt = checkExpr(*x.rhs, scope, handles, binSubst);
            if (x.op == "+" && lt && lt->isStr() && rt && rt->isStr()) return Ty::Str();
            if (x.op == "&&" || x.op == "||") return Ty::Bool();
            if (x.op == "==" || x.op == "!=") {
                bool ok = false;
                if (lt && rt) {
                    ok = (lt->isInteger() && rt->isInteger()) ||
                         (lt->isFloat() && rt->isFloat()) ||
                         (lt->kind == TyKind::Bool && rt->kind == TyKind::Bool) ||
                         (isPointerLike(lt) && isPointerLike(rt));
                }
                if (!ok) {
                    diag_.error(e.loc,
                        std::format("cannot compare values of type '{}' and '{}' with '{}'",
                                    lt ? lt->str() : "?", rt ? rt->str() : "?", x.op),
                        lineFor(e.loc));
                }
                return Ty::Bool();
            }
            if (x.op == "<" || x.op == ">" || x.op == "<=" || x.op == ">=") {
                bool ok = lt && rt && (
                    (lt->isNumeric() && rt->isNumeric()) ||
                    (lt->isPtr()     && rt->isPtr())
                );
                if (!ok) {
                    diag_.error(e.loc,
                        std::format("relational operator '{}' requires numeric or pointer operands, got '{}' and '{}'",
                                    x.op, lt ? lt->str() : "?", rt ? rt->str() : "?"),
                        lineFor(e.loc));
                }
                return Ty::Bool();
            }
            if (rt && rt->isFloat() && lt && !lt->isFloat())
                return rt;
            if (lt && lt->isFloat() && rt && !rt->isFloat())
                return lt;
            return lt ? lt : Ty::I32();
        }

        if constexpr (std::is_same_v<T, UnaryOpExpr>) {
            auto t = checkExpr(*x.operand, scope, handles, subst);
            if (x.op == "!") return Ty::Bool();
            return t;
        }

        if constexpr (std::is_same_v<T, CastExpr>) {
            auto srcTy = checkExpr(*x.operand, scope, handles, subst);
            auto castTy = resolveType(*x.type, subst);
            if (containsIllegalStorageType(castTy)) {
                diag_.error(e.loc,
                    "void and opaque types are only allowed as pointer targets; use a pointer for an opaque type",
                    lineFor(e.loc));
                return Ty::I32();
            }
            if (castTy && castTy->isStr()) {
                bool ok = srcTy && (srcTy->isStr() || srcTy->isPtr());
                if (!ok) {
                    diag_.error(e.loc,
                        std::format("cannot cast type '{}' to 'str'",
                                    srcTy ? srcTy->str() : "?"),
                        lineFor(e.loc));
                    return Ty::Error();
                }
            }
            return castTy;
        }

        if constexpr (std::is_same_v<T, AddressOfExpr>) {
            if (x.operand->template is<IdentExpr>()) {
                auto& name = x.operand->template as<IdentExpr>().name;
                auto fit = funcs_.find(name);
                if (fit != funcs_.end()) {
                    auto& sig = fit->second;
                    std::vector<ResolvedTyPtr> paramTys;
                    if (sig.decl)
                        for (auto& p : sig.decl->params)
                            if (p.type) paramTys.push_back(resolveType(*p.type, subst));
                    return Ty::FuncPtr(std::move(paramTys), sig.retType);
                }
            }
            auto t = checkExpr(*x.operand, scope, handles, subst);
            return Ty::Ptr(t);
        }

        if constexpr (std::is_same_v<T, DerefExpr>) {
            auto t = checkExpr(*x.operand, scope, handles, subst);
            if (t && t->isPtr()) {
                if (t->inner && t->inner->isOpaque()) {
                    diag_.error(e.loc,
                        std::format("cannot dereference pointer to opaque type '{}'", t->inner->name),
                        lineFor(e.loc));
                    return Ty::I32();
                }
                return t->inner ? t->inner : Ty::Void();
            }
            return Ty::I32();
        }

        if constexpr (std::is_same_v<T, CallExpr>) {
            std::string fname;
            if (x.callee->template is<IdentExpr>())
                fname = x.callee->template as<IdentExpr>().name;
            else
                fname = "<expr>";

            if (fname == "<expr>") {
                auto calleeTy = checkExpr(*x.callee, scope, handles, subst);
                for (auto& a : x.args) checkExpr(*a, scope, handles, subst);
                if (calleeTy && calleeTy->isFuncPtr()) {
                    if (x.args.size() != calleeTy->funcParamTypes.size()) {
                        diag_.error(e.loc,
                            std::format("indirect call: expected {} argument(s), got {}",
                                calleeTy->funcParamTypes.size(), x.args.size()),
                            lineFor(e.loc));
                    }
                    return calleeTy->funcRetType ? calleeTy->funcRetType : Ty::Void();
                }
                return calleeTy && !calleeTy->isVoid() ? calleeTy : Ty::I32();
            }

            {
                auto* varInfo = scope.lookup(fname);
                if (varInfo && varInfo->type && varInfo->type->isFuncPtr()) {
                    auto& fpty = varInfo->type;
                    for (auto& a : x.args) checkExpr(*a, scope, handles, subst);
                    if (x.args.size() != fpty->funcParamTypes.size()) {
                        diag_.error(e.loc,
                            std::format("indirect call: expected {} argument(s), got {}",
                                fpty->funcParamTypes.size(), x.args.size()),
                            lineFor(e.loc));
                    }
                    return fpty->funcRetType ? fpty->funcRetType : Ty::Void();
                }
            }

            if (fname == "len") {
                if (x.args.size() != 1) {
                    diag_.error(e.loc,
                        std::format("'len' expects 1 argument, got {}", x.args.size()),
                        lineFor(e.loc));
                } else {
                    auto argTy = checkExpr(*x.args[0], scope, handles, subst);
                    if (!argTy || !argTy->isArray()) {
                        diag_.error(x.args[0]->loc,
                            std::format("'len' argument must be a fixed-size array, got '{}'",
                                argTy ? argTy->str() : "?"),
                            lineFor(x.args[0]->loc));
                    }
                }
                return Ty::I64();
            }

            auto it = funcs_.find(fname);
            if (it == funcs_.end()) {
                if (!fname.empty() && fname != "<expr>")
                    diag_.error(e.loc,
                        std::format("call to undeclared function '{}'", fname),
                        lineFor(e.loc));
                for (auto& a : x.args) checkExpr(*a, scope, handles, subst);
                return Ty::Error();
            }

            if (!it->second.isVariadic) {
                int expected = (int)it->second.paramTypes.size();
                int got      = (int)x.args.size();
                if (got != expected) {
                    diag_.error(e.loc,
                        std::format(
                            "function '{}' expects {} argument(s) but {} {} provided",
                            fname, expected, got, got == 1 ? "was" : "were"),
                        lineFor(e.loc));
                    for (auto& a : x.args) checkExpr(*a, scope, handles, subst);
                    return it->second.retType
                        ? applySubst(it->second.retType, subst) : Ty::Void();
                }
            } else {
                int fixedExpected = (int)it->second.paramTypes.size();
                int got           = (int)x.args.size();
                if (got < fixedExpected) {
                    diag_.error(e.loc,
                        std::format(
                            "function '{}' requires at least {} argument(s) but {} {} provided",
                            fname, fixedExpected, got, got == 1 ? "was" : "were"),
                        lineFor(e.loc));
                    for (auto& a : x.args) checkExpr(*a, scope, handles, subst);
                    return it->second.retType
                        ? applySubst(it->second.retType, subst) : Ty::Void();
                }
            }

            SubstMap callSubst = subst;

            if (!x.typeArgs.empty()) {
                for (size_t i = 0; i < x.typeArgs.size() &&
                                i < it->second.genericParams.size(); ++i) {
                    callSubst[it->second.genericParams[i]] =
                        resolveType(*x.typeArgs[i], subst);
                }
            }

            for (size_t i = 0; i < x.args.size(); ++i) {
                ResolvedTyPtr expectedTy;
                if (i < it->second.paramTypes.size())
                    expectedTy = applySubst(it->second.paramTypes[i], callSubst);

                SubstMap argSubst = callSubst;
                if (expectedTy && !expectedTy->isGeneric())
                    argSubst["#ctx"] = expectedTy;

                auto argTy = checkExpr(*x.args[i], scope, handles, argSubst);

                if (expectedTy && expectedTy->isGeneric() && argTy && !argTy->isGeneric()) {
                    callSubst[expectedTy->genericName] = argTy;
                    expectedTy = applySubst(it->second.paramTypes[i], callSubst);
                }

                if (expectedTy && argTy &&
                    !expectedTy->isGeneric() && !argTy->isGeneric() &&
                    !expectedTy->isError()   && !argTy->isError()   &&
                    !argTy->equals(*expectedTy)) {
                    bool ptrCompat = ptrTypesCompat(argTy, expectedTy);
                    if (!ptrCompat)
                        diag_.error(x.args[i]->loc,
                            std::format(
                                "cannot pass value of type '{}' to parameter {} of '{}' (expected '{}')",
                                argTy->str(), i + 1, fname, expectedTy->str()),
                            lineFor(x.args[i]->loc));
                }
            }

            for (auto& gp : it->second.genericParams) {
                if (gp == "#ctx") {
                    auto ctxIt = callSubst.find("#ctx");
                    if (ctxIt != callSubst.end() && ctxIt->second && !ctxIt->second->isGeneric())
                        continue;

                    diag_.error(e.loc,
                        std::format("call to '{}' requires a concrete contextual return type (#)", fname),
                        lineFor(e.loc));
                    return Ty::I32();
                }

                if (!callSubst.count(gp)) {
                    diag_.error(e.loc,
                        std::format("cannot infer generic type '{}' in call to '{}'", gp, fname),
                        lineFor(e.loc));
                    return Ty::I32();
                }
            }

            if (it->second.isExtern) {
                for (size_t i = 0; i < x.args.size(); ++i) {
                    bool isNomd = (i < it->second.paramNomd.size())
                                && it->second.paramNomd[i];
                    if (!isNomd && x.args[i]->template is<IdentExpr>()) {
                        const std::string& argName =
                            x.args[i]->template as<IdentExpr>().name;
                        auto v = scope.lookup(argName);
                        if (v && v->ownerState != VarInfo::OwnerState::Dropped) {
                            ++v->generation;
                            invalidateReadersOf(argName, v->generation - 1, handles);
                        }
                    }
                }
            } else if (it->second.decl) {
                for (size_t i = 0; i < x.args.size(); ++i) {
                    bool paramNomd = false;
                    if (i < it->second.decl->params.size())
                        paramNomd = it->second.decl->params[i].nomd;
                    if (!paramNomd && x.args[i]->template is<IdentExpr>()) {
                        const std::string& argName =
                            x.args[i]->template as<IdentExpr>().name;
                        auto v = scope.lookup(argName);
                        if (v && v->ownerState != VarInfo::OwnerState::Dropped) {
                            ++v->generation;
                            invalidateReadersOf(argName, v->generation - 1, handles);
                        }
                    }
                }
            }

            if (it->second.decl && it->second.decl->retType && it->second.decl->retType->isHash()) {
                auto ctxIt = callSubst.find("#ctx");
                if (ctxIt == callSubst.end() || !ctxIt->second || ctxIt->second->isGeneric()) {
                    diag_.error(e.loc,
                        std::format("call to '{}' requires a concrete contextual return type (#)", fname),
                        lineFor(e.loc));
                    return Ty::I32();
                }
                return ctxIt->second;
            }

            auto retTy = it->second.retType ? applySubst(it->second.retType, callSubst)
                                            : Ty::Void();
            return retTy;
        }

        if constexpr (std::is_same_v<T, FieldAccessExpr>) {
            auto objTy = checkExpr(*x.obj, scope, handles, subst);
            if (objTy && objTy->isPtr() && objTy->inner)
                objTy = objTy->inner;
            if (objTy && objTy->isOpaque()) {
                diag_.error(e.loc,
                    std::format("opaque type '{}' has no accessible fields", objTy->name),
                    lineFor(e.loc));
                return Ty::I32();
            }
            if (objTy && objTy->isStruct()) {
                auto sit = structs_.find(objTy->name);
                if (sit != structs_.end()) {
                    int idx = sit->second->fieldIndex(x.field);
                    if (idx < 0) {
                        diag_.error(e.loc,
                            std::format("struct '{}' has no field '{}'", objTy->name, x.field),
                            lineFor(e.loc));
                        return Ty::I32();
                    }
                    return sit->second->fieldTypes[idx];
                }
            }
            return Ty::I32();
        }

        if constexpr (std::is_same_v<T, IndexExpr>) {
            auto arrTy = checkExpr(*x.arr, scope, handles, subst);
            SubstMap idxSubst = subst;
            idxSubst["#ctx"] = Ty::I64();
            auto idxTy = checkExpr(*x.idx, scope, handles, idxSubst);
            if (idxTy && !idxTy->isInteger()) {
                diag_.error(x.idx->loc,
                    std::format("array/pointer index must be an integer type, got '{}'",
                                idxTy->str()),
                    lineFor(x.idx->loc));
            }
            if (arrTy && arrTy->isArray()) return arrTy->elemTy;
            if (arrTy && arrTy->isPtr()) return arrTy->inner ? arrTy->inner : Ty::I8();
            return Ty::I32();
        }

        if constexpr (std::is_same_v<T, ArrayLitExpr>) {
            if (x.elems.empty()) {
                diag_.error(e.loc, "array literal cannot be empty", lineFor(e.loc));
                return Ty::Array(Ty::I32(), 0);
            }
            SubstMap elemSubst = subst;
            {
                auto ctxIt = subst.find("#ctx");
                if (ctxIt != subst.end() && ctxIt->second &&
                    ctxIt->second->isArray() && ctxIt->second->elemTy)
                    elemSubst["#ctx"] = ctxIt->second->elemTy;
            }
            auto elemTy = checkExpr(*x.elems[0], scope, handles, elemSubst);
            for (size_t i = 1; i < x.elems.size(); ++i) {
                auto t = checkExpr(*x.elems[i], scope, handles, elemSubst);
                if (elemTy && t && !elemTy->equals(*t) &&
                    !(elemTy->isInteger() && t->isInteger()) &&
                    !(elemTy->isFloat() && t->isFloat())) {
                    diag_.error(x.elems[i]->loc,
                        std::format("array literal element {} has type '{}' but expected '{}'",
                            i + 1, t->str(), elemTy->str()),
                        lineFor(x.elems[i]->loc));
                }
            }
            return Ty::Array(elemTy, (int64_t)x.elems.size());
        }

        if constexpr (std::is_same_v<T, EnumAccessExpr>) {
            auto eit = enums_.find(x.enumName);
            if (eit == enums_.end()) {
                diag_.error(e.loc,
                    std::format("unknown enum '{}'", x.enumName),
                    lineFor(e.loc));
                for (auto& a : x.args) checkExpr(*a, scope, handles, subst);
                return Ty::Named(TyKind::Enum, x.enumName);
            }
            int vi = eit->second->variantIndex(x.variant);
            if (vi < 0) {
                diag_.error(e.loc,
                    std::format("unknown variant '{}' in enum '{}'",
                        x.variant, x.enumName),
                    lineFor(e.loc));
                for (auto& a : x.args) checkExpr(*a, scope, handles, subst);
                return Ty::Named(TyKind::Enum, x.enumName);
            }

            int payloadCount = (int)eit->second->variants[vi].payloadTypes.size();
            if (!x.args.empty() && (int)x.args.size() != payloadCount) {
                diag_.error(e.loc,
                    std::format("enum variant '{}::{}' expects {} payload argument(s), got {}",
                        x.enumName, x.variant, payloadCount, x.args.size()),
                    lineFor(e.loc));
            }
            if (x.args.empty() && payloadCount > 0) {
                diag_.error(e.loc,
                    std::format("enum variant '{}::{}' requires {} payload argument(s); "
                                "use {}->{}(value) to construct it",
                        x.enumName, x.variant, payloadCount, x.enumName, x.variant),
                    lineFor(e.loc));
            }

            for (size_t ai = 0; ai < x.args.size(); ++ai) {
                ResolvedTyPtr payloadTy;
                if (ai < eit->second->variants[vi].payloadTypes.size())
                    payloadTy = applySubst(eit->second->variants[vi].payloadTypes[ai], subst);

                SubstMap payloadSubst = subst;
                if (payloadTy && !payloadTy->isGeneric())
                    payloadSubst["#ctx"] = payloadTy;

                checkExpr(*x.args[ai], scope, handles, payloadSubst);
            }
            return Ty::Named(TyKind::Enum, x.enumName);
        }

        if constexpr (std::is_same_v<T, ReadonlyExpr>) {
            auto t = checkExpr(*x.operand, scope, handles, subst);
            if (x.operand->template is<IdentExpr>()) {
                ownerReadonly(x.operand->template as<IdentExpr>().name, scope, handles, e.loc);
            }
            return t;
        }

        if constexpr (std::is_same_v<T, TakeExpr>) {
            auto t = checkExpr(*x.operand, scope, handles, subst);
            if (x.operand->template is<IdentExpr>()) {
                const auto& idn = x.operand->template as<IdentExpr>().name;
                ownerTake(idn, scope, handles, e.loc);
                crumbleWrite(idn, e.loc);
            }
            return Ty::Ptr(t);
        }

        if constexpr (std::is_same_v<T, ReleaseExpr>) {
            auto t = checkExpr(*x.operand, scope, handles, subst);
            if (x.operand->template is<IdentExpr>()) {
                ownerRelease(x.operand->template as<IdentExpr>().name, scope, handles, e.loc);
            }
            return t;
        }

        if constexpr (std::is_same_v<T, CrumbleExpr>) {
            auto t = checkExpr(*x.operand, scope, handles, subst);
            if (x.operand->template is<IdentExpr>()) {
                auto& idn = x.operand->template as<IdentExpr>().name;
                for (auto& h : std::ranges::reverse_view(handles)) {
                    if (h.varName == idn && h.invalid) {
                        diag_.error(e.loc,
                            std::format("crumble applied to invalid handle '{}'", idn),
                            lineFor(e.loc));
                        break;
                    }
                }
                if (x.readLimit >= 0 || x.writeLimit >= 0) {
                    CrumbleState cs;
                    cs.readLimit  = x.readLimit;
                    cs.writeLimit = x.writeLimit;
                    cs.declLoc    = e.loc;
                    crumbleStates_[idn] = cs;
                }
            }
            return t;
        }

        if constexpr (std::is_same_v<T, TypeswitchExpr>) {
            bool isCtxSwitch = (x.param == "#");
            std::unordered_set<std::string> seenTypes;
            bool hasFallback = false;

            for (auto& arm : x.arms) {
                if (arm.isFallback) {
                    if (isCtxSwitch) {
                        diag_.error(e.loc,
                            "fallback branch is disallowed in a '#' contextual "
                            "typeswitch, it is unsafe because the return type "
                            "is caller-defined and cannot be safely zero-initialised",
                            lineFor(e.loc));
                    }
                    hasFallback = true;
                    scope.push();
                    bool dummy = false;
                    for (auto& s2 : arm.body)
                        checkStmt(*s2, scope, currentRetTy_, dummy, handles, subst);
                    scope.pop();
                    continue;
                }

                auto armTy = resolveType(*arm.matchType);
                std::string key = armTy ? armTy->mangle() : "?";

                if (seenTypes.count(key)) {
                    diag_.error(arm.matchType->loc,
                        std::format("duplicate typecase({}) in typeswitch "
                                    "- each type may only appear once",
                            arm.matchType->str()),
                        lineFor(arm.matchType->loc));
                }
                seenTypes.insert(key);
                scope.push();

                if (!isCtxSwitch && !x.param.empty() && armTy) {
                    auto* v = scope.lookup(x.param);
                    if (v) {
                        VarInfo concreteVI = *v;
                        concreteVI.type = armTy;
                        scope.declare(x.param, std::move(concreteVI));
                    }
                }

                auto armRetTy = isCtxSwitch ? armTy : currentRetTy_;
                bool dummy = false;
                for (auto& s2 : arm.body)
                    checkStmt(*s2, scope, armRetTy, dummy, handles, subst);

                scope.pop();
            }

            if (!isCtxSwitch && !hasFallback) {
                diag_.warning(e.loc,
                    "typeswitch has no 'fallback' branch, if called with a "
                    "type not covered by any typecase, the function will "
                    "silently return a zero-initialised value");
            }

            return Ty::Void();
        }

        if constexpr (std::is_same_v<T, MatchExpr>) {
            auto subjTy = checkExpr(*x.subject, scope, handles, subst);
            bool hasWildcard = false;
            std::unordered_set<std::string> coveredVariants;

            for (auto& arm : x.arms) {
                scope.push();
                if (arm.isWildcard) hasWildcard = true;
                if (subjTy && subjTy->isEnum()) {
                    auto eit = enums_.find(subjTy->name);
                    if (eit != enums_.end()) {
                        int vi = eit->second->variantIndex(arm.variantName);
                        if (vi < 0 && !arm.isWildcard) {
                            std::string knownVariants;
                            for (auto& v : eit->second->variants)
                                knownVariants += (knownVariants.empty() ? "" : ", ") + v.name;
                            diag_.error(arm.loc,
                                std::format("unknown variant '{}' in enum '{}' (known: {})",
                                    arm.variantName, subjTy->name, knownVariants),
                                lineFor(arm.loc));
                        } else if (vi >= 0) {
                            coveredVariants.insert(arm.variantName);
                        }
                    }
                }
                if (subjTy && subjTy->isEnum()) {
                    auto eit = enums_.find(subjTy->name);
                    if (eit != enums_.end()) {
                        int vi = eit->second->variantIndex(arm.variantName);
                        if (vi >= 0) {
                            const auto& variant = eit->second->variants[vi];

                            SubstMap enumSubst = subst;
                            for (size_t i = 0;
                                i < eit->second->typeParams.size() && i < subjTy->typeArgs.size();
                                ++i) {
                                enumSubst[eit->second->typeParams[i]] = subjTy->typeArgs[i];
                            }

                            for (size_t bi = 0; bi < arm.bindNames.size() && bi < variant.payloadTypes.size(); ++bi) {
                                VarInfo vi2;
                                vi2.name = arm.bindNames[bi];
                                vi2.type = applySubst(variant.payloadTypes[bi], enumSubst);
                                scope.declare(arm.bindNames[bi], vi2);
                            }
                        }
                    }
                }
                bool armReturns = false;
                for (auto& s2 : arm.body)
                    checkStmt(*s2, scope, currentRetTy_, armReturns, handles, subst);
                scope.pop();
            }

            if (subjTy && subjTy->isEnum() && !hasWildcard) {
                auto eit = enums_.find(subjTy->name);
                if (eit != enums_.end()) {
                    std::vector<std::string> missing;
                    for (auto& v : eit->second->variants) {
                        if (!coveredVariants.count(v.name))
                            missing.push_back(v.name);
                    }
                    if (!missing.empty()) {
                        std::string missStr;
                        for (auto& m : missing)
                            missStr += (missStr.empty() ? "" : ", ") + m;
                        diag_.error(e.loc,
                            std::format("non-exhaustive match on enum '{}': missing variant(s): {}",
                                subjTy->name, missStr),
                            lineFor(e.loc));
                    }
                }
            }

            return Ty::Void();
        }

    if constexpr (std::is_same_v<T, StructInitExpr>) {
        ResolvedTyPtr structTy;
        std::string structName;
        if (x.type) {
            structTy   = resolveType(*x.type, subst);
            structName = structTy ? structTy->name : std::string{};
        }

        if (structTy && structTy->isOpaque()) {
            diag_.error(e.loc,
                std::format("opaque type '{}' cannot be constructed by value", structTy->name),
                lineFor(e.loc));
            for (auto& f : x.fields) checkExpr(*f.value, scope, handles, subst);
            return Ty::I32();
        }

        const StructTypeInfo* info = nullptr;
        if (!structName.empty()) {
            auto sit = structs_.find(structName);
            if (sit != structs_.end())
                info = sit->second.get();
            else
                diag_.error(e.loc,
                    std::format("unknown struct type '{}'", structName),
                    lineFor(e.loc));
        }

        SubstMap structSubst = subst;
        if (info && !info->typeParams.empty() && structTy && !structTy->typeArgs.empty()) {
            for (size_t i = 0; i < info->typeParams.size() && i < structTy->typeArgs.size(); ++i)
                structSubst[info->typeParams[i]] = structTy->typeArgs[i];
        }

        for (auto& f : x.fields) {
            if (info) {
                int idx = info->fieldIndex(f.name);
                if (idx < 0) {
                    diag_.error(f.value->loc,
                        std::format("struct '{}' has no field '{}'",
                                    structName, f.name),
                        lineFor(f.value->loc));
                    continue;
                }

                auto expectedFTy = applySubst(info->fieldTypes[idx], structSubst);

                SubstMap fieldSubst = structSubst;
                if (expectedFTy && !expectedFTy->isGeneric())
                    fieldSubst["#ctx"] = expectedFTy;

                auto fTy = checkExpr(*f.value, scope, handles, fieldSubst);

                if (fTy && expectedFTy &&
                    !fTy->isGeneric() && !expectedFTy->isGeneric() &&
                    !fTy->equals(*expectedFTy)) {
                    bool ptrCompat = ptrTypesCompat(fTy, expectedFTy);
                    if (!ptrCompat) {
                        diag_.error(f.value->loc,
                            std::format(
                                "cannot initialise field '{}' of type '{}' with value of type '{}'",
                                f.name, expectedFTy->str(), fTy->str()),
                            lineFor(f.value->loc));
                    }
                }
            } else {
                checkExpr(*f.value, scope, handles, structSubst);
            }
        }

        return structTy ? structTy : Ty::Void();
    }

        if constexpr (std::is_same_v<T, TernaryExpr>) {
            checkExpr(*x.cond, scope, handles, subst);
            auto thenTy = checkExpr(*x.thenExpr, scope, handles, subst);
            checkExpr(*x.elseExpr, scope, handles, subst);
            return thenTy ? thenTy : Ty::I32();
        }

        return Ty::Void();
    }, e.v);
}

void SemaPass::crumbleRead(const std::string& name, const SourceLoc& loc) {
    auto it = crumbleStates_.find(name);
    if (it == crumbleStates_.end()) return;
    ++it->second.readCount;
    if (it->second.readLimit >= 0 && it->second.readCount > it->second.readLimit) {
        diag_.error(loc,
            std::format("crumble: '{}' read {} time(s) but limit is {}",
                name, it->second.readCount, it->second.readLimit),
            lineFor(loc));
    }
}

void SemaPass::crumbleWrite(const std::string& name, const SourceLoc& loc) {
    auto it = crumbleStates_.find(name);
    if (it == crumbleStates_.end()) return;
    ++it->second.writeCount;
    if (it->second.writeLimit >= 0 && it->second.writeCount > it->second.writeLimit) {
        diag_.error(loc,
            std::format("crumble: '{}' written {} time(s) but limit is {}",
                name, it->second.writeCount, it->second.writeLimit),
            lineFor(loc));
    }
}

void SemaPass::crumbleCheckUnderuse(const SourceLoc& /*funcLoc*/) {
    for (auto& [name, cs] : crumbleStates_) {
        if (cs.readLimit >= 0 && cs.readCount < cs.readLimit) {
            diag_.warning(cs.declLoc,
                std::format("crumble: '{}' read only {} time(s), expected {}",
                    name, cs.readCount, cs.readLimit),
                lineFor(cs.declLoc));
        }
        if (cs.writeLimit >= 0 && cs.writeCount < cs.writeLimit) {
            diag_.warning(cs.declLoc,
                std::format("crumble: '{}' written only {} time(s), expected {}",
                    name, cs.writeCount, cs.writeLimit),
                lineFor(cs.declLoc));
        }
    }
}

void SemaPass::ownerReadonly(const std::string& varName, Scope& scope,
                              std::vector<HandleInfo>& handles, const SourceLoc& loc) {
    auto* v = scope.lookup(varName);
    if (!v) return;

    if (v->ownerState == VarInfo::OwnerState::Mutable) {
        diag_.error(loc,
            std::format("cannot create readable handle for '{}': exclusive mutable handle exists",
                varName),
            lineFor(loc));
        return;
    }
    if (v->ownerState == VarInfo::OwnerState::Dropped) {
        diag_.error(loc,
            std::format("'{}' has been dropped", varName),
            lineFor(loc));
        return;
    }

    HandleInfo h;
    h.varName     = varName;
    h.isMutable   = false;
    h.generation  = v->generation;
    h.createdAt   = loc;
    handles.push_back(h);
}

void SemaPass::ownerTake(const std::string& varName, Scope& scope,
                          std::vector<HandleInfo>& handles, const SourceLoc& loc) {
    auto* v = scope.lookup(varName);
    if (!v) return;

    if (v->ownerState == VarInfo::OwnerState::Mutable) {
        diag_.error(loc,
            std::format("cannot take '{}': exclusive mutable handle already exists", varName),
            lineFor(loc));
        return;
    }
    if (v->ownerState == VarInfo::OwnerState::Dropped) {
        diag_.error(loc,
            std::format("'{}' has been dropped", varName),
            lineFor(loc));
        return;
    }

    ++v->generation;
    invalidateReadersOf(varName, v->generation - 1, handles);

    {
        auto ait = aliases_.find(varName);
        if (ait != aliases_.end()) {
            for (const auto& aliasName : ait->second) {
                auto* av = scope.lookup(aliasName);
                if (av && av->ownerState != VarInfo::OwnerState::Dropped) {
                    int oldGen = av->generation;
                    ++av->generation;
                    invalidateReadersOf(aliasName, oldGen, handles);
                }
            }
        }
    }

    v->ownerState = VarInfo::OwnerState::Mutable;

    HandleInfo h;
    h.varName    = varName;
    h.isMutable  = true;
    h.generation = v->generation;
    h.createdAt  = loc;
    handles.push_back(h);
}

void SemaPass::ownerRelease(const std::string& varName, Scope& scope,
                             std::vector<HandleInfo>& handles, const SourceLoc& loc) {
    auto* v = scope.lookup(varName);
    if (!v) return;

    if (v->ownerState != VarInfo::OwnerState::Mutable) {
        diag_.error(loc,
            std::format("cannot release '{}': no active mutable handle", varName),
            lineFor(loc));
        return;
    }

    for ([[maybe_unused]] auto& h : handles) {
        if (h.varName == varName && h.isMutable && !h.invalid) {
            h.invalid = true;
            break;
        }
    }

    v->ownerState = VarInfo::OwnerState::Readable;

    HandleInfo h;
    h.varName    = varName;
    h.isMutable  = false;
    h.generation = v->generation;
    h.createdAt  = loc;
    handles.push_back(h);
}

bool SemaPass::isHandleValid(const HandleInfo& h, Scope& scope) const {
    if (h.invalid) return false;
    auto* v = const_cast<Scope&>(scope).lookup(h.varName);
    if (!v) return false;
    return h.generation == v->generation;
}

void SemaPass::invalidateReadersOf(const std::string& varName, int generation,
                                    std::vector<HandleInfo>& handles) {
    for ([[maybe_unused]] auto& h : handles) {
        if (h.varName == varName && !h.isMutable && h.generation == generation) {
            h.invalid = true;
        }
    }
}
