//===----- CGOpenMPRuntime.cpp - Interface to OpenMP Runtimes -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This provides a class for OpenMP runtime code generation.
//
//===----------------------------------------------------------------------===//

#include "CGOpenMPRuntime.h"
#include "CodeGenFunction.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>

using namespace clang;
using namespace CodeGen;

namespace {
/// \brief API for captured statement code generation in OpenMP constructs.
class CGOpenMPRegionInfo : public CodeGenFunction::CGCapturedStmtInfo {
public:
  CGOpenMPRegionInfo(const OMPExecutableDirective &D, const CapturedStmt &CS,
                     const VarDecl *ThreadIDVar)
      : CGCapturedStmtInfo(CS, CR_OpenMP), ThreadIDVar(ThreadIDVar),
        Directive(D) {
    assert(ThreadIDVar != nullptr && "No ThreadID in OpenMP region.");
  }

  /// \brief Gets a variable or parameter for storing global thread id
  /// inside OpenMP construct.
  const VarDecl *getThreadIDVariable() const { return ThreadIDVar; }

  /// \brief Gets an LValue for the current ThreadID variable.
  LValue getThreadIDVariableLValue(CodeGenFunction &CGF);

  static bool classof(const CGCapturedStmtInfo *Info) {
    return Info->getKind() == CR_OpenMP;
  }

  /// \brief Emit the captured statement body.
  void EmitBody(CodeGenFunction &CGF, Stmt *S) override;

  /// \brief Get the name of the capture helper.
  StringRef getHelperName() const override { return ".omp_outlined."; }

private:
  /// \brief A variable or parameter storing global thread id for OpenMP
  /// constructs.
  const VarDecl *ThreadIDVar;
  /// \brief OpenMP executable directive associated with the region.
  const OMPExecutableDirective &Directive;
};
} // namespace

LValue CGOpenMPRegionInfo::getThreadIDVariableLValue(CodeGenFunction &CGF) {
  return CGF.MakeNaturalAlignAddrLValue(
      CGF.GetAddrOfLocalVar(ThreadIDVar),
      CGF.getContext().getPointerType(ThreadIDVar->getType()));
}

void CGOpenMPRegionInfo::EmitBody(CodeGenFunction &CGF, Stmt *S) {
  CodeGenFunction::OMPPrivateScope PrivateScope(CGF);
  CGF.EmitOMPPrivateClause(Directive, PrivateScope);
  CGF.EmitOMPFirstprivateClause(Directive, PrivateScope);
  if (PrivateScope.Privatize()) {
    // Emit implicit barrier to synchronize threads and avoid data races.
    auto Flags = static_cast<CGOpenMPRuntime::OpenMPLocationFlags>(
        CGOpenMPRuntime::OMP_IDENT_KMPC |
        CGOpenMPRuntime::OMP_IDENT_BARRIER_IMPL);
    CGF.CGM.getOpenMPRuntime().EmitOMPBarrierCall(CGF, Directive.getLocStart(),
                                                  Flags);
  }
  CGCapturedStmtInfo::EmitBody(CGF, S);
}

CGOpenMPRuntime::CGOpenMPRuntime(CodeGenModule &CGM)
    : CGM(CGM), DefaultOpenMPPSource(nullptr) {
  IdentTy = llvm::StructType::create(
      "ident_t", CGM.Int32Ty /* reserved_1 */, CGM.Int32Ty /* flags */,
      CGM.Int32Ty /* reserved_2 */, CGM.Int32Ty /* reserved_3 */,
      CGM.Int8PtrTy /* psource */, nullptr);
  // Build void (*kmpc_micro)(kmp_int32 *global_tid, kmp_int32 *bound_tid,...)
  llvm::Type *MicroParams[] = {llvm::PointerType::getUnqual(CGM.Int32Ty),
                               llvm::PointerType::getUnqual(CGM.Int32Ty)};
  Kmpc_MicroTy = llvm::FunctionType::get(CGM.VoidTy, MicroParams, true);
  KmpCriticalNameTy = llvm::ArrayType::get(CGM.Int32Ty, /*NumElements*/ 8);
}

