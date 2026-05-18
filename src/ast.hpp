#pragma once
#include "common.hpp"
#include <variant>

struct TypeNode;
struct Stmt;
struct Expr;

using TypeNodePtr = std::unique_ptr<TypeNode>;
using StmtPtr     = std::unique_ptr<Stmt>;
using ExprPtr     = std::unique_ptr<Expr>;

enum class PrimKind {
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    IPtr, UPtr, ISize, USize,
    Bool, Char, Str,
    F32, F64,
    Void,
};

struct PrimType  { PrimKind kind; };
struct NamedType { std::string name; std::vector<TypeNodePtr> typeArgs; };
struct PtrType   { TypeNodePtr inner; };
struct ArrayType { TypeNodePtr elem; ExprPtr size; };
struct HashType  {};
struct FuncType  {
    std::vector<TypeNodePtr> paramTypes;
    TypeNodePtr              retType;
};

struct TypeNode {
    SourceLoc loc;
    std::variant<PrimType, NamedType, PtrType, ArrayType, HashType, FuncType> v;

    bool isPrim() const   { return std::holds_alternative<PrimType>(v);  }
    bool isNamed() const  { return std::holds_alternative<NamedType>(v); }
    bool isPtr() const    { return std::holds_alternative<PtrType>(v);   }
    bool isArray() const  { return std::holds_alternative<ArrayType>(v); }
    bool isHash() const   { return std::holds_alternative<HashType>(v);  }
    bool isFuncType() const { return std::holds_alternative<FuncType>(v); }
    bool isVoid() const   { return isPrim() && std::get<PrimType>(v).kind == PrimKind::Void; }

    const PrimType&  asPrim()  const { return std::get<PrimType>(v); }
    const NamedType& asNamed() const { return std::get<NamedType>(v);}
    const PtrType&   asPtr()   const { return std::get<PtrType>(v);  }
    const ArrayType& asArray() const { return std::get<ArrayType>(v);}
    const FuncType&  asFuncType() const { return std::get<FuncType>(v); }

    TypeNodePtr clone() const;

    std::string str() const;
};

struct Param {
    TypeNodePtr type;
    std::string name;
    bool        nomd = false;
    SourceLoc   loc;
};

struct StructField {
    TypeNodePtr type;
    std::string name;
    SourceLoc   loc;
};

struct StructDecl {
    std::string              name;
    bool                     packed = false;
    std::vector<std::string> typeParams;
    std::vector<StructField> fields;
    SourceLoc                loc;
};

struct OpaqueDecl {
    std::string name;
    SourceLoc   loc;
};

struct EnumVariant {
    std::string              name;
    std::vector<TypeNodePtr> payload;
    SourceLoc                loc;
};

struct EnumDecl {
    std::string              name;
    std::vector<std::string> typeParams;
    std::vector<EnumVariant> variants;
    SourceLoc                loc;
};

struct IntLitExpr    { int64_t val; };
struct FloatLitExpr  { double  val; };
struct StringLitExpr { std::string val; };
struct CharLitExpr   { char val; };
struct BoolLitExpr   { bool val; };
struct NullLitExpr   {};

struct IdentExpr     { std::string name; };

struct BinOpExpr {
    std::string op;
    ExprPtr     lhs, rhs;
};

struct UnaryOpExpr {
    std::string op;
    ExprPtr     operand;
};

struct CastExpr {
    TypeNodePtr type;
    ExprPtr     operand;
};

struct CallExpr {
    ExprPtr                  callee;
    std::vector<TypeNodePtr> typeArgs;
    std::vector<ExprPtr>     args;
};

struct FieldAccessExpr {
    ExprPtr     obj;
    std::string field;
};

struct StructInitField {
    std::string name;
    ExprPtr     value;
};

struct StructInitExpr {
    TypeNodePtr type;
    std::vector<StructInitField> fields;
};

struct EnumAccessExpr {
    std::string          enumName;
    std::string          variant;
    std::vector<ExprPtr> args;
};

struct IndexExpr {
    ExprPtr arr, idx;
};

struct AddressOfExpr { ExprPtr operand; };
struct DerefExpr     { ExprPtr operand; };

