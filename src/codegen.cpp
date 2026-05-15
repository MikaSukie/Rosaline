#include "codegen.hpp"
#include <cmath>

CodeGen::CodeGen(Program& prog, SemaPass& sema, DiagEngine& diag,
                 std::shared_ptr<SourceFile> sf,
                 const std::string& tdl, const std::string& triple)
    : prog_(prog), sema_(sema), diag_(diag), sf_(std::move(sf)),
      tdl_(tdl), triple_(triple) {}

std::string CodeGen::emit() {
    if (!tdl_.empty())
        out_ << "target datalayout = \"" << tdl_ << "\"\n";
    if (!triple_.empty())
        out_ << "target triple = \"" << triple_ << "\"\n";
    if (!tdl_.empty() || !triple_.empty())
        out_ << "\n";

    buildInsertionMap();
    emitTypeDecls();

    std::vector<GlobalDecl*> runtimeInits;
    emitGlobals(runtimeInits);
    emitBuiltins();

    if (!runtimeInits.empty())
        emitGlobalInitBody(runtimeInits);

    emitGlobalFiniBody();

    for (auto& fd : prog_.funcs) {
        if (!fd->genericParams.empty()) continue;
        std::string mname = fd->isMain ? "main" : fd->name;
        if (!emittedFuncs_.count(mname)) {
            emittedFuncs_.insert(mname);
            std::ostringstream funcOs;
            fnOut_ = &funcOs;
            emitFunc(*fd, {}, mname);
            out_ << funcOs.str() << "\n";
        }
    }

    processMonoQueue();

    std::string result = out_.str();
    while (result.size() >= 2 &&
           result[result.size()-2] == '\n' &&
           result.back()           == '\n')
        result.pop_back();
    return result;
}

bool CodeGen::isGlobalSymbol(const std::string& name) const {
    for (auto& g : prog_.globals)
        if (g && g->name == name) return true;
    for (auto& ed : prog_.externs)
        if (ed && ed->isGlobal && ed->name == name) return true;
    return false;
}

ResolvedTyPtr CodeGen::globalSymbolType(const std::string& name) const {
    for (auto& g : prog_.globals)
        if (g && g->name == name) return sema_.resolveType(*g->type);
    for (auto& ed : prog_.externs)
        if (ed && ed->isGlobal && ed->name == name && ed->globalType)
            return sema_.resolveType(*ed->globalType);
    return nullptr;
}

void CodeGen::emitTypeDecls() {
    for (auto& [name, info] : sema_.opaques()) {
        emitOpaqueType(name);
    }
    for (auto& [name, info] : sema_.structs()) {
        emitStructType(name);
    }
    for (auto& [name, info] : sema_.enums()) {
        if (info->typeParams.empty()) {
            emitEnumType(name, {}, {});
        }
    }
}

void CodeGen::emitOpaqueType(const std::string& name) {
    if (emittedTypes_.count(name)) return;
    emittedTypes_.insert(name);
    out_ << "%" << name << " = type opaque\n";
}

void CodeGen::emitStructType(const std::string& name, bool force) {
    if (!force && emittedTypes_.count(name)) return;
    emittedTypes_.insert(name);

    auto it = sema_.structs().find(name);
    if (it == sema_.structs().end()) return;
    auto& info = *it->second;

    for (auto& ft : info.fieldTypes)
        ensureTypeEmitted(ft, {});

    std::string fields;
    for (size_t i = 0; i < info.fieldTypes.size(); ++i) {
        if (i) fields += ", ";
        fields += llvmTy(info.fieldTypes[i]);
    }

    if (info.packed)
        out_ << "%" << name << " = type <{ " << fields << " }>\n";
    else
        out_ << "%" << name << " = type { " << fields << " }\n";
}

std::string CodeGen::enumTypeName(const std::string& baseName,
                                   const std::vector<ResolvedTyPtr>& typeArgs) const {
    if (typeArgs.empty()) return baseName;
    std::string n = baseName;
    for (auto& ta : typeArgs) n += "_" + ta->mangle();
    return n;
}

