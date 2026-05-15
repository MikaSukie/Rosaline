#pragma once
#include "ast.hpp"
#include <functional>
#include <map>
#include <optional>

enum class TyKind {
    Void, Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    IPtr, UPtr, ISize, USize,
    F32, F64,
    Char, Str,
    Ptr,
    Array,
    Struct,
    Enum,
    Opaque,
    Generic,
    Error,
};

struct ResolvedTy;
using ResolvedTyPtr = std::shared_ptr<ResolvedTy>;

struct ResolvedTy {
    TyKind      kind = TyKind::Void;
    ResolvedTyPtr inner;
    ResolvedTyPtr elemTy;
    int64_t       arraySize = 0;
    std::string   name;
    std::vector<ResolvedTyPtr> typeArgs;
    std::string   genericName;

    bool isInteger() const {
        return kind == TyKind::I8   || kind == TyKind::I16  ||
               kind == TyKind::I32  || kind == TyKind::I64  ||
               kind == TyKind::U8   || kind == TyKind::U16  ||
               kind == TyKind::U32  || kind == TyKind::U64  ||
               kind == TyKind::IPtr || kind == TyKind::UPtr ||
               kind == TyKind::ISize|| kind == TyKind::USize||
               kind == TyKind::Char || kind == TyKind::Bool;
    }
    bool isSigned() const {
        return kind == TyKind::I8   || kind == TyKind::I16  ||
               kind == TyKind::I32  || kind == TyKind::I64  ||
               kind == TyKind::IPtr || kind == TyKind::ISize;
    }
    bool isUnsigned() const {
        return kind == TyKind::U8   || kind == TyKind::U16  ||
               kind == TyKind::U32  || kind == TyKind::U64  ||
               kind == TyKind::UPtr || kind == TyKind::USize;
    }
    bool isFloat()  const { return kind == TyKind::F32 || kind == TyKind::F64; }
    bool isVoid()   const { return kind == TyKind::Void;    }
    bool isStr()    const { return kind == TyKind::Str;     }
    bool isPtr()    const { return kind == TyKind::Ptr;     }
    bool isArray()  const { return kind == TyKind::Array;   }
    bool isStruct() const { return kind == TyKind::Struct;  }
    bool isEnum()   const { return kind == TyKind::Enum;    }
    bool isOpaque() const { return kind == TyKind::Opaque;  }
    bool isGeneric()const { return kind == TyKind::Generic; }
    bool isError()  const { return kind == TyKind::Error;   }
    bool isNumeric()const { return isInteger() || isFloat();}

    int bitWidth() const {
        switch (kind) {
        case TyKind::I8: case TyKind::U8: case TyKind::Char:   return 8;
        case TyKind::I16: case TyKind::U16:                    return 16;
        case TyKind::Bool:                                     return 1;
        case TyKind::I32: case TyKind::U32:                    return 32;
        case TyKind::I64: case TyKind::U64:                    return 64;
        case TyKind::IPtr: case TyKind::UPtr:
        case TyKind::ISize: case TyKind::USize:                return 64;
        default: return 0;
        }
    }

    int byteSize() const {
        switch (kind) {
        case TyKind::I8: case TyKind::U8: case TyKind::Char: case TyKind::Bool: return 1;
        case TyKind::I16: case TyKind::U16: return 2;
        case TyKind::I32: case TyKind::U32: case TyKind::F32: return 4;
        case TyKind::I64: case TyKind::U64: case TyKind::F64: return 8;
        case TyKind::IPtr: case TyKind::UPtr:
        case TyKind::ISize: case TyKind::USize:               return 8;
        case TyKind::Ptr: case TyKind::Str: return 8;
        case TyKind::Void: return 0;
        case TyKind::Array: return (int)(arraySize * (elemTy ? elemTy->byteSize() : 0));
        default: return 8;
        }
    }

    std::string llvmType() const;

    std::string str() const;

    bool equals(const ResolvedTy& other) const;

    std::string mangle() const;
};

