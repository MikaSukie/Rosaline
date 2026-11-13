#include "codegen.hpp"
#include <cstring>

void CodeGen::line(const std::string& s) {
    if (hoistAllocas_ && s.size() > 4 && s[0] == ' ' && s[1] == ' ' &&
        s.find(" = alloca ") != std::string::npos &&
        s.find(", i64 %")    == std::string::npos &&
        s.find(", i32 %")    == std::string::npos) {
        hoistedAllocas_.push_back(s);
        return;
    }
    if (!s.empty() && s[0] != ' ' && s[0] != '\t' && s.back() == ':') {
        blockTerminated_ = false;
        *fnOut_ << s << "\n";
        return;
    }
    if (s == "}") {
        return;
    }
    if (blockTerminated_) return;

    *fnOut_ << s << "\n";
    
    if (s.size() > 4 && s[0] == ' ' && s[1] == ' ') {
        auto sv = std::string_view(s).substr(2);
        if (sv.starts_with("ret ")    || sv == "ret"           ||
            sv.starts_with("br ")     ||
            sv.starts_with("unreachable")                       ||
            sv.starts_with("switch ") || sv.starts_with("indirectbr "))
            blockTerminated_ = true;
    }
}

static std::string implicitCoerce(const std::string& val,
                                   const ResolvedTyPtr& srcTy,
                                   const ResolvedTyPtr& dstTy,
                                   std::ostringstream& out,
                                   int& tmpIdx,
                                   int ptrBits = 64) {
    if (!srcTy || !dstTy) return val;
    if (srcTy->equals(*dstTy)) return val;
    if (srcTy->isVoid() || dstTy->isVoid()) return val;

    std::string tmp = "%t" + std::to_string(tmpIdx++);
    std::string fs = srcTy->llvmType(ptrBits);
    std::string ft = dstTy->llvmType(ptrBits);

    if (srcTy->isInteger() && dstTy->isInteger()) {
        int sw = srcTy->bitWidth(ptrBits), dw = dstTy->bitWidth(ptrBits);
        if (dw > sw)
            out << "  " << tmp << " = " << (srcTy->isSigned()?"sext":"zext")
                << " " << fs << " " << val << " to " << ft << "\n";
        else if (dw < sw)
            out << "  " << tmp << " = trunc " << fs << " " << val
                << " to " << ft << "\n";
        else return val;
        return tmp;
    }
    if (srcTy->isFloat() && dstTy->isFloat()) {
        int sw = srcTy->byteSize(ptrBits), dw = dstTy->byteSize(ptrBits);
        if (dw > sw)
            out << "  " << tmp << " = fpext " << fs << " " << val << " to " << ft << "\n";
        else
            out << "  " << tmp << " = fptrunc " << fs << " " << val << " to " << ft << "\n";
        return tmp;
    }
    if (srcTy->isInteger() && dstTy->isFloat()) {
        out << "  " << tmp << " = " << (srcTy->isSigned()?"sitofp":"uitofp")
            << " " << fs << " " << val << " to " << ft << "\n";
        return tmp;
    }
    if (srcTy->isFloat() && dstTy->isInteger()) {
        out << "  " << tmp << " = " << (dstTy->isSigned()?"fptosi":"fptoui")
            << " " << fs << " " << val << " to " << ft << "\n";
        return tmp;
    }
    return val;
}

static int validAlign(int byteSize, bool packed = false) {
    if (packed || byteSize <= 0) return 1;
    int a = 1;
    while (a < byteSize && a < 8) a <<= 1;
    return a;
}

static std::string formatLLVMFloat(double v, bool asFloat32) {
    double d = asFloat32 ? static_cast<double>(static_cast<float>(v)) : v;
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0')
        << bits;
    return oss.str();
}

static bool foldIntLiteral(const Expr& e, int64_t& out) {
    if (e.is<IntLitExpr>()) { out = e.as<IntLitExpr>().val; return true; }
    if (e.is<UnaryOpExpr>()) {
        auto& u = e.as<UnaryOpExpr>();
        int64_t inner;
        if (u.op == "-" && u.operand && foldIntLiteral(*u.operand, inner)) {
            out = (int64_t)(0 - (uint64_t)inner);
            return true;
        }
    }
    return false;
}

static bool isPureIntLiteral(const Expr& e) {
    if (e.is<IntLitExpr>()) return true;
    if (e.is<UnaryOpExpr>()) {
        auto& u = e.as<UnaryOpExpr>();
        return u.op == "-" && u.operand && isPureIntLiteral(*u.operand);
    }
    return false;
}

std::string CodeGen::emitIntLit(int64_t v, const ResolvedTyPtr& ty) {
    if (ty && ty->isFloat()) {
        return formatLLVMFloat((double)v, ty->byteSize(ptrBits_) == 4);
    }
    if (!ty || !ty->isInteger()) return std::to_string(v);
    int bw = ty->bitWidth(ptrBits_);
    if (bw > 0 && bw < 64) {
        uint64_t mask = (1ULL << bw) - 1;
        v = (int64_t)(v & mask);
    }
    return std::to_string(v);
}

std::string CodeGen::llvmTy(const ResolvedTyPtr& t) const {
    if (!t) return (ptrBits_ == 32) ? "i32" : "i64";
    if (t->isStruct() && !t->typeArgs.empty()) {
        std::string mangledName = t->name;
        for (auto& ta : t->typeArgs) mangledName += "_" + ta->mangle();
        return "%" + mangledName;
    }
    return t->llvmType(ptrBits_);
}

std::string CodeGen::zeroVal(const ResolvedTyPtr& ty) const {
    if (!ty) return "0";
    switch (ty->kind) {
    case TyKind::Void:   return "undef";
    case TyKind::Bool:   return "false";
    case TyKind::I8:
    case TyKind::I16:
    case TyKind::I32:
    case TyKind::I64:
    case TyKind::U8:
    case TyKind::U16:
    case TyKind::U32:
    case TyKind::U64:
    case TyKind::Char:   return "0";
    case TyKind::F32:
    case TyKind::F64:    return "0.0";
    case TyKind::Str:    return "null";
    case TyKind::Ptr:    return "null";
    case TyKind::Array:  return "zeroinitializer";
    case TyKind::Struct: return "zeroinitializer";
    case TyKind::Enum:   return "zeroinitializer";
    case TyKind::Opaque: return "undef";
    default: return "0";
    }
}

void CodeGen::emitAggrCopy(const std::string& dst, const std::string& src,
                           const ResolvedTyPtr& ty) {
    const std::string szTy = is32bit() ? "i32" : "i64";
    line("  call void @" + rtSym("memcpy") + "(ptr " + dst + ", ptr " + src +
         ", " + szTy + " " + std::to_string(llvmByteSize(ty)) + ")");
}

int CodeGen::enumPayloadPad(const std::string& name,
                             const std::vector<ResolvedTyPtr>& typeArgs,
                             const SubstMap& subst) const {
    auto eit = sema_.enums().find(name);
    if (eit == sema_.enums().end()) return 0;
    auto& info = *eit->second;

    SubstMap enumSubst = subst;
    for (size_t i = 0; i < info.typeParams.size() && i < typeArgs.size(); ++i)
        enumSubst[info.typeParams[i]] = typeArgs[i];

    int maxPayAlign = 1;
    const EnumDecl* ed = nullptr;
    for (auto& d : prog_.enums)
        if (d->name == name) { ed = d.get(); break; }

    if (ed) {
        for (auto& v : info.variants) {
            int vi = info.variantIndex(v.name);
            if (vi < 0 || vi >= (int)ed->variants.size()) continue;
            for (auto& pt : ed->variants[vi].payload) {
                auto rty = sema_.resolveType(*pt, enumSubst);
                if (!rty) continue;
                int fsz = llvmByteSize(rty);
                maxPayAlign = std::max(maxPayAlign, std::min(fsz, 8));
            }
        }
    }

    return (maxPayAlign > 4) ? (maxPayAlign - 4) : 0;
}

void CodeGen::ensureTypeEmitted(const ResolvedTyPtr& ty, const SubstMap& subst) {
    if (!ty) return;

    if (ty->isArray()) {
        ensureTypeEmitted(ty->elemTy, subst);
        return;
    }

    if (ty->isStruct()) {
        auto sit = sema_.structs().find(ty->name);
        if (sit == sema_.structs().end()) return;
        auto& info = *sit->second;

        if (!info.typeParams.empty() && !ty->typeArgs.empty()) {
            std::string mangledName = ty->name;
            for (auto& ta : ty->typeArgs) mangledName += "_" + ta->mangle();

            if (!emittedTypes_.count(mangledName)) {
                emittedTypes_.insert(mangledName);
                SubstMap structSubst = subst;
                for (size_t i = 0; i < info.typeParams.size() && i < ty->typeArgs.size(); ++i)
                    structSubst[info.typeParams[i]] = ty->typeArgs[i];
                for (auto& ft : info.fieldTypes)
                    ensureTypeEmitted(applySubst(ft, structSubst), structSubst);
                std::string fields;
                for (size_t i = 0; i < info.fieldTypes.size(); ++i) {
                    if (i) fields += ", ";
                    fields += llvmTy(applySubst(info.fieldTypes[i], structSubst));
                }
                if (info.packed)
                    out_ << "%" << mangledName << " = type <{ " << fields << " }>\n";
                else
                    out_ << "%" << mangledName << " = type { " << fields << " }\n";
            }
        } else if (info.typeParams.empty()) {
            
            emitStructType(ty->name);
        }
        return;
    }

    if (ty->isEnum()) {
        emitEnumType(ty->name, ty->typeArgs, subst);
        return;
    }
}

int CodeGen::llvmByteSize(const ResolvedTyPtr& ty) const {
    if (!ty) return 0;

    if (ty->isArray()) {
        return (int)(ty->arraySize * llvmByteSize(ty->elemTy));
    }

    if (ty->isEnum()) {
        auto eit = sema_.enums().find(ty->name);
        if (eit == sema_.enums().end()) return 4;
        auto& info = *eit->second;

        SubstMap enumSubst = currentSubst_;
        for (size_t i = 0; i < info.typeParams.size() && i < ty->typeArgs.size(); ++i)
            enumSubst[info.typeParams[i]] = ty->typeArgs[i];

        int maxPayloadBytes = 0;
        const EnumDecl* ed = nullptr;
        for (auto& d : prog_.enums)
            if (d->name == ty->name) { ed = d.get(); break; }

        if (ed) {
            for (auto& v : info.variants) {
                int vi = info.variantIndex(v.name);
                if (vi < 0 || vi >= (int)ed->variants.size()) continue;
                int sz = 0;
                for (auto& pt : ed->variants[vi].payload) {
                    auto rty = sema_.resolveType(*pt, enumSubst);
                    if (!rty) continue;
                    int fsz    = llvmByteSize(rty);
                    int falign = std::min(fsz, 8);
                    if (falign > 1)
                        sz = ((sz + falign - 1) / falign) * falign;
                    sz += fsz;
                }
                maxPayloadBytes = std::max(maxPayloadBytes, sz);
            }
        }

        if (maxPayloadBytes == 0) return 4;
        int padBytes = enumPayloadPad(ty->name, ty->typeArgs, enumSubst);
        int total = 4 + padBytes + maxPayloadBytes;
        return (total + 3) & ~3;
    }

    if (ty->isStruct()) {
        auto sit = sema_.structs().find(ty->name);
        if (sit == sema_.structs().end()) return 8;
        auto& info = *sit->second;

        if (info.packed) {
            int sz = 0;
            for (auto& ft : info.fieldTypes)
                sz += ft ? llvmByteSize(ft) : 0;
            return sz;
        }

        int offset    = 0;
        int maxAlign  = 1;
        for (auto& ft : info.fieldTypes) {
            if (!ft) continue;
            int fsz    = llvmByteSize(ft);
            int falign = std::min(fsz, 8);
            if (falign > 0) {
                offset = ((offset + falign - 1) / falign) * falign;
                maxAlign = std::max(maxAlign, falign);
            }
            offset += fsz;
        }
        if (maxAlign > 0)
            offset = ((offset + maxAlign - 1) / maxAlign) * maxAlign;
        return std::max(offset, 1);
    }

    return ty->byteSize(ptrBits_);
}

ResolvedTyPtr CodeGen::resolveType(const TypeNode& tn) const {
    return sema_.resolveType(tn, currentSubst_);
}

std::vector<CodeGen::AbiChunk>
CodeGen::x86_64AbiChunks(const ResolvedTyPtr& ty) const {
    if (!ty || !ty->isStruct()) return {};
    auto sit = sema_.structs().find(ty->name);
    if (sit == sema_.structs().end()) return {};
    auto& info = *sit->second;
    if (info.packed) return {};

    int totalSize = llvmByteSize(ty);
    if (totalSize == 0 || totalSize > 16) return {};

    struct FieldLayout { int offset; ResolvedTyPtr fty; };
    std::vector<FieldLayout> fields;
    {
        int off = 0;
        for (auto& fty : info.fieldTypes) {
            if (!fty) continue;
            int fsz    = llvmByteSize(fty);
            int falign = std::min(fsz, 8);
            if (falign > 1)
                off = ((off + falign - 1) / falign) * falign;
            fields.push_back({off, fty});
            off += fsz;
        }
    }

    std::vector<AbiChunk> chunks;
    int numEb = (totalSize + 7) / 8;
    for (int eb = 0; eb < numEb; ++eb) {
        int ebStart = eb * 8;
        int ebEnd   = std::min(ebStart + 8, totalSize);

        bool allSse = true;
        bool hasAny = false;
        std::vector<std::pair<int /*local offset*/, ResolvedTyPtr>> ebFields;

        for (auto& f : fields) {
            int fStart = f.offset;
            int fEnd   = fStart + llvmByteSize(f.fty);
            if (fStart >= ebEnd || fEnd <= ebStart) continue;
            hasAny = true;
            bool isFloat = f.fty &&
                           (f.fty->kind == TyKind::F32 ||
                            f.fty->kind == TyKind::F64);
            if (!isFloat) allSse = false;
            ebFields.push_back({fStart - ebStart, f.fty});
        }

        if (!hasAny) continue;

        AbiChunk chunk;
        chunk.byteOffset = ebStart;
        chunk.byteSize   = ebEnd - ebStart;

        if (allSse) {
            chunk.cls = AbiChunk::Class::SSE;
            if (ebFields.size() == 2 &&
                ebFields[0].second && ebFields[0].second->kind == TyKind::F32 &&
                ebFields[1].second && ebFields[1].second->kind == TyKind::F32) {
                chunk.llvmType = "<2 x float>";
            } else if (ebFields.size() == 1 && ebFields[0].second) {
                chunk.llvmType =
                    (ebFields[0].second->kind == TyKind::F64) ? "double" : "float";
            } else {
                chunk.llvmType = "double";
            }
        } else {
            chunk.cls = AbiChunk::Class::INTEGER;
            if      (chunk.byteSize <= 1) chunk.llvmType = "i8";
            else if (chunk.byteSize <= 2) chunk.llvmType = "i16";
            else if (chunk.byteSize <= 4) chunk.llvmType = "i32";
            else                          chunk.llvmType = "i64";
        }
        chunks.push_back(chunk);
    }
    return chunks;
}

std::string CodeGen::emitLoadAbiChunk(const std::string& basePtr,
                                       const AbiChunk& chunk) {
    std::string ptr = basePtr;
    if (chunk.byteOffset > 0) {
        std::string gep = newTmp();
        line("  " + gep + " = getelementptr inbounds i8, ptr " + basePtr +
             ", i32 " + std::to_string(chunk.byteOffset));
        ptr = gep;
    }
    int align = (chunk.cls == AbiChunk::Class::SSE)
                    ? 4
                    : std::max(1, std::min(chunk.byteSize, 8));
    std::string val = newTmp();
    line("  " + val + " = load " + chunk.llvmType + ", ptr " + ptr +
         ", align " + std::to_string(align));
    return val;
}