void CodeGen::emitEnumType(const std::string& name,
                            const std::vector<ResolvedTyPtr>& typeArgs,
                            const SubstMap& subst) {
    std::string tyName = enumTypeName(name, typeArgs);
    if (emittedTypes_.count(tyName)) return;
    emittedTypes_.insert(tyName);

    auto eit = sema_.enums().find(name);
    if (eit == sema_.enums().end()) return;
    auto& info = *eit->second;

    SubstMap enumSubst = subst;
    for (size_t i = 0; i < info.typeParams.size() && i < typeArgs.size(); ++i)
        enumSubst[info.typeParams[i]] = typeArgs[i];

    int maxBytes     = 0;
    int maxPayAlign  = 1;
    const EnumDecl* enumDeclPtr = nullptr;
    for (auto& d : prog_.enums)
        if (d->name == name) { enumDeclPtr = d.get(); break; }

    for (auto& v : info.variants) {
        int sz = 0;
        if (enumDeclPtr) {
            int vi = info.variantIndex(v.name);
            if (vi >= 0 && vi < (int)enumDeclPtr->variants.size()) {
                for (auto& pt : enumDeclPtr->variants[vi].payload) {
                    auto rty = sema_.resolveType(*pt, enumSubst);
                    int fsz    = rty ? llvmByteSize(rty) : 0;
                    int falign = rty ? std::min(fsz, 8) : 1;
                    if (falign > 1)
                        sz = ((sz + falign - 1) / falign) * falign;
                    sz += fsz;
                    maxPayAlign = std::max(maxPayAlign, falign);
                }
            }
        }
        if (sz > maxBytes) maxBytes = sz;
    }

    int padBytes = (maxPayAlign > 4) ? (maxPayAlign - 4) : 0;

    if (maxBytes == 0) {
        out_ << "%" << tyName << " = type { i32 }\n";
    } else {
        out_ << "%" << tyName << " = type { i32, ["
             << (padBytes + maxBytes) << " x i8] }\n";
    }
}

