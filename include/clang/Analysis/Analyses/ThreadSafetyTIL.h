//===- ThreadSafetyTIL.h ---------------------------------------*- C++ --*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines a simple intermediate language that is used by the
// thread safety analysis (See ThreadSafety.cpp).  The thread safety analysis
// works by comparing mutex expressions, e.g.
//
// class A { Mutex mu; int dat GUARDED_BY(this->mu); }
// class B { A a; }
//
// void foo(B* b) {
//   (*b).a.mu.lock();     // locks (*b).a.mu
//   b->a.dat = 0;         // substitute &b->a for 'this';
//                         // requires lock on (&b->a)->mu
//   (b->a.mu).unlock();   // unlocks (b->a.mu)
// }
//
// As illustrated by the above example, clang Exprs are not well-suited to
// represent mutex expressions directly, since there is no easy way to compare
// Exprs for equivalence.  The thread safety analysis thus lowers clang Exprs
// into a simple intermediate language (IL).  The IL supports:
//
// (1) comparisons for semantic equality of expressions
// (2) SSA renaming of variables
// (3) wildcards and pattern matching over expressions
// (4) hash-based expression lookup
//
// The IL is currently very experimental, is intended only for use within
// the thread safety analysis, and is subject to change without notice.
// After the API stabilizes and matures, it may be appropriate to make this
// more generally available to other analyses.
//
// UNDER CONSTRUCTION.  USE AT YOUR OWN RISK.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_THREAD_SAFETY_TIL_H
#define LLVM_CLANG_THREAD_SAFETY_TIL_H

#include "clang/Analysis/Analyses/ThreadSafetyUtil.h"
#include "clang/AST/ExprCXX.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"

#include <cassert>
#include <cstddef>
#include <utility>


namespace clang {
namespace threadSafety {
namespace til {

using llvm::StringRef;
using clang::SourceLocation;


enum TIL_Opcode {
#define TIL_OPCODE_DEF(X) COP_##X,
#include "clang/Analysis/Analyses/ThreadSafetyOps.def"
#undef TIL_OPCODE_DEF
  COP_MAX
};


typedef clang::BinaryOperatorKind TIL_BinaryOpcode;
typedef clang::UnaryOperatorKind TIL_UnaryOpcode;
typedef clang::CastKind TIL_CastOpcode;


enum TraversalKind {
  TRV_Normal,
  TRV_Lazy, // subexpression may need to be traversed lazily
  TRV_Tail  // subexpression occurs in a tail position
};


// Base class for AST nodes in the typed intermediate language.
class SExpr {
public:
  TIL_Opcode opcode() const { return static_cast<TIL_Opcode>(Opcode); }

  // Subclasses of SExpr must define the following:
  //
  // This(const This& E, ...) {
  //   copy constructor: construct copy of E, with some additional arguments.
  // }
  //
  // template <class V> typename V::R_SExpr traverse(V &Visitor) {
  //   traverse all subexpressions, following the traversal/rewriter interface
  // }
  //
  // template <class C> typename C::CType compare(CType* E, C& Cmp) {
  //   compare all subexpressions, following the comparator interface
  // }

  void *operator new(size_t S, clang::threadSafety::til::MemRegionRef &R) {
    return ::operator new(S, R);
  }

protected:
  SExpr(TIL_Opcode Op) : Opcode(Op), Reserved(0), Flags(0) {}
  SExpr(const SExpr &E) : Opcode(E.Opcode), Reserved(0), Flags(E.Flags) {}

  const unsigned char Opcode;
  unsigned char Reserved;
  unsigned short Flags;

private:
  SExpr() LLVM_DELETED_FUNCTION;

  // SExpr objects must be created in an arena and cannot be deleted.
  void *operator new(size_t) LLVM_DELETED_FUNCTION;
  void operator delete(void *) LLVM_DELETED_FUNCTION;
};


// Class for owning references to SExprs.
// Includes attach/detach logic for counting variable references and lazy
// rewriting strategies.
class SExprRef {
public:
  SExprRef() : Ptr(nullptr) { }
  SExprRef(std::nullptr_t P) : Ptr(nullptr) { }
  SExprRef(SExprRef &&R) : Ptr(R.Ptr) { R.Ptr = nullptr; }

  // Defined after Variable and Future, below.
  inline SExprRef(SExpr *P);
  inline ~SExprRef();

  SExpr       *get()       { return Ptr; }
  const SExpr *get() const { return Ptr; }

  SExpr       *operator->()       { return get(); }
  const SExpr *operator->() const { return get(); }

  SExpr       &operator*()        { return *Ptr; }
  const SExpr &operator*() const  { return *Ptr; }

  bool operator==(const SExprRef &R) const { return Ptr == R.Ptr; }
  bool operator!=(const SExprRef &R) const { return !operator==(R); }
  bool operator==(const SExpr *P) const { return Ptr == P; }
  bool operator!=(const SExpr *P) const { return !operator==(P); }
  bool operator==(std::nullptr_t) const { return Ptr == nullptr; }
  bool operator!=(std::nullptr_t) const { return Ptr != nullptr; }

  inline void reset(SExpr *E);

private:
  inline void attach();
  inline void detach();