ResolvedTyPtr CodeGen::exprType(const Expr& e) const {
    return std::visit([&](auto& x) -> ResolvedTyPtr {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, IntLitExpr>) {
            auto ctxIt = currentSubst_.find("#ctx");
            if (ctxIt != currentSubst_.end() && ctxIt->second && ctxIt->second->isFloat())
                return ctxIt->second;
            return Ty::I32();
        }
        if constexpr (std::is_same_v<T, FloatLitExpr>) {
            auto ctxIt = currentSubst_.find("#ctx");
            if (ctxIt != currentSubst_.end() && ctxIt->second && ctxIt->second->isFloat())
                return ctxIt->second;
            return Ty::F64();
        }
        if constexpr (std::is_same_v<T, StringLitExpr>) return Ty::Str();
        if constexpr (std::is_same_v<T, CharLitExpr>)   return Ty::Char();
        if constexpr (std::is_same_v<T, BoolLitExpr>)   return Ty::Bool();
        if constexpr (std::is_same_v<T, IdentExpr>) {
            auto it = varTypes_.find(x.name);
            if (it != varTypes_.end()) return it->second;
            auto gty = globalSymbolType(x.name);
            if (gty) return gty;
            if (permissive_ && !sema_.funcs().count(x.name)) {
                auto ii = implicitGlobals_.find(x.name);
                if (ii != implicitGlobals_.end()) return ii->second;
                return ptrBits_ == 32 ? Ty::I32() : Ty::I64();
            }
            return Ty::Void();
        }
        if constexpr (std::is_same_v<T, StructInitExpr>) {
            return sema_.resolveType(*x.type, currentSubst_);
        }
        if constexpr (std::is_same_v<T, ArrayLitExpr>) {
            if (x.elems.empty()) return Ty::Array(Ty::I32(), 0);
            auto elemTy = exprType(*x.elems[0]);
            return Ty::Array(elemTy, (int64_t)x.elems.size());
        }
        if constexpr (std::is_same_v<T, CallExpr>) {
            if (x.callee->template is<IdentExpr>()) {
                auto& nm = x.callee->template as<IdentExpr>().name;
                auto tyit = varTypes_.find(nm);
                if (tyit != varTypes_.end() && tyit->second && tyit->second->isFuncPtr())
                    return tyit->second->funcRetType ? tyit->second->funcRetType : Ty::Void();
                auto& funcs = sema_.funcs();
                auto it = funcs.find(nm);
                if (it != funcs.end()) {
                    if (it->second.decl &&
                        it->second.decl->retType &&
                        it->second.decl->retType->isHash()) {
                        auto hctx = currentSubst_.find("#ctx");
                        if (hctx != currentSubst_.end())
                            return hctx->second;
                    }
                    return it->second.retType;
                }
            } else {
                auto calleeTy = exprType(*x.callee);
                if (calleeTy && calleeTy->isFuncPtr())
                    return calleeTy->funcRetType ? calleeTy->funcRetType : Ty::Void();
            }
            return Ty::I32();
        }
        if constexpr (std::is_same_v<T, BinOpExpr>) {
            if (x.op == "==" || x.op == "!=" || x.op == "<" ||
                x.op == ">"  || x.op == "<=" || x.op == ">=" ||
                x.op == "&&" || x.op == "||")
                return Ty::Bool();
            auto lhsTy = exprType(*x.lhs);
            ResolvedTyPtr rhsTy;
            if (lhsTy && (lhsTy->isFloat() || lhsTy->isInteger())) {
                auto& mutableSubst = const_cast<SubstMap&>(currentSubst_);
                SubstMap saved = mutableSubst;
                mutableSubst["#ctx"] = lhsTy;
                rhsTy = exprType(*x.rhs);
                mutableSubst = saved;
            } else {
                rhsTy = exprType(*x.rhs);
            }
            ResolvedTyPtr result = lhsTy ? lhsTy : Ty::I32();
            if (lhsTy && rhsTy) {
                if (lhsTy->isFloat() && rhsTy->isFloat() &&
                    lhsTy->byteSize(ptrBits_) != rhsTy->byteSize(ptrBits_))
                    result = (lhsTy->byteSize(ptrBits_) >= rhsTy->byteSize(ptrBits_)) ? lhsTy : rhsTy;
                else if (lhsTy->isInteger() && rhsTy->isInteger() &&
                         lhsTy->bitWidth(ptrBits_) != rhsTy->bitWidth(ptrBits_))
                    result = (lhsTy->bitWidth(ptrBits_) >= rhsTy->bitWidth(ptrBits_)) ? lhsTy : rhsTy;
                else if (lhsTy->isFloat()   && rhsTy->isInteger())
                    result = lhsTy;
                else if (lhsTy->isInteger() && rhsTy->isFloat())
                    result = rhsTy;
            }
            auto ctxIt = currentSubst_.find("#ctx");
            if (ctxIt != currentSubst_.end() && ctxIt->second && result &&
                !result->equals(*ctxIt->second)) {
                auto ctxTy = ctxIt->second;
                if (result->isInteger() && ctxTy->isInteger() &&
                    ctxTy->bitWidth(ptrBits_) > result->bitWidth(ptrBits_)) {
                    result = ctxTy;
                } else if (result->isFloat() && ctxTy->isFloat() &&
                           ctxTy->byteSize(ptrBits_) > result->byteSize(ptrBits_)) {
                    result = ctxTy;
                }
            }
            return result;
        }
        if constexpr (std::is_same_v<T, CastExpr>) {
            return sema_.resolveType(*x.type, currentSubst_);
        }
        if constexpr (std::is_same_v<T, FieldAccessExpr>) {
            auto ot = exprType(*x.obj);
            while (ot && ot->isPtr() && ot->inner) ot = ot->inner;
            if (ot && ot->isStruct()) {
                auto& structs = sema_.structs();
                auto it = structs.find(ot->name);
                if (it != structs.end()) {
                    int idx = it->second->fieldIndex(x.field);
                    if (idx >= 0) {
                        auto& info = *it->second;
                        auto rawTy = info.fieldTypes[idx];
                        if (!info.typeParams.empty() && !ot->typeArgs.empty()) {
                            SubstMap structSubst = currentSubst_;
                            for (size_t i = 0;
                                 i < info.typeParams.size() && i < ot->typeArgs.size(); ++i)
                                structSubst[info.typeParams[i]] = ot->typeArgs[i];
                            return applySubst(rawTy, structSubst);
                        }
                        return rawTy;
                    }
                }
            }
            return Ty::I32();
        }
        if constexpr (std::is_same_v<T, IndexExpr>) {
            auto at = exprType(*x.arr);
            if (at && at->isArray()) return at->elemTy;
            if (at && at->isPtr())   return at->inner ? at->inner : Ty::I8();
            if (at && at->isStr())   return Ty::I8();
            return Ty::I32();
        }
        if constexpr (std::is_same_v<T, DerefExpr>) {
            auto t = exprType(*x.operand);
            if (t && t->isPtr()) return t->inner ? t->inner : Ty::I8();
            if (t && t->isStr()) return Ty::Char();
            return Ty::I32();
        }
        if constexpr (std::is_same_v<T, AddressOfExpr>) {
            if (x.operand->template is<IdentExpr>()) {
                auto& name = x.operand->template as<IdentExpr>().name;
                auto fit = sema_.funcs().find(name);
                if (fit != sema_.funcs().end()) {
                    std::vector<ResolvedTyPtr> pts;
                    if (fit->second.decl)
                        for (auto& p : fit->second.decl->params)
                            if (p.type) pts.push_back(sema_.resolveType(*p.type, currentSubst_));
                    return Ty::FuncPtr(std::move(pts), fit->second.retType);
                }
            }
            return Ty::Ptr(exprType(*x.operand));
        }
        if constexpr (std::is_same_v<T, UnaryOpExpr>) {
            if (x.op == "!") return Ty::Bool();
            return exprType(*x.operand);
        }
        if constexpr (std::is_same_v<T, AssignExpr>) {
            auto lhsTy = exprType(*x.lhs);
            return lhsTy ? lhsTy : exprType(*x.rhs);
        }
        if constexpr (std::is_same_v<T, TernaryExpr>) {
            return exprType(*x.thenExpr);
        }
        if constexpr (std::is_same_v<T, EnumAccessExpr>) {
            return Ty::Named(TyKind::Enum, x.enumName);
        }
        if constexpr (std::is_same_v<T, ReadonlyExpr>) return exprType(*x.operand);
        if constexpr (std::is_same_v<T, ReleaseExpr>)  return exprType(*x.operand);
        if constexpr (std::is_same_v<T, CheckExpr>)    return exprType(*x.operand);
        if constexpr (std::is_same_v<T, ClaimExpr>)    return exprType(*x.operand);
        if constexpr (std::is_same_v<T, TakeExpr>) {
            if (x.operand->template is<IdentExpr>())
                return Ty::Ptr(exprType(*x.operand));
            return exprType(*x.operand);
        }
        return Ty::I32();
    }, e.v);
}

std::string CodeGen::allocaFor(const std::string& name, const ResolvedTyPtr& ty) {
    std::string base = name + ".addr";
    std::string ptr  = "%" + base;
    if (varAlloca_.count(name))
        ptr = "%" + base + "." + std::to_string(tmpIdx_++);
    bool packed = false;
    if (ty && ty->isStruct()) {
        auto sit = sema_.structs().find(ty->name);
        if (sit != sema_.structs().end()) packed = sit->second->packed;
    }
    ensureTypeEmitted(ty, currentSubst_);
    int _align = validAlign(llvmByteSize(ty), packed);
    line("  " + ptr + " = alloca " + llvmTy(ty) + ", align " + std::to_string(_align));
    if (ty && (ty->isStr() || ty->isPtr())) {
        line("  store ptr null, ptr " + ptr + ", align 8");
    }
    varAlloca_[name] = ptr;
    varTypes_[name]  = ty;
    return ptr;
}

ResolvedTyPtr CodeGen::implicitExternGlobal(const std::string& name,
                                            const ResolvedTyPtr& hint) {
    auto it = implicitGlobals_.find(name);
    if (it != implicitGlobals_.end()) return it->second;
    ResolvedTyPtr ty = hint;
    if (!ty || ty->isVoid() || ty->isError() || ty->isGeneric())
        ty = (ptrBits_ == 32) ? Ty::I32() : Ty::I64();
    implicitGlobals_[name] = ty;
    return ty;
}

std::string CodeGen::loadVar(const std::string& name) {
    auto ait = varAlloca_.find(name);
    if (ait == varAlloca_.end()) {
        auto gty = globalSymbolType(name);
        if (gty) {
            if (isAggTy(gty)) return gsym(name);
            if (staticStrGlobals_.count(name)) return gsym(name);
            std::string tmp = newTmp();
            int align = validAlign(llvmByteSize(gty));
            line("  " + tmp + " = load " + llvmTy(gty) +
                 ", ptr " + gsym(name) + ", align " + std::to_string(align));
            return tmp;
        }
        if (permissive_ && !sema_.funcs().count(name)) {
            auto ity = implicitExternGlobal(name);
            std::string tmp = newTmp();
            line("  " + tmp + " = load " + llvmTy(ity) + ", ptr @" + name +
                 ", align " + std::to_string(validAlign(llvmByteSize(ity))));
            return tmp;
        }
        if (sema_.funcs().count(name)) {
            SourceLoc loc; loc.file = sf_ ? sf_->path : "<codegen>";
            loc.line = 1; loc.col = 1; loc.len = 1;
            diag_.error(loc,
                std::format("'{}' is a function, not a variable; "
                            "did you mean '{}(...)'?", name, name));
        } else {
            SourceLoc loc; loc.file = sf_ ? sf_->path : "<codegen>";
            loc.line = 1; loc.col = 1; loc.len = 1;
            diag_.error(loc,
                std::format("internal: '{}' has no alloca at codegen; "
                            "this is a compiler bug or a missing sema check", name));
        }
        return "undef";
    }
    auto tit = varTypes_.find(name);
    if (tit == varTypes_.end()) return "undef";
    if (isAggTy(tit->second)) return ait->second;
    std::string tmp = newTmp();
    int align = validAlign(llvmByteSize(tit->second));
    line("  " + tmp + " = load " + llvmTy(tit->second) +
         ", ptr " + ait->second + ", align " + std::to_string(align));
    return tmp;
}

void CodeGen::storeVar(const std::string& name, const std::string& val,
                       const ResolvedTyPtr& ty, bool ownedVal, bool freeOld) {
    auto isCurrentlyOwnedLocal = [&](const std::string& n) {
        for (auto it = dropScopes_.rbegin(); it != dropScopes_.rend(); ++it) {
            for (auto& de : *it) {
                if (de.varName == n && de.kind == DropKind::OwnedStr)
                    return true;
            }
        }
        return false;
    };

    if (isGlobalSymbol(name)) {
        auto gty = globalSymbolType(name);
        if (!gty) return;

        if (gty->isStr()) {
            std::string storeVal = val;
            if (!ownedVal && val != "null") {
                storeVal = emitOwnedStrCopy(val);
            }

            std::string oldPtr = newTmp();
            line("  " + oldPtr + " = load ptr, ptr " + gsym(name) + ", align 8");
            emitStrFree(oldPtr);

            line("  store ptr " + storeVal + ", ptr " + gsym(name) + ", align 8");
            return;
        }

        if (isAggTy(gty)) {
            emitAggrCopy(gsym(name), val, gty);
            return;
        }

        int align = validAlign(llvmByteSize(gty));
        line("  store " + llvmTy(gty) + " " + val + ", ptr " + gsym(name) +
             ", align " + std::to_string(align));
        return;
    }

    auto ait = varAlloca_.find(name);
    if (ait == varAlloca_.end()) {
        if (permissive_ && !sema_.funcs().count(name)) {
            auto ity = implicitExternGlobal(name, ty);
            if (isAggTy(ity)) { emitAggrCopy("@" + name, val, ity); return; }
            line("  store " + llvmTy(ity) + " " + val + ", ptr @" + name +
                 ", align " + std::to_string(validAlign(llvmByteSize(ity))));
        }
        return;
    }
    if (isAggTy(ty)) {
        if (val == "zeroinitializer")
            line("  store " + llvmTy(ty) + " zeroinitializer, ptr " + ait->second);
        else
            emitAggrCopy(ait->second, val, ty);
    } else {
        if (ty && ty->isStr() && isCurrentlyOwnedLocal(name)) {
            std::string storeVal = val;
            if (!ownedVal && val != "null") {
                storeVal = emitOwnedStrCopy(val);
            }

            if (freeOld) {
                std::string oldPtr = newTmp();
                line("  " + oldPtr + " = load ptr, ptr " + ait->second + ", align 8");
                emitStrFree(oldPtr);
            }

            int align = validAlign(llvmByteSize(ty));
            line("  store ptr " + storeVal + ", ptr " + ait->second +
                 ", align " + std::to_string(align));
        } else {
            int align = validAlign(llvmByteSize(ty));
            line("  store " + llvmTy(ty) + " " + val + ", ptr " + ait->second +
                 ", align " + std::to_string(align));
        }
    }
}

void CodeGen::emitFuncSignature(std::ostringstream& os,
                                 const std::string& name,
                                 const FuncDecl& fd,
                                 const SubstMap& subst) {
    ResolvedTyPtr retTy;
    if (fd.retType && fd.retType->isHash()) {
        auto it = subst.find("#ctx");
        retTy = (it != subst.end()) ? it->second : Ty::Void();
    } else if (fd.retType) {
        retTy = sema_.resolveType(*fd.retType, subst);
    } else {
        retTy = Ty::Void();
    }

    if (fd.isMain) retTy = Ty::I32();

    ensureTypeEmitted(retTy, subst);
    bool aggRet = isAggTy(retTy) && !fd.isMain;
    if (aggRet)
        os << "define void @" << name << "(ptr sret(" << llvmTy(retTy) << ") %.sret";
    else
        os << "define " << llvmTy(retTy) << " @" << name << "(";

    if (fd.isMain && fd.params.size() >= 2) {
        os << "i32 %" << fd.params[0].name;
        os << ", ptr %" << fd.params[1].name;
        os << ") {\n";
        return;
    }

    for (size_t i = 0; i < fd.params.size(); ++i) {
        if (aggRet || i) os << ", ";
        auto pty = sema_.resolveType(*fd.params[i].type, subst);
        ensureTypeEmitted(pty, subst);
        if (is32bit() && pty && !fd.isMain &&
            (pty->kind == TyKind::I64 || pty->kind == TyKind::U64 ||
             pty->kind == TyKind::F64)) {
            diag_.warning(fd.params[i].loc,
                std::format("parameter '{}' has 64-bit type '{}' in a 32-bit target function; "
                            "calling convention may require special handling",
                            fd.params[i].name, pty->str()));
        }
        bool isAgg = pty && (pty->isStruct() || pty->isEnum());
        os << (isAgg ? "ptr" : llvmTy(pty)) << " %" << fd.params[i].name;
    }
    os << ") {\n";
}

void CodeGen::emitFunc(const FuncDecl& fd, const SubstMap& subst,
                       const std::string& mangledName) {
    tmpIdx_    = 0;
    labelIdx_  = 0;
    varAlloca_.clear();
    varTypes_.clear();
    breakLabels_.clear();
    contLabels_.clear();
    dropScopes_.clear();
    loopDropDepths_.clear();
    blockTerminated_ = false;
    currentSubst_    = subst;
    curFuncName_     = mangledName;
    curFuncOrigName_ = fd.name;

    if (fd.retType && fd.retType->isHash()) {
        auto hctx = subst.find("#ctx");
        curRetTy_ = (hctx != subst.end()) ? hctx->second : Ty::Void();
    } else if (fd.retType) {
        curRetTy_ = sema_.resolveType(*fd.retType, subst);
    } else {
        curRetTy_ = Ty::Void();
    }
    if (fd.isMain) curRetTy_ = Ty::I32();
    curSretPtr_ = (isAggTy(curRetTy_) && !fd.isMain) ? "%.sret" : "";
    
    for (auto& p : fd.params)
        ensureTypeEmitted(sema_.resolveType(*p.type, subst), subst);
    ensureTypeEmitted(curRetTy_, subst);

    emitFuncSignature(*fnOut_, mangledName, fd, subst);
    line("entry:");

    std::ostringstream  bodyOs;
    std::ostringstream* realOut = fnOut_;
    fnOut_ = &bodyOs;
    hoistedAllocas_.clear();
    hoistAllocas_ = true;

    for (auto& p : fd.params) {
        ResolvedTyPtr pty;
        if (fd.isMain) {
            if (&p == &fd.params[1])
                pty = Ty::Ptr(Ty::Str());
            else
                pty = Ty::I32();
        } else {
            pty = sema_.resolveType(*p.type, subst);
        }
        bool isAgg = pty && (pty->isStruct() || pty->isEnum());
        if (isAgg) {
            bool packed = false;
            if (pty->isStruct()) {
                auto sit = sema_.structs().find(pty->name);
                if (sit != sema_.structs().end()) packed = sit->second->packed;
            }
            int sz    = llvmByteSize(pty);
            int align = validAlign(sz, packed);
            const std::string szTy = is32bit() ? "i32" : "i64";
            std::string localPtr = "%" + p.name + ".local";
            line("  " + localPtr + " = alloca " + llvmTy(pty) +
                 ", align " + std::to_string(align));
            line("  call void @" + rtSym("memcpy") + "(ptr " + localPtr + ", ptr %" + p.name +
                 ", " + szTy + " " + std::to_string(sz) + ")");
            varAlloca_[p.name] = localPtr;
            varTypes_[p.name]  = pty;
        } else {
            allocaFor(p.name, pty);
            storeVar(p.name, "%" + p.name, pty);
        }
    }

    pushDropScope();

    if (fd.isMain) {
        if (hasGlobalFini_ && !raw_)
            line("  call i32 @atexit(ptr @__rosaline_global_fini)");
        if (hasGlobalInit_)
            line("  call void @__rosaline_global_init()");
    }

    for (auto& s : fd.body) {
        emitStmt(*s);
    }

    if (!blockTerminated_) {
        std::string fallLabel = newLabel();
        emitAllDropsExcept();
        popNoDrops();
        line("  br label %" + fallLabel);
        line(fallLabel + ":");
        if (curRetTy_->isVoid() || !curSretPtr_.empty())
            line("  ret void");
        else
            line("  ret " + llvmTy(curRetTy_) + " " + zeroVal(curRetTy_));
    } else {
        popNoDrops();
    }

    line("}");

    hoistAllocas_ = false;
    fnOut_ = realOut;
    for (auto& a : hoistedAllocas_) *fnOut_ << a << "\n";
    *fnOut_ << bodyOs.str();
    hoistedAllocas_.clear();
}

