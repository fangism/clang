//===--- ObjCMT.cpp - ObjC Migrate Tool -----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Transforms.h"
#include "clang/ARCMigrate/ARCMTActions.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/NSAPI.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileManager.h"
#include "clang/Edit/Commit.h"
#include "clang/Edit/EditedSource.h"
#include "clang/Edit/EditsReceiver.h"
#include "clang/Edit/Rewriters.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/PPConditionalDirectiveRecord.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Analysis/DomainSpecific/CocoaConventions.h"
#include "clang/StaticAnalyzer/Checkers/ObjCRetainCount.h"
#include "clang/AST/Attr.h"
#include "llvm/ADT/SmallString.h"

using namespace clang;
using namespace arcmt;
using namespace ento::objc_retain;

namespace {

class ObjCMigrateASTConsumer : public ASTConsumer {
  enum CF_BRIDGING_KIND {
    CF_BRIDGING_NONE,
    CF_BRIDGING_ENABLE,
    CF_BRIDGING_MAY_INCLUDE
  };
  
  void migrateDecl(Decl *D);
  void migrateObjCInterfaceDecl(ASTContext &Ctx, ObjCContainerDecl *D);
  void migrateProtocolConformance(ASTContext &Ctx,
                                  const ObjCImplementationDecl *ImpDecl);
  void migrateNSEnumDecl(ASTContext &Ctx, const EnumDecl *EnumDcl,
                     const TypedefDecl *TypedefDcl);
  void migrateMethods(ASTContext &Ctx, ObjCContainerDecl *CDecl);
  void migrateMethodInstanceType(ASTContext &Ctx, ObjCContainerDecl *CDecl,
                                 ObjCMethodDecl *OM);
  bool migrateProperty(ASTContext &Ctx, ObjCContainerDecl *D, ObjCMethodDecl *OM);
  void migrateNsReturnsInnerPointer(ASTContext &Ctx, ObjCMethodDecl *OM);
  void migrateFactoryMethod(ASTContext &Ctx, ObjCContainerDecl *CDecl,
                            ObjCMethodDecl *OM,
                            ObjCInstanceTypeFamily OIT_Family = OIT_None);
  
  void migrateCFAnnotation(ASTContext &Ctx, const Decl *Decl);
  void AddCFAnnotations(ASTContext &Ctx, const CallEffects &CE,
                        const FunctionDecl *FuncDecl, bool ResultAnnotated);
  void AddCFAnnotations(ASTContext &Ctx, const CallEffects &CE,
                        const ObjCMethodDecl *MethodDecl, bool ResultAnnotated);
  
  void AnnotateImplicitBridging(ASTContext &Ctx);
  
  CF_BRIDGING_KIND migrateAddFunctionAnnotation(ASTContext &Ctx,
                                                const FunctionDecl *FuncDecl);
  
  void migrateARCSafeAnnotation(ASTContext &Ctx, ObjCContainerDecl *CDecl);
  
  void migrateAddMethodAnnotation(ASTContext &Ctx,
                                  const ObjCMethodDecl *MethodDecl);
public:
  std::string MigrateDir;
  bool MigrateLiterals;
  bool MigrateSubscripting;
  bool MigrateProperty;
  bool MigrateReadonlyProperty;
  unsigned  FileId;
  OwningPtr<NSAPI> NSAPIObj;
  OwningPtr<edit::EditedSource> Editor;
  FileRemapper &Remapper;
  FileManager &FileMgr;
  const PPConditionalDirectiveRecord *PPRec;
  Preprocessor &PP;
  bool IsOutputFile;
  llvm::SmallPtrSet<ObjCProtocolDecl *, 32> ObjCProtocolDecls;
  llvm::SmallVector<const Decl *, 8> CFFunctionIBCandidates;
  
  ObjCMigrateASTConsumer(StringRef migrateDir,
                         bool migrateLiterals,
                         bool migrateSubscripting,
                         bool migrateProperty,
                         bool migrateReadonlyProperty,
                         FileRemapper &remapper,
                         FileManager &fileMgr,
                         const PPConditionalDirectiveRecord *PPRec,
                         Preprocessor &PP,
                         bool isOutputFile = false)
  : MigrateDir(migrateDir),
    MigrateLiterals(migrateLiterals),
    MigrateSubscripting(migrateSubscripting),
    MigrateProperty(migrateProperty), 
    MigrateReadonlyProperty(migrateReadonlyProperty), 
    FileId(0), Remapper(remapper), FileMgr(fileMgr), PPRec(PPRec), PP(PP),
    IsOutputFile(isOutputFile) { }

protected:
  virtual void Initialize(ASTContext &Context) {
    NSAPIObj.reset(new NSAPI(Context));
    Editor.reset(new edit::EditedSource(Context.getSourceManager(),
                                        Context.getLangOpts(),
                                        PPRec));
  }

  virtual bool HandleTopLevelDecl(DeclGroupRef DG) {
    for (DeclGroupRef::iterator I = DG.begin(), E = DG.end(); I != E; ++I)
      migrateDecl(*I);
    return true;
  }
  virtual void HandleInterestingDecl(DeclGroupRef DG) {
    // Ignore decls from the PCH.
  }
  virtual void HandleTopLevelDeclInObjCContainer(DeclGroupRef DG) {
    ObjCMigrateASTConsumer::HandleTopLevelDecl(DG);
  }

  virtual void HandleTranslationUnit(ASTContext &Ctx);
};

}

ObjCMigrateAction::ObjCMigrateAction(FrontendAction *WrappedAction,
                             StringRef migrateDir,
                             bool migrateLiterals,
                             bool migrateSubscripting,
                             bool migrateProperty,
                             bool migrateReadonlyProperty)
  : WrapperFrontendAction(WrappedAction), MigrateDir(migrateDir),
    MigrateLiterals(migrateLiterals), MigrateSubscripting(migrateSubscripting),
    MigrateProperty(migrateProperty),
    MigrateReadonlyProperty(migrateReadonlyProperty),
    CompInst(0) {
  if (MigrateDir.empty())
    MigrateDir = "."; // user current directory if none is given.
}

ASTConsumer *ObjCMigrateAction::CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef InFile) {
  PPConditionalDirectiveRecord *
    PPRec = new PPConditionalDirectiveRecord(CompInst->getSourceManager());
  CompInst->getPreprocessor().addPPCallbacks(PPRec);
  ASTConsumer *
    WrappedConsumer = WrapperFrontendAction::CreateASTConsumer(CI, InFile);
  ASTConsumer *MTConsumer = new ObjCMigrateASTConsumer(MigrateDir,
                                                       MigrateLiterals,
                                                       MigrateSubscripting,
                                                       MigrateProperty,
                                                       MigrateReadonlyProperty,
                                                       Remapper,
                                                    CompInst->getFileManager(),
                                                       PPRec,
                                                       CompInst->getPreprocessor());
  ASTConsumer *Consumers[] = { MTConsumer, WrappedConsumer };
  return new MultiplexConsumer(Consumers);
}

bool ObjCMigrateAction::BeginInvocation(CompilerInstance &CI) {
  Remapper.initFromDisk(MigrateDir, CI.getDiagnostics(),
                        /*ignoreIfFilesChanges=*/true);
  CompInst = &CI;
  CI.getDiagnostics().setIgnoreAllWarnings(true);
  return true;
}

