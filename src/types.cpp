#include "types.hpp"

namespace {
    std::optional<int64_t> evalConstIntExpr(
        const Expr& e,
        const TypeResolver::IntConstResolver& intConstResolver) {
        return std::visit([&](auto& x) -> std::optional<int64_t> {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, IntLitExpr>) {
                return x.val;
            }
            if constexpr (std::is_same_v<T, IdentExpr>) {
                if (intConstResolver) return intConstResolver(x.name);
                return std::nullopt;
            }
            if constexpr (std::is_same_v<T, UnaryOpExpr>) {
                auto v = evalConstIntExpr(*x.operand, intConstResolver);
                if (!v) return std::nullopt;
                if (x.op == "+") return *v;
                if (x.op == "-") return -*v;
                return std::nullopt;
            }
            if constexpr (std::is_same_v<T, BinOpExpr>) {
                auto l = evalConstIntExpr(*x.lhs, intConstResolver);
                auto r = evalConstIntExpr(*x.rhs, intConstResolver);
                if (!l || !r) return std::nullopt;
                if (x.op == "+") return *l + *r;
                if (x.op == "-") return *l - *r;
                if (x.op == "*") return *l * *r;
                if (x.op == "/" && *r != 0) return *l / *r;
                if (x.op == "%" && *r != 0) return *l % *r;
                if (x.op == "<<" && *r >= 0 && *r < 63) return *l << *r;
                if (x.op == ">>" && *r >= 0 && *r < 63) return *l >> *r;
                if (x.op == "&") return *l & *r;
                if (x.op == "|") return *l | *r;
                if (x.op == "^") return *l ^ *r;
                return std::nullopt;
            }
            if constexpr (std::is_same_v<T, CastExpr>) {
                return evalConstIntExpr(*x.operand, intConstResolver);
            }
            return std::nullopt;
        }, e.v);
    }
}

std::string ResolvedTy::str() const {
    switch (kind) {
        case TyKind::Void:    return "void";
        case TyKind::Bool:    return "bool";
        case TyKind::I8:      return "i8";
        case TyKind::I16:     return "i16";
        case TyKind::I32:     return "i32";
        case TyKind::I64:     return "i64";
        case TyKind::U8:      return "u8";
        case TyKind::U16:     return "u16";
        case TyKind::U32:     return "u32";
        case TyKind::U64:     return "u64";
        case TyKind::F32:     return "f32";
        case TyKind::F64:     return "f64";
        case TyKind::Char:    return "char";
        case TyKind::Str:     return "str";
        case TyKind::Ptr:     return (inner ? inner->str() : "?") + "*";
        case TyKind::Array:   return (elemTy ? elemTy->str() : "?") + "[" + std::to_string(arraySize) + "]";
        case TyKind::Struct:  return name;
        case TyKind::Enum:    return name;
        case TyKind::Opaque:  return name;
        case TyKind::Generic: return genericName;
        case TyKind::Error:   return "<error>";
    }
    return "?";
}

std::string ResolvedTy::mangle() const {
    switch (kind) {
        case TyKind::Void:    return "void";
        case TyKind::Bool:    return "bool";
        case TyKind::I8:      return "i8";
        case TyKind::I16:     return "i16";
        case TyKind::I32:     return "i32";
        case TyKind::I64:     return "i64";
        case TyKind::U8:      return "u8";
        case TyKind::U16:     return "u16";
        case TyKind::U32:     return "u32";
        case TyKind::U64:     return "u64";
        case TyKind::F32:     return "f32";
        case TyKind::F64:     return "f64";
        case TyKind::Char:    return "char";
        case TyKind::Str:     return "str";
        case TyKind::Ptr:     return "p" + (inner ? inner->mangle() : "void");
        case TyKind::Array:   return "a" + std::to_string(arraySize) + (elemTy ? elemTy->mangle() : "void");
        case TyKind::Struct:
        case TyKind::Enum:
        case TyKind::Opaque: {
            std::string s = name;
            if (!typeArgs.empty()) {
                s += "G";
                for (auto& ta : typeArgs) s += ta->mangle();
            }
            return s;
        }
        case TyKind::Generic: return genericName;
        case TyKind::Error:   return "err";
    }
    return "unknown";
}

std::string ResolvedTy::llvmType() const {
    switch (kind) {
        case TyKind::Void:    return "void";
        case TyKind::Bool:    return "i1";
        case TyKind::I8:  case TyKind::U8:  case TyKind::Char: return "i8";
        case TyKind::I16: case TyKind::U16: return "i16";
        case TyKind::I32: case TyKind::U32: return "i32";
        case TyKind::I64: case TyKind::U64: return "i64";
        case TyKind::F32:     return "float";
        case TyKind::F64:     return "double";
        case TyKind::Str:     return "ptr";
        case TyKind::Ptr:     return "ptr";
        case TyKind::Array:
            if (elemTy)
                return "[" + std::to_string(arraySize) + " x " + elemTy->llvmType() + "]";
        return "[0 x i8]";
        case TyKind::Struct: {
            return "%" + name;
        }
        case TyKind::Enum: {
            std::string n = name;
            for (auto& ta : typeArgs) n += "_" + ta->mangle();
            return "%" + n;
        }
        case TyKind::Opaque:
            return "%" + name;
        case TyKind::Generic:
            return "ptr";
        case TyKind::Error:
            return "i32";
    }
    return "i32";
}