void CodeGen::emitStmt(const Stmt& s) {
    std::visit([&](auto& x) {
        using T = std::decay_t<decltype(x)>;

        if constexpr (std::is_same_v<T, VarDeclStmt>)    emitVarDecl(x, s.loc);
        else if constexpr (std::is_same_v<T, ExprStmt>) {
            bool saved = usedAsValue_;
            usedAsValue_ = !x.expr->template is<CallExpr>();
            emitExpr(*x.expr);
            usedAsValue_ = saved;
        }
        else if constexpr (std::is_same_v<T, ReturnStmt>) emitReturn(x, curRetTy_);
        else if constexpr (std::is_same_v<T, IfStmt>)    emitIf(x);
        else if constexpr (std::is_same_v<T, WhileStmt>) emitWhile(x);
        else if constexpr (std::is_same_v<T, BreakStmt>) {
            if (!breakLabels_.empty()) {
                int loopDepth = loopDropDepths_.empty() ? 0 : loopDropDepths_.back();
                emitDropsFromDepth(loopDepth);
                line("  br label %" + breakLabels_.back());
            } else {
                diag_.error(s.loc, "break outside loop");
            }
        }
        else if constexpr (std::is_same_v<T, ContinueStmt>) {
            if (!contLabels_.empty()) {
                int loopDepth = loopDropDepths_.empty() ? 0 : loopDropDepths_.back();
                emitDropsFromDepth(loopDepth);
                line("  br label %" + contLabels_.back());
            } else {
                diag_.error(s.loc, "continue outside loop");
            }
        }
        else if constexpr (std::is_same_v<T, BlockStmt>) {
            pushDropScope();
            for (auto& st : x.stmts) emitStmt(*st);
            popAndEmitDrops();
        }
        else if constexpr (std::is_same_v<T, AsmBlockStmt>) {
            std::string escaped;
            for (unsigned char ch : x.asmTemplate) {
                if (ch == '"')       escaped += "\\22";
                else if (ch == '\\') escaped += "\\5C";
                else if (ch == '\n') escaped += "\\0A";
                else if (ch == '\r') escaped += "";
                else                 escaped += (char)ch;
            }

            std::string llvmConstraints = x.constraints.empty() ? "" : x.constraints;
            if (!x.clobbers.empty()) {
                if (!llvmConstraints.empty()) llvmConstraints += ",";
                llvmConstraints += x.clobbers;
            }

            std::string vol = x.isVolatile ? "sideeffect " : "";
            line("  call void asm " + vol + "\"" + escaped + "\", \""
                 + llvmConstraints + "\"()");
        }
    }, s.v);
}

void CodeGen::emitVarDecl(const VarDeclStmt& vd, const SourceLoc& loc) {
    const bool isDiscard = (vd.name == "_");
    const std::string effectiveName = isDiscard
        ? ("__discard_" + std::to_string(tmpIdx_++))
        : vd.name;

    auto ty = sema_.resolveType(*vd.type, currentSubst_);

    if (ty->isGeneric()) {
        diag_.error(loc,
            std::format("variable '{}' has unresolved generic type '{}' at codegen time; "
                        "ensure the enclosing function is called with explicit type arguments",
                        vd.name, ty->str()),
            lineFor(loc));
    }

    if (ty->isArray() && ty->arraySize == 0 && vd.type->isArray()) {
        auto& arrNode = vd.type->asArray();
        if (arrNode.size) {
            if (arrNode.size->template is<IntLitExpr>())
                ty = Ty::Array(ty->elemTy, arrNode.size->template as<IntLitExpr>().val);
            else
                ty = Ty::Array(ty->elemTy, 1);
        } else {
            if (vd.init && vd.init->template is<ArrayLitExpr>()) {
                int64_t n = (int64_t)vd.init->template as<ArrayLitExpr>().elems.size();
                ty = Ty::Array(ty->elemTy, n);
            } else {
                ty = Ty::Array(ty->elemTy, 1);
            }
        }
    }
    if (ty->isArray() && ty->arraySize == 0 && vd.init &&
        vd.init->template is<ArrayLitExpr>()) {
        auto litTy = exprType(*vd.init);
        if (litTy && litTy->isArray() && litTy->arraySize > 0)
            ty = litTy;
    }
    allocaFor(effectiveName, ty);

    if (!dropScopes_.empty()) {
        if (!vd.nomd &&
            ((ty->isStr() && (isOwnedInit(vd.init.get()) || !vd.init)) ||
             (ty->isPtr() && isOwnedInit(vd.init.get())))) {
            addToDropScope(effectiveName, ty, DropKind::OwnedStr);
        } else if (ty->isEnum() &&
                   enumHasDroppablePayload(ty->name, ty->typeArgs)) {
            if (vd.init) {
                bool hasOwnedPayload = false;
                if (vd.init->template is<EnumAccessExpr>()) {
                    auto& eax = vd.init->template as<EnumAccessExpr>();
                    for (auto& arg : eax.args) {
                        if (arg->template is<StringLitExpr>()) continue;
                        if (arg->template is<IdentExpr>())     continue;
                        if (arg->template is<ReadonlyExpr>())  continue;
                        if (arg->template is<TakeExpr>())      continue;
                        if (arg->template is<ReleaseExpr>())   continue;
                        hasOwnedPayload = true;
                        break;
                    }
                } else {
                    hasOwnedPayload = true;
                }
                if (hasOwnedPayload)
                    addToDropScope(effectiveName, ty, DropKind::EnumWithStrPayload);
            }
        }
    }

    if (vd.init) {
        if (vd.init->template is<CallExpr>()) {
            auto& callE = vd.init->template as<CallExpr>();
            if (callE.callee->template is<IdentExpr>()) {
                const std::string& fname = callE.callee->template as<IdentExpr>().name;
                auto fit = sema_.funcs().find(fname);
                if (fit != sema_.funcs().end()) {
                    bool hashRet = fit->second.decl &&
                                fit->second.decl->retType &&
                                fit->second.decl->retType->isHash();
                    if (hashRet && !ty->isVoid()) {
                        SubstMap saved = currentSubst_;
                        currentSubst_["#ctx"] = ty;

                        std::vector<ResolvedTyPtr> typeArgs;
                        for (auto& gp : fit->second.genericParams) {
                            if (gp == "#ctx") { typeArgs.push_back(ty); continue; }
                            auto sit2 = currentSubst_.find(gp);
                            if (sit2 != currentSubst_.end())
                                typeArgs.push_back(sit2->second);
                        }

                        schedMono(fname, typeArgs);
                        std::string mname = mangleCall(fname, typeArgs);

                        std::string argStr;
                        std::vector<std::string> argVals;
                        for (auto& a : callE.args) argVals.push_back(emitExpr(*a));
                        for (size_t i = 0; i < argVals.size(); ++i) {
                            if (i) argStr += ", ";
                            ResolvedTyPtr apt = (i < fit->second.paramTypes.size())
                                ? applySubst(fit->second.paramTypes[i], currentSubst_)
                                : Ty::I32();
                            bool isAgg = apt && (apt->isStruct() || apt->isEnum());
                            if (isAgg) {
                                argStr += "ptr " + argVals[i];
                            } else {
                                argStr += llvmTy(apt) + " " + argVals[i];
                            }
                        }

                        currentSubst_ = saved;

                        if (isAggTy(ty)) {
                            std::string fullArgs = "ptr " + varAlloca_[effectiveName];
                            if (!argStr.empty()) fullArgs += ", " + argStr;
                            line("  call void @" + mname + "(" + fullArgs + ")");
                        } else {
                            std::string res = newTmp();
                            line("  " + res + " = call " + llvmTy(ty) +
                                " @" + mname + "(" + argStr + ")");
                            storeVar(effectiveName, res, ty);
                        }
                        return;
                    }
                }
            }
        }

        SubstMap saved = currentSubst_;
        if (ty && !ty->isVoid())
            currentSubst_["#ctx"] = ty;

        if (vd.init->template is<CallExpr>() && !ty->isGeneric()) {
            auto& callE = vd.init->template as<CallExpr>();
            if (callE.callee->template is<IdentExpr>()) {
                const std::string& cname = callE.callee->template as<IdentExpr>().name;
                auto fit2 = sema_.funcs().find(cname);
                if (fit2 != sema_.funcs().end() && fit2->second.isGeneric) {
                    auto retTy2 = fit2->second.retType;
                    if (retTy2 && retTy2->isGeneric()) {
                        currentSubst_[retTy2->genericName] = ty;
                    }
                }
            }
        }

        std::string val = emitExpr(*vd.init);

        if (vd.init->template is<CallExpr>()) {
            auto& callE2 = vd.init->template as<CallExpr>();
            if (callE2.callee->template is<IdentExpr>()) {
                const std::string& cname2 = callE2.callee->template as<IdentExpr>().name;
                auto fit3 = sema_.funcs().find(cname2);
                if (fit3 != sema_.funcs().end() && fit3->second.isGeneric) {
                    auto retTy3 = fit3->second.retType;
                    if (retTy3 && retTy3->isGeneric())
                        currentSubst_.erase(retTy3->genericName);
                }
            }
        }

        currentSubst_ = saved;

        auto initTy = exprType(*vd.init);

        bool initIsCtxFloat = ty && ty->isFloat() &&
            initTy && initTy->isFloat() && !ty->equals(*initTy) &&
            (vd.init->template is<FloatLitExpr>() ||
             vd.init->template is<IntLitExpr>()   ||
             vd.init->template is<UnaryOpExpr>()  ||
             vd.init->template is<BinOpExpr>());
        if (!initIsCtxFloat && ty && ty->isFloat() && isPureIntLiteral(*vd.init))
            initIsCtxFloat = true;

        if (initIsCtxFloat) {
            storeVar(effectiveName, val, ty);
        } else if (initTy && ty &&
            initTy->isInteger() && ty->isInteger() &&
            ty->bitWidth(ptrBits_) > initTy->bitWidth(ptrBits_)) {
            storeVar(effectiveName, val, ty);
        } else if (initTy && ty &&
                   initTy->isFloat() && ty->isFloat() &&
                   ty->byteSize(ptrBits_) > initTy->byteSize(ptrBits_)) {
            storeVar(effectiveName, val, ty);
        } else {
            val = implicitCoerce(val, initTy, ty, *fnOut_, tmpIdx_, ptrBits_);
            storeVar(effectiveName, val, ty,
                     ty->isStr() && isOwnedInit(vd.init.get()));
        }
    } else {
        if (isAggTy(ty))
            line("  store " + llvmTy(ty) + " zeroinitializer, ptr " + varAlloca_[effectiveName]);
        else
            storeVar(effectiveName, zeroVal(ty), ty);
    }
}

void CodeGen::emitIf(const IfStmt& s) {
    std::string condVal = emitExpr(*s.cond);
    std::string cond1 = newTmp();
    auto condTy = exprType(*s.cond);
    if (condTy && condTy->kind == TyKind::Bool) {
        cond1 = condVal;
    } else if (condTy && (condTy->isPtr() || condTy->isStr())) {
        line("  " + cond1 + " = icmp ne ptr " + condVal + ", null");
    } else {
        line("  " + cond1 + " = icmp ne " + llvmTy(condTy) + " " + condVal + ", 0");
    }

    std::string thenLbl = newLabel();
    std::string elseLbl = newLabel();
    std::string endLbl  = newLabel();

    bool hasElse = !s.elseBody.empty();
    if (hasElse)
        line("  br i1 " + cond1 + ", label %" + thenLbl + ", label %" + elseLbl);
    else
        line("  br i1 " + cond1 + ", label %" + thenLbl + ", label %" + endLbl);

    line(thenLbl + ":");
    pushDropScope();
    for (auto& st : s.thenBody) emitStmt(*st);
    popAndEmitDrops();
    line("  br label %" + endLbl);

    if (hasElse) {
        line(elseLbl + ":");
        pushDropScope();
        for (auto& st : s.elseBody) emitStmt(*st);
        popAndEmitDrops();
        line("  br label %" + endLbl);
    }

    line(endLbl + ":");
}

void CodeGen::emitWhile(const WhileStmt& s) {
    std::string condLbl = newLabel();
    std::string bodyLbl = newLabel();
    std::string endLbl  = newLabel();

    breakLabels_.push_back(endLbl);
    contLabels_.push_back(condLbl);
    loopDropDepths_.push_back((int)dropScopes_.size());

    line("  br label %" + condLbl);
    line(condLbl + ":");

    std::string condVal = emitExpr(*s.cond);
    std::string cond1   = condVal;
    auto condTy = exprType(*s.cond);
    if (!condTy || condTy->kind != TyKind::Bool) {
        cond1 = newTmp();
        if (condTy && (condTy->isPtr() || condTy->isStr()))
            line("  " + cond1 + " = icmp ne ptr " + condVal + ", null");
        else
            line("  " + cond1 + " = icmp ne " + llvmTy(condTy) + " " + condVal + ", 0");
    }
    line("  br i1 " + cond1 + ", label %" + bodyLbl + ", label %" + endLbl);

    line(bodyLbl + ":");
    pushDropScope();
    for (auto& st : s.body) emitStmt(*st);
    popAndEmitDrops();
    line("  br label %" + condLbl);

    line(endLbl + ":");

    loopDropDepths_.pop_back();
    breakLabels_.pop_back();
    contLabels_.pop_back();
}

void CodeGen::emitReturn(const ReturnStmt& s, const ResolvedTyPtr& retTy) {
    if (!s.value) {
        emitAllDropsExcept();
        if (!retTy->isVoid() && curSretPtr_.empty())
            line("  ret " + llvmTy(retTy) + " " + zeroVal(retTy));
        else
            line("  ret void");
        return;
    }
    if (retTy->isVoid()) {
        emitExpr(*s.value);
        emitAllDropsExcept();
        line("  ret void");
        return;
    }

    std::string val = emitExpr(*s.value);
    std::string skipDrop;
    if (s.value->template is<IdentExpr>()) {
        const std::string& rname = s.value->template as<IdentExpr>().name;
        for (auto& frame : dropScopes_) {
            for (auto& de : frame) {
                if (de.varName == rname && de.kind == DropKind::OwnedStr) {
                    skipDrop = rname;
                    break;
                }
            }
            if (!skipDrop.empty()) break;
        }
    }

    emitAllDropsExcept(skipDrop);

    if (!curSretPtr_.empty()) {
        emitAggrCopy(curSretPtr_, val, retTy);
        line("  ret void");
    } else if (isAggTy(retTy)) {
        std::string loaded = newTmp();
        bool retPacked = false;
        if (retTy->isStruct()) {
            auto sit = sema_.structs().find(retTy->name);
            if (sit != sema_.structs().end()) retPacked = sit->second->packed;
        }
        int align = validAlign(llvmByteSize(retTy), retPacked);
        line("  " + loaded + " = load " + llvmTy(retTy) + ", ptr " + val +
             ", align " + std::to_string(align));
        line("  ret " + llvmTy(retTy) + " " + loaded);
    } else {
        auto valTy = exprType(*s.value);
        if (valTy && !valTy->equals(*retTy)) {
            std::string coerced = newTmp();
            if (retTy->isInteger() && valTy->isInteger()) {
                if (retTy->bitWidth(ptrBits_) > valTy->bitWidth(ptrBits_))
                    line("  " + coerced + " = " +
                         (valTy->isSigned() ? "sext" : "zext") + " " +
                         llvmTy(valTy) + " " + val + " to " + llvmTy(retTy));
                else if (retTy->bitWidth(ptrBits_) < valTy->bitWidth(ptrBits_))
                    line("  " + coerced + " = trunc " +
                         llvmTy(valTy) + " " + val + " to " + llvmTy(retTy));
                else coerced = val;
                val = coerced;
            } else if (retTy->isFloat() && valTy->isFloat()) {
                if (retTy->byteSize(ptrBits_) > valTy->byteSize(ptrBits_))
                    line("  " + coerced + " = fpext " +
                         llvmTy(valTy) + " " + val + " to " + llvmTy(retTy));
                else
                    line("  " + coerced + " = fptrunc " +
                         llvmTy(valTy) + " " + val + " to " + llvmTy(retTy));
                val = coerced;
            } else if (valTy->isInteger() && retTy->isFloat()) {
                line("  " + coerced + " = " +
                     (valTy->isSigned() ? "sitofp" : "uitofp") + " " +
                     llvmTy(valTy) + " " + val + " to " + llvmTy(retTy));
                val = coerced;
            } else if (valTy->isFloat() && retTy->isInteger()) {
                line("  " + coerced + " = " +
                     (retTy->isSigned() ? "fptosi" : "fptoui") + " " +
                     llvmTy(valTy) + " " + val + " to " + llvmTy(retTy));
                val = coerced;
            }
        }
        line("  ret " + llvmTy(retTy) + " " + val);
    }
}