namespace {
class ObjCMigrator : public RecursiveASTVisitor<ObjCMigrator> {
  ObjCMigrateASTConsumer &Consumer;
  ParentMap &PMap;

public:
  ObjCMigrator(ObjCMigrateASTConsumer &consumer, ParentMap &PMap)
    : Consumer(consumer), PMap(PMap) { }

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldWalkTypesOfTypeLocs() const { return false; }

  bool VisitObjCMessageExpr(ObjCMessageExpr *E) {
    if (Consumer.MigrateLiterals) {
      edit::Commit commit(*Consumer.Editor);
      edit::rewriteToObjCLiteralSyntax(E, *Consumer.NSAPIObj, commit, &PMap);
      Consumer.Editor->commit(commit);
    }

    if (Consumer.MigrateSubscripting) {
      edit::Commit commit(*Consumer.Editor);
      edit::rewriteToObjCSubscriptSyntax(E, *Consumer.NSAPIObj, commit);
      Consumer.Editor->commit(commit);
    }

    return true;
  }

  bool TraverseObjCMessageExpr(ObjCMessageExpr *E) {
    // Do depth first; we want to rewrite the subexpressions first so that if
    // we have to move expressions we will move them already rewritten.
    for (Stmt::child_range range = E->children(); range; ++range)
      if (!TraverseStmt(*range))
        return false;

    return WalkUpFromObjCMessageExpr(E);
  }
};

class BodyMigrator : public RecursiveASTVisitor<BodyMigrator> {
  ObjCMigrateASTConsumer &Consumer;
  OwningPtr<ParentMap> PMap;

public:
  BodyMigrator(ObjCMigrateASTConsumer &consumer) : Consumer(consumer) { }

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldWalkTypesOfTypeLocs() const { return false; }

  bool TraverseStmt(Stmt *S) {
    PMap.reset(new ParentMap(S));
    ObjCMigrator(Consumer, *PMap).TraverseStmt(S);
    return true;
  }
};
}

void ObjCMigrateASTConsumer::migrateDecl(Decl *D) {
  if (!D)
    return;
  if (isa<ObjCMethodDecl>(D))
    return; // Wait for the ObjC container declaration.

  BodyMigrator(*this).TraverseDecl(D);
}

static void append_attr(std::string &PropertyString, const char *attr) {
  PropertyString += ", ";
  PropertyString += attr;
}

static bool rewriteToObjCProperty(const ObjCMethodDecl *Getter,
                                  const ObjCMethodDecl *Setter,
                                  const NSAPI &NS, edit::Commit &commit,
                                  unsigned LengthOfPrefix) {
  ASTContext &Context = NS.getASTContext();
  std::string PropertyString = "@property(nonatomic";
  std::string PropertyNameString = Getter->getNameAsString();
  StringRef PropertyName(PropertyNameString);
  if (LengthOfPrefix > 0) {
    PropertyString += ", getter=";
    PropertyString += PropertyNameString;
  }
  // Property with no setter may be suggested as a 'readonly' property.
  if (!Setter)
    append_attr(PropertyString, "readonly");

  // Short circuit properties that contain the name "delegate" or "dataSource",
  // or have exact name "target" to have unsafe_unretained attribute.
  if (PropertyName.equals("target") ||
      (PropertyName.find("delegate") != StringRef::npos) ||
      (PropertyName.find("dataSource") != StringRef::npos))
    append_attr(PropertyString, "unsafe_unretained");
  else if (Setter) {
    const ParmVarDecl *argDecl = *Setter->param_begin();
    QualType ArgType = Context.getCanonicalType(argDecl->getType());
    Qualifiers::ObjCLifetime propertyLifetime = ArgType.getObjCLifetime();
    bool RetainableObject = ArgType->isObjCRetainableType();
    if (RetainableObject && propertyLifetime == Qualifiers::OCL_Strong) {
      if (const ObjCObjectPointerType *ObjPtrTy =
          ArgType->getAs<ObjCObjectPointerType>()) {
        ObjCInterfaceDecl *IDecl = ObjPtrTy->getObjectType()->getInterface();
        if (IDecl &&
            IDecl->lookupNestedProtocol(&Context.Idents.get("NSCopying")))
          append_attr(PropertyString, "copy");
        else
          append_attr(PropertyString, "retain");
      }
    } else if (propertyLifetime == Qualifiers::OCL_Weak)
      // TODO. More precise determination of 'weak' attribute requires
      // looking into setter's implementation for backing weak ivar.
      append_attr(PropertyString, "weak");
    else if (RetainableObject)
      append_attr(PropertyString, "retain");
  }
  PropertyString += ')';
  
  QualType RT = Getter->getResultType();
  if (!isa<TypedefType>(RT)) {
    // strip off any ARC lifetime qualifier.
    QualType CanResultTy = Context.getCanonicalType(RT);
    if (CanResultTy.getQualifiers().hasObjCLifetime()) {
      Qualifiers Qs = CanResultTy.getQualifiers();
      Qs.removeObjCLifetime();
      RT = Context.getQualifiedType(CanResultTy.getUnqualifiedType(), Qs);
    }
  }
  PropertyString += " ";
  PropertyString += RT.getAsString(Context.getPrintingPolicy());
  PropertyString += " ";
  if (LengthOfPrefix > 0) {
    // property name must strip off "is" and lower case the first character
    // after that; e.g. isContinuous will become continuous.
    StringRef PropertyNameStringRef(PropertyNameString);
    PropertyNameStringRef = PropertyNameStringRef.drop_front(LengthOfPrefix);
    PropertyNameString = PropertyNameStringRef;
    std::string NewPropertyNameString = PropertyNameString;
    bool NoLowering = (isUppercase(NewPropertyNameString[0]) &&
                       NewPropertyNameString.size() > 1 &&
                       isUppercase(NewPropertyNameString[1]));
    if (!NoLowering)
      NewPropertyNameString[0] = toLowercase(NewPropertyNameString[0]);
    PropertyString += NewPropertyNameString;
  }
  else
    PropertyString += PropertyNameString;
  SourceLocation StartGetterSelectorLoc = Getter->getSelectorStartLoc();
  Selector GetterSelector = Getter->getSelector();
  
  SourceLocation EndGetterSelectorLoc =
    StartGetterSelectorLoc.getLocWithOffset(GetterSelector.getNameForSlot(0).size());
  commit.replace(CharSourceRange::getCharRange(Getter->getLocStart(),
                                               EndGetterSelectorLoc),
                 PropertyString);
  if (Setter) {
    SourceLocation EndLoc = Setter->getDeclaratorEndLoc();
    // Get location past ';'
    EndLoc = EndLoc.getLocWithOffset(1);
    commit.remove(CharSourceRange::getCharRange(Setter->getLocStart(), EndLoc));
  }
  return true;
}

void ObjCMigrateASTConsumer::migrateObjCInterfaceDecl(ASTContext &Ctx,
                                                      ObjCContainerDecl *D) {
  if (D->isDeprecated())
    return;
  
  for (ObjCContainerDecl::method_iterator M = D->meth_begin(), MEnd = D->meth_end();
       M != MEnd; ++M) {
    ObjCMethodDecl *Method = (*M);
    if (Method->isDeprecated())
      continue;
    if (!migrateProperty(Ctx, D, Method))
      migrateNsReturnsInnerPointer(Ctx, Method);
  }
}