std::string CodeGen::tryFoldConstGlobal(const Expr& init, const ResolvedTyPtr& ty) {
    if (ty && (ty->isStr() || ty->isPtr())) {
        if (init.is<IntLitExpr>() && init.as<IntLitExpr>().val == 0)
            return "null";

        if (init.is<CastExpr>()) {
            auto& c = init.as<CastExpr>();
            if (c.operand && c.operand->is<IntLitExpr>() &&
                c.operand->as<IntLitExpr>().val == 0) {
                return "null";
            }
        }
    }

    if (!init.is<CallExpr>()) return "";
    auto& c = init.as<CallExpr>();
    if (!c.callee->is<IdentExpr>()) return "";
    const std::string& fname = c.callee->as<IdentExpr>().name;

    const FuncDecl* fd = prog_.findFunc(fname);
    if (!fd) return "";
    if (!ty || !ty->isStruct()) return "";

    auto sit = sema_.structs().find(ty->name);
    if (sit == sema_.structs().end()) return "";
    auto& sinfo = *sit->second;

    std::vector<int64_t> constArgs;
    constArgs.reserve(c.args.size());
    for (auto& arg : c.args) {
        if (arg->is<IntLitExpr>())
            constArgs.push_back(arg->as<IntLitExpr>().val);
        else if (arg->is<CharLitExpr>())
            constArgs.push_back((unsigned char)arg->as<CharLitExpr>().val);
        else
            return "";
    }

    if (constArgs.size() != fd->params.size()) return "";
    std::unordered_map<std::string, int64_t> fieldVals;
    std::unordered_map<std::string, int64_t> paramConst;
    for (size_t i = 0; i < fd->params.size(); ++i)
        paramConst[fd->params[i].name] = constArgs[i];

    for (auto& s : fd->body) {
        bool ok = false;
        std::visit([&](auto& sv) {
            using T = std::decay_t<decltype(sv)>;
            if constexpr (std::is_same_v<T, ReturnStmt>) {
                ok = true;
            } else if constexpr (std::is_same_v<T, VarDeclStmt>) {
                if (!sv.init) { ok = true; return; }
                if (sv.init->template is<IntLitExpr>() &&
                    sv.init->template as<IntLitExpr>().val == 0) {
                    ok = true;
                    return;
                }
            } else if constexpr (std::is_same_v<T, ExprStmt>) {
                if (!sv.expr) return;
                if (!sv.expr->template is<AssignExpr>()) return;
                auto& ae = sv.expr->template as<AssignExpr>();
                if (ae.op != "=") return;
                if (!ae.lhs->template is<FieldAccessExpr>()) return;
                auto& fa = ae.lhs->template as<FieldAccessExpr>();

                int64_t val = 0;
                if (ae.rhs->template is<IdentExpr>()) {
                    auto it = paramConst.find(ae.rhs->template as<IdentExpr>().name);
                    if (it == paramConst.end()) return;
                    val = it->second;
                } else if (ae.rhs->template is<IntLitExpr>()) {
                    val = ae.rhs->template as<IntLitExpr>().val;
                } else {
                    return;
                }

                fieldVals[fa.field] = val;
                ok = true;
            }
        }, s->v);

        if (!ok) return "";
    }

    bool isPacked = sinfo.packed;
    std::string result = isPacked ? "<{ " : "{ ";
    for (size_t i = 0; i < sinfo.fieldNames.size(); ++i) {
        if (i) result += ", ";
        auto& ft = sinfo.fieldTypes[i];
        auto it = fieldVals.find(sinfo.fieldNames[i]);
        int64_t rawVal = (it != fieldVals.end()) ? it->second : 0;

        if (ft && ft->isInteger()) {
            int bw = ft->bitWidth();
            if (bw < 64) {
                uint64_t mask = (1ULL << bw) - 1;
                rawVal = (int64_t)(rawVal & mask);
            }
        }
        result += llvmTy(ft) + " " + std::to_string(rawVal);
    }
    result += isPacked ? " }>" : " }";
    return result;
}

void CodeGen::emitGlobals(std::vector<GlobalDecl*>& runtimeInits) {
    runtimeInits.clear();

    for (auto& ed : prog_.externs) {
        if (!ed->isGlobal || !ed->globalType) continue;
        auto ty = sema_.resolveType(*ed->globalType);
        ensureTypeEmitted(ty, {});
        out_ << "@" << ed->name << " = external global " << llvmTy(ty) << "\n";
    }

    for (auto& g : prog_.globals) {
        auto ty = sema_.resolveType(*g->type);
        ensureTypeEmitted(ty, {});
        std::string llty = llvmTy(ty);
        std::string init = zeroVal(ty);
        bool needsRuntimeInit = false;

        if (g->init) {
            if (ty && (ty->isStr() || ty->isPtr())) {
                std::string folded = tryFoldConstGlobal(*g->init, ty);
                if (!folded.empty()) {
                    init = folded;
                } else if (g->init->is<StringLitExpr>()) {
                    needsRuntimeInit = true;
                } else {
                    needsRuntimeInit = true;
                }
            } else if (g->init->is<IntLitExpr>()) {
                init = std::to_string(g->init->as<IntLitExpr>().val);
            } else if (g->init->is<FloatLitExpr>()) {
                double d = g->init->as<FloatLitExpr>().val;
                if (ty && ty->kind == TyKind::F32) {
                    float f = static_cast<float>(d);
                    double asDouble = static_cast<double>(f);
                    uint64_t bits64;
                    __builtin_memcpy(&bits64, &asDouble, sizeof(bits64));
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "0x%016llX",
                                  static_cast<unsigned long long>(bits64));
                    init = buf;
                } else {
                    init = std::format("{:.17g}", d);
                    if (init.find_first_of(".eE") == std::string::npos)
                        init += ".0";
                }
            } else if (g->init->is<BoolLitExpr>()) {
                init = g->init->as<BoolLitExpr>().val ? "1" : "0";
            } else if (g->init->is<StringLitExpr>()) {
                needsRuntimeInit = true;
            } else {
                std::string folded = tryFoldConstGlobal(*g->init, ty);
                if (!folded.empty()) {
                    init = folded;
                } else {
                    needsRuntimeInit = true;
                }
            }
        }

        std::string linkage = (g->nomd && !needsRuntimeInit) ? "constant" : "global";
        out_ << "@" << g->name << " = " << linkage << " "
             << llty << " " << init << "\n";

        if (needsRuntimeInit)
            runtimeInits.push_back(g.get());

        if (!g->nomd) {
            if (ty && ty->isStr()) {
                globalDrops_.push_back({g->name, ty, DropKind::OwnedStr});
            } else if (ty && ty->isEnum() &&
                       enumHasDroppablePayload(ty->name, ty->typeArgs)) {
                globalDrops_.push_back({g->name, ty, DropKind::EnumWithStrPayload});
            }
        }
    }

    if (!prog_.globals.empty()) out_ << "\n";
}