std::string CodeGen::emitExpr(const Expr& e) {
    {
        auto ety = exprType(e);
        if (ety && ety->isError()) {
            const std::string safeTy = (ptrBits_ == 32) ? "i32" : "i64";
            return "undef";
        }
    }

    return std::visit([&](auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;

        if constexpr (std::is_same_v<T, IntLitExpr>) {
            auto ctxIt = currentSubst_.find("#ctx");
            if (ctxIt != currentSubst_.end() && ctxIt->second && ctxIt->second->isFloat())
                return emitIntLit(x.val, ctxIt->second);
            return std::to_string(x.val);
        }

        if constexpr (std::is_same_v<T, FloatLitExpr>) {
            auto ctxIt = currentSubst_.find("#ctx");
            bool asFloat32 = (ctxIt != currentSubst_.end() && ctxIt->second &&
                              ctxIt->second->isFloat() && ctxIt->second->byteSize(ptrBits_) == 4);
            return formatLLVMFloat((double)x.val, asFloat32);
        }

        if constexpr (std::is_same_v<T, StringLitExpr>) {
            std::string sym = internString(x.val);
            std::string tmp = newTmp();
            int len = (int)x.val.size() + 1;
            line("  " + tmp + " = getelementptr inbounds [" +
                 std::to_string(len) + " x i8], ptr @" + sym + ", i32 0, i32 0");
            return tmp;
        }

        if constexpr (std::is_same_v<T, CharLitExpr>)
            return std::to_string((unsigned char)x.val);

        if constexpr (std::is_same_v<T, BoolLitExpr>)
            return x.val ? "true" : "false";

        if constexpr (std::is_same_v<T, IdentExpr>)
            return loadVar(x.name);

        if constexpr (std::is_same_v<T, AssignExpr>)
            return emitAssign(x, e.loc);

        if constexpr (std::is_same_v<T, BinOpExpr>)
            return emitBinOp(x, e.loc);

        if constexpr (std::is_same_v<T, UnaryOpExpr>) {
            std::string v = emitExpr(*x.operand);
            auto ty = exprType(*x.operand);
            if (x.op == "!") {
                std::string tmp = newTmp();
                if (ty && ty->kind == TyKind::Bool) {
                    line("  " + tmp + " = xor i1 " + v + ", true");
                } else if (ty && ty->isFloat()) {
                    line("  " + tmp + " = fcmp oeq " + llvmTy(ty) + " " + v + ", 0.0");
                } else if (ty && (ty->isPtr() || ty->isStr())) {
                    line("  " + tmp + " = icmp eq ptr " + v + ", null");
                } else if (ty && ty->isInteger()) {
                    line("  " + tmp + " = icmp eq " + llvmTy(ty) + " " + v + ", 0");
                } else {
                    diag_.error(e.loc,
                        std::format("operator '!' is not defined for type '{}'", ty ? ty->str() : "?"),
                        lineFor(e.loc));
                    line("  " + tmp + " = icmp eq i32 0, 0");
                }
                return tmp;
            }
            if (x.op == "-") {
                std::string tmp = newTmp();
                if (ty && ty->isFloat())
                    line("  " + tmp + " = fneg " + llvmTy(ty) + " " + v);
                else
                    line("  " + tmp + " = sub " + llvmTy(ty) + " 0, " + v);
                return tmp;
            }
            if (x.op == "~") {
                std::string tmp = newTmp();
                line("  " + tmp + " = xor " + llvmTy(ty) + " " + v + ", -1");
                return tmp;
            }
            return v;
        }

        if constexpr (std::is_same_v<T, CastExpr>)
            return emitCast(x);

        if constexpr (std::is_same_v<T, AddressOfExpr>) {
            if (x.operand->template is<IdentExpr>()) {
                auto& id = x.operand->template as<IdentExpr>();
                if (sema_.funcs().count(id.name))
                    return "@" + id.name;
                auto ait = varAlloca_.find(id.name);
                if (ait != varAlloca_.end()) return ait->second;
                if (isGlobalSymbol(id.name)) return gsym(id.name);
            }
            return emitLValuePtr(*x.operand, e.loc);
        }

        if constexpr (std::is_same_v<T, DerefExpr>) {
            std::string ptr = emitExpr(*x.operand);
            auto ptrTy = exprType(*x.operand);
            ResolvedTyPtr innerTy;
            if (ptrTy && ptrTy->isPtr() && ptrTy->inner)
                innerTy = ptrTy->inner;
            else if (ptrTy && ptrTy->isStr())
                innerTy = Ty::Char();
            else
                innerTy = Ty::I8();
            if (isAggTy(innerTy)) return ptr;
            std::string tmp = newTmp();
            line("  " + tmp + " = load " + llvmTy(innerTy) + ", ptr " + ptr);
            return tmp;
        }

        if constexpr (std::is_same_v<T, ArrayLitExpr>) {
            if (x.elems.empty()) {
                diag_.error(e.loc, "array literal cannot be empty", lineFor(e.loc));
                return "undef";
            }
            ResolvedTyPtr elemTy;
            {
                auto ctxIt = currentSubst_.find("#ctx");
                if (ctxIt != currentSubst_.end() && ctxIt->second &&
                    ctxIt->second->isArray() && ctxIt->second->elemTy)
                    elemTy = ctxIt->second->elemTy;
                else
                    elemTy = exprType(*x.elems[0]);
            }
            int64_t N   = (int64_t)x.elems.size();
            auto arrTy  = Ty::Array(elemTy, N);

            std::string ptr = newTmp();
            int align = validAlign(elemTy ? llvmByteSize(elemTy) : 1);
            line("  " + ptr + " = alloca " + llvmTy(arrTy) +
                 ", align " + std::to_string(align));
            line("  store " + llvmTy(arrTy) + " zeroinitializer, ptr " + ptr);

            SubstMap savedSubst = currentSubst_;
            if (elemTy && !elemTy->isGeneric())
                currentSubst_["#ctx"] = elemTy;

            for (int64_t i = 0; i < N; ++i) {
                std::string val = emitExpr(*x.elems[i]);
                auto vty = exprType(*x.elems[i]);
                std::string ep = newTmp();
                line("  " + ep + " = getelementptr inbounds " + llvmTy(arrTy) +
                     ", ptr " + ptr + ", i32 0, i32 " + std::to_string(i));
                if (isAggTy(elemTy)) {
                    emitAggrCopy(ep, val, elemTy);
                } else {
                    val = implicitCoerce(val, vty, elemTy, *fnOut_, tmpIdx_, ptrBits_);
                    line("  store " + llvmTy(elemTy) + " " + val +
                         ", ptr " + ep + ", align " + std::to_string(align));
                }
            }

            currentSubst_ = savedSubst;
            return ptr;
        }

        if constexpr (std::is_same_v<T, StructInitExpr>) {
            auto ty = sema_.resolveType(*x.type, currentSubst_);
            if (!ty || !ty->isStruct()) {
                diag_.error(e.loc, "struct literal requires a concrete struct type", lineFor(e.loc));
                return "undef";
            }

            auto sit = sema_.structs().find(ty->name);
            if (sit == sema_.structs().end()) {
                diag_.error(e.loc, std::format("unknown struct '{}'", ty->name), lineFor(e.loc));
                return "undef";
            }
            auto& sinfo = *sit->second;

            SubstMap structSubst = currentSubst_;
            if (!sinfo.typeParams.empty() && !ty->typeArgs.empty())
                for (size_t i = 0; i < sinfo.typeParams.size() && i < ty->typeArgs.size(); ++i)
                    structSubst[sinfo.typeParams[i]] = ty->typeArgs[i];

            std::string llvmStructName = ty->name;
            if (!ty->typeArgs.empty())
                for (auto& ta : ty->typeArgs) llvmStructName += "_" + ta->mangle();

            std::unordered_map<std::string, bool> seen;

            std::string ptr = newTmp();
            bool initPacked = sinfo.packed;
            int align = validAlign(llvmByteSize(ty), initPacked);
            line("  " + ptr + " = alloca " + llvmTy(ty) + ", align " + std::to_string(align));
            line("  store " + llvmTy(ty) + " zeroinitializer, ptr " + ptr);

            for (auto& f : x.fields) {
                if (seen[f.name]) {
                    diag_.error(e.loc, std::format("duplicate field '{}'", f.name), lineFor(e.loc));
                    continue;
                }
                seen[f.name] = true;

                int idx = sinfo.fieldIndex(f.name);
                if (idx < 0) {
                    diag_.error(e.loc, std::format("unknown field '{}' in struct '{}'", f.name, ty->name), lineFor(e.loc));
                    continue;
                }

                auto fty = applySubst(sinfo.fieldTypes[idx], structSubst);

                std::string fp = newTmp();
                line("  " + fp + " = getelementptr inbounds %" + llvmStructName +
                    ", ptr " + ptr + ", i32 0, i32 " + std::to_string(idx));

                SubstMap savedSubst = currentSubst_;
                if (fty && !fty->isGeneric())
                    currentSubst_["#ctx"] = fty;

                std::string val = emitExpr(*f.value);
                auto vty = exprType(*f.value);

                currentSubst_ = savedSubst;

                val = implicitCoerce(val, vty, fty, *fnOut_, tmpIdx_, ptrBits_);

                if (isAggTy(fty)) {
                    emitAggrCopy(fp, val, fty);
                } else {
                    line("  store " + llvmTy(fty) + " " + val + ", ptr " + fp);
                }
            }

            return ptr;
        }

        if constexpr (std::is_same_v<T, CallExpr>)
            return emitCall(x, e.loc);

        if constexpr (std::is_same_v<T, FieldAccessExpr>)
            return emitFieldAccess(x, e.loc);

        if constexpr (std::is_same_v<T, IndexExpr>)
            return emitIndex(x, e.loc);

        if constexpr (std::is_same_v<T, EnumAccessExpr>) {
            auto eit = sema_.enums().find(x.enumName);
            if (eit == sema_.enums().end()) return "undef";

            int tag = eit->second->variantIndex(x.variant);

            std::vector<ResolvedTyPtr> enumTypeArgs;
            auto& einfo = *eit->second;
            if (!einfo.typeParams.empty()) {
                SubstMap inferredSubst;

                if (tag >= 0) {
                    const EnumDecl* ed = nullptr;
                    for (auto& d : prog_.enums)
                        if (d->name == x.enumName) { ed = d.get(); break; }
                    if (ed && tag < (int)ed->variants.size()) {
                        auto& vd = ed->variants[tag];
                        for (size_t ai = 0;
                             ai < x.args.size() && ai < vd.payload.size(); ++ai) {
                            auto payTy = sema_.resolveType(*vd.payload[ai]);
                            auto argTy = exprType(*x.args[ai]);
                            if (payTy && payTy->isGeneric() && argTy && !argTy->isGeneric())
                                inferredSubst[payTy->genericName] = argTy;
                        }
                    }
                }

                if (curRetTy_ && curRetTy_->isEnum() &&
                    curRetTy_->name == x.enumName) {
                    for (size_t i = 0;
                         i < einfo.typeParams.size() &&
                         i < curRetTy_->typeArgs.size(); ++i) {
                        if (!inferredSubst.count(einfo.typeParams[i]))
                            inferredSubst[einfo.typeParams[i]] = curRetTy_->typeArgs[i];
                    }
                }

                {
                    auto hctxIt = currentSubst_.find("#ctx");
                    if (hctxIt != currentSubst_.end() &&
                        hctxIt->second && hctxIt->second->isEnum() &&
                        hctxIt->second->name == x.enumName) {
                        for (size_t i = 0;
                             i < einfo.typeParams.size() &&
                             i < hctxIt->second->typeArgs.size(); ++i) {
                            inferredSubst[einfo.typeParams[i]] =
                                hctxIt->second->typeArgs[i];
                        }
                    }
                }

                for (auto& tp : einfo.typeParams) {
                    if (!inferredSubst.count(tp)) {
                        auto sit = currentSubst_.find(tp);
                        if (sit != currentSubst_.end() && !sit->second->isGeneric())
                            inferredSubst[tp] = sit->second;
                    }
                }

                for (auto& tp : einfo.typeParams) {
                    auto sit = inferredSubst.find(tp);
                    if (sit != inferredSubst.end())
                        enumTypeArgs.push_back(sit->second);
                }
            }

            std::string tyName = enumTypeName(x.enumName, enumTypeArgs);
            if (!enumTypeArgs.empty())
                emitEnumType(x.enumName, enumTypeArgs, currentSubst_);

            std::string ptr = newTmp() + ".enum";
            line("  " + ptr + " = alloca %" + tyName);

            std::string tagPtr = newTmp();
            line("  " + tagPtr + " = getelementptr inbounds %" + tyName +
                 ", ptr " + ptr + ", i32 0, i32 0");
            line("  store i32 " + std::to_string(tag) + ", ptr " + tagPtr);

            if (!x.args.empty()) {
                const EnumDecl* ed = nullptr;
                for (auto& d : prog_.enums)
                    if (d->name == x.enumName) { ed = d.get(); break; }

                int byteOffset = enumPayloadPad(x.enumName, enumTypeArgs, currentSubst_);
                SubstMap enumSubst = currentSubst_;
                for (size_t i = 0; i < einfo.typeParams.size() && i < enumTypeArgs.size(); ++i)
                    enumSubst[einfo.typeParams[i]] = enumTypeArgs[i];

                for (size_t ai = 0; ai < x.args.size(); ++ai) {
                    ResolvedTyPtr payloadTy;
                    if (ed && tag >= 0 && tag < (int)ed->variants.size() &&
                        ai < ed->variants[tag].payload.size()) {
                        auto declared = sema_.resolveType(
                            *ed->variants[tag].payload[ai], enumSubst);
                        if (declared && !declared->isGeneric())
                            payloadTy = declared;
                    }

                    SubstMap savedSubst = currentSubst_;
                    if (payloadTy)
                        currentSubst_["#ctx"] = payloadTy;

                    std::string argVal = emitExpr(*x.args[ai]);
                    auto argTy = payloadTy ? payloadTy : exprType(*x.args[ai]);

                    currentSubst_ = savedSubst;

                    if (ed && tag >= 0 && tag < (int)ed->variants.size() &&
                        ai < ed->variants[tag].payload.size()) {
                        auto declTy = sema_.resolveType(
                            *ed->variants[tag].payload[ai], enumSubst);
                        if (declTy && !declTy->isGeneric()) argTy = declTy;
                    }

                    std::string payloadBase = newTmp();
                    line("  " + payloadBase +
                         " = getelementptr inbounds %" + tyName +
                         ", ptr " + ptr + ", i32 0, i32 1");
                    std::string elemPtr = newTmp();
                    line("  " + elemPtr +
                         " = getelementptr inbounds i8, ptr " + payloadBase +
                         ", i32 " + std::to_string(byteOffset));
                    if (isAggTy(argTy)) {
                        emitAggrCopy(elemPtr, argVal, argTy);
                    } else {
                        line("  store " + llvmTy(argTy) + " " + argVal +
                             ", ptr " + elemPtr);
                    }
                    byteOffset += argTy ? llvmByteSize(argTy) : 8;
                }
            }
            return ptr;
        }

        if constexpr (std::is_same_v<T, ReadonlyExpr>)  return emitReadonly(x);
        if constexpr (std::is_same_v<T, TakeExpr>)      return emitTake(x);
        if constexpr (std::is_same_v<T, ReleaseExpr>)   return emitRelease(x);

        if constexpr (std::is_same_v<T, CheckExpr>)     return emitExpr(*x.operand);
        if constexpr (std::is_same_v<T, ClaimExpr>)     return emitExpr(*x.operand);

        if constexpr (std::is_same_v<T, MatchExpr>)     return emitMatch(x, e.loc);

        if constexpr (std::is_same_v<T, TypeswitchExpr>) return emitTypeswitch(x, e.loc);

        if constexpr (std::is_same_v<T, TernaryExpr>)   return emitTernary(x, e.loc);

        return "undef";
    }, e.v);
}

std::string CodeGen::emitLValuePtr(const Expr& e, const SourceLoc& loc) {
    if (e.template is<IdentExpr>()) {
        const std::string& name = e.template as<IdentExpr>().name;
        auto ait = varAlloca_.find(name);
        if (ait != varAlloca_.end()) return ait->second;
        if (isGlobalSymbol(name)) return gsym(name);
        if (permissive_ && !sema_.funcs().count(name)) {
            implicitExternGlobal(name);
            return "@" + name;
        }
    }

    if (e.template is<DerefExpr>()) {
        auto& operand = *e.template as<DerefExpr>().operand;
        auto operandTy = exprType(operand);
        if (!operandTy || (!operandTy->isPtr() && !operandTy->isStr())) {
            diag_.error(loc,
                std::format("cannot dereference expression of type '{}': "
                            "only pointers can be dereferenced, "
                            "did you mean to write 'expr.field' instead of '*expr.field'?",
                            operandTy ? operandTy->str() : "?"),
                lineFor(loc));
            return "undef";
        }
        return emitExpr(operand);
    }

    if (e.template is<IndexExpr>()) {
        auto& ix = e.template as<IndexExpr>();
        auto arrTy = exprType(*ix.arr);

        if (ix.arr->template is<IdentExpr>()) {
            const std::string& varName = ix.arr->template as<IdentExpr>().name;
            std::string basePtr;
            auto ait = varAlloca_.find(varName);
            if (ait != varAlloca_.end()) basePtr = ait->second;
            else {
                if (isGlobalSymbol(varName)) basePtr = gsym(varName);
            }
            if (!basePtr.empty()) {
                auto idxTy  = exprType(*ix.idx);
                std::string idxVal = emitExpr(*ix.idx);
                const std::string ptrIntTy = (ptrBits_ == 32) ? "i32" : "i64";
                if (idxTy && idxTy->isInteger() && idxTy->bitWidth(ptrBits_) != ptrBits_) {
                    std::string coerced = newTmp();
                    std::string ext = idxTy->isSigned() ? "sext" : "zext";
                    line("  " + coerced + " = " + ext + " " + llvmTy(idxTy) +
                         " " + idxVal + " to " + ptrIntTy);
                    idxVal = coerced;
                }
                const std::string idxLLTy = ptrIntTy;
                std::string ep = newTmp();
                auto elemTy = (arrTy && arrTy->isArray()) ? arrTy->elemTy
                            : (arrTy && arrTy->isPtr())   ? (arrTy->inner ? arrTy->inner : Ty::I8())
                            : (arrTy && arrTy->isStr())   ? Ty::I8()
                            : nullptr;
                if (arrTy && arrTy->isArray()) {
                    line("  " + ep + " = getelementptr inbounds " +
                         llvmTy(arrTy) + ", ptr " + basePtr +
                         ", i32 0, " + idxLLTy + " " + idxVal);
                } else if (elemTy) {
                    std::string loadedPtr = newTmp();
                    line("  " + loadedPtr + " = load ptr, ptr " + basePtr);
                    line("  " + ep + " = getelementptr inbounds " +
                         llvmTy(elemTy) + ", ptr " + loadedPtr +
                         ", " + idxLLTy + " " + idxVal);
                }
                return ep;
            }
        }
    }

    if (e.template is<FieldAccessExpr>()) {
        auto& fa = e.template as<FieldAccessExpr>();
        auto oty = exprType(*fa.obj);
        int derefLevels = 0;
        while (oty && oty->isPtr() && oty->inner) {
            oty = oty->inner;
            ++derefLevels;
        }
        bool autoDeref = derefLevels > 0;
        if (oty && oty->isStruct()) {
            std::string basePtr;
            if (fa.obj->template is<IdentExpr>()) {
                const std::string& objName = fa.obj->template as<IdentExpr>().name;
                auto ait = varAlloca_.find(objName);
                std::string startPtr;
                if (ait != varAlloca_.end()) startPtr = ait->second;
                else if (isGlobalSymbol(objName)) startPtr = gsym(objName);

                if (!startPtr.empty()) {
                    if (autoDeref) {
                        std::string cur = startPtr;
                        for (int i = 0; i < derefLevels; ++i) {
                            std::string loaded = newTmp();
                            line("  " + loaded + " = load ptr, ptr " + cur);
                            cur = loaded;
                        }
                        basePtr = cur;
                    } else {
                        basePtr = startPtr;
                    }
                }
            }
            if (basePtr.empty()) {
                basePtr = emitExpr(*fa.obj);
                for (int i = 0; i < derefLevels - 1; ++i) {
                    std::string loaded = newTmp();
                    line("  " + loaded + " = load ptr, ptr " + basePtr);
                    basePtr = loaded;
                }
            }

            if (!basePtr.empty() && basePtr != "undef") {
                auto sit = sema_.structs().find(oty->name);
                if (sit != sema_.structs().end()) {
                    int idx = sit->second->fieldIndex(fa.field);
                    if (idx >= 0) {
                        std::string fp = newTmp();
                        line("  " + fp + " = getelementptr inbounds %" + oty->name +
                             ", ptr " + basePtr +
                             ", i32 0, i32 " + std::to_string(idx));
                        return fp;
                    }
                }
            }
        }
    }

    diag_.error(loc, "expression is not assignable", lineFor(loc));
    return "undef";
}