  SExpr *Ptr;
};


// Contains various helper functions for SExprs.
namespace ThreadSafetyTIL {
  inline bool isTrivial(const SExpr *E) {
    unsigned Op = E->opcode();
    return Op == COP_Variable || Op == COP_Literal || Op == COP_LiteralPtr;
  }
}

class Function;
class SFunction;
class BasicBlock;


// A named variable, e.g. "x".
//
// There are two distinct places in which a Variable can appear in the AST.
// A variable declaration introduces a new variable, and can occur in 3 places:
//   Let-expressions:           (Let (x = t) u)
//   Functions:                 (Function (x : t) u)
//   Self-applicable functions  (SFunction (x) t)
//
// If a variable occurs in any other location, it is a reference to an existing
// variable declaration -- e.g. 'x' in (x * y + z). To save space, we don't
// allocate a separate AST node for variable references; a reference is just a
// pointer to the original declaration.
class Variable : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Variable; }

  // Let-variable, function parameter, or self-variable
  enum VariableKind {
    VK_Let,
    VK_Fun,
    VK_SFun
  };

  // These are defined after SExprRef contructor, below
  inline Variable(VariableKind K, SExpr *D = nullptr,
                  const clang::ValueDecl *Cvd = nullptr);
  inline Variable(SExpr *D = nullptr, const clang::ValueDecl *Cvd = nullptr);
  inline Variable(const Variable &Vd, SExpr *D);

  VariableKind kind() const { return static_cast<VariableKind>(Flags); }

  const StringRef name() const { return Cvdecl ? Cvdecl->getName() : "_x"; }
  const clang::ValueDecl *clangDecl() const { return Cvdecl; }

  // Returns the definition (for let vars) or type (for parameter & self vars)
  SExpr *definition() { return Definition.get(); }
  const SExpr *definition() const { return Definition.get(); }

  void attachVar() const { ++NumUses; }
  void detachVar() const { assert(NumUses > 0); --NumUses; }

  unsigned getID() const { return Id; }
  unsigned getBlockID() const { return BlockID; }

  void setID(unsigned Bid, unsigned I) {
    BlockID = static_cast<unsigned short>(Bid);
    Id = static_cast<unsigned short>(I);
  }
  void setClangDecl(const clang::ValueDecl *VD) { Cvdecl = VD; }
  void setDefinition(SExpr *E);

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    // This routine is only called for variable references.
    return Visitor.reduceVariableRef(this);
  }

  template <class C> typename C::CType compare(Variable* E, C& Cmp) {
    return Cmp.compareVariableRefs(this, E);
  }

private:
  friend class Function;
  friend class SFunction;
  friend class BasicBlock;

  // Function, SFunction, and BasicBlock will reset the kind.
  void setKind(VariableKind K) { Flags = K; }

  SExprRef Definition;             // The TIL type or definition
  const clang::ValueDecl *Cvdecl;  // The clang declaration for this variable.

  unsigned short BlockID;
  unsigned short Id;
  mutable unsigned NumUses;
};


// Placeholder for an expression that has not yet been created.
// Used to implement lazy copy and rewriting strategies.
class Future : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Future; }

  enum FutureStatus {
    FS_pending,
    FS_evaluating,
    FS_done
  };

  Future() :
    SExpr(COP_Future), Status(FS_pending), Result(nullptr), Location(nullptr)
  {}
private:
  virtual ~Future() LLVM_DELETED_FUNCTION;
public:

  // Registers the location in the AST where this future is stored.
  // Forcing the future will automatically update the AST.
  static inline void registerLocation(SExprRef *Member) {
    if (Future *F = dyn_cast_or_null<Future>(Member->get()))
      F->Location = Member;
  }

  // A lazy rewriting strategy should subclass Future and override this method.
  virtual SExpr *create() { return nullptr; }

  // Return the result of this future if it exists, otherwise return null.
  SExpr *maybeGetResult() {
    return Result;
  }

  // Return the result of this future; forcing it if necessary.
  SExpr *result() {
    switch (Status) {
    case FS_pending:
      force();
      return Result;
    case FS_evaluating:
      return nullptr; // infinite loop; illegal recursion.
    case FS_done:
      return Result;
    }
  }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    assert(Result && "Cannot traverse Future that has not been forced.");
    return Visitor.traverse(Result);
  }

  template <class C> typename C::CType compare(Future* E, C& Cmp) {
    if (!Result || !E->Result)
      return Cmp.comparePointers(this, E);
    return Cmp.compare(Result, E->Result);
  }

private:
  // Force the future.
  inline void force();

  FutureStatus Status;
  SExpr *Result;
  SExprRef *Location;
};


inline void SExprRef::attach() {
  if (!Ptr)
    return;

  TIL_Opcode Op = Ptr->opcode();
  if (Op == COP_Variable) {
    cast<Variable>(Ptr)->attachVar();
  } else if (Op == COP_Future) {
    cast<Future>(Ptr)->registerLocation(this);
  }
}

inline void SExprRef::detach() {
  if (Ptr && Ptr->opcode() == COP_Variable) {
    cast<Variable>(Ptr)->detachVar();
  }
}

inline SExprRef::SExprRef(SExpr *P) : Ptr(P) {
  attach();
}

inline SExprRef::~SExprRef() {
  detach();
}

inline void SExprRef::reset(SExpr *P) {
  detach();
  Ptr = P;
  attach();
}