static bool 
ClassImplementsAllMethodsAndProperties(ASTContext &Ctx,
                                      const ObjCImplementationDecl *ImpDecl,
                                       const ObjCInterfaceDecl *IDecl,
                                      ObjCProtocolDecl *Protocol) {
  // In auto-synthesis, protocol properties are not synthesized. So,
  // a conforming protocol must have its required properties declared
  // in class interface.
  bool HasAtleastOneRequiredProperty = false;
  if (const ObjCProtocolDecl *PDecl = Protocol->getDefinition())
    for (ObjCProtocolDecl::prop_iterator P = PDecl->prop_begin(),
         E = PDecl->prop_end(); P != E; ++P) {
      ObjCPropertyDecl *Property = *P;
      if (Property->getPropertyImplementation() == ObjCPropertyDecl::Optional)
        continue;
      HasAtleastOneRequiredProperty = true;
      DeclContext::lookup_const_result R = IDecl->lookup(Property->getDeclName());
      if (R.size() == 0) {
        // Relax the rule and look into class's implementation for a synthesize
        // or dynamic declaration. Class is implementing a property coming from
        // another protocol. This still makes the target protocol as conforming.
        if (!ImpDecl->FindPropertyImplDecl(
                                  Property->getDeclName().getAsIdentifierInfo()))
          return false;
      }
      else if (ObjCPropertyDecl *ClassProperty = dyn_cast<ObjCPropertyDecl>(R[0])) {
          if ((ClassProperty->getPropertyAttributes()
              != Property->getPropertyAttributes()) ||
              !Ctx.hasSameType(ClassProperty->getType(), Property->getType()))
            return false;
      }
      else
        return false;
    }
  
  // At this point, all required properties in this protocol conform to those
  // declared in the class.
  // Check that class implements the required methods of the protocol too.
  bool HasAtleastOneRequiredMethod = false;
  if (const ObjCProtocolDecl *PDecl = Protocol->getDefinition()) {
    if (PDecl->meth_begin() == PDecl->meth_end())
      return HasAtleastOneRequiredProperty;
    for (ObjCContainerDecl::method_iterator M = PDecl->meth_begin(),
         MEnd = PDecl->meth_end(); M != MEnd; ++M) {
      ObjCMethodDecl *MD = (*M);
      if (MD->isImplicit())
        continue;
      if (MD->getImplementationControl() == ObjCMethodDecl::Optional)
        continue;
      DeclContext::lookup_const_result R = ImpDecl->lookup(MD->getDeclName());
      if (R.size() == 0)
        return false;
      bool match = false;
      HasAtleastOneRequiredMethod = true;
      for (unsigned I = 0, N = R.size(); I != N; ++I)
        if (ObjCMethodDecl *ImpMD = dyn_cast<ObjCMethodDecl>(R[0]))
          if (Ctx.ObjCMethodsAreEqual(MD, ImpMD)) {
            match = true;
            break;
          }
      if (!match)
        return false;
    }
  }
  if (HasAtleastOneRequiredProperty || HasAtleastOneRequiredMethod)
    return true;
  return false;
}

static bool rewriteToObjCInterfaceDecl(const ObjCInterfaceDecl *IDecl,
                    llvm::SmallVectorImpl<ObjCProtocolDecl*> &ConformingProtocols,
                    const NSAPI &NS, edit::Commit &commit) {
  const ObjCList<ObjCProtocolDecl> &Protocols = IDecl->getReferencedProtocols();
  std::string ClassString;
  SourceLocation EndLoc =
  IDecl->getSuperClass() ? IDecl->getSuperClassLoc() : IDecl->getLocation();
  
  if (Protocols.empty()) {
    ClassString = '<';
    for (unsigned i = 0, e = ConformingProtocols.size(); i != e; i++) {
      ClassString += ConformingProtocols[i]->getNameAsString();
      if (i != (e-1))
        ClassString += ", ";
    }
    ClassString += "> ";
  }
  else {
    ClassString = ", ";
    for (unsigned i = 0, e = ConformingProtocols.size(); i != e; i++) {
      ClassString += ConformingProtocols[i]->getNameAsString();
      if (i != (e-1))
        ClassString += ", ";
    }
    ObjCInterfaceDecl::protocol_loc_iterator PL = IDecl->protocol_loc_end() - 1;
    EndLoc = *PL;
  }
  
  commit.insertAfterToken(EndLoc, ClassString);
  return true;
}

static bool rewriteToNSEnumDecl(const EnumDecl *EnumDcl,
                                const TypedefDecl *TypedefDcl,
                                const NSAPI &NS, edit::Commit &commit,
                                bool IsNSIntegerType,
                                bool NSOptions) {
  std::string ClassString;
  if (NSOptions)
    ClassString = "typedef NS_OPTIONS(NSUInteger, ";
  else
    ClassString =
      IsNSIntegerType ? "typedef NS_ENUM(NSInteger, "
                      : "typedef NS_ENUM(NSUInteger, ";
  
  ClassString += TypedefDcl->getIdentifier()->getName();
  ClassString += ')';
  SourceRange R(EnumDcl->getLocStart(), EnumDcl->getLocStart());
  commit.replace(R, ClassString);
  SourceLocation EndOfTypedefLoc = TypedefDcl->getLocEnd();
  EndOfTypedefLoc = trans::findLocationAfterSemi(EndOfTypedefLoc, NS.getASTContext());
  if (!EndOfTypedefLoc.isInvalid()) {
    commit.remove(SourceRange(TypedefDcl->getLocStart(), EndOfTypedefLoc));
    return true;
  }
  return false;
}

static bool rewriteToNSMacroDecl(const EnumDecl *EnumDcl,
                                const TypedefDecl *TypedefDcl,
                                const NSAPI &NS, edit::Commit &commit,
                                 bool IsNSIntegerType) {
  std::string ClassString =
    IsNSIntegerType ? "NS_ENUM(NSInteger, " : "NS_OPTIONS(NSUInteger, ";
  ClassString += TypedefDcl->getIdentifier()->getName();
  ClassString += ')';
  SourceRange R(EnumDcl->getLocStart(), EnumDcl->getLocStart());
  commit.replace(R, ClassString);
  SourceLocation TypedefLoc = TypedefDcl->getLocEnd();
  commit.remove(SourceRange(TypedefLoc, TypedefLoc));
  return true;
}

static bool UseNSOptionsMacro(Preprocessor &PP, ASTContext &Ctx,
                              const EnumDecl *EnumDcl) {
  bool PowerOfTwo = true;
  bool FoundHexdecimalEnumerator = false;
  uint64_t MaxPowerOfTwoVal = 0;
  for (EnumDecl::enumerator_iterator EI = EnumDcl->enumerator_begin(),
       EE = EnumDcl->enumerator_end(); EI != EE; ++EI) {
    EnumConstantDecl *Enumerator = (*EI);
    const Expr *InitExpr = Enumerator->getInitExpr();
    if (!InitExpr) {
      PowerOfTwo = false;
      continue;
    }
    InitExpr = InitExpr->IgnoreParenCasts();
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(InitExpr))
      if (BO->isShiftOp() || BO->isBitwiseOp())
        return true;
    
    uint64_t EnumVal = Enumerator->getInitVal().getZExtValue();
    if (PowerOfTwo && EnumVal) {
      if (!llvm::isPowerOf2_64(EnumVal))
        PowerOfTwo = false;
      else if (EnumVal > MaxPowerOfTwoVal)
        MaxPowerOfTwoVal = EnumVal;
    }
    if (!FoundHexdecimalEnumerator) {
      SourceLocation EndLoc = Enumerator->getLocEnd();
      Token Tok;
      if (!PP.getRawToken(EndLoc, Tok, /*IgnoreWhiteSpace=*/true))
        if (Tok.isLiteral() && Tok.getLength() > 2) {
          if (const char *StringLit = Tok.getLiteralData())
            FoundHexdecimalEnumerator =
              (StringLit[0] == '0' && (toLowercase(StringLit[1]) == 'x'));
        }
    }
  }
  return FoundHexdecimalEnumerator || (PowerOfTwo && (MaxPowerOfTwoVal > 2));
}