bool ResolvedTy::equals(const ResolvedTy& o) const {
    if (kind == TyKind::Error || o.kind == TyKind::Error) return false;
    if (kind != o.kind) return false;
    switch (kind) {
        case TyKind::Ptr:
            return inner && o.inner && inner->equals(*o.inner);
        case TyKind::Array:
            return arraySize == o.arraySize && elemTy && o.elemTy && elemTy->equals(*o.elemTy);
        case TyKind::Struct:
        case TyKind::Enum:
        case TyKind::Opaque:
            return name == o.name;
        case TyKind::Generic:
            return genericName == o.genericName;
        default:
            return true;
    }
}

ResolvedTyPtr applySubst(const ResolvedTyPtr& ty, const SubstMap& subst) {
    if (!ty) return ty;
    if (ty->isGeneric()) {
        auto it = subst.find(ty->genericName);
        if (it != subst.end()) return it->second;
        return ty;
    }
    if (ty->isPtr() && ty->inner) {
        auto ni = applySubst(ty->inner, subst);
        if (ni == ty->inner) return ty;
        return Ty::Ptr(ni);
    }
    if (ty->isArray() && ty->elemTy) {
        auto ne = applySubst(ty->elemTy, subst);
        if (ne == ty->elemTy) return ty;
        return Ty::Array(ne, ty->arraySize);
    }
    if ((ty->isStruct() || ty->isEnum()) && !ty->typeArgs.empty()) {
        bool changed = false;
        std::vector<ResolvedTyPtr> newArgs;
        for (auto& ta : ty->typeArgs) {
            auto na = applySubst(ta, subst);
            if (na != ta) changed = true;
            newArgs.push_back(na);
        }
        if (!changed) return ty;
        auto nt = std::make_shared<ResolvedTy>(*ty);
        nt->typeArgs = std::move(newArgs);
        return nt;
    }
    return ty;
}

ResolvedTyPtr TypeResolver::resolve(const TypeNode& tn, const SubstMap& subst) const {
    return std::visit([&](auto& x) -> ResolvedTyPtr {
        using T = std::decay_t<decltype(x)>;

        if constexpr (std::is_same_v<T, PrimType>) {
            switch (x.kind) {
                case PrimKind::Void: return Ty::Void();
                case PrimKind::I8:   return Ty::I8();
                case PrimKind::I16:  return Ty::I16();
                case PrimKind::I32:  return Ty::I32();
                case PrimKind::I64:  return Ty::I64();
                case PrimKind::U8:   return Ty::U8();
                case PrimKind::U16:  return Ty::U16();
                case PrimKind::U32:  return Ty::U32();
                case PrimKind::U64:  return Ty::U64();
                case PrimKind::Bool: return Ty::Bool();
                case PrimKind::Char: return Ty::Char();
                case PrimKind::Str:  return Ty::Str();
                case PrimKind::F32:  return Ty::F32();
                case PrimKind::F64:  return Ty::F64();
            }
        }

        if constexpr (std::is_same_v<T, HashType>) {
            return Ty::Void();
        }

        if constexpr (std::is_same_v<T, PtrType>) {
            if (!x.inner) return Ty::Ptr(Ty::Void());
            return Ty::Ptr(resolve(*x.inner, subst));
        }

        if constexpr (std::is_same_v<T, ArrayType>) {
            auto elemTy = x.elem ? resolve(*x.elem, subst) : Ty::I8();
            int64_t sz = 0;
            if (x.size) {
                auto folded = evalConstIntExpr(*x.size, intConstResolver_);
                if (folded) {
                    sz = *folded;
                } else {
                    diag_.error(tn.loc,
                                "array size must be a compile-time integer constant",
                                lineFor(tn.loc));
                }
            }
            return Ty::Array(elemTy, sz);
        }

        if constexpr (std::is_same_v<T, NamedType>) {
            auto it = subst.find(x.name);
            if (it != subst.end()) return it->second;

            {
                auto oit = opaques_.find(x.name);
                if (oit != opaques_.end()) {
                    if (!x.typeArgs.empty()) {
                        diag_.error(tn.loc,
                                    std::format("opaque type '{}' does not take type arguments", x.name),
                                    lineFor(tn.loc));
                        return Ty::I32();
                    }
                    return Ty::Opaque(x.name);
                }
            }

            {
                auto sit = structs_.find(x.name);
                if (sit != structs_.end()) {
                    return Ty::Named(TyKind::Struct, x.name);
                }
            }

            {
                auto eit = enums_.find(x.name);
                if (eit != enums_.end()) {
                    auto& info = eit->second;
                    if (!info->typeParams.empty() &&
                        x.typeArgs.size() != info->typeParams.size()) {
                        diag_.error(tn.loc,
                                    std::format("enum '{}' expects {} type argument(s), got {}",
                                                x.name,
                                                info->typeParams.size(),
                                                x.typeArgs.size()),
                                    lineFor(tn.loc));
                        return Ty::I32();
                        }
                        auto t = Ty::Named(TyKind::Enum, x.name);
                    for (auto& ta : x.typeArgs)
                        t->typeArgs.push_back(resolve(*ta, subst));
                    return t;
                }
            }

            if (x.name.size() == 1 && std::isupper(x.name[0])) {
                return Ty::Generic(x.name);
            }

            diag_.error(tn.loc,
                        std::format("unknown type '{}'", x.name),
                        lineFor(tn.loc));
            return Ty::I32();
        }

        return Ty::Void();
    }, tn.v);
}