inline Variable::Variable(VariableKind K, SExpr *D, const clang::ValueDecl *Cvd)
    : SExpr(COP_Variable), Definition(D), Cvdecl(Cvd),
      BlockID(0), Id(0),  NumUses(0) {
  Flags = K;
}

inline Variable::Variable(SExpr *D, const clang::ValueDecl *Cvd)
    : SExpr(COP_Variable), Definition(D), Cvdecl(Cvd),
      BlockID(0), Id(0),  NumUses(0) {
  Flags = VK_Let;
}

inline Variable::Variable(const Variable &Vd, SExpr *D) // rewrite constructor
    : SExpr(Vd), Definition(D), Cvdecl(Vd.Cvdecl),
      BlockID(0), Id(0), NumUses(0) {
  Flags = Vd.kind();
}

inline void Variable::setDefinition(SExpr *E) {
  Definition.reset(E);
}

void Future::force() {
  Status = FS_evaluating;
  SExpr *R = create();
  Result = R;
  if (Location)
    Location->reset(R);
  Status = FS_done;
}


// Placeholder for C++ expressions that cannot be represented in the TIL.
class Undefined : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Undefined; }

  Undefined(const clang::Stmt *S = nullptr) : SExpr(COP_Undefined), Cstmt(S) {}
  Undefined(const Undefined &U) : SExpr(U), Cstmt(U.Cstmt) {}

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    return Visitor.reduceUndefined(*this);
  }

  template <class C> typename C::CType compare(Undefined* E, C& Cmp) {
    return Cmp.comparePointers(Cstmt, E->Cstmt);
  }

private:
  const clang::Stmt *Cstmt;
};


// Placeholder for a wildcard that matches any other expression.
class Wildcard : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Wildcard; }

  Wildcard() : SExpr(COP_Wildcard) {}
  Wildcard(const Wildcard &W) : SExpr(W) {}

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    return Visitor.reduceWildcard(*this);
  }

  template <class C> typename C::CType compare(Wildcard* E, C& Cmp) {
    return Cmp.trueResult();
  }
};


// Base class for literal values.
class Literal : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Literal; }

  Literal(const clang::Expr *C) : SExpr(COP_Literal), Cexpr(C) {}
  Literal(const Literal &L) : SExpr(L), Cexpr(L.Cexpr) {}

  // The clang expression for this literal.
  const clang::Expr *clangExpr() const { return Cexpr; }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    return Visitor.reduceLiteral(*this);
  }

  template <class C> typename C::CType compare(Literal* E, C& Cmp) {
    // TODO -- use value, not pointer equality
    return Cmp.comparePointers(Cexpr, E->Cexpr);
  }

private:
  const clang::Expr *Cexpr;
};

// Literal pointer to an object allocated in memory.
// At compile time, pointer literals are represented by symbolic names.
class LiteralPtr : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_LiteralPtr; }

  LiteralPtr(const clang::ValueDecl *D) : SExpr(COP_LiteralPtr), Cvdecl(D) {}
  LiteralPtr(const LiteralPtr &R) : SExpr(R), Cvdecl(R.Cvdecl) {}

  // The clang declaration for the value that this pointer points to.
  const clang::ValueDecl *clangDecl() const { return Cvdecl; }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    return Visitor.reduceLiteralPtr(*this);
  }

  template <class C> typename C::CType compare(LiteralPtr* E, C& Cmp) {
    return Cmp.comparePointers(Cvdecl, E->Cvdecl);
  }

private:
  const clang::ValueDecl *Cvdecl;
};

// A function -- a.k.a. lambda abstraction.
// Functions with multiple arguments are created by currying,
// e.g. (function (x: Int) (function (y: Int) (add x y)))
class Function : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Function; }

  Function(Variable *Vd, SExpr *Bd)
      : SExpr(COP_Function), VarDecl(Vd), Body(Bd) {
    Vd->setKind(Variable::VK_Fun);
  }
  Function(const Function &F, Variable *Vd, SExpr *Bd) // rewrite constructor
      : SExpr(F), VarDecl(Vd), Body(Bd) {
    Vd->setKind(Variable::VK_Fun);
  }

  Variable *variableDecl()  { return VarDecl; }
  const Variable *variableDecl() const { return VarDecl; }

  SExpr *body() { return Body.get(); }
  const SExpr *body() const { return Body.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    // This is a variable declaration, so traverse the definition.
    typename V::R_SExpr E0 = Visitor.traverse(VarDecl->Definition, TRV_Lazy);
    // Tell the rewriter to enter the scope of the function.
    Variable *Nvd = Visitor.enterScope(*VarDecl, E0);
    typename V::R_SExpr E1 = Visitor.traverse(Body);
    Visitor.exitScope(*VarDecl);
    return Visitor.reduceFunction(*this, Nvd, E1);
  }

  template <class C> typename C::CType compare(Function* E, C& Cmp) {
    typename C::CType Ct =
      Cmp.compare(VarDecl->definition(), E->VarDecl->definition());
    if (Cmp.notTrue(Ct))
      return Ct;
    Cmp.enterScope(variableDecl(), E->variableDecl());
    Ct = Cmp.compare(body(), E->body());
    Cmp.leaveScope();
    return Ct;
  }

private:
  Variable *VarDecl;
  SExprRef Body;
};