std::string CodeGen::emitAssign(const AssignExpr& a, const SourceLoc& loc) {
    ResolvedTyPtr ctxFloatTy;
    {
        auto lhsTy = exprType(*a.lhs);
        if (lhsTy && !lhsTy->isGeneric() && !lhsTy->isVoid() &&
            (lhsTy->isInteger() || lhsTy->isFloat())) {
            currentSubst_["#ctx"] = lhsTy;
            if (lhsTy->isFloat()) ctxFloatTy = lhsTy;
        }
    }
    std::string rval = emitExpr(*a.rhs);
    currentSubst_.erase("#ctx");
    auto rty = exprType(*a.rhs);
    if (ctxFloatTy && isPureIntLiteral(*a.rhs)) rty = ctxFloatTy;
    ResolvedTyPtr rvalTy = rty;

    if (a.op != "=") {
        std::string lval = emitExpr(*a.lhs);
        auto lhsTy = exprType(*a.lhs);
        auto opTy  = (lhsTy && !lhsTy->isVoid()) ? lhsTy : rty;
        std::string tmp  = newTmp();
        std::string opStr = a.op.substr(0, a.op.size() - 1);

        if (opTy && (opTy->isPtr() || opTy->isStr()) && rty &&
            (rty->isInteger() || rty->isPtr() || rty->isStr()) &&
            (opStr == "+" || opStr == "-")) {
            std::string idx = rval;
            const std::string pArrIntTy = (ptrBits_ == 32) ? "i32" : "i64";
            if (rty->isPtr() || rty->isStr()) {
                std::string asInt = newTmp();
                line("  " + asInt + " = ptrtoint ptr " + idx + " to " + pArrIntTy);
                idx = asInt;
            } else if (rty->bitWidth(ptrBits_) != ptrBits_) {
                std::string c = newTmp();
                line("  " + c + " = " + (rty->isSigned() ? "sext" : "zext") +
                     " " + llvmTy(rty) + " " + idx + " to " + pArrIntTy);
                idx = c;
            }
            if (opStr == "-") {
                std::string neg = newTmp();
                line("  " + neg + " = sub " + pArrIntTy + " 0, " + idx);
                idx = neg;
            }
            ResolvedTyPtr elemTy = (opTy->isStr()) ? Ty::I8()
                                 : (opTy->inner    ? opTy->inner : Ty::I8());
            std::string elemLLTy = elemTy->isVoid() ? "i8" : llvmTy(elemTy);
            std::string newPtr = newTmp();
            line("  " + newPtr + " = getelementptr inbounds " + elemLLTy +
                 ", ptr " + lval + ", " + pArrIntTy + " " + idx);
            std::string lptr = emitLValuePtr(*a.lhs, loc);
            line("  store ptr " + newPtr + ", ptr " + lptr);
            return newPtr;
        }

        if (opTy && rty && opTy->isInteger() && rty->isInteger() &&
            opTy->bitWidth(ptrBits_) != rty->bitWidth(ptrBits_)) {
            std::string coerced = newTmp();
            if (opTy->bitWidth(ptrBits_) > rty->bitWidth(ptrBits_)) {
                std::string ext = rty->isSigned() ? "sext" : "zext";
                line("  " + coerced + " = " + ext + " " + llvmTy(rty) + " " + rval + " to " + llvmTy(opTy));
            } else {
                line("  " + coerced + " = trunc " + llvmTy(rty) + " " + rval + " to " + llvmTy(opTy));
            }
            rval = coerced;
        }

        if (opTy && rty && opTy->isFloat() && rty->isFloat() &&
            opTy->byteSize(ptrBits_) != rty->byteSize(ptrBits_)) {
            std::string coerced = newTmp();
            if (opTy->byteSize(ptrBits_) < rty->byteSize(ptrBits_))
                line("  " + coerced + " = fptrunc " + llvmTy(rty) + " " + rval + " to " + llvmTy(opTy));
            else
                line("  " + coerced + " = fpext " + llvmTy(rty) + " " + rval + " to " + llvmTy(opTy));
            rval = coerced;
        }

        if (opTy && opTy->isFloat()) {
            std::string irop;
            if      (opStr == "+") irop = "fadd";
            else if (opStr == "-") irop = "fsub";
            else if (opStr == "*") irop = "fmul";
            else if (opStr == "/") irop = "fdiv";
            else irop = "fadd";
            line("  " + tmp + " = " + irop + " " + llvmTy(opTy) + " " + lval + ", " + rval);
        } else {
            std::string irop;
            if      (opStr == "+") irop = "add";
            else if (opStr == "-") irop = "sub";
            else if (opStr == "*") irop = "mul";
            else if (opStr == "/") irop = opTy && opTy->isUnsigned() ? "udiv" : "sdiv";
            else if (opStr == "%") irop = opTy && opTy->isUnsigned() ? "urem" : "srem";
            else if (opStr == "&") irop = "and";
            else if (opStr == "|") irop = "or";
            else if (opStr == "^") irop = "xor";
            else if (opStr == "<<") irop = "shl";
            else if (opStr == ">>") irop = opTy && opTy->isUnsigned() ? "lshr" : "ashr";
            else irop = "add";
            line("  " + tmp + " = " + irop + " " + llvmTy(opTy) + " " + lval + ", " + rval);
        }
        rval = tmp;
        rvalTy = opTy;
    }

    std::string lptr = emitLValuePtr(*a.lhs, loc);
    auto lty = exprType(*a.lhs);
    if (!lty) lty = rvalTy;

    auto alreadyTrackedOwned = [&](const std::string& n) {
        for (auto& frame : dropScopes_)
            for (auto& de : frame)
                if (de.varName == n && de.kind == DropKind::OwnedStr)
                    return true;
        return false;
    };

    bool rhsIsOwnedInit = isOwnedInit(a.rhs.get());
    bool rhsIsMovedIdent = false;
    std::string rhsIdentName;
    if (a.rhs->template is<IdentExpr>()) {
        rhsIdentName = a.rhs->template as<IdentExpr>().name;
        rhsIsMovedIdent = alreadyTrackedOwned(rhsIdentName);
        if (rhsIsMovedIdent && a.lhs->template is<IdentExpr>() &&
            rhsIdentName == a.lhs->template as<IdentExpr>().name) {
            rhsIsMovedIdent = false;
        }
    }
    bool rhsIsOwned = rhsIsOwnedInit || rhsIsMovedIdent;
    bool lhsWasOwned = a.lhs->template is<IdentExpr>() &&
        alreadyTrackedOwned(a.lhs->template as<IdentExpr>().name);

    if (a.op == "=" && lty && (lty->isStr() || lty->isPtr()) && rhsIsOwned) {
        if (a.lhs->template is<IdentExpr>()) {
            const std::string& name = a.lhs->template as<IdentExpr>().name;
            bool isGlobalVar = isGlobalSymbol(name);

            if (!isGlobalVar && !alreadyTrackedOwned(name))
                addToDropScope(name, lty, DropKind::OwnedStr);

            if (rhsIsMovedIdent) {
                for (auto& frame : dropScopes_) {
                    frame.erase(
                        std::remove_if(frame.begin(), frame.end(),
                            [&](const DropEntry& de) {
                                return de.varName == rhsIdentName &&
                                       de.kind == DropKind::OwnedStr;
                            }),
                        frame.end());
                }
                auto rhsAlloca = varAlloca_.find(rhsIdentName);
                if (rhsAlloca != varAlloca_.end()) {
                    line("  store ptr null, ptr " + rhsAlloca->second + ", align 8");
                }
            }
        }
    }

    auto rhsIsCtxSensitiveFloatExpr = [&]() {
        return a.rhs->template is<FloatLitExpr>() ||
               a.rhs->template is<IntLitExpr>()   ||
               a.rhs->template is<UnaryOpExpr>()  ||
               a.rhs->template is<BinOpExpr>();
    };
    if (lty && rvalTy && lty->isFloat() && rvalTy->isFloat() &&
        !lty->equals(*rvalTy) && rhsIsCtxSensitiveFloatExpr()) {
        rvalTy = lty;
    }

    if (lty && rvalTy && !lty->equals(*rvalTy))
        rval = implicitCoerce(rval, rvalTy, lty, *fnOut_, tmpIdx_, ptrBits_);

    if (a.lhs->template is<IdentExpr>()) {
        const std::string& name = a.lhs->template as<IdentExpr>().name;
        storeVar(name, rval, lty, rhsIsOwned, lhsWasOwned || isGlobalSymbol(name));
    } else if (isAggTy(lty)) {
        emitAggrCopy(lptr, rval, lty);
    } else {
        line("  store " + llvmTy(lty) + " " + rval + ", ptr " + lptr);
    }

    return rval;
}

std::string CodeGen::emitBinOp(const BinOpExpr& b, const SourceLoc& loc) {
    if (b.op == "&&" || b.op == "||") {
        std::string lv    = emitExpr(*b.lhs);
        std::string rhsLbl = newLabel();
        std::string endLbl = newLabel();
        std::string resPtr = newTmp() + ".sc";
        line("  " + resPtr + " = alloca i1");

        std::string cond1 = newTmp();
        auto lty = exprType(*b.lhs);
        if (lty && lty->kind != TyKind::Bool)
            line("  " + cond1 + " = icmp ne " + llvmTy(lty) + " " + lv + ", 0");
        else
            cond1 = lv;

        if (b.op == "&&") {
            line("  store i1 false, ptr " + resPtr);
            line("  br i1 " + cond1 + ", label %" + rhsLbl + ", label %" + endLbl);
        } else {
            line("  store i1 true, ptr " + resPtr);
            line("  br i1 " + cond1 + ", label %" + endLbl + ", label %" + rhsLbl);
        }

        line(rhsLbl + ":");
        std::string rv = emitExpr(*b.rhs);
        auto rty = exprType(*b.rhs);
        std::string cond2 = newTmp();
        if (rty && rty->kind != TyKind::Bool)
            line("  " + cond2 + " = icmp ne " + llvmTy(rty) + " " + rv + ", 0");
        else
            cond2 = rv;
        line("  store i1 " + cond2 + ", ptr " + resPtr);
        line("  br label %" + endLbl);

        line(endLbl + ":");
        std::string res = newTmp();
        line("  " + res + " = load i1, ptr " + resPtr);
        return res;
    }

    auto ctxIt = currentSubst_.find("#ctx");
    ResolvedTyPtr ctxTyBin = (ctxIt != currentSubst_.end()) ? ctxIt->second : nullptr;

    std::string lv = emitExpr(*b.lhs);
    auto lty = exprType(*b.lhs);
    auto lty_emitted = lty;
    if (b.lhs->template is<IntLitExpr>()) {
        auto ctxIt2 = currentSubst_.find("#ctx");
        if (ctxIt2 != currentSubst_.end() && ctxIt2->second && ctxIt2->second->isFloat()) {
            lty         = ctxIt2->second;
            lty_emitted = ctxIt2->second;
        }
    }
    if (b.lhs->template is<BinOpExpr>()) {
        if (ctxTyBin && lty_emitted && lty_emitted->isInteger() && ctxTyBin->isInteger() &&
            ctxTyBin->bitWidth(ptrBits_) > lty_emitted->bitWidth(ptrBits_)) {
            lty_emitted = ctxTyBin;
        }
        if (ctxTyBin && lty_emitted && lty_emitted->isFloat() && ctxTyBin->isFloat() &&
            ctxTyBin->byteSize(ptrBits_) > lty_emitted->byteSize(ptrBits_)) {
            lty_emitted = ctxTyBin;
        }
    }

    if (ctxTyBin && lty && lty->isInteger() && ctxTyBin->isInteger() &&
        ctxTyBin->bitWidth(ptrBits_) > lty->bitWidth(ptrBits_)) {
        lty = ctxTyBin;
    }
    if (ctxTyBin && lty && lty->isFloat() && ctxTyBin->isFloat() &&
        ctxTyBin->byteSize(ptrBits_) > lty->byteSize(ptrBits_)) {
        lty = ctxTyBin;
    }

    SubstMap savedBin = currentSubst_;
    if (lty && (lty->isInteger() || lty->isFloat()))
        currentSubst_["#ctx"] = lty;
    std::string rv = emitExpr(*b.rhs);
    auto rty = exprType(*b.rhs);
    auto rty_emitted = rty;
    if (b.rhs->template is<BinOpExpr>()) {
        if (lty && rty_emitted && rty_emitted->isInteger() && lty->isInteger() &&
            lty->bitWidth(ptrBits_) > rty_emitted->bitWidth(ptrBits_)) {
            rty_emitted = lty;
        }
        if (lty && rty_emitted && rty_emitted->isFloat() && lty->isFloat() &&
            lty->byteSize(ptrBits_) > rty_emitted->byteSize(ptrBits_)) {
            rty_emitted = lty;
        }
    }
    if (b.rhs->template is<IntLitExpr>()) {
        auto ctxIt2 = currentSubst_.find("#ctx");
        if (ctxIt2 != currentSubst_.end() && ctxIt2->second && ctxIt2->second->isFloat()) {
            rty         = ctxIt2->second;
            rty_emitted = ctxIt2->second;
        }
    }
    currentSubst_ = savedBin;

    if (b.op == "+" && lty && lty->isStr()) {
        auto rtyCheck = exprType(*b.rhs);
        if (!rtyCheck || !rtyCheck->isStr()) {
            diag_.error(loc,
                std::format("cannot concatenate string with '{}'",
                            rtyCheck ? rtyCheck->str() : "?"),
                lineFor(loc));
            return "undef";
        }

        auto isTransientStr = [&](const Expr& expr) -> bool {
            if (expr.template is<BinOpExpr>()) {
                auto& inner = expr.template as<BinOpExpr>();
                if (inner.op != "+") return false;
                auto lt2 = exprType(*inner.lhs);
                return lt2 && lt2->isStr();
            }
            if (expr.template is<CallExpr>()) {
                auto ty2 = exprType(expr);
                if (!ty2 || !ty2->isStr()) return false;
                auto& c2 = expr.template as<CallExpr>();
                if (!c2.callee->template is<IdentExpr>()) return false;
                const std::string& fn =
                    c2.callee->template as<IdentExpr>().name;
                if (funcAlwaysAllocatesStr(fn)) return true;
                for (auto& arg : c2.args) {
                    if (!arg->template is<CallExpr>()) continue;
                    auto argTy = exprType(*arg);
                    if (argTy && argTy->isEnum() &&
                        enumHasDroppablePayload(argTy->name, argTy->typeArgs))
                        return true;
                }
                return false;
            }
            return false;
        };

        std::string result = emitStringConcat(lv, rv);
        if (isTransientStr(*b.lhs)) emitStrFree(lv);
        if (isTransientStr(*b.rhs)) emitStrFree(rv);
        return result;
    }

    if (lty && lty->isPtr()) {
        const std::string pBinIntTy = (ptrBits_ == 32) ? "i32" : "i64";
        auto coerceIdxI64 = [&](const std::string& val,
                                 const ResolvedTyPtr& vty) -> std::string {
            if (!vty || !vty->isInteger() || vty->bitWidth(ptrBits_) == ptrBits_) return val;
            std::string c = newTmp();
            line("  " + c + " = " + (vty->isSigned() ? "sext" : "zext") +
                 " " + llvmTy(vty) + " " + val + " to " + pBinIntTy);
            return c;
        };
        ResolvedTyPtr elemTy = lty->inner ? lty->inner : Ty::I8();
        std::string elemLLTy = elemTy->isVoid() ? "i8" : llvmTy(elemTy);

        if ((b.op == "+" || b.op == "-") && rty && rty->isInteger()) {
            std::string idx = coerceIdxI64(rv, rty);
            if (b.op == "-") {
                std::string neg = newTmp();
                line("  " + neg + " = sub i64 0, " + idx);
                idx = neg;
            }
            std::string res = newTmp();
            line("  " + res + " = getelementptr inbounds " + elemLLTy +
                 ", ptr " + lv + ", i64 " + idx);
            return res;
        }
        if (b.op == "-" && rty && rty->isPtr()) {
            std::string li = newTmp(), ri2 = newTmp(), diff = newTmp();
            line("  " + li  + " = ptrtoint ptr " + lv + " to i64");
            line("  " + ri2 + " = ptrtoint ptr " + rv + " to i64");
            line("  " + diff + " = sub i64 " + li + ", " + ri2);
            int esz = elemTy->isVoid() ? 1 : llvmByteSize(elemTy);
            if (esz > 1) {
                std::string quot = newTmp();
                line("  " + quot + " = sdiv i64 " + diff + ", " + std::to_string(esz));
                return quot;
            }
            return diff;
        }
        if ((b.op == "<" || b.op == ">" || b.op == "<=" || b.op == ">=")
            && rty && rty->isPtr()) {
            std::string li = newTmp(), ri2 = newTmp(), res = newTmp();
            line("  " + li  + " = ptrtoint ptr " + lv + " to i64");
            line("  " + ri2 + " = ptrtoint ptr " + rv + " to i64");
            std::string pred = (b.op == "<")  ? "ult" :
                               (b.op == ">")  ? "ugt" :
                               (b.op == "<=") ? "ule" : "uge";
            line("  " + res + " = icmp " + pred + " i64 " + li + ", " + ri2);
            return res;
        }
    }

    ResolvedTyPtr ty = lty;
    if (!ty || ty->isVoid()) ty = rty;
    if (lty && rty && lty->isInteger() && rty->isInteger() &&
        lty->bitWidth(ptrBits_) != rty->bitWidth(ptrBits_)) {
        ty = (lty->bitWidth(ptrBits_) >= rty->bitWidth(ptrBits_)) ? lty : rty;
    }
    if (lty && rty && lty->isFloat() && rty->isFloat() &&
        lty->byteSize(ptrBits_) != rty->byteSize(ptrBits_)) {
        ty = (lty->byteSize(ptrBits_) >= rty->byteSize(ptrBits_)) ? lty : rty;
    }
    if (lty && rty && lty->isFloat()   && rty->isInteger()) ty = lty;
    if (lty && rty && lty->isInteger() && rty->isFloat())   ty = rty;
    bool flt = ty && ty->isFloat();
    bool uns = ty && ty->isUnsigned();
    bool i1  = ty && ty->kind == TyKind::Bool;

    std::string tmp = newTmp();
    std::string llt = llvmTy(ty);

    auto coerceToTy = [&](std::string& val, const ResolvedTyPtr& vty) {
        if (!vty || !ty) return;
        if (vty->equals(*ty)) return;
        std::string srcTy = llvmTy(vty);
        if (srcTy == llt) return;
        std::string coerced = newTmp();
        if (vty->isInteger() && ty->isInteger()) {
            int sw = vty->bitWidth(ptrBits_), dw = ty->bitWidth(ptrBits_);
            if (dw > sw) {
                std::string ext = vty->isSigned() ? "sext" : "zext";
                line("  " + coerced + " = " + ext + " " + srcTy + " " + val + " to " + llt);
            } else {
                line("  " + coerced + " = trunc " + srcTy + " " + val + " to " + llt);
            }
            val = coerced;
        } else if (vty->isFloat() && ty->isFloat()) {
            if (vty->byteSize(ptrBits_) < ty->byteSize(ptrBits_))
                line("  " + coerced + " = fpext " + srcTy + " " + val + " to " + llt);
            else
                line("  " + coerced + " = fptrunc " + srcTy + " " + val + " to " + llt);
            val = coerced;
        } else if (vty->isInteger() && ty->isFloat()) {
            std::string cvt = vty->isSigned() ? "sitofp" : "uitofp";
            line("  " + coerced + " = " + cvt + " " + srcTy + " " + val + " to " + llt);
            val = coerced;
        } else if (vty->isFloat() && ty->isInteger()) {
            std::string cvt = ty->isSigned() ? "fptosi" : "fptoui";
            line("  " + coerced + " = " + cvt + " " + srcTy + " " + val + " to " + llt);
            val = coerced;
        }
    };
    coerceToTy(lv, lty_emitted);
    coerceToTy(rv, rty_emitted);

    auto cmpInst = [&](const std::string& signed_pred,
                       const std::string& unsigned_pred,
                       const std::string& float_pred) {
        if (flt)
            line("  " + tmp + " = fcmp " + float_pred + " " + llt + " " + lv + ", " + rv);
        else if (uns || i1)
            line("  " + tmp + " = icmp " + unsigned_pred + " " + llt + " " + lv + ", " + rv);
        else
            line("  " + tmp + " = icmp " + signed_pred + " " + llt + " " + lv + ", " + rv);
    };

    if      (b.op == "+")  { line("  " + tmp + " = " + (flt?"fadd":"add") + " " + llt + " " + lv + ", " + rv); }
    else if (b.op == "-")  { line("  " + tmp + " = " + (flt?"fsub":"sub") + " " + llt + " " + lv + ", " + rv); }
    else if (b.op == "*")  { line("  " + tmp + " = " + (flt?"fmul":"mul") + " " + llt + " " + lv + ", " + rv); }
    else if (b.op == "/")  { line("  " + tmp + " = " + (flt?"fdiv":uns?"udiv":"sdiv") + " " + llt + " " + lv + ", " + rv); }
    else if (b.op == "%")  { line("  " + tmp + " = " + (flt?"frem":uns?"urem":"srem") + " " + llt + " " + lv + ", " + rv); }
    else if (b.op == "&")  { line("  " + tmp + " = and " + llt + " " + lv + ", " + rv); }
    else if (b.op == "|")  { line("  " + tmp + " = or "  + llt + " " + lv + ", " + rv); }
    else if (b.op == "^")  { line("  " + tmp + " = xor " + llt + " " + lv + ", " + rv); }
    else if (b.op == "<<") { line("  " + tmp + " = shl " + llt + " " + lv + ", " + rv); }
    else if (b.op == ">>") { line("  " + tmp + " = " + (uns?"lshr":"ashr") + " " + llt + " " + lv + ", " + rv); }
    else if (b.op == "==") { cmpInst("eq","eq","oeq"); }
    else if (b.op == "!=") { cmpInst("ne","ne","one"); }
    else if (b.op == "<")  { cmpInst("slt","ult","olt"); }
    else if (b.op == ">")  { cmpInst("sgt","ugt","ogt"); }
    else if (b.op == "<=") { cmpInst("sle","ule","ole"); }
    else if (b.op == ">=") { cmpInst("sge","uge","oge"); }
    else {
        diag_.error(loc,
            std::format("unsupported binary operator '{}' for operand types '{}' and '{}'",
                        b.op,
                        lty ? lty->str() : "?",
                        rty ? rty->str() : "?"),
            lineFor(loc));
        line("  " + tmp + " = add " + llt + " " + lv + ", " + rv);
    }

    return tmp;
}