void ObjCMigrateASTConsumer::migrateProtocolConformance(ASTContext &Ctx,   
                                            const ObjCImplementationDecl *ImpDecl) {
  const ObjCInterfaceDecl *IDecl = ImpDecl->getClassInterface();
  if (!IDecl || ObjCProtocolDecls.empty() || IDecl->isDeprecated())
    return;
  // Find all implicit conforming protocols for this class
  // and make them explicit.
  llvm::SmallPtrSet<ObjCProtocolDecl *, 8> ExplicitProtocols;
  Ctx.CollectInheritedProtocols(IDecl, ExplicitProtocols);
  llvm::SmallVector<ObjCProtocolDecl *, 8> PotentialImplicitProtocols;
  
  for (llvm::SmallPtrSet<ObjCProtocolDecl*, 32>::iterator I =
       ObjCProtocolDecls.begin(),
       E = ObjCProtocolDecls.end(); I != E; ++I)
    if (!ExplicitProtocols.count(*I))
      PotentialImplicitProtocols.push_back(*I);
  
  if (PotentialImplicitProtocols.empty())
    return;

  // go through list of non-optional methods and properties in each protocol
  // in the PotentialImplicitProtocols list. If class implements every one of the
  // methods and properties, then this class conforms to this protocol.
  llvm::SmallVector<ObjCProtocolDecl*, 8> ConformingProtocols;
  for (unsigned i = 0, e = PotentialImplicitProtocols.size(); i != e; i++)
    if (ClassImplementsAllMethodsAndProperties(Ctx, ImpDecl, IDecl,
                                              PotentialImplicitProtocols[i]))
      ConformingProtocols.push_back(PotentialImplicitProtocols[i]);
  
  if (ConformingProtocols.empty())
    return;
  
  // Further reduce number of conforming protocols. If protocol P1 is in the list
  // protocol P2 (P2<P1>), No need to include P1.
  llvm::SmallVector<ObjCProtocolDecl*, 8> MinimalConformingProtocols;
  for (unsigned i = 0, e = ConformingProtocols.size(); i != e; i++) {
    bool DropIt = false;
    ObjCProtocolDecl *TargetPDecl = ConformingProtocols[i];
    for (unsigned i1 = 0, e1 = ConformingProtocols.size(); i1 != e1; i1++) {
      ObjCProtocolDecl *PDecl = ConformingProtocols[i1];
      if (PDecl == TargetPDecl)
        continue;
      if (PDecl->lookupProtocolNamed(
            TargetPDecl->getDeclName().getAsIdentifierInfo())) {
        DropIt = true;
        break;
      }
    }
    if (!DropIt)
      MinimalConformingProtocols.push_back(TargetPDecl);
  }
  edit::Commit commit(*Editor);
  rewriteToObjCInterfaceDecl(IDecl, MinimalConformingProtocols,
                             *NSAPIObj, commit);
  Editor->commit(commit);
}

void ObjCMigrateASTConsumer::migrateNSEnumDecl(ASTContext &Ctx,
                                           const EnumDecl *EnumDcl,
                                           const TypedefDecl *TypedefDcl) {
  if (!EnumDcl->isCompleteDefinition() || EnumDcl->getIdentifier() ||
      !TypedefDcl->getIdentifier() ||
      EnumDcl->isDeprecated() || TypedefDcl->isDeprecated())
    return;
  
  QualType qt = TypedefDcl->getTypeSourceInfo()->getType();
  bool IsNSIntegerType = NSAPIObj->isObjCNSIntegerType(qt);
  bool IsNSUIntegerType = !IsNSIntegerType && NSAPIObj->isObjCNSUIntegerType(qt);
  
  if (!IsNSIntegerType && !IsNSUIntegerType) {
    // Also check for typedef enum {...} TD;
    if (const EnumType *EnumTy = qt->getAs<EnumType>()) {
      if (EnumTy->getDecl() == EnumDcl) {
        bool NSOptions = UseNSOptionsMacro(PP, Ctx, EnumDcl);
        if (NSOptions) {
          if (!Ctx.Idents.get("NS_OPTIONS").hasMacroDefinition())
            return;
        }
        else if (!Ctx.Idents.get("NS_ENUM").hasMacroDefinition())
          return;
        edit::Commit commit(*Editor);
        rewriteToNSMacroDecl(EnumDcl, TypedefDcl, *NSAPIObj, commit, !NSOptions);
        Editor->commit(commit);
      }
    }
    return;
  }
  
  // We may still use NS_OPTIONS based on what we find in the enumertor list.
  bool NSOptions = UseNSOptionsMacro(PP, Ctx, EnumDcl);
  // NS_ENUM must be available.
  if (IsNSIntegerType && !Ctx.Idents.get("NS_ENUM").hasMacroDefinition())
    return;
  // NS_OPTIONS must be available.
  if (IsNSUIntegerType && !Ctx.Idents.get("NS_OPTIONS").hasMacroDefinition())
    return;
  edit::Commit commit(*Editor);
  rewriteToNSEnumDecl(EnumDcl, TypedefDcl, *NSAPIObj, commit, IsNSIntegerType, NSOptions);
  Editor->commit(commit);
}

static void ReplaceWithInstancetype(const ObjCMigrateASTConsumer &ASTC,
                                    ObjCMethodDecl *OM) {
  SourceRange R;
  std::string ClassString;
  if (TypeSourceInfo *TSInfo =  OM->getResultTypeSourceInfo()) {
    TypeLoc TL = TSInfo->getTypeLoc();
    R = SourceRange(TL.getBeginLoc(), TL.getEndLoc());
    ClassString = "instancetype";
  }
  else {
    R = SourceRange(OM->getLocStart(), OM->getLocStart());
    ClassString = OM->isInstanceMethod() ? '-' : '+';
    ClassString += " (instancetype)";
  }
  edit::Commit commit(*ASTC.Editor);
  commit.replace(R, ClassString);
  ASTC.Editor->commit(commit);
}

void ObjCMigrateASTConsumer::migrateMethodInstanceType(ASTContext &Ctx,
                                                       ObjCContainerDecl *CDecl,
                                                       ObjCMethodDecl *OM) {
  ObjCInstanceTypeFamily OIT_Family =
    Selector::getInstTypeMethodFamily(OM->getSelector());
  
  std::string ClassName;
  switch (OIT_Family) {
    case OIT_None:
      migrateFactoryMethod(Ctx, CDecl, OM);
      return;
    case OIT_Array:
      ClassName = "NSArray";
      break;
    case OIT_Dictionary:
      ClassName = "NSDictionary";
      break;
    case OIT_Singleton:
      migrateFactoryMethod(Ctx, CDecl, OM, OIT_Singleton);
      return;
    case OIT_Init:
      if (OM->getResultType()->isObjCIdType())
        ReplaceWithInstancetype(*this, OM);
      return;
  }
  if (!OM->getResultType()->isObjCIdType())
    return;
  
  ObjCInterfaceDecl *IDecl = dyn_cast<ObjCInterfaceDecl>(CDecl);
  if (!IDecl) {
    if (ObjCCategoryDecl *CatDecl = dyn_cast<ObjCCategoryDecl>(CDecl))
      IDecl = CatDecl->getClassInterface();
    else if (ObjCImplDecl *ImpDecl = dyn_cast<ObjCImplDecl>(CDecl))
      IDecl = ImpDecl->getClassInterface();
  }
  if (!IDecl ||
      !IDecl->lookupInheritedClass(&Ctx.Idents.get(ClassName))) {
    migrateFactoryMethod(Ctx, CDecl, OM);
    return;
  }
  ReplaceWithInstancetype(*this, OM);
}