// A self-applicable function.
// A self-applicable function can be applied to itself.  It's useful for
// implementing objects and late binding
class SFunction : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_SFunction; }

  SFunction(Variable *Vd, SExpr *B)
      : SExpr(COP_SFunction), VarDecl(Vd), Body(B) {
    assert(Vd->Definition == nullptr);
    Vd->setKind(Variable::VK_SFun);
    Vd->Definition.reset(this);
  }
  SFunction(const SFunction &F, Variable *Vd, SExpr *B) // rewrite constructor
      : SExpr(F), VarDecl(Vd), Body(B) {
    assert(Vd->Definition == nullptr);
    Vd->setKind(Variable::VK_SFun);
    Vd->Definition.reset(this);
  }

  Variable *variableDecl() { return VarDecl; }
  const Variable *variableDecl() const { return VarDecl; }

  SExpr *body() { return Body.get(); }
  const SExpr *body() const { return Body.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    // A self-variable points to the SFunction itself.
    // A rewrite must introduce the variable with a null definition, and update
    // it after 'this' has been rewritten.
    Variable *Nvd = Visitor.enterScope(*VarDecl, nullptr /* def */);
    typename V::R_SExpr E1 = Visitor.traverse(Body);
    Visitor.exitScope(*VarDecl);
    // A rewrite operation will call SFun constructor to set Vvd->Definition.
    return Visitor.reduceSFunction(*this, Nvd, E1);
  }

  template <class C> typename C::CType compare(SFunction* E, C& Cmp) {
    Cmp.enterScope(variableDecl(), E->variableDecl());
    typename C::CType Ct = Cmp.compare(body(), E->body());
    Cmp.leaveScope();
    return Ct;
  }

private:
  Variable *VarDecl;
  SExprRef Body;
};


// A block of code -- e.g. the body of a function.
class Code : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Code; }

  Code(SExpr *T, SExpr *B) : SExpr(COP_Code), ReturnType(T), Body(B) {}
  Code(const Code &C, SExpr *T, SExpr *B) // rewrite constructor
      : SExpr(C), ReturnType(T), Body(B) {}

  SExpr *returnType() { return ReturnType.get(); }
  const SExpr *returnType() const { return ReturnType.get(); }

  SExpr *body() { return Body.get(); }
  const SExpr *body() const { return Body.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Nt = Visitor.traverse(ReturnType, TRV_Lazy);
    typename V::R_SExpr Nb = Visitor.traverse(Body, TRV_Lazy);
    return Visitor.reduceCode(*this, Nt, Nb);
  }

  template <class C> typename C::CType compare(Code* E, C& Cmp) {
    typename C::CType Ct = Cmp.compare(returnType(), E->returnType());
    if (Cmp.notTrue(Ct))
      return Ct;
    return Cmp.compare(body(), E->body());
  }

private:
  SExprRef ReturnType;
  SExprRef Body;
};


// Apply an argument to a function
class Apply : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Apply; }

  Apply(SExpr *F, SExpr *A) : SExpr(COP_Apply), Fun(F), Arg(A) {}
  Apply(const Apply &A, SExpr *F, SExpr *Ar)  // rewrite constructor
      : SExpr(A), Fun(F), Arg(Ar)
  {}

  SExpr *fun() { return Fun.get(); }
  const SExpr *fun() const { return Fun.get(); }

  SExpr *arg() { return Arg.get(); }
  const SExpr *arg() const { return Arg.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Nf = Visitor.traverse(Fun);
    typename V::R_SExpr Na = Visitor.traverse(Arg);
    return Visitor.reduceApply(*this, Nf, Na);
  }

  template <class C> typename C::CType compare(Apply* E, C& Cmp) {
    typename C::CType Ct = Cmp.compare(fun(), E->fun());
    if (Cmp.notTrue(Ct))
      return Ct;
    return Cmp.compare(arg(), E->arg());
  }

private:
  SExprRef Fun;
  SExprRef Arg;
};


// Apply a self-argument to a self-applicable function
class SApply : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_SApply; }

  SApply(SExpr *Sf, SExpr *A = nullptr) : SExpr(COP_SApply), Sfun(Sf), Arg(A) {}
  SApply(SApply &A, SExpr *Sf, SExpr *Ar = nullptr) // rewrite constructor
      : SExpr(A), Sfun(Sf), Arg(Ar) {}

  SExpr *sfun() { return Sfun.get(); }
  const SExpr *sfun() const { return Sfun.get(); }

  SExpr *arg() { return Arg.get() ? Arg.get() : Sfun.get(); }
  const SExpr *arg() const { return Arg.get() ? Arg.get() : Sfun.get(); }

  bool isDelegation() const { return Arg == nullptr; }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Nf = Visitor.traverse(Sfun);
    typename V::R_SExpr Na = Arg.get() ? Visitor.traverse(Arg) : nullptr;
    return Visitor.reduceSApply(*this, Nf, Na);
  }

  template <class C> typename C::CType compare(SApply* E, C& Cmp) {
    typename C::CType Ct = Cmp.compare(sfun(), E->sfun());
    if (Cmp.notTrue(Ct) || (!arg() && !E->arg()))
      return Ct;
    return Cmp.compare(arg(), E->arg());
  }

private:
  SExprRef Sfun;
  SExprRef Arg;
};