void CodeGen::emitGlobalInitBody(const std::vector<GlobalDecl*>& runtimeInits) {
    if (runtimeInits.empty()) return;

    std::ostringstream initOs;
    fnOut_ = &initOs;
    tmpIdx_   = 0;
    labelIdx_ = 0;
    varAlloca_.clear();
    varTypes_.clear();
    dropScopes_.clear();
    currentSubst_.clear();
    curFuncName_ = "__rosaline_global_init";
    curRetTy_    = Ty::Void();
    curSretPtr_  = "";
    blockTerminated_ = false;

    line("define void @__rosaline_global_init() {");
    line("entry:");

    for (auto* g : runtimeInits) {
        auto ty  = sema_.resolveType(*g->type);

        SubstMap savedSubst = currentSubst_;
        if (ty && (ty->isFloat() || ty->isInteger()))
            currentSubst_["#ctx"] = ty;

        if (ty && ty->isStr()) {
            std::string rawVal;
            if (g->init->is<StringLitExpr>()) {
                std::string sym = internString(g->init->as<StringLitExpr>().val);
                rawVal = "@" + sym;
            } else {
                rawVal = emitExpr(*g->init);
            }
            std::string owned = emitOwnedStrCopy(rawVal);
            line("  store ptr " + owned + ", ptr @" + g->name + ", align 8");
        } else if (isAggTy(ty)) {
            std::string val = emitExpr(*g->init);
            line("  call ptr @memcpy(ptr @" + g->name + ", ptr " + val +
                 ", i64 " + std::to_string(llvmByteSize(ty)) + ")");
        } else {
            std::string val = emitExpr(*g->init);
            auto initTy = exprType(*g->init);
            if (ty && initTy && !initTy->equals(*ty) &&
                !ty->isVoid() && !initTy->isVoid()) {
                std::string tmp = "%t" + std::to_string(tmpIdx_++);
                std::string fs  = initTy->llvmType();
                std::string ft  = ty->llvmType();

                if (initTy->isInteger() && ty->isInteger()) {
                    int sw = initTy->bitWidth();
                    int dw = ty->bitWidth();
                    if (dw > sw) {
                        line("  " + tmp + " = " + (initTy->isSigned() ? "sext" : "zext") +
                             " " + fs + " " + val + " to " + ft);
                        val = tmp;
                    } else if (dw < sw) {
                        line("  " + tmp + " = trunc " + fs + " " + val + " to " + ft);
                        val = tmp;
                    }
                } else if (initTy->isFloat() && ty->isFloat()) {
                    int sw = initTy->byteSize();
                    int dw = ty->byteSize();
                    if (dw > sw)
                        line("  " + tmp + " = fpext " + fs + " " + val + " to " + ft);
                    else
                        line("  " + tmp + " = fptrunc " + fs + " " + val + " to " + ft);
                    val = tmp;
                } else if (initTy->isInteger() && ty->isFloat()) {
                    line("  " + tmp + " = " + (initTy->isSigned() ? "sitofp" : "uitofp") +
                         " " + fs + " " + val + " to " + ft);
                    val = tmp;
                } else if (initTy->isFloat() && ty->isInteger()) {
                    line("  " + tmp + " = " + (ty->isSigned() ? "fptosi" : "fptoui") +
                         " " + fs + " " + val + " to " + ft);
                    val = tmp;
                }
            }
            line("  store " + llvmTy(ty) + " " + val + ", ptr @" + g->name);
        }

        currentSubst_ = savedSubst;
    }

    line("  ret void");
    line("}");

    fnOut_ = nullptr;
    out_ << initOs.str();
    out_ << "\n";
    hasGlobalInit_ = true;
}