llvm::Value *
CGOpenMPRuntime::EmitOpenMPOutlinedFunction(const OMPExecutableDirective &D,
                                            const VarDecl *ThreadIDVar) {
  const CapturedStmt *CS = cast<CapturedStmt>(D.getAssociatedStmt());
  CodeGenFunction CGF(CGM, true);
  CGOpenMPRegionInfo CGInfo(D, *CS, ThreadIDVar);
  CGF.CapturedStmtInfo = &CGInfo;
  return CGF.GenerateCapturedStmtFunction(*CS);
}

llvm::Value *
CGOpenMPRuntime::GetOrCreateDefaultOpenMPLocation(OpenMPLocationFlags Flags) {
  llvm::Value *Entry = OpenMPDefaultLocMap.lookup(Flags);
  if (!Entry) {
    if (!DefaultOpenMPPSource) {
      // Initialize default location for psource field of ident_t structure of
      // all ident_t objects. Format is ";file;function;line;column;;".
      // Taken from
      // http://llvm.org/svn/llvm-project/openmp/trunk/runtime/src/kmp_str.c
      DefaultOpenMPPSource =
          CGM.GetAddrOfConstantCString(";unknown;unknown;0;0;;");
      DefaultOpenMPPSource =
          llvm::ConstantExpr::getBitCast(DefaultOpenMPPSource, CGM.Int8PtrTy);
    }
    auto DefaultOpenMPLocation = new llvm::GlobalVariable(
        CGM.getModule(), IdentTy, /*isConstant*/ true,
        llvm::GlobalValue::PrivateLinkage, /*Initializer*/ nullptr);
    DefaultOpenMPLocation->setUnnamedAddr(true);

    llvm::Constant *Zero = llvm::ConstantInt::get(CGM.Int32Ty, 0, true);
    llvm::Constant *Values[] = {Zero,
                                llvm::ConstantInt::get(CGM.Int32Ty, Flags),
                                Zero, Zero, DefaultOpenMPPSource};
    llvm::Constant *Init = llvm::ConstantStruct::get(IdentTy, Values);
    DefaultOpenMPLocation->setInitializer(Init);
    OpenMPDefaultLocMap[Flags] = DefaultOpenMPLocation;
    return DefaultOpenMPLocation;
  }
  return Entry;
}

llvm::Value *CGOpenMPRuntime::EmitOpenMPUpdateLocation(
    CodeGenFunction &CGF, SourceLocation Loc, OpenMPLocationFlags Flags) {
  // If no debug info is generated - return global default location.
  if (CGM.getCodeGenOpts().getDebugInfo() == CodeGenOptions::NoDebugInfo ||
      Loc.isInvalid())
    return GetOrCreateDefaultOpenMPLocation(Flags);

  assert(CGF.CurFn && "No function in current CodeGenFunction.");

  llvm::Value *LocValue = nullptr;
  OpenMPLocThreadIDMapTy::iterator I = OpenMPLocThreadIDMap.find(CGF.CurFn);
  if (I != OpenMPLocThreadIDMap.end()) {
    LocValue = I->second.DebugLoc;
  } else {
    // Generate "ident_t .kmpc_loc.addr;"
    llvm::AllocaInst *AI = CGF.CreateTempAlloca(IdentTy, ".kmpc_loc.addr");
    AI->setAlignment(CGM.getDataLayout().getPrefTypeAlignment(IdentTy));
    auto &Elem = OpenMPLocThreadIDMap.FindAndConstruct(CGF.CurFn);
    Elem.second.DebugLoc = AI;
    LocValue = AI;

    CGBuilderTy::InsertPointGuard IPG(CGF.Builder);
    CGF.Builder.SetInsertPoint(CGF.AllocaInsertPt);
    CGF.Builder.CreateMemCpy(LocValue, GetOrCreateDefaultOpenMPLocation(Flags),
                             llvm::ConstantExpr::getSizeOf(IdentTy),
                             CGM.PointerAlignInBytes);
  }

  // char **psource = &.kmpc_loc_<flags>.addr.psource;
  llvm::Value *PSource =
      CGF.Builder.CreateConstInBoundsGEP2_32(LocValue, 0, IdentField_PSource);

  auto OMPDebugLoc = OpenMPDebugLocMap.lookup(Loc.getRawEncoding());
  if (OMPDebugLoc == nullptr) {
    SmallString<128> Buffer2;
    llvm::raw_svector_ostream OS2(Buffer2);
    // Build debug location
    PresumedLoc PLoc = CGF.getContext().getSourceManager().getPresumedLoc(Loc);
    OS2 << ";" << PLoc.getFilename() << ";";
    if (const FunctionDecl *FD =
            dyn_cast_or_null<FunctionDecl>(CGF.CurFuncDecl)) {
      OS2 << FD->getQualifiedNameAsString();
    }
    OS2 << ";" << PLoc.getLine() << ";" << PLoc.getColumn() << ";;";
    OMPDebugLoc = CGF.Builder.CreateGlobalStringPtr(OS2.str());
    OpenMPDebugLocMap[Loc.getRawEncoding()] = OMPDebugLoc;
  }
  // *psource = ";<File>;<Function>;<Line>;<Column>;;";
  CGF.Builder.CreateStore(OMPDebugLoc, PSource);

  return LocValue;
}