namespace Ty {
    inline ResolvedTyPtr make(TyKind k) {
        auto t = std::make_shared<ResolvedTy>();
        t->kind = k;
        return t;
    }
    inline ResolvedTyPtr Void()  { return make(TyKind::Void);  }
    inline ResolvedTyPtr Bool()  { return make(TyKind::Bool);  }
    inline ResolvedTyPtr I8()    { return make(TyKind::I8);    }
    inline ResolvedTyPtr I16()   { return make(TyKind::I16);   }
    inline ResolvedTyPtr I32()   { return make(TyKind::I32);   }
    inline ResolvedTyPtr I64()   { return make(TyKind::I64);   }
    inline ResolvedTyPtr U8()    { return make(TyKind::U8);    }
    inline ResolvedTyPtr U16()   { return make(TyKind::U16);   }
    inline ResolvedTyPtr U32()   { return make(TyKind::U32);   }
    inline ResolvedTyPtr U64()   { return make(TyKind::U64);   }
    inline ResolvedTyPtr IPtr()  { return make(TyKind::IPtr);  }
    inline ResolvedTyPtr UPtr()  { return make(TyKind::UPtr);  }
    inline ResolvedTyPtr ISize() { return make(TyKind::ISize); }
    inline ResolvedTyPtr USize() { return make(TyKind::USize); }
    inline ResolvedTyPtr F32()   { return make(TyKind::F32);   }
    inline ResolvedTyPtr F64()   { return make(TyKind::F64);   }
    inline ResolvedTyPtr Char()  { return make(TyKind::Char);  }
    inline ResolvedTyPtr Str()   { return make(TyKind::Str);   }
    inline ResolvedTyPtr Ptr(ResolvedTyPtr inner) {
        auto t = make(TyKind::Ptr);
        t->inner = std::move(inner);
        return t;
    }
    inline ResolvedTyPtr Array(ResolvedTyPtr elem, int64_t size) {
        auto t = make(TyKind::Array);
        t->elemTy    = std::move(elem);
        t->arraySize = size;
        return t;
    }
    inline ResolvedTyPtr Named(TyKind k, std::string name) {
        auto t = make(k);
        t->name = std::move(name);
        return t;
    }
    inline ResolvedTyPtr Opaque(std::string name) {
        auto t = make(TyKind::Opaque);
        t->name = std::move(name);
        return t;
    }
    inline ResolvedTyPtr Generic(std::string name) {
        auto t = make(TyKind::Generic);
        t->genericName = std::move(name);
        return t;
    }
    inline ResolvedTyPtr Error() { return make(TyKind::Error); }
}

struct OpaqueTypeInfo {
    std::string name;
};

struct StructTypeInfo {
    std::string              name;
    bool                     packed = false;
    std::vector<std::string> fieldNames;
    std::vector<ResolvedTyPtr> fieldTypes;

    int fieldIndex(const std::string& fname) const {
        for (int i = 0; i < (int)fieldNames.size(); ++i)
            if (fieldNames[i] == fname) return i;
        return -1;
    }
};

struct EnumVariantInfo {
    std::string                name;
    int                        tag;
    std::vector<ResolvedTyPtr> payloadTypes;
};

struct EnumTypeInfo {
    std::string                    name;
    std::vector<std::string>       typeParams;
    std::vector<EnumVariantInfo>   variants;
    int                            maxPayloadBytes = 0;

    int variantIndex(const std::string& vname) const {
        for (int i = 0; i < (int)variants.size(); ++i)
            if (variants[i].name == vname) return i;
        return -1;
    }
};

using SubstMap = std::unordered_map<std::string, ResolvedTyPtr>;

ResolvedTyPtr applySubst(const ResolvedTyPtr& ty, const SubstMap& subst);

struct MonoKey {
    std::string              funcName;
    std::vector<std::string> typeArgMangled;

    std::string mangledName() const {
        std::string s = "__ROSALINE__" + funcName;
        for (auto& t : typeArgMangled) s += "_" + t;
        return s;
    }

    bool operator==(const MonoKey& o) const {
        return funcName == o.funcName && typeArgMangled == o.typeArgMangled;
    }
};

struct MonoKeyHash {
    size_t operator()(const MonoKey& k) const {
        size_t h = std::hash<std::string>{}(k.funcName);
        for (auto& t : k.typeArgMangled)
            h ^= std::hash<std::string>{}(t) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class TypeResolver {
public:
    using IntConstResolver = std::function<std::optional<int64_t>(const std::string&)>;

    TypeResolver(
        const std::unordered_map<std::string, std::shared_ptr<OpaqueTypeInfo>>& opaques,
        const std::unordered_map<std::string, std::shared_ptr<StructTypeInfo>>& structs,
        const std::unordered_map<std::string, std::shared_ptr<EnumTypeInfo>>&   enums,
        DiagEngine& diag,
        std::shared_ptr<SourceFile> sf = nullptr,
        IntConstResolver intConstResolver = nullptr)
        : opaques_(opaques), structs_(structs), enums_(enums), diag_(diag), sf_(std::move(sf)),
          intConstResolver_(std::move(intConstResolver)) {}

    ResolvedTyPtr resolve(const TypeNode& tn, const SubstMap& subst = {}) const;

private:
    const std::unordered_map<std::string, std::shared_ptr<OpaqueTypeInfo>>& opaques_;
    const std::unordered_map<std::string, std::shared_ptr<StructTypeInfo>>& structs_;
    const std::unordered_map<std::string, std::shared_ptr<EnumTypeInfo>>&   enums_;
    DiagEngine& diag_;
    std::shared_ptr<SourceFile> sf_;
    IntConstResolver intConstResolver_;

    std::string lineFor(const SourceLoc& loc) const {
        if (sf_) return sf_->getLine(loc.line);
        return {};
    }
};