void CodeGen::emitGlobalFiniBody() {
    if (globalDrops_.empty()) return;

    std::ostringstream finiOs;
    fnOut_ = &finiOs;
    tmpIdx_   = 0;
    labelIdx_ = 0;
    varAlloca_.clear();
    varTypes_.clear();
    dropScopes_.clear();
    currentSubst_.clear();
    curFuncName_ = "__rosaline_global_fini";
    curRetTy_    = Ty::Void();
    curSretPtr_  = "";
    blockTerminated_ = false;

    line("define void @__rosaline_global_fini() {");
    line("entry:");

    for (int i = (int)globalDrops_.size() - 1; i >= 0; --i) {
        auto& de = globalDrops_[i];
        if (de.kind == DropKind::OwnedStr) {
            varAlloca_[de.varName] = "@" + de.varName;
            varTypes_[de.varName]  = de.ty;
            emitDropEntry(de);
            varAlloca_.erase(de.varName);
            varTypes_.erase(de.varName);
        } else if (de.kind == DropKind::EnumWithStrPayload) {
            varAlloca_[de.varName] = "@" + de.varName;
            varTypes_[de.varName]  = de.ty;
            emitEnumPayloadDrop(de.varName, de.ty);
            varAlloca_.erase(de.varName);
            varTypes_.erase(de.varName);
        }
    }

    line("  ret void");
    line("}");

    fnOut_ = nullptr;
    out_ << finiOs.str();
    out_ << "\n";
    hasGlobalFini_ = true;
}