llvm::Value *CGOpenMPRuntime::GetOpenMPThreadID(CodeGenFunction &CGF,
                                                SourceLocation Loc) {
  assert(CGF.CurFn && "No function in current CodeGenFunction.");

  llvm::Value *ThreadID = nullptr;
  // Check whether we've already cached a load of the thread id in this
  // function.
  OpenMPLocThreadIDMapTy::iterator I = OpenMPLocThreadIDMap.find(CGF.CurFn);
  if (I != OpenMPLocThreadIDMap.end()) {
    ThreadID = I->second.ThreadID;
    if (ThreadID != nullptr)
      return ThreadID;
  }
  if (auto OMPRegionInfo =
                 dyn_cast_or_null<CGOpenMPRegionInfo>(CGF.CapturedStmtInfo)) {
    // Check if this an outlined function with thread id passed as argument.
    auto ThreadIDVar = OMPRegionInfo->getThreadIDVariable();
    auto LVal = OMPRegionInfo->getThreadIDVariableLValue(CGF);
    auto RVal = CGF.EmitLoadOfLValue(LVal, Loc);
    LVal = CGF.MakeNaturalAlignAddrLValue(RVal.getScalarVal(),
                                          ThreadIDVar->getType());
    ThreadID = CGF.EmitLoadOfLValue(LVal, Loc).getScalarVal();
    // If value loaded in entry block, cache it and use it everywhere in
    // function.
    if (CGF.Builder.GetInsertBlock() == CGF.AllocaInsertPt->getParent()) {
      auto &Elem = OpenMPLocThreadIDMap.FindAndConstruct(CGF.CurFn);
      Elem.second.ThreadID = ThreadID;
    }
  } else {
    // This is not an outlined function region - need to call __kmpc_int32
    // kmpc_global_thread_num(ident_t *loc).
    // Generate thread id value and cache this value for use across the
    // function.
    CGBuilderTy::InsertPointGuard IPG(CGF.Builder);
    CGF.Builder.SetInsertPoint(CGF.AllocaInsertPt);
    llvm::Value *Args[] = {EmitOpenMPUpdateLocation(CGF, Loc)};
    ThreadID = CGF.EmitRuntimeCall(
        CreateRuntimeFunction(OMPRTL__kmpc_global_thread_num), Args);
    auto &Elem = OpenMPLocThreadIDMap.FindAndConstruct(CGF.CurFn);
    Elem.second.ThreadID = ThreadID;
  }
  return ThreadID;
}

void CGOpenMPRuntime::FunctionFinished(CodeGenFunction &CGF) {
  assert(CGF.CurFn && "No function in current CodeGenFunction.");
  if (OpenMPLocThreadIDMap.count(CGF.CurFn))
    OpenMPLocThreadIDMap.erase(CGF.CurFn);
}

llvm::Type *CGOpenMPRuntime::getIdentTyPointerTy() {
  return llvm::PointerType::getUnqual(IdentTy);
}

llvm::Type *CGOpenMPRuntime::getKmpc_MicroPointerTy() {
  return llvm::PointerType::getUnqual(Kmpc_MicroTy);
}