// Project a named slot from a C++ struct or class.
class Project : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Project; }

  Project(SExpr *R, clang::ValueDecl *Cvd)
      : SExpr(COP_Project), Rec(R), Cvdecl(Cvd) {}
  Project(const Project &P, SExpr *R) : SExpr(P), Rec(R), Cvdecl(P.Cvdecl) {}

  SExpr *record() { return Rec.get(); }
  const SExpr *record() const { return Rec.get(); }

  const clang::ValueDecl *clangValueDecl() const { return Cvdecl; }

  StringRef slotName() const { return Cvdecl->getName(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Nr = Visitor.traverse(Rec);
    return Visitor.reduceProject(*this, Nr);
  }

  template <class C> typename C::CType compare(Project* E, C& Cmp) {
    typename C::CType Ct = Cmp.compare(record(), E->record());
    if (Cmp.notTrue(Ct))
      return Ct;
    return Cmp.comparePointers(Cvdecl, E->Cvdecl);
  }

private:
  SExprRef Rec;
  clang::ValueDecl *Cvdecl;
};


// Call a function (after all arguments have been applied).
class Call : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Call; }

  Call(SExpr *T, const clang::CallExpr *Ce = nullptr)
      : SExpr(COP_Call), Target(T), Cexpr(Ce) {}
  Call(const Call &C, SExpr *T) : SExpr(C), Target(T), Cexpr(C.Cexpr) {}

  SExpr *target() { return Target.get(); }
  const SExpr *target() const { return Target.get(); }

  const clang::CallExpr *clangCallExpr() const { return Cexpr; }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Nt = Visitor.traverse(Target);
    return Visitor.reduceCall(*this, Nt);
  }

  template <class C> typename C::CType compare(Call* E, C& Cmp) {
    return Cmp.compare(target(), E->target());
  }

private:
  SExprRef Target;
  const clang::CallExpr *Cexpr;
};


// Allocate memory for a new value on the heap or stack.
class Alloc : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Call; }

  enum AllocKind {
    AK_Stack,
    AK_Heap
  };

  Alloc(SExpr *D, AllocKind K) : SExpr(COP_Alloc), Dtype(D) { Flags = K; }
  Alloc(const Alloc &A, SExpr *Dt) : SExpr(A), Dtype(Dt) { Flags = A.kind(); }

  AllocKind kind() const { return static_cast<AllocKind>(Flags); }

  SExpr *dataType() { return Dtype.get(); }
  const SExpr *dataType() const { return Dtype.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Nd = Visitor.traverse(Dtype);
    return Visitor.reduceAlloc(*this, Nd);
  }

  template <class C> typename C::CType compare(Alloc* E, C& Cmp) {
    typename C::CType Ct = Cmp.compareIntegers(kind(), E->kind());
    if (Cmp.notTrue(Ct))
      return Ct;
    return Cmp.compare(dataType(), E->dataType());
  }

private:
  SExprRef Dtype;
};


// Load a value from memory.
class Load : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Load; }

  Load(SExpr *P) : SExpr(COP_Load), Ptr(P) {}
  Load(const Load &L, SExpr *P) : SExpr(L), Ptr(P) {}

  SExpr *pointer() { return Ptr.get(); }
  const SExpr *pointer() const { return Ptr.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Np = Visitor.traverse(Ptr);
    return Visitor.reduceLoad(*this, Np);
  }

  template <class C> typename C::CType compare(Load* E, C& Cmp) {
    return Cmp.compare(pointer(), E->pointer());
  }

private:
  SExprRef Ptr;
};


// Store a value to memory.
// Source is a pointer, destination is the value to store.
class Store : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Store; }

  Store(SExpr *P, SExpr *V) : SExpr(COP_Store), Dest(P), Source(V) {}
  Store(const Store &S, SExpr *P, SExpr *V) : SExpr(S), Dest(P), Source(V) {}

  SExpr *destination() { return Dest.get(); }  // Address to store to
  const SExpr *destination() const { return Dest.get(); }

  SExpr *source() { return Source.get(); }     // Value to store
  const SExpr *source() const { return Source.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Np = Visitor.traverse(Dest);
    typename V::R_SExpr Nv = Visitor.traverse(Source);
    return Visitor.reduceStore(*this, Np, Nv);
  }

  template <class C> typename C::CType compare(Store* E, C& Cmp) {
    typename C::CType Ct = Cmp.compare(destination(), E->destination());
    if (Cmp.notTrue(Ct))
      return Ct;
    return Cmp.compare(source(), E->source());
  }

private:
  SExprRef Dest;
  SExprRef Source;
};


// If p is a reference to an array, then first(p) is a reference to the first
// element.  The usual array notation p[i]  becomes first(p + i).
class ArrayFirst : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_ArrayFirst; }

  ArrayFirst(SExpr *A) : SExpr(COP_ArrayFirst), Array(A) {}
  ArrayFirst(const ArrayFirst &E, SExpr *A) : SExpr(E), Array(A) {}

  SExpr *array() { return Array.get(); }
  const SExpr *array() const { return Array.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Na = Visitor.traverse(Array);
    return Visitor.reduceArrayFirst(*this, Na);
  }

  template <class C> typename C::CType compare(ArrayFirst* E, C& Cmp) {
    return Cmp.compare(array(), E->array());
  }

private:
  SExprRef Array;
};