std::string CodeGen::emitStringConcat(const std::string& lv, const std::string& rv) {
    const std::string szTy = is32bit() ? "i32" : "i64";
    std::string ll = newTmp();
    std::string rl = newTmp();
    line("  " + ll + " = call " + szTy + " @" + rtSym("strlen") + "(ptr " + lv + ")");
    line("  " + rl + " = call " + szTy + " @" + rtSym("strlen") + "(ptr " + rv + ")");
    std::string tot = newTmp();
    line("  " + tot + " = add " + szTy + " " + ll + ", " + rl);
    std::string tot1 = newTmp();
    line("  " + tot1 + " = add " + szTy + " " + tot + ", 1");
    std::string buf = newTmp();
    line("  " + buf + " = call ptr @" + rtSym("alloc") + "(" + szTy + " " + tot1 + ")");
    line("  call void @" + rtSym("strcpy") + "(ptr " + buf + ", ptr " + lv + ")");
    line("  call void @" + rtSym("strcat") + "(ptr " + buf + ", ptr " + rv + ")");
    return buf;
}

std::string CodeGen::emitCast(const CastExpr& c) {
    auto toTy   = sema_.resolveType(*c.type, currentSubst_);

    if (toTy && toTy->isInteger() && toTy->kind != TyKind::Bool) {
        int64_t lit;
        if (foldIntLiteral(*c.operand, lit)) {
            int bw = toTy->bitWidth(ptrBits_);
            if (bw > 0 && bw < 64) {
                uint64_t mask = (1ULL << bw) - 1;
                uint64_t u = (uint64_t)lit & mask;
                if (toTy->isSigned() && (u >> (bw - 1)))
                    lit = (int64_t)u - (int64_t)(1ULL << bw);
                else
                    lit = (int64_t)u;
            }
            return std::to_string(lit);
        }
    }

    if (toTy && (toTy->isPtr() || toTy->isStr())) {
        int64_t lit;
        if (foldIntLiteral(*c.operand, lit)) {
            if (lit == 0) return "null";
            const std::string pit = (ptrBits_ == 32) ? "i32" : "i64";
            return "inttoptr (" + pit + " " + std::to_string(lit) + " to ptr)";
        }
    }

    SubstMap savedCast = currentSubst_;
    if (toTy && (toTy->isInteger() || toTy->isFloat()))
        currentSubst_["#ctx"] = toTy;
    auto fromTy = exprType(*c.operand);
    std::string v = emitExpr(*c.operand);
    currentSubst_ = savedCast;

    if (!fromTy || !toTy) return v;
    if (fromTy->equals(*toTy)) return v;

    std::string fll = llvmTy(fromTy);
    std::string tll = llvmTy(toTy);

    if (fll == tll) return v;

    bool fromFlt = fromTy->isFloat();
    bool toFlt   = toTy->isFloat();
    bool fromInt = fromTy->isInteger();
    bool toInt   = toTy->isInteger();
    bool fromPtr = fromTy->isPtr() || fromTy->isStr();
    bool toPtr   = toTy->isPtr()   || toTy->isStr();
    int fromW    = fromTy->bitWidth(ptrBits_);
    int toW      = toTy->bitWidth(ptrBits_);

    std::string tmp = newTmp();

    if (toTy->kind == TyKind::Bool && (fromInt || fromFlt || fromPtr)) {
        if (fromFlt)
            line("  " + tmp + " = fcmp une " + fll + " " + v + ", 0.0");
        else if (fromPtr)
            line("  " + tmp + " = icmp ne ptr " + v + ", null");
        else
            line("  " + tmp + " = icmp ne " + fll + " " + v + ", 0");
    } else if (fromInt && toInt) {
        if (toW > fromW)
            line("  " + tmp + " = " + (fromTy->isSigned()?"sext":"zext") +
                 " " + fll + " " + v + " to " + tll);
        else if (toW < fromW)
            line("  " + tmp + " = trunc " + fll + " " + v + " to " + tll);
        else
            tmp = v;
    } else if (fromFlt && toInt) {
        line("  " + tmp + " = " + (toTy->isSigned()?"fptosi":"fptoui") +
             " " + fll + " " + v + " to " + tll);
    } else if (fromInt && toFlt) {
        line("  " + tmp + " = " + (fromTy->isSigned()?"sitofp":"uitofp") +
             " " + fll + " " + v + " to " + tll);
    } else if (fromFlt && toFlt) {
        if (toTy->byteSize(ptrBits_) > fromTy->byteSize(ptrBits_))
            line("  " + tmp + " = fpext " + fll + " " + v + " to " + tll);
        else
            line("  " + tmp + " = fptrunc " + fll + " " + v + " to " + tll);
    } else {
        const std::string ptrIntTy = (ptrBits_ == 32) ? "i32" : "i64";
        if (fromPtr && toInt) {
            if (toW == ptrBits_) {
                line("  " + tmp + " = ptrtoint ptr " + v + " to " + ptrIntTy);
            } else {
                std::string wide = newTmp();
                line("  " + wide + " = ptrtoint ptr " + v + " to " + ptrIntTy);
                line("  " + tmp + " = trunc " + ptrIntTy + " " + wide + " to " + tll);
            }
        } else if (fromInt && toPtr) {
            std::string wide = v;
            if (fromW != ptrBits_) {
                wide = newTmp();
                std::string ext = fromTy->isSigned() ? "sext" : "zext";
                line("  " + wide + " = " + ext + " " + fll + " " + v + " to " + ptrIntTy);
            }
            line("  " + tmp + " = inttoptr " + ptrIntTy + " " + wide + " to ptr");
        } else
            tmp = v;
    }
    return tmp;
}

std::string CodeGen::emitFieldAccess(const FieldAccessExpr& fa, const SourceLoc& loc) {
    auto oty = exprType(*fa.obj);

    int derefLevels = 0;
    while (oty && oty->isPtr() && oty->inner) {
        oty = oty->inner;
        ++derefLevels;
    }
    bool autoDeref = derefLevels > 0;

    if (oty && oty->isStruct()) {
        auto sit = sema_.structs().find(oty->name);
        if (sit == sema_.structs().end()) {
            diag_.error(loc, std::format("unknown struct '{}'", oty->name));
            return "undef";
        }

        int idx = sit->second->fieldIndex(fa.field);
        if (idx < 0) {
            diag_.error(loc, std::format("no field '{}' in struct '{}'", fa.field, oty->name));
            return "undef";
        }

        SubstMap structSubst = currentSubst_;
        auto& sinfo2 = *sit->second;
        if (!sinfo2.typeParams.empty() && !oty->typeArgs.empty())
            for (size_t i = 0; i < sinfo2.typeParams.size() && i < oty->typeArgs.size(); ++i)
                structSubst[sinfo2.typeParams[i]] = oty->typeArgs[i];
        auto fty = applySubst(sinfo2.fieldTypes[idx], structSubst);

        std::string llvmStructName = oty->name;
        if (!oty->typeArgs.empty())
            for (auto& ta : oty->typeArgs) llvmStructName += "_" + ta->mangle();

        auto resolveBase = [&](const std::string& allocaPtr) -> std::string {
            if (!autoDeref) return allocaPtr;
            std::string cur = allocaPtr;
            for (int i = 0; i < derefLevels; ++i) {
                std::string loaded = newTmp();
                line("  " + loaded + " = load ptr, ptr " + cur);
                cur = loaded;
            }
            return cur;
        };

        if (fa.obj->template is<IdentExpr>()) {
            std::string objName = fa.obj->template as<IdentExpr>().name;
            std::string basePtr;
            auto ait = varAlloca_.find(objName);
            if (ait != varAlloca_.end()) {
                basePtr = resolveBase(ait->second);
            } else {
                if (isGlobalSymbol(objName)) basePtr = resolveBase(gsym(objName));
            }

            if (!basePtr.empty()) {
                std::string fp = newTmp();
                line("  " + fp + " = getelementptr inbounds %" + llvmStructName +
                     ", ptr " + basePtr +
                     ", i32 0, i32 " + std::to_string(idx));

                if (isAggTy(fty))
                    return fp;

                std::string fv = newTmp();
                line("  " + fv + " = load " + llvmTy(fty) + ", ptr " + fp);
                return fv;
            }
        }

        std::string objVal = emitExpr(*fa.obj);
        std::string basePtr = objVal;
        for (int i = 0; i < derefLevels - 1; ++i) {
            std::string loaded = newTmp();
            line("  " + loaded + " = load ptr, ptr " + basePtr);
            basePtr = loaded;
        }

        std::string fp = newTmp();
        line("  " + fp + " = getelementptr inbounds %" + llvmStructName +
             ", ptr " + basePtr + ", i32 0, i32 " + std::to_string(idx));

        if (isAggTy(fty))
            return fp;

        std::string fv = newTmp();
        line("  " + fv + " = load " + llvmTy(fty) + ", ptr " + fp);
        return fv;
    }

    return "undef";
}

std::string CodeGen::emitIndex(const IndexExpr& ix, const SourceLoc& loc) {
    auto arrTy  = exprType(*ix.arr);
    auto idxTy  = exprType(*ix.idx);

    std::string idxVal = emitExpr(*ix.idx);

    const std::string ptrIntTy = (ptrBits_ == 32) ? "i32" : "i64";
    if (idxTy && idxTy->isInteger() && idxTy->bitWidth(ptrBits_) != ptrBits_) {
        std::string coerced = newTmp();
        std::string ext = idxTy->isSigned() ? "sext" : "zext";
        line("  " + coerced + " = " + ext + " " + llvmTy(idxTy) + " " + idxVal + " to " + ptrIntTy);
        idxVal = coerced;
    }
    const std::string idxLLTy = ptrIntTy;

    ResolvedTyPtr elemTy;
    if (arrTy && arrTy->isArray()) elemTy = arrTy->elemTy;
    else if (arrTy && arrTy->isPtr()) elemTy = arrTy->inner ? arrTy->inner : Ty::I8();
    else if (arrTy && arrTy->isStr()) elemTy = Ty::I8();
    else {
        diag_.error(loc,
            std::format("cannot index into expression of type '{}': "
                        "only arrays and pointers support the [] operator",
                        arrTy ? arrTy->str() : "?"),
            lineFor(loc));
        elemTy = Ty::I8();
    }

    bool elemIsAgg = isAggTy(elemTy);

    std::string basePtr;
    if (ix.arr->template is<IdentExpr>()) {
        auto& id = ix.arr->template as<IdentExpr>().name;
        auto ait = varAlloca_.find(id);
        if (ait != varAlloca_.end()) {
            if (arrTy && arrTy->isArray()) {
                std::string ep = newTmp();
                line("  " + ep + " = getelementptr inbounds " + llvmTy(arrTy) +
                     ", ptr " + ait->second + ", i32 0, " + idxLLTy + " " + idxVal);
                if (elemIsAgg) return ep;
                std::string ev = newTmp();
                line("  " + ev + " = load " + llvmTy(elemTy) + ", ptr " + ep);
                return ev;
            } else {
                basePtr = loadVar(id);
            }
        } else {
            basePtr = loadVar(id);
        }
    } else {
        basePtr = emitExpr(*ix.arr);
    }

    std::string ep = newTmp();
    line("  " + ep + " = getelementptr inbounds " + llvmTy(elemTy) +
         ", ptr " + basePtr + ", " + idxLLTy + " " + idxVal);
    if (elemIsAgg) return ep;
    std::string ev = newTmp();
    line("  " + ev + " = load " + llvmTy(elemTy) + ", ptr " + ep);
    return ev;
}