llvm::Constant *
CGOpenMPRuntime::CreateRuntimeFunction(OpenMPRTLFunction Function) {
  llvm::Constant *RTLFn = nullptr;
  switch (Function) {
  case OMPRTL__kmpc_fork_call: {
    // Build void __kmpc_fork_call(ident_t *loc, kmp_int32 argc, kmpc_micro
    // microtask, ...);
    llvm::Type *TypeParams[] = {getIdentTyPointerTy(), CGM.Int32Ty,
                                getKmpc_MicroPointerTy()};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ true);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_fork_call");
    break;
  }
  case OMPRTL__kmpc_global_thread_num: {
    // Build kmp_int32 __kmpc_global_thread_num(ident_t *loc);
    llvm::Type *TypeParams[] = {getIdentTyPointerTy()};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.Int32Ty, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_global_thread_num");
    break;
  }
  case OMPRTL__kmpc_threadprivate_cached: {
    // Build void *__kmpc_threadprivate_cached(ident_t *loc,
    // kmp_int32 global_tid, void *data, size_t size, void ***cache);
    llvm::Type *TypeParams[] = {getIdentTyPointerTy(), CGM.Int32Ty,
                                CGM.VoidPtrTy, CGM.SizeTy,
                                CGM.VoidPtrTy->getPointerTo()->getPointerTo()};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidPtrTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_threadprivate_cached");
    break;
  }
  case OMPRTL__kmpc_critical: {
    // Build void __kmpc_critical(ident_t *loc, kmp_int32 global_tid,
    // kmp_critical_name *crit);
    llvm::Type *TypeParams[] = {
        getIdentTyPointerTy(), CGM.Int32Ty,
        llvm::PointerType::getUnqual(KmpCriticalNameTy)};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_critical");
    break;
  }
  case OMPRTL__kmpc_threadprivate_register: {
    // Build void __kmpc_threadprivate_register(ident_t *, void *data,
    // kmpc_ctor ctor, kmpc_cctor cctor, kmpc_dtor dtor);
    // typedef void *(*kmpc_ctor)(void *);
    auto KmpcCtorTy =
        llvm::FunctionType::get(CGM.VoidPtrTy, CGM.VoidPtrTy,
                                /*isVarArg*/ false)->getPointerTo();
    // typedef void *(*kmpc_cctor)(void *, void *);
    llvm::Type *KmpcCopyCtorTyArgs[] = {CGM.VoidPtrTy, CGM.VoidPtrTy};
    auto KmpcCopyCtorTy =
        llvm::FunctionType::get(CGM.VoidPtrTy, KmpcCopyCtorTyArgs,
                                /*isVarArg*/ false)->getPointerTo();
    // typedef void (*kmpc_dtor)(void *);
    auto KmpcDtorTy =
        llvm::FunctionType::get(CGM.VoidTy, CGM.VoidPtrTy, /*isVarArg*/ false)
            ->getPointerTo();
    llvm::Type *FnTyArgs[] = {getIdentTyPointerTy(), CGM.VoidPtrTy, KmpcCtorTy,
                              KmpcCopyCtorTy, KmpcDtorTy};
    auto FnTy = llvm::FunctionType::get(CGM.VoidTy, FnTyArgs,
                                        /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_threadprivate_register");
    break;
  }
  case OMPRTL__kmpc_end_critical: {
    // Build void __kmpc_end_critical(ident_t *loc, kmp_int32 global_tid,
    // kmp_critical_name *crit);
    llvm::Type *TypeParams[] = {
        getIdentTyPointerTy(), CGM.Int32Ty,
        llvm::PointerType::getUnqual(KmpCriticalNameTy)};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_end_critical");
    break;
  }
  case OMPRTL__kmpc_barrier: {
    // Build void __kmpc_barrier(ident_t *loc, kmp_int32 global_tid);
    llvm::Type *TypeParams[] = {getIdentTyPointerTy(), CGM.Int32Ty};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, /*Name*/ "__kmpc_barrier");
    break;
  }
  case OMPRTL__kmpc_push_num_threads: {
    // Build void __kmpc_push_num_threads(ident_t *loc, kmp_int32 global_tid,
    // kmp_int32 num_threads)
    llvm::Type *TypeParams[] = {getIdentTyPointerTy(), CGM.Int32Ty,
                                CGM.Int32Ty};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_push_num_threads");
    break;
  }
  case OMPRTL__kmpc_serialized_parallel: {
    // Build void __kmpc_serialized_parallel(ident_t *loc, kmp_int32
    // global_tid);
    llvm::Type *TypeParams[] = {getIdentTyPointerTy(), CGM.Int32Ty};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_serialized_parallel");
    break;
  }
  case OMPRTL__kmpc_end_serialized_parallel: {
    // Build void __kmpc_end_serialized_parallel(ident_t *loc, kmp_int32
    // global_tid);
    llvm::Type *TypeParams[] = {getIdentTyPointerTy(), CGM.Int32Ty};
    llvm::FunctionType *FnTy =
        llvm::FunctionType::get(CGM.VoidTy, TypeParams, /*isVarArg*/ false);
    RTLFn = CGM.CreateRuntimeFunction(FnTy, "__kmpc_end_serialized_parallel");
    break;
  }
  }
  return RTLFn;
}