struct ReadonlyExpr  { ExprPtr operand; };
struct TakeExpr      { ExprPtr operand; };
struct ReleaseExpr   { ExprPtr operand; };
struct CrumbleExpr   {
    ExprPtr     operand;
    int         readLimit  = -1;
    int         writeLimit = -1;
};

struct TypecaseArm {
    TypeNodePtr     matchType;
    bool            isFallback = false;
    std::vector<StmtPtr> body;
};

struct TypeswitchExpr {
    std::string              param;
    std::vector<TypecaseArm> arms;
};

struct MatchArm {
    std::string              variantName;
    std::vector<std::string> bindNames;
    std::vector<StmtPtr>     body;
    bool                     isWildcard = false;
    SourceLoc                loc;
};

struct MatchExpr {
    ExprPtr               subject;
    std::vector<MatchArm> arms;
};

struct AssignExpr {
    std::string op;
    ExprPtr     lhs, rhs;
};

struct TernaryExpr {
    ExprPtr cond;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
};

struct ArrayLitExpr {
    std::vector<ExprPtr> elems;
};

struct Expr {
    SourceLoc loc;
    std::variant<
        IntLitExpr, FloatLitExpr, StringLitExpr, CharLitExpr, BoolLitExpr,
        IdentExpr, BinOpExpr, UnaryOpExpr, CastExpr,
        CallExpr, FieldAccessExpr, EnumAccessExpr, IndexExpr,
        AddressOfExpr, DerefExpr,
        ReadonlyExpr, TakeExpr, ReleaseExpr, CrumbleExpr,
        TypeswitchExpr, MatchExpr, AssignExpr,
        TernaryExpr, StructInitExpr, ArrayLitExpr
    > v;

    template<typename T>
    bool is() const { return std::holds_alternative<T>(v); }

    template<typename T>
    T& as() { return std::get<T>(v); }

    template<typename T>
    const T& as() const { return std::get<T>(v); }
};

struct ExprStmt     { ExprPtr expr; };
struct BlockStmt    { std::vector<StmtPtr> stmts; };
struct ReturnStmt   { ExprPtr value;};
struct BreakStmt    {};
struct ContinueStmt {};

struct VarDeclStmt {
    bool        nomd = false;
    TypeNodePtr type;
    std::string name;
    ExprPtr     init;
};

struct IfStmt {
    ExprPtr              cond;
    std::vector<StmtPtr> thenBody;
    std::vector<StmtPtr> elseBody;
};

struct WhileStmt {
    ExprPtr              cond;
    std::vector<StmtPtr> body;
};

struct Stmt {
    SourceLoc loc;
    std::variant<
        ExprStmt, BlockStmt, ReturnStmt, BreakStmt, ContinueStmt,
        VarDeclStmt, IfStmt, WhileStmt
    > v;

    template<typename T>
    bool is() const { return std::holds_alternative<T>(v); }

    template<typename T>
    T& as() { return std::get<T>(v); }

    template<typename T>
    const T& as() const { return std::get<T>(v); }
};

struct FuncDecl {
    std::string              name;
    std::vector<Param>       params;
    TypeNodePtr              retType;
    std::vector<std::string> genericParams;
    std::vector<StmtPtr>     body;
    bool                     isMain = false;
    SourceLoc                loc;
};

struct ExternDecl {
    std::string        name;
    TypeNodePtr        globalType;
    std::vector<Param> params;
    TypeNodePtr        retType;
    bool               isVariadic = false;
    bool               isUnsafe   = false;
    bool               isGlobal   = false;
    SourceLoc          loc;
};

struct Insertion {
    std::string   targetFunc;
    TypecaseArm   arm;
    std::string   forParam;
    SourceLoc     loc;
};

struct GlobalDecl {
    bool        nomd = false;
    TypeNodePtr type;
    std::string name;
    ExprPtr     init;
    SourceLoc   loc;
};

struct DirectiveDecl {
    std::string key;
    std::string value;
    SourceLoc   loc;
};