void CodeGen::emitBuiltins() {
    out_ << "; ==== Start of Rosaline Builtins ==== \n";
    out_ << "declare i64  @strlen(ptr)\n";
    out_ << "declare ptr  @malloc(i64)\n";
    out_ << "declare void @free(ptr)\n";
    out_ << "declare ptr  @strcat(ptr, ptr)\n";
    out_ << "declare ptr  @strcpy(ptr, ptr)\n";
    out_ << "declare ptr  @memcpy(ptr, ptr, i64)\n";
    out_ << "declare i32  @atexit(ptr)\n";
    out_ << "; ==== End of Rosaline Builtins ==== \n";
    out_ << "\n";
    static const std::unordered_set<std::string> builtinNames = {
        "strlen", "malloc", "free", "strcat", "strcpy", "memcpy", "atexit"
    };
    for (auto& ed : prog_.externs) {
        if (ed->isGlobal) continue;
        if (builtinNames.count(ed->name)) continue;
        auto resolvedRet = ed->retType ? sema_.resolveType(*ed->retType) : nullptr;
        bool retNeedsSret = false;
        std::string retTy;
        if (resolvedRet && isAggTy(resolvedRet)) {
            bool retPacked = false;
            if (resolvedRet->isStruct()) {
                auto rsit = sema_.structs().find(resolvedRet->name);
                if (rsit != sema_.structs().end()) retPacked = rsit->second->packed;
            }
            int retSz = llvmByteSize(resolvedRet);
            if (retPacked && retSz == 4) {
                retTy = "i32";
            } else if (retSz > 16) {
                retNeedsSret = true;
                retTy = "void";
            } else {
                if (resolvedRet->isStruct()) {
                    auto chunks = x86_64AbiChunks(resolvedRet);
                    if (!chunks.empty()) {
                        retTy = "{ ";
                        for (size_t ci = 0; ci < chunks.size(); ++ci) {
                            if (ci) retTy += ", ";
                            retTy += chunks[ci].llvmType;
                        }
                        retTy += " }";
                    } else {
                        retTy = llvmTy(resolvedRet);
                    }
                } else {
                    retTy = llvmTy(resolvedRet);
                }
            }
        } else {
            retTy = resolvedRet ? llvmTy(resolvedRet) : "void";
        }

        std::string sig = "declare " + retTy + " @" + ed->name + "(";
        bool first = true;
        if (retNeedsSret) {
            sig += "ptr sret(" + llvmTy(resolvedRet) + ")";
            first = false;
        }
        for (auto& p : ed->params) {
            auto pty = sema_.resolveType(*p.type);
            if (pty && (pty->isStruct() || pty->isEnum())) {
                auto sit = pty->isStruct() ? sema_.structs().find(pty->name) : sema_.structs().end();
                bool isPacked4 = pty->isStruct() && sit != sema_.structs().end()
                                 && sit->second->packed && llvmByteSize(pty) == 4;
                if (isPacked4) {
                    if (!first) sig += ", ";
                    sig += "i32";
                    first = false;
                } else if (llvmByteSize(pty) > 16) {
                    if (!first) sig += ", ";
                    int align = std::max(1, std::min(llvmByteSize(pty), 8));
                    sig += "ptr byval(" + llvmTy(pty) + ") align " + std::to_string(align);
                    first = false;
                } else if (pty->isStruct()) {
                    auto chunks = x86_64AbiChunks(pty);
                    if (!chunks.empty()) {
                        for (auto& chunk : chunks) {
                            if (!first) sig += ", ";
                            sig += chunk.llvmType;
                            first = false;
                        }
                    } else {
                        if (!first) sig += ", ";
                        sig += llvmTy(pty);
                        first = false;
                    }
                } else {
                    if (!first) sig += ", ";
                    sig += llvmTy(pty);
                    first = false;
                }
            } else {
                if (!first) sig += ", ";
                sig += llvmTy(pty);
                first = false;
            }
        }
        if (ed->isVariadic) {
            if (!ed->params.empty()) sig += ", ";
            sig += "...";
        }
        sig += ")";
        gline(sig);
    }
    gline("");
}

std::string CodeGen::internString(const std::string& s) {
    auto it = strConsts_.find(s);
    if (it != strConsts_.end()) return it->second;
    std::string sym = ".str." + std::to_string(strConstIdx_++);
    strConsts_[s] = sym;
    emitStringConst(s, sym);
    return sym;
}

void CodeGen::emitStringConst(const std::string& val, const std::string& symName) {
    std::string escaped;
    for (unsigned char c : val) {
        if (c == '"')       escaped += "\\22";
        else if (c == '\\') escaped += "\\5C";
        else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\%02X", c);
            escaped += buf;
        } else {
            escaped += (char)c;
        }
    }
    int len = (int)val.size() + 1;
    out_ << "@" << symName << " = private unnamed_addr constant ["
         << len << " x i8] c\"" << escaped << "\\00\"\n";
}

std::string CodeGen::emitOwnedStrCopy(const std::string& src) {
    if (src == "null") return "null";

    std::string isNull  = newTmp();
    std::string nullLbl = newLabel();
    std::string copyLbl = newLabel();
    std::string endLbl  = newLabel();
    std::string len     = newTmp();
    std::string len1    = newTmp();
    std::string buf     = newTmp();
    std::string outPtr  = newTmp();

    line("  " + isNull + " = icmp eq ptr " + src + ", null");
    line("  br i1 " + isNull + ", label %" + nullLbl + ", label %" + copyLbl);

    line(nullLbl + ":");
    line("  br label %" + endLbl);

    line(copyLbl + ":");
    line("  " + len + " = call i64 @strlen(ptr " + src + ")");
    line("  " + len1 + " = add i64 " + len + ", 1");
    line("  " + buf + " = call ptr @malloc(i64 " + len1 + ")");
    line("  call ptr @memcpy(ptr " + buf + ", ptr " + src + ", i64 " + len1 + ")");
    line("  br label %" + endLbl);

    line(endLbl + ":");
    line("  " + outPtr + " = phi ptr [ null, %" + nullLbl + " ], [ " + buf + ", %" + copyLbl + " ]");
    return outPtr;
}