llvm::Constant *
CGOpenMPRuntime::getOrCreateThreadPrivateCache(const VarDecl *VD) {
  // Lookup the entry, lazily creating it if necessary.
  return GetOrCreateInternalVariable(CGM.Int8PtrPtrTy,
                                     Twine(CGM.getMangledName(VD)) + ".cache.");
}

llvm::Value *CGOpenMPRuntime::getOMPAddrOfThreadPrivate(CodeGenFunction &CGF,
                                                        const VarDecl *VD,
                                                        llvm::Value *VDAddr,
                                                        SourceLocation Loc) {
  auto VarTy = VDAddr->getType()->getPointerElementType();
  llvm::Value *Args[] = {EmitOpenMPUpdateLocation(CGF, Loc),
                         GetOpenMPThreadID(CGF, Loc),
                         CGF.Builder.CreatePointerCast(VDAddr, CGM.Int8PtrTy),
                         CGM.getSize(CGM.GetTargetTypeStoreSize(VarTy)),
                         getOrCreateThreadPrivateCache(VD)};
  return CGF.EmitRuntimeCall(
      CreateRuntimeFunction(OMPRTL__kmpc_threadprivate_cached), Args);
}

void CGOpenMPRuntime::EmitOMPThreadPrivateVarInit(
    CodeGenFunction &CGF, llvm::Value *VDAddr, llvm::Value *Ctor,
    llvm::Value *CopyCtor, llvm::Value *Dtor, SourceLocation Loc) {
  // Call kmp_int32 __kmpc_global_thread_num(&loc) to init OpenMP runtime
  // library.
  auto OMPLoc = EmitOpenMPUpdateLocation(CGF, Loc);
  CGF.EmitRuntimeCall(CreateRuntimeFunction(OMPRTL__kmpc_global_thread_num),
                      OMPLoc);
  // Call __kmpc_threadprivate_register(&loc, &var, ctor, cctor/*NULL*/, dtor)
  // to register constructor/destructor for variable.
  llvm::Value *Args[] = {OMPLoc,
                         CGF.Builder.CreatePointerCast(VDAddr, CGM.VoidPtrTy),
                         Ctor, CopyCtor, Dtor};
  CGF.EmitRuntimeCall(CreateRuntimeFunction(
                          CGOpenMPRuntime::OMPRTL__kmpc_threadprivate_register),
                      Args);
}