struct Program {
    std::vector<std::unique_ptr<OpaqueDecl>>    opaques;
    std::vector<std::unique_ptr<StructDecl>>    structs;
    std::vector<std::unique_ptr<EnumDecl>>      enums;
    std::vector<std::unique_ptr<ExternDecl>>    externs;
    std::vector<std::unique_ptr<FuncDecl>>      funcs;
    std::vector<std::unique_ptr<GlobalDecl>>    globals;
    std::vector<Insertion>                      insertions;
    std::vector<DirectiveDecl>                  directives;

    FuncDecl* findFunc(const std::string& name) const {
        for (auto& f : funcs)
            if (f->name == name) return f.get();
        return nullptr;
    }

    GlobalDecl* findGlobal(const std::string& name) const {
        for (auto& g : globals)
            if (g && g->name == name) return g.get();
        return nullptr;
    }

    const ExternDecl* findExtern(const std::string& name) const {
        for (auto& e : externs)
            if (e && e->name == name) return e.get();
        return nullptr;
    }
};

inline std::string TypeNode::str() const {
    return std::visit([](auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, PrimType>) {
            switch (x.kind) {
            case PrimKind::I8:   return "i8";
            case PrimKind::I16:  return "i16";
            case PrimKind::I32:  return "i32";
            case PrimKind::I64:  return "i64";
            case PrimKind::U8:   return "u8";
            case PrimKind::U16:  return "u16";
            case PrimKind::U32:  return "u32";
            case PrimKind::U64:  return "u64";
            case PrimKind::IPtr: return "iptr";
            case PrimKind::UPtr: return "uptr";
            case PrimKind::ISize:return "isize";
            case PrimKind::USize:return "usize";
            case PrimKind::Bool: return "bool";
            case PrimKind::Char: return "char";
            case PrimKind::Str:  return "str";
            case PrimKind::F32:  return "f32";
            case PrimKind::F64:  return "f64";
            case PrimKind::Void: return "#";
            }
        } else if constexpr (std::is_same_v<T, NamedType>) {
            if (x.typeArgs.empty()) return x.name;
            std::string s = x.name + "<";
            for (size_t i = 0; i < x.typeArgs.size(); ++i) {
                if (i) s += ",";
                s += x.typeArgs[i]->str();
            }
            return s + ">";
        } else if constexpr (std::is_same_v<T, PtrType>) {
            return x.inner->str() + "*";
        } else if constexpr (std::is_same_v<T, ArrayType>) {
            return x.elem->str() + "[]";
        } else if constexpr (std::is_same_v<T, FuncType>) {
            std::string s = "fn(";
            for (size_t i = 0; i < x.paramTypes.size(); ++i) {
                if (i) s += ",";
                s += x.paramTypes[i]->str();
            }
            s += ")->";
            s += x.retType ? x.retType->str() : "#";
            return s;
        } else if constexpr (std::is_same_v<T, HashType>) {
            return "#";
        }
        return "?";
    }, v);
}

inline TypeNodePtr TypeNode::clone() const {
    auto n = std::make_unique<TypeNode>();
    n->loc = loc;
    n->v   = std::visit([](auto& x) -> decltype(TypeNode::v) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, PrimType>) {
            return PrimType{x.kind};
        } else if constexpr (std::is_same_v<T, NamedType>) {
            NamedType nt;
            nt.name = x.name;
            for (auto& ta : x.typeArgs)
                nt.typeArgs.push_back(ta->clone());
            return nt;
        } else if constexpr (std::is_same_v<T, PtrType>) {
            return PtrType{x.inner->clone()};
        } else if constexpr (std::is_same_v<T, ArrayType>) {
            ArrayType at;
            at.elem = x.elem->clone();
            return at;
        } else if constexpr (std::is_same_v<T, FuncType>) {
            FuncType ft;
            for (auto& pt : x.paramTypes)
                ft.paramTypes.push_back(pt->clone());
            if (x.retType) ft.retType = x.retType->clone();
            return ft;
        } else if constexpr (std::is_same_v<T, HashType>) {
            return HashType{};
        }
        return PrimType{PrimKind::Void};
    }, v);
    return n;
}

void dumpProgram(const Program& prog, int indent = 0);
void dumpFuncDecl(const FuncDecl& f, int indent = 0);
void dumpStmt(const Stmt& s, int indent = 0);
void dumpExpr(const Expr& e, int indent = 0);