// Pointer arithmetic, restricted to arrays only.
// If p is a reference to an array, then p + n, where n is an integer, is
// a reference to a subarray.
class ArrayAdd : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_ArrayAdd; }

  ArrayAdd(SExpr *A, SExpr *N) : SExpr(COP_ArrayAdd), Array(A), Index(N) {}
  ArrayAdd(const ArrayAdd &E, SExpr *A, SExpr *N)
    : SExpr(E), Array(A), Index(N) {}

  SExpr *array() { return Array.get(); }
  const SExpr *array() const { return Array.get(); }

  SExpr *index() { return Index.get(); }
  const SExpr *index() const { return Index.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Na = Visitor.traverse(Array);
    typename V::R_SExpr Ni = Visitor.traverse(Index);
    return Visitor.reduceArrayAdd(*this, Na, Ni);
  }

  template <class C> typename C::CType compare(ArrayAdd* E, C& Cmp) {
    typename C::CType Ct = Cmp.compare(array(), E->array());
    if (Cmp.notTrue(Ct))
      return Ct;
    return Cmp.compare(index(), E->index());
  }

private:
  SExprRef Array;
  SExprRef Index;
};


// Simple unary operation -- e.g. !, ~, etc.
class UnaryOp : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_UnaryOp; }

  UnaryOp(TIL_UnaryOpcode Op, SExpr *E) : SExpr(COP_UnaryOp), Expr0(E) {
    Flags = Op;
  }
  UnaryOp(const UnaryOp &U, SExpr *E) : SExpr(U) { Flags = U.Flags; }

  TIL_UnaryOpcode unaryOpcode() const {
    return static_cast<TIL_UnaryOpcode>(Flags);
  }

  SExpr *expr() { return Expr0.get(); }
  const SExpr *expr() const { return Expr0.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Ne = Visitor.traverse(Expr0);
    return Visitor.reduceUnaryOp(*this, Ne);
  }

  template <class C> typename C::CType compare(UnaryOp* E, C& Cmp) {
    typename C::CType Ct =
      Cmp.compareIntegers(unaryOpcode(), E->unaryOpcode());
    if (Cmp.notTrue(Ct))
      return Ct;
    return Cmp.compare(expr(), E->expr());
  }

private:
  SExprRef Expr0;
};


// Simple binary operation -- e.g. +, -, etc.
class BinaryOp : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_BinaryOp; }

  BinaryOp(TIL_BinaryOpcode Op, SExpr *E0, SExpr *E1)
      : SExpr(COP_BinaryOp), Expr0(E0), Expr1(E1) {
    Flags = Op;
  }
  BinaryOp(const BinaryOp &B, SExpr *E0, SExpr *E1)
      : SExpr(B), Expr0(E0), Expr1(E1) {
    Flags = B.Flags;
  }

  TIL_BinaryOpcode binaryOpcode() const {
    return static_cast<TIL_BinaryOpcode>(Flags);
  }

  SExpr *expr0() { return Expr0.get(); }
  const SExpr *expr0() const { return Expr0.get(); }

  SExpr *expr1() { return Expr1.get(); }
  const SExpr *expr1() const { return Expr1.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Ne0 = Visitor.traverse(Expr0);
    typename V::R_SExpr Ne1 = Visitor.traverse(Expr1);
    return Visitor.reduceBinaryOp(*this, Ne0, Ne1);
  }

  template <class C> typename C::CType compare(BinaryOp* E, C& Cmp) {
    typename C::CType Ct =
      Cmp.compareIntegers(binaryOpcode(), E->binaryOpcode());
    if (Cmp.notTrue(Ct))
      return Ct;
    Ct = Cmp.compare(expr0(), E->expr0());
    if (Cmp.notTrue(Ct))
      return Ct;
    return Cmp.compare(expr1(), E->expr1());
  }

private:
  SExprRef Expr0;
  SExprRef Expr1;
};


// Cast expression
class Cast : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Cast; }

  Cast(TIL_CastOpcode Op, SExpr *E) : SExpr(COP_Cast), Expr0(E) { Flags = Op; }
  Cast(const Cast &C, SExpr *E) : SExpr(C), Expr0(E) { Flags = C.Flags; }

  TIL_CastOpcode castOpcode() const {
    return static_cast<TIL_CastOpcode>(Flags);
  }

  SExpr *expr() { return Expr0.get(); }
  const SExpr *expr() const { return Expr0.get(); }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Ne = Visitor.traverse(Expr0);
    return Visitor.reduceCast(*this, Ne);
  }

  template <class C> typename C::CType compare(Cast* E, C& Cmp) {
    typename C::CType Ct =
      Cmp.compareIntegers(castOpcode(), E->castOpcode());
    if (Cmp.notTrue(Ct))
      return Ct;
    return Cmp.compare(expr(), E->expr());
  }

private:
  SExprRef Expr0;
};



class BasicBlock;

// An SCFG is a control-flow graph.  It consists of a set of basic blocks, each
// of which terminates in a branch to another basic block.  There is one
// entry point, and one exit point.
class SCFG : public SExpr {
public:
  typedef SimpleArray<BasicBlock *> BlockArray;
  typedef BlockArray::iterator iterator;
  typedef BlockArray::const_iterator const_iterator;

  static bool classof(const SExpr *E) { return E->opcode() == COP_SCFG; }