static bool TypeIsInnerPointer(QualType T) {
  if (!T->isAnyPointerType())
    return false;
  if (T->isObjCObjectPointerType() || T->isObjCBuiltinType() ||
      T->isBlockPointerType() || ento::coreFoundation::isCFObjectRef(T))
    return false;
  // Also, typedef-of-pointer-to-incomplete-struct is something that we assume
  // is not an innter pointer type.
  QualType OrigT = T;
  while (const TypedefType *TD = dyn_cast<TypedefType>(T.getTypePtr()))
    T = TD->getDecl()->getUnderlyingType();
  if (OrigT == T || !T->isPointerType())
    return true;
  const PointerType* PT = T->getAs<PointerType>();
  QualType UPointeeT = PT->getPointeeType().getUnqualifiedType();
  if (UPointeeT->isRecordType()) {
    const RecordType *RecordTy = UPointeeT->getAs<RecordType>();
    if (!RecordTy->getDecl()->isCompleteDefinition())
      return false;
  }
  return true;
}

static bool AttributesMatch(const Decl *Decl1, const Decl *Decl2) {
  if (Decl1->hasAttrs() != Decl2->hasAttrs())
    return false;
  
  if (!Decl1->hasAttrs())
    return true;
  
  const AttrVec &Attrs1 = Decl1->getAttrs();
  const AttrVec &Attrs2 = Decl2->getAttrs();
  // This list is very small, so this need not be optimized.
  for (unsigned i = 0, e = Attrs1.size(); i != e; i++) {
    bool match = false;
    for (unsigned j = 0, f = Attrs2.size(); j != f; j++) {
      // Matching attribute kind only. We are not getting into
      // details of the attributes. For all practical purposes
      // this is sufficient.
      if (Attrs1[i]->getKind() == Attrs2[j]->getKind()) {
        match = true;
        break;
      }
    }
    if (!match)
      return false;
  }
  return true;
}

bool ObjCMigrateASTConsumer::migrateProperty(ASTContext &Ctx,
                             ObjCContainerDecl *D,
                             ObjCMethodDecl *Method) {
  if (Method->isPropertyAccessor() || !Method->isInstanceMethod() ||
      Method->param_size() != 0)
    return false;
  // Is this method candidate to be a getter?
  QualType GRT = Method->getResultType();
  if (GRT->isVoidType())
    return false;
  
  Selector GetterSelector = Method->getSelector();
  IdentifierInfo *getterName = GetterSelector.getIdentifierInfoForSlot(0);
  Selector SetterSelector =
  SelectorTable::constructSetterSelector(PP.getIdentifierTable(),
                                         PP.getSelectorTable(),
                                         getterName);
  ObjCMethodDecl *SetterMethod = D->getInstanceMethod(SetterSelector);
  unsigned LengthOfPrefix = 0;
  if (!SetterMethod) {
    // try a different naming convention for getter: isXxxxx
    StringRef getterNameString = getterName->getName();
    bool IsPrefix = getterNameString.startswith("is");
    // Note that we don't want to change an isXXX method of retainable object
    // type to property (readonly or otherwise).
    if (IsPrefix && GRT->isObjCRetainableType())
      return false;
    if (IsPrefix || getterNameString.startswith("get")) {
      LengthOfPrefix = (IsPrefix ? 2 : 3);
      const char *CGetterName = getterNameString.data() + LengthOfPrefix;
      // Make sure that first character after "is" or "get" prefix can
      // start an identifier.
      if (!isIdentifierHead(CGetterName[0]))
        return false;
      if (CGetterName[0] && isUppercase(CGetterName[0])) {
        getterName = &Ctx.Idents.get(CGetterName);
        SetterSelector =
        SelectorTable::constructSetterSelector(PP.getIdentifierTable(),
                                               PP.getSelectorTable(),
                                               getterName);
        SetterMethod = D->getInstanceMethod(SetterSelector);
      }
    }
  }
  
  if (SetterMethod) {
    if (SetterMethod->isDeprecated() ||
        !AttributesMatch(Method, SetterMethod))
      return false;
    
    // Is this a valid setter, matching the target getter?
    QualType SRT = SetterMethod->getResultType();
    if (!SRT->isVoidType())
      return false;
    const ParmVarDecl *argDecl = *SetterMethod->param_begin();
    QualType ArgType = argDecl->getType();
    if (!Ctx.hasSameUnqualifiedType(ArgType, GRT))
      return false;
    edit::Commit commit(*Editor);
    rewriteToObjCProperty(Method, SetterMethod, *NSAPIObj, commit,
                          LengthOfPrefix);
    Editor->commit(commit);
    return true;
  }
  else if (MigrateReadonlyProperty) {
    // Try a non-void method with no argument (and no setter or property of same name
    // as a 'readonly' property.
    edit::Commit commit(*Editor);
    rewriteToObjCProperty(Method, 0 /*SetterMethod*/, *NSAPIObj, commit,
                          LengthOfPrefix);
    Editor->commit(commit);
    return true;
  }
  return false;
}

void ObjCMigrateASTConsumer::migrateNsReturnsInnerPointer(ASTContext &Ctx,
                                                          ObjCMethodDecl *OM) {
  if (OM->hasAttr<ObjCReturnsInnerPointerAttr>())
    return;
  
  QualType RT = OM->getResultType();
  if (!TypeIsInnerPointer(RT) ||
      !Ctx.Idents.get("NS_RETURNS_INNER_POINTER").hasMacroDefinition())
    return;
  
  edit::Commit commit(*Editor);
  commit.insertBefore(OM->getLocEnd(), " NS_RETURNS_INNER_POINTER");
  Editor->commit(commit);
}

void ObjCMigrateASTConsumer::migrateMethods(ASTContext &Ctx,
                                                 ObjCContainerDecl *CDecl) {
  if (CDecl->isDeprecated())
    return;
  
  // migrate methods which can have instancetype as their result type.
  for (ObjCContainerDecl::method_iterator M = CDecl->meth_begin(),
       MEnd = CDecl->meth_end();
       M != MEnd; ++M) {
    ObjCMethodDecl *Method = (*M);
    if (Method->isDeprecated())
      continue;
    migrateMethodInstanceType(Ctx, CDecl, Method);
  }
}