std::string CodeGen::emitCall(const CallExpr& c, const SourceLoc& loc) {
    std::string fname;
    if (c.callee->template is<IdentExpr>())
        fname = c.callee->template as<IdentExpr>().name;
    else {
        auto calleeTy = exprType(*c.callee);
        std::string fptr = emitExpr(*c.callee);

        std::vector<std::string> argVals;
        std::vector<std::string> argTys;
        for (auto& a : c.args) {
            argVals.push_back(emitExpr(*a));
            auto aty = exprType(*a);
            argTys.push_back(aty ? llvmTy(aty) : "i32");
        }

        std::string retLLVM = "i32";
        if (calleeTy && calleeTy->isFuncPtr() && calleeTy->funcRetType)
            retLLVM = llvmTy(calleeTy->funcRetType);

        std::string fnSig = retLLVM + " (";
        for (size_t i = 0; i < argTys.size(); ++i) {
            if (i) fnSig += ", ";
            fnSig += argTys[i];
        }
        fnSig += ")";

        std::string args;
        for (size_t i = 0; i < argVals.size(); ++i) {
            if (i) args += ", ";
            args += argTys[i] + " " + argVals[i];
        }

        if (retLLVM == "void") {
            line("  call " + fnSig + " " + fptr + "(" + args + ")");
            return "0";
        }
        std::string tmp = newTmp();
        line("  " + tmp + " = call " + fnSig + " " + fptr + "(" + args + ")");
        return tmp;
    }

    if (fname == "len") {
        if (c.args.size() != 1) {
            diag_.error(loc,
                std::format("'len' expects 1 argument, got {}", c.args.size()),
                lineFor(loc));
            return "0";
        }
        auto arrTy = exprType(*c.args[0]);

        int64_t sz = arrTy ? arrTy->arraySize : 0;

        if (sz == 0 && c.args[0]->template is<IdentExpr>()) {
            const std::string& vname = c.args[0]->template as<IdentExpr>().name;
            auto vit = varTypes_.find(vname);
            if (vit != varTypes_.end() && vit->second && vit->second->isArray())
                sz = vit->second->arraySize;
        }

        if (sz <= 0) {
            diag_.error(loc,
                "'len' could not determine array size at compile time; "
                "ensure the variable was declared with an integer-literal size",
                lineFor(loc));
            return "0";
        }
        return std::to_string(sz);
    }

    auto fit = sema_.funcs().find(fname);
    if (fit == sema_.funcs().end()) {
        auto vit = varAlloca_.find(fname);
        auto tyit = varTypes_.find(fname);
        if (vit != varAlloca_.end() && tyit != varTypes_.end() &&
            tyit->second && tyit->second->isFuncPtr()) {
            auto& fpty = tyit->second;

            std::string fptr = newTmp();
            line("  " + fptr + " = load ptr, ptr " + vit->second);

            std::vector<std::string> argVals, argTys;
            for (auto& a : c.args) {
                argVals.push_back(emitExpr(*a));
                auto aty = exprType(*a);
                argTys.push_back(aty ? llvmTy(aty) : "i32");
            }

            std::string retLLVM = fpty->funcRetType ? llvmTy(fpty->funcRetType) : "void";
            std::string fnSig = retLLVM + " (";
            for (size_t i = 0; i < argTys.size(); ++i) { if (i) fnSig += ", "; fnSig += argTys[i]; }
            fnSig += ")";

            std::string args;
            for (size_t i = 0; i < argVals.size(); ++i) {
                if (i) args += ", ";
                args += argTys[i] + " " + argVals[i];
            }

            if (retLLVM == "void") {
                line("  call " + fnSig + " " + fptr + "(" + args + ")");
                return "0";
            }
            std::string tmp = newTmp();
            line("  " + tmp + " = call " + fnSig + " " + fptr + "(" + args + ")");
            return tmp;
        }

        if (permissive_) {
            std::vector<std::string> argVals, argTys;
            const bool wasValue = usedAsValue_;
            usedAsValue_ = true;
            for (auto& a : c.args) {
                auto aty = exprType(*a);
                std::string v = emitExpr(*a);
                if (aty && isAggTy(aty)) { argTys.push_back("ptr"); }
                else                     { argTys.push_back(aty && !aty->isVoid() ? llvmTy(aty) : "i32"); }
                argVals.push_back(v);
            }
            usedAsValue_ = wasValue;
            std::string retLLVM = "void";
            if (usedAsValue_) {
                auto cit = currentSubst_.find("#ctx");
                if (cit != currentSubst_.end() && cit->second &&
                    !cit->second->isGeneric() && !cit->second->isVoid())
                    retLLVM = llvmTy(cit->second);
                else
                    retLLVM = "i32";
            }
            std::string args;
            for (size_t i = 0; i < argVals.size(); ++i) {
                if (i) args += ", ";
                args += argTys[i] + " " + argVals[i];
            }
            std::string sigParams;
            for (size_t i = 0; i < argTys.size(); ++i) {
                if (i) sigParams += ", ";
                sigParams += argTys[i];
            }
            std::string key = fname + "|" + retLLVM + "|" + sigParams;
            if (!permissiveDecls_.count(key)) {
                permissiveDecls_.insert(key);
                permissiveDeclLines_.push_back("declare " + retLLVM + " @" + fname +
                                               "(" + sigParams + ")");
            }
            if (retLLVM == "void") {
                line("  call void @" + fname + "(" + args + ")");
                return "undef";
            }
            std::string tmp = newTmp();
            line("  " + tmp + " = call " + retLLVM + " @" + fname + "(" + args + ")");
            return tmp;
        }
        diag_.error(loc, std::format("unknown function '{}'", fname), lineFor(loc));
        return "undef";
    }
    ResolvedTyPtr ctxTy;
    if (fit->second.decl &&
        fit->second.decl->retType &&
        fit->second.decl->retType->isHash()) {

        auto it = currentSubst_.find("#ctx");
        if (it == currentSubst_.end() || !it->second || it->second->isGeneric()) {
            diag_.error(loc,
                std::format("call to '{}' requires a concrete contextual return type (#)",
                            fname),
                lineFor(loc));
            return "undef";
        }
        ctxTy = it->second;
    }

    std::unordered_map<std::string, ResolvedTyPtr> inferMap;
    for (size_t i = 0; i < c.args.size() && i < fit->second.paramTypes.size(); ++i) {
        auto prmTy = fit->second.paramTypes[i];
        if (!prmTy || !prmTy->isGeneric()) continue;
        auto argTy = exprType(*c.args[i]);
        if (argTy && !argTy->isGeneric())
            inferMap[prmTy->genericName] = argTy;
    }

    std::vector<ResolvedTyPtr> typeArgs;
    for (auto& gp : fit->second.genericParams) {
        if (gp == "#ctx") {
            typeArgs.push_back(ctxTy);
            continue;
        }

        auto sit = currentSubst_.find(gp);
        if (sit != currentSubst_.end() && sit->second && !sit->second->isGeneric()) {
            typeArgs.push_back(sit->second);
            continue;
        }

        auto it = inferMap.find(gp);
        if (it != inferMap.end()) {
            typeArgs.push_back(it->second);
            continue;
        }

        diag_.error(loc,
            std::format("cannot infer generic type '{}' in call to '{}'", gp, fname),
            lineFor(loc));
        return "undef";
    }

    std::string actualName = typeArgs.empty() ? fname : mangleCall(fname, typeArgs);

    if (!typeArgs.empty()) {
        schedMono(fname, typeArgs);
        for (auto& ta : typeArgs) {
            if (ta && ta->isEnum())
                emitEnumType(ta->name, ta->typeArgs, currentSubst_);
        }
    }

    enum class TransientKind { None, Str };

    struct ArgInfo {
        std::string             val;
        ResolvedTyPtr           ty;
        TransientKind           transient = TransientKind::None;
        std::vector<std::string> freeAfterCall;
    };

    auto isTransientStrArg = [&](const Expr& e) -> bool {
        if (e.template is<BinOpExpr>()) {
            auto& b2 = e.template as<BinOpExpr>();
            if (b2.op != "+") return false;
            auto lt2 = exprType(*b2.lhs);
            return lt2 && lt2->isStr();
        }
        if (e.template is<CallExpr>()) {
            auto ty2 = exprType(e);
            if (!ty2 || !ty2->isStr()) return false;
            auto& c2 = e.template as<CallExpr>();
            if (!c2.callee->template is<IdentExpr>()) return false;
            const std::string& fn2 =
                c2.callee->template as<IdentExpr>().name;
            if (funcAlwaysAllocatesStr(fn2)) return true;
            for (auto& arg : c2.args) {
                if (!arg->template is<CallExpr>()) continue;
                auto argTy = exprType(*arg);
                if (argTy && argTy->isEnum() &&
                    enumHasDroppablePayload(argTy->name, argTy->typeArgs))
                    return true;
            }
            return false;
        }
        return false;
    };

    bool calleeReturnsMayAliasArg = false;
    {
        ResolvedTyPtr callRetTy = fit->second.retType;
        if (callRetTy && callRetTy->isStr() && !funcAlwaysAllocatesStr(fname)) {
            calleeReturnsMayAliasArg = true;
        }
    }

    const ExternDecl* externDeclForCall = nullptr;
    for (auto& ed : prog_.externs)
        if (ed->name == fname) { externDeclForCall = ed.get(); break; }

    if (!calleeReturnsMayAliasArg && externDeclForCall && externDeclForCall->retType) {
        auto extRetTy = sema_.resolveType(*externDeclForCall->retType);
        if (extRetTy && extRetTy->isStr() && !externDeclForCall->isUnsafe) {
            calleeReturnsMayAliasArg = true;
        }
    }

    std::vector<ArgInfo> argInfos;
    for (size_t i = 0; i < c.args.size(); ++i) {
        ArgInfo ai;
        ResolvedTyPtr expectedTy = (i < fit->second.paramTypes.size())
            ? applySubst(fit->second.paramTypes[i], currentSubst_)
            : nullptr;

        if (c.args[i]->template is<IntLitExpr>() &&
            expectedTy && (expectedTy->isInteger() || expectedTy->isFloat()) && !expectedTy->isGeneric()) {
            ai.val = emitIntLit(c.args[i]->template as<IntLitExpr>().val, expectedTy);
            ai.ty  = expectedTy;
        } else {
            SubstMap savedArgCtx = currentSubst_;
            if (expectedTy && !expectedTy->isGeneric() &&
                (expectedTy->isInteger() || expectedTy->isFloat()))
                currentSubst_["#ctx"] = expectedTy;
            ai.val = emitExpr(*c.args[i]);
            ai.ty  = exprType(*c.args[i]);
            currentSubst_ = savedArgCtx;

            if (expectedTy && !expectedTy->isGeneric() && ai.ty && !ai.ty->equals(*expectedTy)) {
                ai.val = implicitCoerce(ai.val, ai.ty, expectedTy, *fnOut_, tmpIdx_, ptrBits_);
                ai.ty  = expectedTy;
            }
        }

        std::string originalVal = ai.val;

        if (!calleeReturnsMayAliasArg && isTransientStrArg(*c.args[i])) {
            ai.transient = TransientKind::Str;
            ai.freeAfterCall.push_back(originalVal);
        }

        bool isVariadicArg = fit->second.isVariadic &&
                             (i >= fit->second.paramTypes.size());
        if (isVariadicArg && ai.ty && ai.ty->kind == TyKind::F32) {
            std::string promoted = newTmp();
            line("  " + promoted + " = fpext float " + ai.val + " to double");
            ai.val = promoted;
            ai.ty  = Ty::F64();
        }

        argInfos.push_back(std::move(ai));
    }

    std::string argStr;
    for (size_t i = 0; i < argInfos.size(); ++i) {
        if (i) argStr += ", ";

        auto& aty = argInfos[i].ty;
        bool isAgg = aty && (aty->isStruct() || aty->isEnum());

        if (externDeclForCall) {
            std::string v = argInfos[i].val;
            std::string tyStr = llvmTy(aty);

            if (aty && aty->isStruct()) {
                auto sit = sema_.structs().find(aty->name);
                bool argPacked = sit != sema_.structs().end() && sit->second->packed;
                if (argPacked && llvmByteSize(aty) == 4) {
                    std::string tmp = newTmp();
                    line("  " + tmp + " = load i32, ptr " + v + ", align 1");
                    v = tmp;
                    tyStr = "i32";
                    argStr += tyStr + " " + v;
                } else if (llvmByteSize(aty) > 16) {
                    int align = validAlign(llvmByteSize(aty), argPacked);
                    argStr += "ptr byval(" + llvmTy(aty) + ") align " +
                              std::to_string(align) + " " + v;
                } else if (!argPacked) {
                    auto chunks = isX86_64() ? x86_64AbiChunks(aty)
                                             : std::vector<AbiChunk>{};
                    if (!chunks.empty()) {
                        bool firstChunk = true;
                        for (auto& chunk : chunks) {
                            if (!firstChunk) argStr += ", ";
                            std::string chunkVal = emitLoadAbiChunk(v, chunk);
                            argStr += chunk.llvmType + " " + chunkVal;
                            firstChunk = false;
                        }
                    } else {
                        int align = validAlign(llvmByteSize(aty), argPacked);
                        std::string tmp = newTmp();
                        line("  " + tmp + " = load " + llvmTy(aty) + ", ptr " + v +
                             ", align " + std::to_string(align));
                        v = tmp;
                        argStr += tyStr + " " + v;
                    }
                } else {
                    int align = validAlign(llvmByteSize(aty), argPacked);
                    std::string tmp = newTmp();
                    line("  " + tmp + " = load " + llvmTy(aty) + ", ptr " + v +
                         ", align " + std::to_string(align));
                    v = tmp;
                    argStr += tyStr + " " + v;
                }
            } else if (aty && aty->isEnum()) {
                if (llvmByteSize(aty) > 16) {
                    int align = validAlign(llvmByteSize(aty));
                    argStr += "ptr byval(" + llvmTy(aty) + ") align " +
                              std::to_string(align) + " " + v;
                } else {
                    int align = validAlign(llvmByteSize(aty));
                    std::string tmp = newTmp();
                    line("  " + tmp + " = load " + llvmTy(aty) + ", ptr " + v +
                         ", align " + std::to_string(align));
                    v = tmp;
                    tyStr = llvmTy(aty);
                    argStr += tyStr + " " + v;
                }
            } else {
                argStr += tyStr + " " + v;
            }
        } else {
            argStr += isAgg ? ("ptr " + argInfos[i].val)
                            : (llvmTy(aty) + " " + argInfos[i].val);
        }
    }

    ResolvedTyPtr retTy;
    if (fit->second.decl && fit->second.decl->retType && fit->second.decl->retType->isHash()) {
        retTy = ctxTy;
    } else {
        SubstMap callSubst = currentSubst_;
        size_t ti = 0;
        for (auto& gp : fit->second.genericParams) {
            if (gp == "#ctx") continue;
            if (ti < typeArgs.size())
                callSubst[gp] = typeArgs[ti++];
        }
        retTy = applySubst(fit->second.retType, callSubst);
        if (retTy && retTy->isGeneric()) retTy = Ty::Void();
    }

    bool isVariadicCall = fit->second.isVariadic;
    std::string variadicFuncTy;
    if (isVariadicCall) {
        std::string fixedParams;
        for (size_t i = 0; i < fit->second.paramTypes.size(); ++i) {
            if (i) fixedParams += ", ";
            fixedParams += llvmTy(fit->second.paramTypes[i]);
        }
        if (!fixedParams.empty()) fixedParams += ", ";
        fixedParams += "...";
        variadicFuncTy = llvmTy(retTy) + " (" + fixedParams + ")";
    }

    std::string callResult;
    if (retTy && retTy->isVoid()) {
        if (isVariadicCall)
            line("  call " + variadicFuncTy + " @" + actualName + "(" + argStr + ")");
        else
            line("  call void @" + actualName + "(" + argStr + ")");
        callResult = "undef";
    } else if (isAggTy(retTy) && externDeclForCall) {
        std::string retBuf = newTmp() + ".ret";
        bool retPacked = false;
        if (retTy->isStruct()) {
            auto sit = sema_.structs().find(retTy->name);
            if (sit != sema_.structs().end()) retPacked = sit->second->packed;
        }
        int retSz    = llvmByteSize(retTy);
        int align    = validAlign(retSz, retPacked);
        line("  " + retBuf + " = alloca " + llvmTy(retTy) +
             ", align " + std::to_string(align));

        if (retPacked && retSz == 4) {
            std::string tmp = newTmp();
            line("  " + tmp + " = call i32 @" + actualName + "(" + argStr + ")");
            line("  store i32 " + tmp + ", ptr " + retBuf + ", align 1");
        } else if (retSz > 16) {
            std::string fullArgs = "ptr sret(" + llvmTy(retTy) + ") " + retBuf;
            if (!argStr.empty()) fullArgs += ", " + argStr;
            line("  call void @" + actualName + "(" + fullArgs + ")");
        } else {
            if (retTy->isStruct()) {
                auto chunks = isX86_64() ? x86_64AbiChunks(retTy)
                                         : std::vector<AbiChunk>{};
                if (!chunks.empty()) {
                    std::string abiRetTy = "{ ";
                    for (size_t ci = 0; ci < chunks.size(); ++ci) {
                        if (ci) abiRetTy += ", ";
                        abiRetTy += chunks[ci].llvmType;
                    }
                    abiRetTy += " }";
                    std::string tmp = newTmp();
                    line("  " + tmp + " = call " + abiRetTy +
                         " @" + actualName + "(" + argStr + ")");
                    for (size_t ci = 0; ci < chunks.size(); ++ci) {
                        std::string chunkVal = newTmp();
                        line("  " + chunkVal + " = extractvalue " + abiRetTy +
                             " " + tmp + ", " + std::to_string(ci));
                        std::string chunkPtr;
                        if (chunks[ci].byteOffset > 0) {
                            chunkPtr = newTmp();
                            line("  " + chunkPtr + " = getelementptr inbounds i8, ptr " +
                                 retBuf + ", i32 " + std::to_string(chunks[ci].byteOffset));
                        } else {
                            chunkPtr = retBuf;
                        }
                        int chunkAlign = (chunks[ci].cls == AbiChunk::Class::SSE)
                                             ? 4
                                             : std::max(1, std::min(chunks[ci].byteSize, 8));
                        line("  store " + chunks[ci].llvmType + " " + chunkVal +
                             ", ptr " + chunkPtr + ", align " + std::to_string(chunkAlign));
                    }
                } else {
                    std::string tmp = newTmp();
                    line("  " + tmp + " = call " + llvmTy(retTy) +
                         " @" + actualName + "(" + argStr + ")");
                    line("  store " + llvmTy(retTy) + " " + tmp + ", ptr " + retBuf +
                         ", align " + std::to_string(align));
                }
            } else {
                std::string tmp = newTmp();
                line("  " + tmp + " = call " + llvmTy(retTy) +
                     " @" + actualName + "(" + argStr + ")");
                line("  store " + llvmTy(retTy) + " " + tmp + ", ptr " + retBuf +
                     ", align " + std::to_string(align));
            }
        }
        callResult = retBuf;
    } else if (isAggTy(retTy)) {
        std::string sretBuf = newTmp() + ".sret";
        bool sretPacked = false;
        if (retTy->isStruct()) {
            auto sit = sema_.structs().find(retTy->name);
            if (sit != sema_.structs().end()) sretPacked = sit->second->packed;
        }
        int align = validAlign(llvmByteSize(retTy), sretPacked);
        line("  " + sretBuf + " = alloca " + llvmTy(retTy) +
            ", align " + std::to_string(align));

        std::string fullArgs = "ptr sret(" + llvmTy(retTy) + ") " + sretBuf;
        if (!argStr.empty()) fullArgs += ", " + argStr;
        line("  call void @" + actualName + "(" + fullArgs + ")");

        callResult = sretBuf;
    } else {
        std::string tmp = newTmp();
        if (isVariadicCall)
            line("  " + tmp + " = call " + variadicFuncTy +
                 " @" + actualName + "(" + argStr + ")");
        else
            line("  " + tmp + " = call " + llvmTy(retTy) +
             " @" + actualName + "(" + argStr + ")");
        callResult = tmp;
    }

    for (auto& ai : argInfos) {
        for (auto it = ai.freeAfterCall.rbegin(); it != ai.freeAfterCall.rend(); ++it)
            emitStrFree(*it);
    }

    return callResult;
}

std::string CodeGen::materializeValue(const std::string& v,
                                      const ResolvedTyPtr& ty) {
    if (!ty) return v;

    if (ty->isStruct() || ty->isEnum()) {
        std::string tmp = newTmp();
        line("  " + tmp + " = load " + llvmTy(ty) + ", ptr " + v);
        return tmp;
    }

    return v;
}