  SCFG(MemRegionRef A, unsigned Nblocks)
      : SExpr(COP_SCFG), Blocks(A, Nblocks),
        Entry(nullptr), Exit(nullptr) {}
  SCFG(const SCFG &Cfg, BlockArray &&Ba) // steals memory from Ba
      : SExpr(COP_SCFG), Blocks(std::move(Ba)), Entry(nullptr), Exit(nullptr) {
    // TODO: set entry and exit!
  }

  iterator begin() { return Blocks.begin(); }
  iterator end() { return Blocks.end(); }

  const_iterator begin() const { return cbegin(); }
  const_iterator end() const { return cend(); }

  const_iterator cbegin() const { return Blocks.cbegin(); }
  const_iterator cend() const { return Blocks.cend(); }

  const BasicBlock *entry() const { return Entry; }
  BasicBlock *entry() { return Entry; }
  const BasicBlock *exit() const { return Exit; }
  BasicBlock *exit() { return Exit; }

  void add(BasicBlock *BB);
  void setEntry(BasicBlock *BB) { Entry = BB; }
  void setExit(BasicBlock *BB) { Exit = BB; }

  template <class V> typename V::R_SExpr traverse(V &Visitor);

  template <class C> typename C::CType compare(SCFG *E, C &Cmp) {
    // TODO -- implement CFG comparisons
    return Cmp.comparePointers(this, E);
  }

private:
  BlockArray Blocks;
  BasicBlock *Entry;
  BasicBlock *Exit;
};


// A basic block is part of an SCFG, and can be treated as a function in
// continuation passing style.  It consists of a sequence of phi nodes, which
// are "arguments" to the function, followed by a sequence of instructions.
// Both arguments and instructions define new variables.  It ends with a
// branch or goto to another basic block in the same SCFG.
class BasicBlock {
public:
  typedef SimpleArray<Variable*> VarArray;

  BasicBlock(MemRegionRef A, unsigned Nargs, unsigned Nins,
             SExpr *Term = nullptr)
      : BlockID(0), NumVars(0), NumPredecessors(0), Parent(nullptr),
        Args(A, Nargs), Instrs(A, Nins), Terminator(Term) {}
  BasicBlock(const BasicBlock &B, VarArray &&As, VarArray &&Is, SExpr *T)
      : BlockID(0),  NumVars(B.NumVars), NumPredecessors(B.NumPredecessors),
        Parent(nullptr), Args(std::move(As)), Instrs(std::move(Is)),
        Terminator(T) {}

  unsigned blockID() const { return BlockID; }
  unsigned numPredecessors() const { return NumPredecessors; }

  const BasicBlock *parent() const { return Parent; }
  BasicBlock *parent() { return Parent; }

  const VarArray &arguments() const { return Args; }
  VarArray &arguments() { return Args; }

  const VarArray &instructions() const { return Instrs; }
  VarArray &instructions() { return Instrs; }

  const SExpr *terminator() const { return Terminator.get(); }
  SExpr *terminator() { return Terminator.get(); }

  void setBlockID(unsigned i) { BlockID = i; }
  void setParent(BasicBlock *P) { Parent = P; }
  void setNumPredecessors(unsigned NP) { NumPredecessors = NP; }
  void setTerminator(SExpr *E) { Terminator.reset(E); }

  void addArgument(Variable *V) {
    V->setID(BlockID, NumVars++);
    Args.push_back(V);
  }
  void addInstruction(Variable *V) {
    V->setID(BlockID, NumVars++);
    Instrs.push_back(V);
  }

  template <class V> BasicBlock *traverse(V &Visitor) {
    typename V::template Container<Variable*> Nas(Visitor, Args.size());
    typename V::template Container<Variable*> Nis(Visitor, Instrs.size());

    for (auto *A : Args) {
      typename V::R_SExpr Ne = Visitor.traverse(A->Definition);
      Variable *Nvd = Visitor.enterScope(*A, Ne);
      Nas.push_back(Nvd);
    }
    for (auto *I : Instrs) {
      typename V::R_SExpr Ne = Visitor.traverse(I->Definition);
      Variable *Nvd = Visitor.enterScope(*I, Ne);
      Nis.push_back(Nvd);
    }
    typename V::R_SExpr Nt = Visitor.traverse(Terminator);

    // TODO: use reverse iterator
    for (unsigned J = 0, JN = Instrs.size(); J < JN; ++J)
      Visitor.exitScope(*Instrs[JN-J]);
    for (unsigned I = 0, IN = Instrs.size(); I < IN; ++I)
      Visitor.exitScope(*Args[IN-I]);

    return Visitor.reduceBasicBlock(*this, Nas, Nis, Nt);
  }

  template <class C> typename C::CType compare(BasicBlock *E, C &Cmp) {
    // TODO: implement CFG comparisons
    return Cmp.comparePointers(this, E);
  }

private:
  friend class SCFG;

  unsigned BlockID;
  unsigned NumVars;
  unsigned NumPredecessors; // Number of blocks which jump to this one.

  BasicBlock *Parent;       // The parent block is the enclosing lexical scope.
                            // The parent dominates this block.
  VarArray Args;            // Phi nodes.  One argument per predecessor.
  VarArray Instrs;
  SExprRef Terminator;
};