void ObjCMigrateASTConsumer::migrateFactoryMethod(ASTContext &Ctx,
                                                  ObjCContainerDecl *CDecl,
                                                  ObjCMethodDecl *OM,
                                                  ObjCInstanceTypeFamily OIT_Family) {
  if (OM->isInstanceMethod() ||
      OM->getResultType() == Ctx.getObjCInstanceType() ||
      !OM->getResultType()->isObjCIdType())
    return;
  
  // Candidate factory methods are + (id) NaMeXXX : ... which belong to a class
  // NSYYYNamE with matching names be at least 3 characters long.
  ObjCInterfaceDecl *IDecl = dyn_cast<ObjCInterfaceDecl>(CDecl);
  if (!IDecl) {
    if (ObjCCategoryDecl *CatDecl = dyn_cast<ObjCCategoryDecl>(CDecl))
      IDecl = CatDecl->getClassInterface();
    else if (ObjCImplDecl *ImpDecl = dyn_cast<ObjCImplDecl>(CDecl))
      IDecl = ImpDecl->getClassInterface();
  }
  if (!IDecl)
    return;
  
  std::string StringClassName = IDecl->getName();
  StringRef LoweredClassName(StringClassName);
  std::string StringLoweredClassName = LoweredClassName.lower();
  LoweredClassName = StringLoweredClassName;
  
  IdentifierInfo *MethodIdName = OM->getSelector().getIdentifierInfoForSlot(0);
  // Handle method with no name at its first selector slot; e.g. + (id):(int)x.
  if (!MethodIdName)
    return;
  
  std::string MethodName = MethodIdName->getName();
  if (OIT_Family == OIT_Singleton) {
    StringRef STRefMethodName(MethodName);
    size_t len = 0;
    if (STRefMethodName.startswith("standard"))
      len = strlen("standard");
    else if (STRefMethodName.startswith("shared"))
      len = strlen("shared");
    else if (STRefMethodName.startswith("default"))
      len = strlen("default");
    else
      return;
    MethodName = STRefMethodName.substr(len);
  }
  std::string MethodNameSubStr = MethodName.substr(0, 3);
  StringRef MethodNamePrefix(MethodNameSubStr);
  std::string StringLoweredMethodNamePrefix = MethodNamePrefix.lower();
  MethodNamePrefix = StringLoweredMethodNamePrefix;
  size_t Ix = LoweredClassName.rfind(MethodNamePrefix);
  if (Ix == StringRef::npos)
    return;
  std::string ClassNamePostfix = LoweredClassName.substr(Ix);
  StringRef LoweredMethodName(MethodName);
  std::string StringLoweredMethodName = LoweredMethodName.lower();
  LoweredMethodName = StringLoweredMethodName;
  if (!LoweredMethodName.startswith(ClassNamePostfix))
    return;
  ReplaceWithInstancetype(*this, OM);
}

static bool IsVoidStarType(QualType Ty) {
  if (!Ty->isPointerType())
    return false;
  
  while (const TypedefType *TD = dyn_cast<TypedefType>(Ty.getTypePtr()))
    Ty = TD->getDecl()->getUnderlyingType();
  
  // Is the type void*?
  const PointerType* PT = Ty->getAs<PointerType>();
  if (PT->getPointeeType().getUnqualifiedType()->isVoidType())
    return true;
  return IsVoidStarType(PT->getPointeeType());
}

/// AuditedType - This routine audits the type AT and returns false if it is one of known
/// CF object types or of the "void *" variety. It returns true if we don't care about the type
/// such as a non-pointer or pointers which have no ownership issues (such as "int *").
static bool AuditedType (QualType AT) {
  if (!AT->isAnyPointerType() && !AT->isBlockPointerType())
    return true;
  // FIXME. There isn't much we can say about CF pointer type; or is there?
  if (ento::coreFoundation::isCFObjectRef(AT) ||
      IsVoidStarType(AT) ||
      // If an ObjC object is type, assuming that it is not a CF function and
      // that it is an un-audited function.
      AT->isObjCObjectPointerType() || AT->isObjCBuiltinType())
    return false;
  // All other pointers are assumed audited as harmless.
  return true;
}

void ObjCMigrateASTConsumer::AnnotateImplicitBridging(ASTContext &Ctx) {
  if (CFFunctionIBCandidates.empty())
    return;
  if (!Ctx.Idents.get("CF_IMPLICIT_BRIDGING_ENABLED").hasMacroDefinition()) {
    CFFunctionIBCandidates.clear();
    FileId = 0;
    return;
  }
  // Insert CF_IMPLICIT_BRIDGING_ENABLE/CF_IMPLICIT_BRIDGING_DISABLED
  const Decl *FirstFD = CFFunctionIBCandidates[0];
  const Decl *LastFD  =
    CFFunctionIBCandidates[CFFunctionIBCandidates.size()-1];
  const char *PragmaString = "\nCF_IMPLICIT_BRIDGING_ENABLED\n\n";
  edit::Commit commit(*Editor);
  commit.insertBefore(FirstFD->getLocStart(), PragmaString);
  PragmaString = "\n\nCF_IMPLICIT_BRIDGING_DISABLED\n";
  SourceLocation EndLoc = LastFD->getLocEnd();
  // get location just past end of function location.
  EndLoc = PP.getLocForEndOfToken(EndLoc);
  if (isa<FunctionDecl>(LastFD)) {
    // For Methods, EndLoc points to the ending semcolon. So,
    // not of these extra work is needed.
    Token Tok;
    // get locaiton of token that comes after end of function.
    bool Failed = PP.getRawToken(EndLoc, Tok, /*IgnoreWhiteSpace=*/true);
    if (!Failed)
      EndLoc = Tok.getLocation();
  }
  commit.insertAfterToken(EndLoc, PragmaString);
  Editor->commit(commit);
  FileId = 0;
  CFFunctionIBCandidates.clear();
}

void ObjCMigrateASTConsumer::migrateCFAnnotation(ASTContext &Ctx, const Decl *Decl) {
  if (Decl->isDeprecated())
    return;
  
  if (Decl->hasAttr<CFAuditedTransferAttr>()) {
    assert(CFFunctionIBCandidates.empty() &&
           "Cannot have audited functions/methods inside user "
           "provided CF_IMPLICIT_BRIDGING_ENABLE");
    return;
  }
  
  // Finction must be annotated first.
  if (const FunctionDecl *FuncDecl = dyn_cast<FunctionDecl>(Decl)) {
    CF_BRIDGING_KIND AuditKind = migrateAddFunctionAnnotation(Ctx, FuncDecl);
    if (AuditKind == CF_BRIDGING_ENABLE) {
      CFFunctionIBCandidates.push_back(Decl);
      if (!FileId)
        FileId = PP.getSourceManager().getFileID(Decl->getLocation()).getHashValue();
    }
    else if (AuditKind == CF_BRIDGING_MAY_INCLUDE) {
      if (!CFFunctionIBCandidates.empty()) {
        CFFunctionIBCandidates.push_back(Decl);
        if (!FileId)
          FileId = PP.getSourceManager().getFileID(Decl->getLocation()).getHashValue();
      }
    }
    else
      AnnotateImplicitBridging(Ctx);
  }
  else {
    migrateAddMethodAnnotation(Ctx, cast<ObjCMethodDecl>(Decl));
    AnnotateImplicitBridging(Ctx);
  }
}