std::string CodeGen::emitMatch(const MatchExpr& mx, const SourceLoc& loc) {
    std::string subjVal = emitExpr(*mx.subject);
    auto subjTy = exprType(*mx.subject);

    if (!subjTy || !subjTy->isEnum()) {
        diag_.error(loc, "match subject must be an enum type");
        return "undef";
    }

    auto eit = sema_.enums().find(subjTy->name);
    if (eit == sema_.enums().end()) {
        diag_.error(loc, std::format("unknown enum type '{}'", subjTy->name));
        return "undef";
    }
    auto& einfo = *eit->second;

    std::string tyName = enumTypeName(subjTy->name, subjTy->typeArgs);
    if (!subjTy->typeArgs.empty())
        emitEnumType(subjTy->name, subjTy->typeArgs, currentSubst_);

    std::string subjPtr = subjVal;

    std::string tagPtr = newTmp();
    line("  " + tagPtr + " = getelementptr inbounds %" + tyName +
         ", ptr " + subjPtr + ", i32 0, i32 0");
    std::string tagVal = newTmp();
    line("  " + tagVal + " = load i32, ptr " + tagPtr);

    std::vector<std::string> armLabels;
    for (size_t i = 0; i < mx.arms.size(); ++i)
        armLabels.push_back(newLabel());
    std::string endLbl = newLabel();
    std::string defaultLbl = endLbl;

    for (size_t i = 0; i < mx.arms.size(); ++i) {
        if (mx.arms[i].isWildcard) { defaultLbl = armLabels[i]; break; }
    }

    std::string sw = "  switch i32 " + tagVal + ", label %" + defaultLbl + " [";
    for (size_t i = 0; i < mx.arms.size(); ++i) {
        if (mx.arms[i].isWildcard) continue;
        int tag = einfo.variantIndex(mx.arms[i].variantName);
        if (tag < 0) continue;
        sw += " i32 " + std::to_string(tag) + ", label %" + armLabels[i];
    }
    sw += " ]";
    line(sw);

    for (size_t i = 0; i < mx.arms.size(); ++i) {
        line(armLabels[i] + ":");
        auto& arm = mx.arms[i];

        if (!arm.bindNames.empty() && !arm.isWildcard) {
            int vi = einfo.variantIndex(arm.variantName);
            if (vi >= 0) {
                const EnumDecl* ed = nullptr;
                for (auto& d : prog_.enums)
                    if (d->name == subjTy->name) { ed = d.get(); break; }

                if (ed && vi < (int)ed->variants.size()) {
                    auto& varDecl = ed->variants[vi];
                    int byteOffset = enumPayloadPad(
                        subjTy->name, subjTy->typeArgs, currentSubst_);
                    SubstMap enumSubst = currentSubst_;
                    for (size_t i = 0; i < einfo.typeParams.size() && i < subjTy->typeArgs.size(); ++i)
                        enumSubst[einfo.typeParams[i]] = subjTy->typeArgs[i];
                    for (size_t bi = 0; bi < arm.bindNames.size() &&
                                        bi < varDecl.payload.size(); ++bi) {
                        auto pty = sema_.resolveType(*varDecl.payload[bi], enumSubst);

                        std::string payloadPtr = newTmp();
                        line("  " + payloadPtr + " = getelementptr inbounds %" + tyName +
                            ", ptr " + subjPtr + ", i32 0, i32 1");
                        std::string elemPtr = newTmp();
                        line("  " + elemPtr + " = getelementptr inbounds i8, ptr " +
                            payloadPtr + ", i32 " + std::to_string(byteOffset));

                        allocaFor(arm.bindNames[bi], pty);
                        if (isAggTy(pty)) {
                            emitAggrCopy(varAlloca_[arm.bindNames[bi]], elemPtr, pty);
                        } else {
                            std::string bindVal = newTmp();
                            int loadAlign = validAlign(llvmByteSize(pty));
                            line("  " + bindVal + " = load " + llvmTy(pty) + ", ptr " + elemPtr +
                                ", align " + std::to_string(loadAlign));
                            storeVar(arm.bindNames[bi], bindVal, pty);
                        }
                        byteOffset += llvmByteSize(pty);
                    }
                }
            }
        }

        for (auto& s : arm.body) emitStmt(*s);
        line("  br label %" + endLbl);
    }

    line(endLbl + ":");
    return "undef";
}

std::string CodeGen::emitTypeswitch(const TypeswitchExpr& ts, const SourceLoc& loc) {
    ResolvedTyPtr concreteType;

    if (ts.param == "#") {
        auto hctx = currentSubst_.find("#ctx");
        concreteType = (hctx != currentSubst_.end()) ? hctx->second : curRetTy_;
    } else {
        auto sit = currentSubst_.find(ts.param);
        if (sit != currentSubst_.end()) {
            concreteType = sit->second;
        } else {
            auto vit = varTypes_.find(ts.param);
            if (vit != varTypes_.end())
                concreteType = vit->second;
        }
    }

    if (!concreteType || concreteType->isGeneric()) {
        diag_.error(loc,
            std::format("typeswitch on \'{}\' could not be resolved to a "
                        "concrete type; make sure the function is called with "
                        "explicit type arguments or is monomorphised",
                ts.param),
            lineFor(loc));
        return "undef";
    }

    std::string fname = curFuncOrigName_;

    emitTypeswitchDCE(fname, ts.param, ts.arms, concreteType, loc);
    return "undef";
}

std::string CodeGen::emitReadonly(const ReadonlyExpr& r) {
    return emitExpr(*r.operand);
}

std::string CodeGen::emitTake(const TakeExpr& t) {
    if (t.operand->template is<IdentExpr>())
        return emitLValuePtr(*t.operand, t.operand->loc);
    return emitExpr(*t.operand);
}

std::string CodeGen::emitRelease(const ReleaseExpr& r) {
    return emitExpr(*r.operand);
}

std::string CodeGen::emitTernary(const TernaryExpr& t, const SourceLoc& /*loc*/) {
    auto resTy = exprType(*t.thenExpr);

    std::string condVal = emitExpr(*t.cond);
    auto condTy = exprType(*t.cond);

    std::string cond1;
    if (condTy && condTy->kind == TyKind::Bool) {
        cond1 = condVal;
    } else {
        cond1 = newTmp();
        line("  " + cond1 + " = icmp ne " + llvmTy(condTy) + " " + condVal + ", 0");
    }

    std::string thenLbl = newLabel();
    std::string elseLbl = newLabel();
    std::string mergeLbl = newLabel();

    std::string resPtr;
    if (!resTy->isVoid()) {
        resPtr = newTmp() + ".tern";
        bool ternPacked = false;
        if (resTy->isStruct()) {
            auto sit = sema_.structs().find(resTy->name);
            if (sit != sema_.structs().end()) ternPacked = sit->second->packed;
        }
        int align = validAlign(llvmByteSize(resTy), ternPacked);
        line("  " + resPtr + " = alloca " + llvmTy(resTy) +
             ", align " + std::to_string(align));
    }

    line("  br i1 " + cond1 + ", label %" + thenLbl + ", label %" + elseLbl);

    line(thenLbl + ":");
    std::string thenVal = emitExpr(*t.thenExpr);
    if (!resTy->isVoid()) {
        if (isAggTy(resTy)) emitAggrCopy(resPtr, thenVal, resTy);
        else line("  store " + llvmTy(resTy) + " " + thenVal + ", ptr " + resPtr);
    }
    line("  br label %" + mergeLbl);

    line(elseLbl + ":");
    std::string elseVal = emitExpr(*t.elseExpr);
    if (!resTy->isVoid()) {
        if (isAggTy(resTy)) emitAggrCopy(resPtr, elseVal, resTy);
        else line("  store " + llvmTy(resTy) + " " + elseVal + ", ptr " + resPtr);
    }
    line("  br label %" + mergeLbl);

    line(mergeLbl + ":");
    if (resTy->isVoid()) return "undef";
    if (isAggTy(resTy)) return resPtr;

    std::string res = newTmp();
    line("  " + res + " = load " + llvmTy(resTy) + ", ptr " + resPtr);
    return res;
}

void CodeGen::pushDropScope() {
    dropScopes_.push_back({});
}

void CodeGen::popNoDrops() {
    if (!dropScopes_.empty())
        dropScopes_.pop_back();
}

void CodeGen::popAndEmitDrops() {
    if (dropScopes_.empty()) return;
    emitDropsForFrame(dropScopes_.back());
    dropScopes_.pop_back();
}

void CodeGen::addToDropScope(const std::string& name,
                              const ResolvedTyPtr& ty,
                              DropKind kind) {
    if (dropScopes_.empty()) return;
    dropScopes_.back().push_back({name, ty, kind});
}

void CodeGen::emitAllDropsExcept(const std::string& skipVar) {
    if (blockTerminated_) return;
    for (int i = (int)dropScopes_.size() - 1; i >= 0; --i)
        emitDropsForFrame(dropScopes_[i], skipVar);
}

void CodeGen::emitDropsFromDepth(int fromDepth, const std::string& skipVar) {
    if (blockTerminated_) return;
    for (int i = (int)dropScopes_.size() - 1; i >= fromDepth; --i)
        emitDropsForFrame(dropScopes_[i], skipVar);
}

void CodeGen::emitDropsForFrame(const std::vector<DropEntry>& frame,
                                 const std::string& skipVar) {
    if (blockTerminated_) return;
    for (int i = (int)frame.size() - 1; i >= 0; --i) {
        if (!skipVar.empty() && frame[i].varName == skipVar) continue;
        emitDropEntry(frame[i]);
    }
}

void CodeGen::emitDropEntry(const DropEntry& de) {
    if (blockTerminated_) return;
    switch (de.kind) {
    case DropKind::OwnedStr: {
        std::string storagePtr;
        auto ait = varAlloca_.find(de.varName);
        if (ait != varAlloca_.end()) {
            storagePtr = ait->second;
        } else {
            if (!isGlobalSymbol(de.varName)) return;
            storagePtr = gsym(de.varName);
        }
        std::string ptr = newTmp();
        line("  " + ptr + " = load ptr, ptr " + storagePtr + ", align 8");
        emitStrFree(ptr);
        line("  store ptr null, ptr " + storagePtr + ", align 8");
        break;
    }
    case DropKind::EnumWithStrPayload:
        emitEnumPayloadDrop(de.varName, de.ty);
        break;
    }
}

void CodeGen::emitStrFree(const std::string& llvmPtr) {
    if (blockTerminated_) return;
    std::string nn   = newTmp();
    std::string freeLbl = newLabel();
    std::string skipLbl = newLabel();
    line("  " + nn + " = icmp ne ptr " + llvmPtr + ", null");
    line("  br i1 " + nn + ", label %" + freeLbl + ", label %" + skipLbl);
    line(freeLbl + ":");
    line("  call void @" + rtSym("free") + "(ptr " + llvmPtr + ")");
    line("  br label %" + skipLbl);
    line(skipLbl + ":");
}

bool CodeGen::enumHasDroppablePayload(const std::string& enumName,
                                       const std::vector<ResolvedTyPtr>& typeArgs) const {
    auto eit = sema_.enums().find(enumName);
    if (eit == sema_.enums().end()) return false;
    auto& einfo = *eit->second;

    SubstMap enumSubst = currentSubst_;
    for (size_t i = 0; i < einfo.typeParams.size() && i < typeArgs.size(); ++i)
        enumSubst[einfo.typeParams[i]] = typeArgs[i];

    const EnumDecl* ed = nullptr;
    for (auto& d : prog_.enums)
        if (d->name == enumName) { ed = d.get(); break; }
    if (!ed) return false;

    for (auto& v : ed->variants) {
        for (auto& pt : v.payload) {
            auto rty = sema_.resolveType(*pt, enumSubst);
            if (rty && rty->isStr())
                return true;
        }
    }
    return false;
}

void CodeGen::emitEnumPayloadDrop(const std::string& varName,
                                   const ResolvedTyPtr& ty) {
    if (blockTerminated_) return;
    auto eit = sema_.enums().find(ty->name);
    if (eit == sema_.enums().end()) return;
    auto& einfo = *eit->second;

    SubstMap enumSubst = currentSubst_;
    for (size_t i = 0; i < einfo.typeParams.size() && i < ty->typeArgs.size(); ++i)
        enumSubst[einfo.typeParams[i]] = ty->typeArgs[i];

    const EnumDecl* ed = nullptr;
    for (auto& d : prog_.enums)
        if (d->name == ty->name) { ed = d.get(); break; }
    if (!ed) return;

    std::string tyName  = enumTypeName(ty->name, ty->typeArgs);
    std::string varAddr;
    auto ait = varAlloca_.find(varName);
    if (ait != varAlloca_.end()) {
        varAddr = ait->second;
    } else {
        if (!isGlobalSymbol(varName)) return;
        varAddr = gsym(varName);
    }

    std::string tagPtr = newTmp();
    std::string tagVal = newTmp();
    line("  " + tagPtr + " = getelementptr inbounds %" + tyName +
         ", ptr " + varAddr + ", i32 0, i32 0");
    line("  " + tagVal + " = load i32, ptr " + tagPtr);

    std::string dropEndLbl = newLabel();

    for (auto& v : ed->variants) {
        int vi = einfo.variantIndex(v.name);
        if (vi < 0 || v.payload.empty()) continue;

        struct DropField { int byteOffset; };
        std::vector<DropField> fields;
        int padBytes  = enumPayloadPad(ty->name, ty->typeArgs, enumSubst);
        int byteOff   = padBytes;
        bool anyDrop  = false;

        for (auto& pt : v.payload) {
            auto rty = sema_.resolveType(*pt, enumSubst);
            if (rty && rty->isStr()) {
                fields.push_back({byteOff});
                anyDrop = true;
            }
            byteOff += rty ? llvmByteSize(rty) : 0;
        }
        if (!anyDrop) continue;

        std::string matchLbl = newLabel();
        std::string skipLbl  = newLabel();
        std::string cmpTmp   = newTmp();
        line("  " + cmpTmp + " = icmp eq i32 " + tagVal +
             ", " + std::to_string(vi));
        line("  br i1 " + cmpTmp + ", label %" + matchLbl +
             ", label %" + skipLbl);
        line(matchLbl + ":");

        std::string payBase = newTmp();
        line("  " + payBase + " = getelementptr inbounds %" + tyName +
             ", ptr " + varAddr + ", i32 0, i32 1");

        for (auto& f : fields) {
            std::string fptr = newTmp();
            if (f.byteOffset > 0) {
                line("  " + fptr + " = getelementptr inbounds i8, ptr " +
                     payBase + ", i32 " + std::to_string(f.byteOffset));
            } else {
                fptr = payBase;
            }
            std::string fval = newTmp();
            line("  " + fval + " = load ptr, ptr " + fptr);
            emitStrFree(fval);
            line("  store ptr null, ptr " + fptr + ", align 8");
        }

        line("  br label %" + skipLbl);
        line(skipLbl + ":");
    }

    line("  br label %" + dropEndLbl);
    line(dropEndLbl + ":");
}

static void collectRetVals(const std::vector<StmtPtr>& stmts,
                            std::vector<const Expr*>& out) {
    for (auto& s : stmts) {
        std::visit([&](auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, ReturnStmt>) {
                if (x.value) out.push_back(x.value.get());
            } else if constexpr (std::is_same_v<T, IfStmt>) {
                collectRetVals(x.thenBody, out);
                collectRetVals(x.elseBody, out);
            } else if constexpr (std::is_same_v<T, WhileStmt>) {
                collectRetVals(x.body, out);
            } else if constexpr (std::is_same_v<T, BlockStmt>) {
                collectRetVals(x.stmts, out);
            } else if constexpr (std::is_same_v<T, ExprStmt>) {
                if (!x.expr) return;
                if (x.expr->template is<TypeswitchExpr>()) {
                    for (auto& arm : x.expr->template as<TypeswitchExpr>().arms)
                        collectRetVals(arm.body, out);
                } else if (x.expr->template is<MatchExpr>()) {
                    for (auto& arm : x.expr->template as<MatchExpr>().arms)
                        collectRetVals(arm.body, out);
                }
            }
        }, s->v);
    }
}

bool CodeGen::funcAlwaysAllocatesStr(const std::string& fname) const {
    auto cit = strAllocCache_.find(fname);
    if (cit != strAllocCache_.end()) return cit->second;

    auto cache = [&](bool v) -> bool {
        strAllocCache_[fname] = v;
        return v;
    };

    for (auto& ed : prog_.externs) {
        if (ed->name != fname) continue;
        if (ed->isUnsafe) return cache(false);
        if (!ed->retType) return cache(false);
        auto rt = sema_.resolveType(*ed->retType);
        return cache(rt && rt->isStr());
    }

    const FuncDecl* fd = prog_.findFunc(fname);
    if (!fd) return cache(false);

    if (!fd->retType || fd->retType->isHash()) return cache(false);
    auto retTy = sema_.resolveType(*fd->retType);
    if (!retTy || !retTy->isStr()) return cache(false);

    std::vector<const Expr*> retVals;
    collectRetVals(fd->body, retVals);

    std::function<bool(const Expr*)> isAllocExpr = [&](const Expr* rv) -> bool {
        if (!rv) return false;
        if (rv->template is<IdentExpr>())     return false;
        if (rv->template is<StringLitExpr>()) return false;
        if (rv->template is<TernaryExpr>()) {
            auto& t = rv->template as<TernaryExpr>();
            return isAllocExpr(t.thenExpr.get()) || isAllocExpr(t.elseExpr.get());
        }
        return true;
    };

    bool anyAllocating = false;
    for (auto* rv : retVals) {
        if (isAllocExpr(rv)) { anyAllocating = true; break; }
    }
    return cache(anyAllocating);
}

bool CodeGen::isOwnedInit(const Expr* init) const {
    if (!init) return false;

    if (init->template is<CastExpr>()) {
        return isOwnedInit(init->template as<CastExpr>().operand.get());
    }

    if (init->template is<BinOpExpr>()) {
        auto& b = init->template as<BinOpExpr>();
        if (b.op == "+") {
            auto lt = exprType(*b.lhs);
            return lt && lt->isStr();
        }
        return false;
    }

    if (init->template is<CallExpr>()) {
        auto& c = init->template as<CallExpr>();
        if (c.callee->template is<IdentExpr>()) {
            const std::string& fname =
                c.callee->template as<IdentExpr>().name;
            for (auto& ed : prog_.externs) {
                if (ed->name == fname && ed->isUnsafe)
                    return false;
            }

            auto it = sema_.funcs().find(fname);
            if (it != sema_.funcs().end() && it->second.retType) {
                auto rt = it->second.retType;
                if (!rt->isStr() && !rt->isPtr()) return false;
                return funcAlwaysAllocatesStr(fname);
            }

            if (funcAlwaysAllocatesStr(fname)) return true;
        }
        return false;
    }

    if (init->template is<StringLitExpr>()) return false;

    if (init->template is<IdentExpr>()) return false;

    if (init->template is<ReadonlyExpr>()) return false;
    if (init->template is<TakeExpr>())    return false;
    if (init->template is<ReleaseExpr>()) return false;

    return false;
}