void CodeGen::processMonoQueue() {
    while (!monoQueue_.empty()) {
        MonoWork work = std::move(monoQueue_.back());
        monoQueue_.pop_back();

        std::string mname = work.key.mangledName();
        if (emittedFuncs_.count(mname)) continue;
        emittedFuncs_.insert(mname);

        std::ostringstream funcOs;
        fnOut_ = &funcOs;
        emitFunc(*work.decl, work.subst, mname);
        out_ << funcOs.str() << "\n";
    }
}

void CodeGen::schedMono(const std::string& funcName,
                         const std::vector<ResolvedTyPtr>& typeArgs) {
    const FuncDecl* decl = prog_.findFunc(funcName);
    if (!decl) return;

    MonoKey key;
    key.funcName = funcName;
    for (auto& ta : typeArgs) key.typeArgMangled.push_back(ta->mangle());

    std::string mname = key.mangledName();
    if (monoQueued_.count(mname)) return;
    monoQueued_.insert(mname);

    SubstMap subst;
    for (size_t i = 0; i < decl->genericParams.size() && i < typeArgs.size(); ++i) {
        subst[decl->genericParams[i]] = typeArgs[i];
    }

    monoQueue_.push_back({key, subst, decl});
}

std::string CodeGen::mangleCall(const std::string& funcName,
                                 const std::vector<ResolvedTyPtr>& typeArgs) const {
    if (typeArgs.empty()) return funcName;
    MonoKey key;
    key.funcName = funcName;
    for (auto& ta : typeArgs) key.typeArgMangled.push_back(ta->mangle());
    return key.mangledName();
}

void CodeGen::buildInsertionMap() {
    insertions_.clear();
    for (auto& ins : prog_.insertions) {
        insertions_[ins.targetFunc][ins.forParam].push_back(&ins.arm);
    }
}

bool CodeGen::emitTypeswitchDCE(
        const std::string& funcName,
        const std::string& paramName,
        const std::vector<TypecaseArm>& baseArms,
        const ResolvedTyPtr& concreteType,
        const SourceLoc& loc) {

    std::vector<const TypecaseArm*> allArms;
    for (auto& a : baseArms)
        allArms.push_back(&a);

    auto fit = insertions_.find(funcName);
    if (fit != insertions_.end()) {
        auto pit = fit->second.find(paramName);
        if (pit != fit->second.end()) {
            for (auto* ins : pit->second)
                allArms.push_back(ins);
        }
    }

    std::unordered_map<std::string, const TypecaseArm*> seen;
    const TypecaseArm* fallback = nullptr;

    for (auto* arm : allArms) {
        if (arm->isFallback) {
            fallback = arm;
            continue;
        }

        auto armTy = sema_.resolveType(*arm->matchType, currentSubst_);
        if (!armTy) continue;

        std::string key = armTy->mangle();

        auto it = seen.find(key);
        if (it != seen.end()) {
            diag_.error(loc,
                std::format("duplicate typecase for type '{}'", armTy->str()),
                lineFor(loc));
            continue;
        }

        seen[key] = arm;
    }

    auto matchIt = seen.find(concreteType->mangle());
    if (matchIt != seen.end()) {
        for (auto& s : matchIt->second->body)
            emitStmt(*s);
        return true;
    }

    if (fallback) {
        for (auto& s : fallback->body)
            emitStmt(*s);
        return true;
    }

    if (paramName == "#") {
        return false;
    }

    diag_.error(loc,
        std::format("typeswitch on '{}' is non-exhaustive: no typecase matches "
                    "concrete type '{}' and no fallback branch is present",
                    paramName,
                    concreteType ? concreteType->str() : "?"),
        lineFor(loc));
    return false;
}