void ObjCMigrateASTConsumer::AddCFAnnotations(ASTContext &Ctx,
                                              const CallEffects &CE,
                                              const FunctionDecl *FuncDecl,
                                              bool ResultAnnotated) {
  // Annotate function.
  if (!ResultAnnotated) {
    RetEffect Ret = CE.getReturnValue();
    const char *AnnotationString = 0;
    if (Ret.getObjKind() == RetEffect::CF) {
      if (Ret.isOwned() &&
          Ctx.Idents.get("CF_RETURNS_RETAINED").hasMacroDefinition())
        AnnotationString = " CF_RETURNS_RETAINED";
      else if (Ret.notOwned() &&
               Ctx.Idents.get("CF_RETURNS_NOT_RETAINED").hasMacroDefinition())
        AnnotationString = " CF_RETURNS_NOT_RETAINED";
    }
    else if (Ret.getObjKind() == RetEffect::ObjC) {
      if (Ret.isOwned() &&
          Ctx.Idents.get("NS_RETURNS_RETAINED").hasMacroDefinition())
        AnnotationString = " NS_RETURNS_RETAINED";
    }
    
    if (AnnotationString) {
      edit::Commit commit(*Editor);
      commit.insertAfterToken(FuncDecl->getLocEnd(), AnnotationString);
      Editor->commit(commit);
    }
  }
  llvm::ArrayRef<ArgEffect> AEArgs = CE.getArgs();
  unsigned i = 0;
  for (FunctionDecl::param_const_iterator pi = FuncDecl->param_begin(),
       pe = FuncDecl->param_end(); pi != pe; ++pi, ++i) {
    const ParmVarDecl *pd = *pi;
    ArgEffect AE = AEArgs[i];
    if (AE == DecRef && !pd->getAttr<CFConsumedAttr>() &&
        Ctx.Idents.get("CF_CONSUMED").hasMacroDefinition()) {
      edit::Commit commit(*Editor);
      commit.insertBefore(pd->getLocation(), "CF_CONSUMED ");
      Editor->commit(commit);
    }
    else if (AE == DecRefMsg && !pd->getAttr<NSConsumedAttr>() &&
             Ctx.Idents.get("NS_CONSUMED").hasMacroDefinition()) {
      edit::Commit commit(*Editor);
      commit.insertBefore(pd->getLocation(), "NS_CONSUMED ");
      Editor->commit(commit);
    }
  }
}


ObjCMigrateASTConsumer::CF_BRIDGING_KIND
  ObjCMigrateASTConsumer::migrateAddFunctionAnnotation(
                                                  ASTContext &Ctx,
                                                  const FunctionDecl *FuncDecl) {
  if (FuncDecl->hasBody())
    return CF_BRIDGING_NONE;
    
  CallEffects CE  = CallEffects::getEffect(FuncDecl);
  bool FuncIsReturnAnnotated = (FuncDecl->getAttr<CFReturnsRetainedAttr>() ||
                                FuncDecl->getAttr<CFReturnsNotRetainedAttr>() ||
                                FuncDecl->getAttr<NSReturnsRetainedAttr>() ||
                                FuncDecl->getAttr<NSReturnsNotRetainedAttr>() ||
                                FuncDecl->getAttr<NSReturnsAutoreleasedAttr>());
  
  // Trivial case of when funciton is annotated and has no argument.
  if (FuncIsReturnAnnotated && FuncDecl->getNumParams() == 0)
    return CF_BRIDGING_NONE;
  
  bool ReturnCFAudited = false;
  if (!FuncIsReturnAnnotated) {
    RetEffect Ret = CE.getReturnValue();
    if (Ret.getObjKind() == RetEffect::CF &&
        (Ret.isOwned() || Ret.notOwned()))
      ReturnCFAudited = true;
    else if (!AuditedType(FuncDecl->getResultType()))
      return CF_BRIDGING_NONE;
  }
  
  // At this point result type is audited for potential inclusion.
  // Now, how about argument types.
  llvm::ArrayRef<ArgEffect> AEArgs = CE.getArgs();
  unsigned i = 0;
  bool ArgCFAudited = false;
  for (FunctionDecl::param_const_iterator pi = FuncDecl->param_begin(),
       pe = FuncDecl->param_end(); pi != pe; ++pi, ++i) {
    const ParmVarDecl *pd = *pi;
    ArgEffect AE = AEArgs[i];
    if (AE == DecRef /*CFConsumed annotated*/ || AE == IncRef) {
      if (AE == DecRef && !pd->getAttr<CFConsumedAttr>())
        ArgCFAudited = true;
      else if (AE == IncRef)
        ArgCFAudited = true;
    }
    else {
      QualType AT = pd->getType();
      if (!AuditedType(AT)) {
        AddCFAnnotations(Ctx, CE, FuncDecl, FuncIsReturnAnnotated);
        return CF_BRIDGING_NONE;
      }
    }
  }
  if (ReturnCFAudited || ArgCFAudited)
    return CF_BRIDGING_ENABLE;
  
  return CF_BRIDGING_MAY_INCLUDE;
}

void ObjCMigrateASTConsumer::migrateARCSafeAnnotation(ASTContext &Ctx,
                                                 ObjCContainerDecl *CDecl) {
  if (!isa<ObjCInterfaceDecl>(CDecl) || CDecl->isDeprecated())
    return;
  
  // migrate methods which can have instancetype as their result type.
  for (ObjCContainerDecl::method_iterator M = CDecl->meth_begin(),
       MEnd = CDecl->meth_end();
       M != MEnd; ++M) {
    ObjCMethodDecl *Method = (*M);
    migrateCFAnnotation(Ctx, Method);
  }
}

void ObjCMigrateASTConsumer::AddCFAnnotations(ASTContext &Ctx,
                                              const CallEffects &CE,
                                              const ObjCMethodDecl *MethodDecl,
                                              bool ResultAnnotated) {
  // Annotate function.
  if (!ResultAnnotated) {
    RetEffect Ret = CE.getReturnValue();
    const char *AnnotationString = 0;
    if (Ret.getObjKind() == RetEffect::CF) {
      if (Ret.isOwned() &&
          Ctx.Idents.get("CF_RETURNS_RETAINED").hasMacroDefinition())
        AnnotationString = " CF_RETURNS_RETAINED";
      else if (Ret.notOwned() &&
               Ctx.Idents.get("CF_RETURNS_NOT_RETAINED").hasMacroDefinition())
        AnnotationString = " CF_RETURNS_NOT_RETAINED";
    }
    else if (Ret.getObjKind() == RetEffect::ObjC) {
      ObjCMethodFamily OMF = MethodDecl->getMethodFamily();
      switch (OMF) {
        case clang::OMF_alloc:
        case clang::OMF_new:
        case clang::OMF_copy:
        case clang::OMF_init:
        case clang::OMF_mutableCopy:
          break;
          
        default:
          if (Ret.isOwned() &&
              Ctx.Idents.get("NS_RETURNS_RETAINED").hasMacroDefinition())
            AnnotationString = " NS_RETURNS_RETAINED";
          break;
      }
    }
    
    if (AnnotationString) {
      edit::Commit commit(*Editor);
      commit.insertBefore(MethodDecl->getLocEnd(), AnnotationString);
      Editor->commit(commit);
    }
  }
  llvm::ArrayRef<ArgEffect> AEArgs = CE.getArgs();
  unsigned i = 0;
  for (ObjCMethodDecl::param_const_iterator pi = MethodDecl->param_begin(),
       pe = MethodDecl->param_end(); pi != pe; ++pi, ++i) {
    const ParmVarDecl *pd = *pi;
    ArgEffect AE = AEArgs[i];
    if (AE == DecRef && !pd->getAttr<CFConsumedAttr>() &&
        Ctx.Idents.get("CF_CONSUMED").hasMacroDefinition()) {
      edit::Commit commit(*Editor);
      commit.insertBefore(pd->getLocation(), "CF_CONSUMED ");
      Editor->commit(commit);
    }
  }
}