inline void SCFG::add(BasicBlock *BB) {
  BB->setBlockID(Blocks.size());
  Blocks.push_back(BB);
}


template <class V>
typename V::R_SExpr SCFG::traverse(V &Visitor) {
  Visitor.enterCFG(*this);
  typename V::template Container<BasicBlock *> Bbs(Visitor, Blocks.size());
  for (auto *B : Blocks) {
    BasicBlock *Nbb = B->traverse(Visitor);
    Bbs.push_back(Nbb);
  }
  Visitor.exitCFG(*this);
  return Visitor.reduceSCFG(*this, Bbs);
}


class Phi : public SExpr {
public:
  // TODO: change to SExprRef
  typedef SimpleArray<SExpr *> ValArray;

  // In minimal SSA form, all Phi nodes are MultiVal.
  // During conversion to SSA, incomplete Phi nodes may be introduced, which
  // are later determined to be SingleVal.
  enum Status {
    PH_MultiVal = 0, // Phi node has multiple distinct values.  (Normal)
    PH_SingleVal,    // Phi node has one distinct value, and can be eliminated
    PH_Incomplete    // Phi node is incomplete
  };

  static bool classof(const SExpr *E) { return E->opcode() == COP_Phi; }

  Phi(MemRegionRef A, unsigned Nvals) : SExpr(COP_Phi), Values(A, Nvals) {}
  Phi(const Phi &P, ValArray &&Vs) // steals memory of Vs
      : SExpr(COP_Phi), Values(std::move(Vs)) {}

  const ValArray &values() const { return Values; }
  ValArray &values() { return Values; }

  Status status() const { return static_cast<Status>(Flags); }
  void setStatus(Status s) { Flags = s; }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::template Container<typename V::R_SExpr> Nvs(Visitor,
                                                            Values.size());
    for (auto *Val : Values) {
      typename V::R_SExpr Nv = Visitor.traverse(Val);
      Nvs.push_back(Nv);
    }
    return Visitor.reducePhi(*this, Nvs);
  }

  template <class C> typename C::CType compare(Phi *E, C &Cmp) {
    // TODO: implement CFG comparisons
    return Cmp.comparePointers(this, E);
  }

private:
  ValArray Values;
};


class Goto : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Goto; }

  Goto(BasicBlock *B, unsigned Index)
      : SExpr(COP_Goto), TargetBlock(B), Index(0) {}
  Goto(const Goto &G, BasicBlock *B, unsigned I)
      : SExpr(COP_Goto), TargetBlock(B), Index(I) {}

  const BasicBlock *targetBlock() const { return TargetBlock; }
  BasicBlock *targetBlock() { return TargetBlock; }

  unsigned index() const { return Index; }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    // TODO -- rewrite indices properly
    BasicBlock *Ntb = Visitor.reduceBasicBlockRef(TargetBlock);
    return Visitor.reduceGoto(*this, Ntb, Index);
  }

  template <class C> typename C::CType compare(Goto *E, C &Cmp) {
    // TODO -- implement CFG comparisons
    return Cmp.comparePointers(this, E);
  }

private:
  BasicBlock *TargetBlock;
  unsigned Index;   // Index into Phi nodes of target block.
};


class Branch : public SExpr {
public:
  static bool classof(const SExpr *E) { return E->opcode() == COP_Branch; }

  Branch(SExpr *C, BasicBlock *T, BasicBlock *E)
      : SExpr(COP_Branch), Condition(C), ThenBlock(T), ElseBlock(E),
        ThenIndex(0), ElseIndex(0)
  {}
  Branch(const Branch &Br, SExpr *C, BasicBlock *T, BasicBlock *E)
      : SExpr(COP_Branch), Condition(C), ThenBlock(T), ElseBlock(E),
        ThenIndex(0), ElseIndex(0)
  {}

  const SExpr *condition() const { return Condition; }
  SExpr *condition() { return Condition; }

  const BasicBlock *thenBlock() const { return ThenBlock; }
  BasicBlock *thenBlock() { return ThenBlock; }

  const BasicBlock *elseBlock() const { return ElseBlock; }
  BasicBlock *elseBlock() { return ElseBlock; }

  unsigned thenIndex() const { return ThenIndex; }
  unsigned elseIndex() const { return ElseIndex; }

  template <class V> typename V::R_SExpr traverse(V &Visitor) {
    typename V::R_SExpr Nc = Visitor.traverse(Condition);
    BasicBlock *Ntb = Visitor.reduceBasicBlockRef(ThenBlock);
    BasicBlock *Nte = Visitor.reduceBasicBlockRef(ElseBlock);
    return Visitor.reduceBranch(*this, Nc, Ntb, Nte);
  }

  template <class C> typename C::CType compare(Branch *E, C &Cmp) {
    // TODO -- implement CFG comparisons
    return Cmp.comparePointers(this, E);
  }

private:
  SExpr *Condition;
  BasicBlock *ThenBlock;
  BasicBlock *ElseBlock;
  unsigned ThenIndex;
  unsigned ElseIndex;
};


SExpr *getCanonicalVal(SExpr *E);
void simplifyIncompleteArg(Variable *V, til::Phi *Ph);




} // end namespace til
} // end namespace threadSafety
} // end namespace clang

#endif // LLVM_CLANG_THREAD_SAFETY_TIL_H