llvm::Function *CGOpenMPRuntime::EmitOMPThreadPrivateVarDefinition(
    const VarDecl *VD, llvm::Value *VDAddr, SourceLocation Loc,
    bool PerformInit, CodeGenFunction *CGF) {
  VD = VD->getDefinition(CGM.getContext());
  if (VD && ThreadPrivateWithDefinition.count(VD) == 0) {
    ThreadPrivateWithDefinition.insert(VD);
    QualType ASTTy = VD->getType();

    llvm::Value *Ctor = nullptr, *CopyCtor = nullptr, *Dtor = nullptr;
    auto Init = VD->getAnyInitializer();
    if (CGM.getLangOpts().CPlusPlus && PerformInit) {
      // Generate function that re-emits the declaration's initializer into the
      // threadprivate copy of the variable VD
      CodeGenFunction CtorCGF(CGM);
      FunctionArgList Args;
      ImplicitParamDecl Dst(CGM.getContext(), /*DC=*/nullptr, SourceLocation(),
                            /*Id=*/nullptr, CGM.getContext().VoidPtrTy);
      Args.push_back(&Dst);

      auto &FI = CGM.getTypes().arrangeFreeFunctionDeclaration(
          CGM.getContext().VoidPtrTy, Args, FunctionType::ExtInfo(),
          /*isVariadic=*/false);
      auto FTy = CGM.getTypes().GetFunctionType(FI);
      auto Fn = CGM.CreateGlobalInitOrDestructFunction(
          FTy, ".__kmpc_global_ctor_.", Loc);
      CtorCGF.StartFunction(GlobalDecl(), CGM.getContext().VoidPtrTy, Fn, FI,
                            Args, SourceLocation());
      auto ArgVal = CtorCGF.EmitLoadOfScalar(
          CtorCGF.GetAddrOfLocalVar(&Dst),
          /*Volatile=*/false, CGM.PointerAlignInBytes,
          CGM.getContext().VoidPtrTy, Dst.getLocation());
      auto Arg = CtorCGF.Builder.CreatePointerCast(
          ArgVal,
          CtorCGF.ConvertTypeForMem(CGM.getContext().getPointerType(ASTTy)));
      CtorCGF.EmitAnyExprToMem(Init, Arg, Init->getType().getQualifiers(),
                               /*IsInitializer=*/true);
      ArgVal = CtorCGF.EmitLoadOfScalar(
          CtorCGF.GetAddrOfLocalVar(&Dst),
          /*Volatile=*/false, CGM.PointerAlignInBytes,
          CGM.getContext().VoidPtrTy, Dst.getLocation());
      CtorCGF.Builder.CreateStore(ArgVal, CtorCGF.ReturnValue);
      CtorCGF.FinishFunction();
      Ctor = Fn;
    }
    if (VD->getType().isDestructedType() != QualType::DK_none) {
      // Generate function that emits destructor call for the threadprivate copy
      // of the variable VD
      CodeGenFunction DtorCGF(CGM);
      FunctionArgList Args;
      ImplicitParamDecl Dst(CGM.getContext(), /*DC=*/nullptr, SourceLocation(),
                            /*Id=*/nullptr, CGM.getContext().VoidPtrTy);
      Args.push_back(&Dst);

      auto &FI = CGM.getTypes().arrangeFreeFunctionDeclaration(
          CGM.getContext().VoidTy, Args, FunctionType::ExtInfo(),
          /*isVariadic=*/false);
      auto FTy = CGM.getTypes().GetFunctionType(FI);
      auto Fn = CGM.CreateGlobalInitOrDestructFunction(
          FTy, ".__kmpc_global_dtor_.", Loc);
      DtorCGF.StartFunction(GlobalDecl(), CGM.getContext().VoidTy, Fn, FI, Args,
                            SourceLocation());
      auto ArgVal = DtorCGF.EmitLoadOfScalar(
          DtorCGF.GetAddrOfLocalVar(&Dst),
          /*Volatile=*/false, CGM.PointerAlignInBytes,
          CGM.getContext().VoidPtrTy, Dst.getLocation());
      DtorCGF.emitDestroy(ArgVal, ASTTy,
                          DtorCGF.getDestroyer(ASTTy.isDestructedType()),
                          DtorCGF.needsEHCleanup(ASTTy.isDestructedType()));
      DtorCGF.FinishFunction();
      Dtor = Fn;
    }
    // Do not emit init function if it is not required.
    if (!Ctor && !Dtor)
      return nullptr;

    llvm::Type *CopyCtorTyArgs[] = {CGM.VoidPtrTy, CGM.VoidPtrTy};
    auto CopyCtorTy =
        llvm::FunctionType::get(CGM.VoidPtrTy, CopyCtorTyArgs,
                                /*isVarArg=*/false)->getPointerTo();
    // Copying constructor for the threadprivate variable.
    // Must be NULL - reserved by runtime, but currently it requires that this
    // parameter is always NULL. Otherwise it fires assertion.
    CopyCtor = llvm::Constant::getNullValue(CopyCtorTy);
    if (Ctor == nullptr) {
      auto CtorTy = llvm::FunctionType::get(CGM.VoidPtrTy, CGM.VoidPtrTy,
                                            /*isVarArg=*/false)->getPointerTo();
      Ctor = llvm::Constant::getNullValue(CtorTy);
    }
    if (Dtor == nullptr) {
      auto DtorTy = llvm::FunctionType::get(CGM.VoidTy, CGM.VoidPtrTy,
                                            /*isVarArg=*/false)->getPointerTo();
      Dtor = llvm::Constant::getNullValue(DtorTy);
    }
    if (!CGF) {
      auto InitFunctionTy =
          llvm::FunctionType::get(CGM.VoidTy, /*isVarArg*/ false);
      auto InitFunction = CGM.CreateGlobalInitOrDestructFunction(
          InitFunctionTy, ".__omp_threadprivate_init_.");
      CodeGenFunction InitCGF(CGM);
      FunctionArgList ArgList;
      InitCGF.StartFunction(GlobalDecl(), CGM.getContext().VoidTy, InitFunction,
                            CGM.getTypes().arrangeNullaryFunction(), ArgList,
                            Loc);
      EmitOMPThreadPrivateVarInit(InitCGF, VDAddr, Ctor, CopyCtor, Dtor, Loc);
      InitCGF.FinishFunction();
      return InitFunction;
    }
    EmitOMPThreadPrivateVarInit(*CGF, VDAddr, Ctor, CopyCtor, Dtor, Loc);
  }
  return nullptr;
}