void ObjCMigrateASTConsumer::migrateAddMethodAnnotation(
                                            ASTContext &Ctx,
                                            const ObjCMethodDecl *MethodDecl) {
  if (MethodDecl->hasBody() || MethodDecl->isImplicit())
    return;
  
  CallEffects CE  = CallEffects::getEffect(MethodDecl);
  bool MethodIsReturnAnnotated = (MethodDecl->getAttr<CFReturnsRetainedAttr>() ||
                                  MethodDecl->getAttr<CFReturnsNotRetainedAttr>() ||
                                  MethodDecl->getAttr<NSReturnsRetainedAttr>() ||
                                  MethodDecl->getAttr<NSReturnsNotRetainedAttr>() ||
                                  MethodDecl->getAttr<NSReturnsAutoreleasedAttr>());
  
  if (CE.getReceiver() ==  DecRefMsg &&
      !MethodDecl->getAttr<NSConsumesSelfAttr>() &&
      MethodDecl->getMethodFamily() != OMF_init &&
      MethodDecl->getMethodFamily() != OMF_release &&
      Ctx.Idents.get("NS_CONSUMES_SELF").hasMacroDefinition()) {
    edit::Commit commit(*Editor);
    commit.insertBefore(MethodDecl->getLocEnd(), " NS_CONSUMES_SELF");
    Editor->commit(commit);
  }
  
  // Trivial case of when funciton is annotated and has no argument.
  if (MethodIsReturnAnnotated &&
      (MethodDecl->param_begin() == MethodDecl->param_end()))
    return;
  
  if (!MethodIsReturnAnnotated) {
    RetEffect Ret = CE.getReturnValue();
    if ((Ret.getObjKind() == RetEffect::CF ||
         Ret.getObjKind() == RetEffect::ObjC) &&
        (Ret.isOwned() || Ret.notOwned())) {
      AddCFAnnotations(Ctx, CE, MethodDecl, false);
      return;
    }
    else if (!AuditedType(MethodDecl->getResultType()))
      return;
  }
  
  // At this point result type is either annotated or audited.
  // Now, how about argument types.
  llvm::ArrayRef<ArgEffect> AEArgs = CE.getArgs();
  unsigned i = 0;
  for (ObjCMethodDecl::param_const_iterator pi = MethodDecl->param_begin(),
       pe = MethodDecl->param_end(); pi != pe; ++pi, ++i) {
    const ParmVarDecl *pd = *pi;
    ArgEffect AE = AEArgs[i];
    if ((AE == DecRef && !pd->getAttr<CFConsumedAttr>()) || AE == IncRef ||
        !AuditedType(pd->getType())) {
      AddCFAnnotations(Ctx, CE, MethodDecl, MethodIsReturnAnnotated);
      return;
    }
  }
  return;
}

namespace {

class RewritesReceiver : public edit::EditsReceiver {
  Rewriter &Rewrite;

public:
  RewritesReceiver(Rewriter &Rewrite) : Rewrite(Rewrite) { }

  virtual void insert(SourceLocation loc, StringRef text) {
    Rewrite.InsertText(loc, text);
  }
  virtual void replace(CharSourceRange range, StringRef text) {
    Rewrite.ReplaceText(range.getBegin(), Rewrite.getRangeSize(range), text);
  }
};

}

void ObjCMigrateASTConsumer::HandleTranslationUnit(ASTContext &Ctx) {
  
  TranslationUnitDecl *TU = Ctx.getTranslationUnitDecl();
  if (MigrateProperty) {
    for (DeclContext::decl_iterator D = TU->decls_begin(), DEnd = TU->decls_end();
         D != DEnd; ++D) {
      if (unsigned FID =
            PP.getSourceManager().getFileID((*D)->getLocation()).getHashValue())
        if (FileId && FileId != FID)
          AnnotateImplicitBridging(Ctx);
          
      if (ObjCInterfaceDecl *CDecl = dyn_cast<ObjCInterfaceDecl>(*D))
        migrateObjCInterfaceDecl(Ctx, CDecl);
      if (ObjCCategoryDecl *CatDecl = dyn_cast<ObjCCategoryDecl>(*D))
        migrateObjCInterfaceDecl(Ctx, CatDecl);
      else if (ObjCProtocolDecl *PDecl = dyn_cast<ObjCProtocolDecl>(*D))
        ObjCProtocolDecls.insert(PDecl);
      else if (const ObjCImplementationDecl *ImpDecl =
               dyn_cast<ObjCImplementationDecl>(*D))
        migrateProtocolConformance(Ctx, ImpDecl);
      else if (const EnumDecl *ED = dyn_cast<EnumDecl>(*D)) {
        DeclContext::decl_iterator N = D;
        ++N;
        if (N != DEnd)
          if (const TypedefDecl *TD = dyn_cast<TypedefDecl>(*N))
            migrateNSEnumDecl(Ctx, ED, TD);
      }
      else if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(*D))
        migrateCFAnnotation(Ctx, FD);
      
      if (ObjCContainerDecl *CDecl = dyn_cast<ObjCContainerDecl>(*D)) {
        // migrate methods which can have instancetype as their result type.
        migrateMethods(Ctx, CDecl);
        // annotate methods with CF annotations.
        migrateARCSafeAnnotation(Ctx, CDecl);
      }
    }
    AnnotateImplicitBridging(Ctx);
  }
  
  Rewriter rewriter(Ctx.getSourceManager(), Ctx.getLangOpts());
  RewritesReceiver Rec(rewriter);
  Editor->applyRewrites(Rec);

  for (Rewriter::buffer_iterator
        I = rewriter.buffer_begin(), E = rewriter.buffer_end(); I != E; ++I) {
    FileID FID = I->first;
    RewriteBuffer &buf = I->second;
    const FileEntry *file = Ctx.getSourceManager().getFileEntryForID(FID);
    assert(file);
    SmallString<512> newText;
    llvm::raw_svector_ostream vecOS(newText);
    buf.write(vecOS);
    vecOS.flush();
    llvm::MemoryBuffer *memBuf = llvm::MemoryBuffer::getMemBufferCopy(
                   StringRef(newText.data(), newText.size()), file->getName());
    SmallString<64> filePath(file->getName());
    FileMgr.FixupRelativePath(filePath);
    Remapper.remap(filePath.str(), memBuf);
  }

  if (IsOutputFile) {
    Remapper.flushToFile(MigrateDir, Ctx.getDiagnostics());
  } else {
    Remapper.flushToDisk(MigrateDir, Ctx.getDiagnostics());
  }
}

bool MigrateSourceAction::BeginInvocation(CompilerInstance &CI) {
  CI.getDiagnostics().setIgnoreAllWarnings(true);
  return true;
}

ASTConsumer *MigrateSourceAction::CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef InFile) {
  PPConditionalDirectiveRecord *
    PPRec = new PPConditionalDirectiveRecord(CI.getSourceManager());
  CI.getPreprocessor().addPPCallbacks(PPRec);
  return new ObjCMigrateASTConsumer(CI.getFrontendOpts().OutputFile,
                                    /*MigrateLiterals=*/true,
                                    /*MigrateSubscripting=*/true,
                                    /*MigrateProperty*/true,
                                    /*MigrateReadonlyProperty*/true,
                                    Remapper,
                                    CI.getFileManager(),
                                    PPRec,
                                    CI.getPreprocessor(),
                                    /*isOutputFile=*/true); 
}