void CGOpenMPRuntime::EmitOMPParallelCall(CodeGenFunction &CGF,
                                          SourceLocation Loc,
                                          llvm::Value *OutlinedFn,
                                          llvm::Value *CapturedStruct) {
  // Build call __kmpc_fork_call(loc, 1, microtask, captured_struct/*context*/)
  llvm::Value *Args[] = {
      EmitOpenMPUpdateLocation(CGF, Loc),
      CGF.Builder.getInt32(1), // Number of arguments after 'microtask' argument
      // (there is only one additional argument - 'context')
      CGF.Builder.CreateBitCast(OutlinedFn, getKmpc_MicroPointerTy()),
      CGF.EmitCastToVoidPtr(CapturedStruct)};
  auto RTLFn = CreateRuntimeFunction(CGOpenMPRuntime::OMPRTL__kmpc_fork_call);
  CGF.EmitRuntimeCall(RTLFn, Args);
}

void CGOpenMPRuntime::EmitOMPSerialCall(CodeGenFunction &CGF,
                                        SourceLocation Loc,
                                        llvm::Value *OutlinedFn,
                                        llvm::Value *CapturedStruct) {
  auto ThreadID = GetOpenMPThreadID(CGF, Loc);
  // Build calls:
  // __kmpc_serialized_parallel(&Loc, GTid);
  llvm::Value *SerArgs[] = {EmitOpenMPUpdateLocation(CGF, Loc), ThreadID};
  auto RTLFn =
      CreateRuntimeFunction(CGOpenMPRuntime::OMPRTL__kmpc_serialized_parallel);
  CGF.EmitRuntimeCall(RTLFn, SerArgs);

  // OutlinedFn(&GTid, &zero, CapturedStruct);
  auto ThreadIDAddr = EmitThreadIDAddress(CGF, Loc);
  auto Int32Ty =
      CGF.getContext().getIntTypeForBitwidth(/*DestWidth*/ 32, /*Signed*/ true);
  auto ZeroAddr = CGF.CreateMemTemp(Int32Ty, /*Name*/ ".zero.addr");
  CGF.InitTempAlloca(ZeroAddr, CGF.Builder.getInt32(/*C*/ 0));
  llvm::Value *OutlinedFnArgs[] = {ThreadIDAddr, ZeroAddr, CapturedStruct};
  CGF.EmitCallOrInvoke(OutlinedFn, OutlinedFnArgs);

  // __kmpc_end_serialized_parallel(&Loc, GTid);
  llvm::Value *EndSerArgs[] = {EmitOpenMPUpdateLocation(CGF, Loc), ThreadID};
  RTLFn = CreateRuntimeFunction(
      CGOpenMPRuntime::OMPRTL__kmpc_end_serialized_parallel);
  CGF.EmitRuntimeCall(RTLFn, EndSerArgs);
}

// If we're inside an (outlined) parallel region, use the region info's
// thread-ID variable (it is passed in a first argument of the outlined function
// as "kmp_int32 *gtid"). Otherwise, if we're not inside parallel region, but in
// regular serial code region, get thread ID by calling kmp_int32
// kmpc_global_thread_num(ident_t *loc), stash this thread ID in a temporary and
// return the address of that temp.
llvm::Value *CGOpenMPRuntime::EmitThreadIDAddress(CodeGenFunction &CGF,
                                                  SourceLocation Loc) {
  if (auto OMPRegionInfo =
          dyn_cast_or_null<CGOpenMPRegionInfo>(CGF.CapturedStmtInfo))
    return CGF.EmitLoadOfLValue(OMPRegionInfo->getThreadIDVariableLValue(CGF),
                                SourceLocation()).getScalarVal();
  auto ThreadID = GetOpenMPThreadID(CGF, Loc);
  auto Int32Ty =
      CGF.getContext().getIntTypeForBitwidth(/*DestWidth*/ 32, /*Signed*/ true);
  auto ThreadIDTemp = CGF.CreateMemTemp(Int32Ty, /*Name*/ ".threadid_temp.");
  CGF.EmitStoreOfScalar(ThreadID,
                        CGF.MakeNaturalAlignAddrLValue(ThreadIDTemp, Int32Ty));

  return ThreadIDTemp;
}

llvm::Constant *
CGOpenMPRuntime::GetOrCreateInternalVariable(llvm::Type *Ty,
                                             const llvm::Twine &Name) {
  SmallString<256> Buffer;
  llvm::raw_svector_ostream Out(Buffer);
  Out << Name;
  auto RuntimeName = Out.str();
  auto &Elem = InternalVars.GetOrCreateValue(RuntimeName);
  if (Elem.getValue()) {
    assert(Elem.getValue()->getType()->getPointerElementType() == Ty &&
           "OMP internal variable has different type than requested");
    return &*Elem.getValue();
  }

  auto Item = new llvm::GlobalVariable(
      CGM.getModule(), Ty, /*IsConstant*/ false,
      llvm::GlobalValue::CommonLinkage,
      llvm::Constant::getNullValue(Ty), Elem.getKey());
  Elem.setValue(Item);
  return Item;
}

llvm::Value *CGOpenMPRuntime::GetCriticalRegionLock(StringRef CriticalName) {
  llvm::Twine Name(".gomp_critical_user_", CriticalName);
  return GetOrCreateInternalVariable(KmpCriticalNameTy, Name.concat(".var"));
}

void CGOpenMPRuntime::EmitOMPCriticalRegionStart(CodeGenFunction &CGF,
                                                 llvm::Value *RegionLock,
                                                 SourceLocation Loc) {
  // Prepare other arguments and build a call to __kmpc_critical
  llvm::Value *Args[] = {EmitOpenMPUpdateLocation(CGF, Loc),
                         GetOpenMPThreadID(CGF, Loc), RegionLock};
  auto RTLFn = CreateRuntimeFunction(CGOpenMPRuntime::OMPRTL__kmpc_critical);
  CGF.EmitRuntimeCall(RTLFn, Args);
}

void CGOpenMPRuntime::EmitOMPCriticalRegionEnd(CodeGenFunction &CGF,
                                               llvm::Value *RegionLock,
                                               SourceLocation Loc) {
  // Prepare other arguments and build a call to __kmpc_end_critical
  llvm::Value *Args[] = {EmitOpenMPUpdateLocation(CGF, Loc),
                         GetOpenMPThreadID(CGF, Loc), RegionLock};
  auto RTLFn =
      CreateRuntimeFunction(CGOpenMPRuntime::OMPRTL__kmpc_end_critical);
  CGF.EmitRuntimeCall(RTLFn, Args);
}

void CGOpenMPRuntime::EmitOMPBarrierCall(CodeGenFunction &CGF,
                                         SourceLocation Loc,
                                         OpenMPLocationFlags Flags) {
  // Build call __kmpc_barrier(loc, thread_id)
  llvm::Value *Args[] = {EmitOpenMPUpdateLocation(CGF, Loc, Flags),
                         GetOpenMPThreadID(CGF, Loc)};
  auto RTLFn = CreateRuntimeFunction(CGOpenMPRuntime::OMPRTL__kmpc_barrier);
  CGF.EmitRuntimeCall(RTLFn, Args);
}

void CGOpenMPRuntime::EmitOMPNumThreadsClause(CodeGenFunction &CGF,
                                              llvm::Value *NumThreads,
                                              SourceLocation Loc) {
  // Build call __kmpc_push_num_threads(&loc, global_tid, num_threads)
  llvm::Value *Args[] = {
      EmitOpenMPUpdateLocation(CGF, Loc), GetOpenMPThreadID(CGF, Loc),
      CGF.Builder.CreateIntCast(NumThreads, CGF.Int32Ty, /*isSigned*/ true)};
  llvm::Constant *RTLFn = CGF.CGM.getOpenMPRuntime().CreateRuntimeFunction(
      CGOpenMPRuntime::OMPRTL__kmpc_push_num_threads);
  CGF.EmitRuntimeCall(RTLFn, Args);
}

