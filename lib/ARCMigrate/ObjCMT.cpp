//===--- ObjCMT.cpp - ObjC Migrate Tool -----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

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
#include "llvm/ADT/SmallString.h"

using namespace clang;
using namespace arcmt;

namespace {

class ObjCMigrateASTConsumer : public ASTConsumer {
  void migrateDecl(Decl *D);
  void migrateObjCInterfaceDecl(ASTContext &Ctx, ObjCInterfaceDecl *D);
  void migrateProtocolConformance(ASTContext &Ctx,
                                  const ObjCImplementationDecl *ImpDecl);

public:
  std::string MigrateDir;
  bool MigrateLiterals;
  bool MigrateSubscripting;
  bool MigrateProperty;
  OwningPtr<NSAPI> NSAPIObj;
  OwningPtr<edit::EditedSource> Editor;
  FileRemapper &Remapper;
  FileManager &FileMgr;
  const PPConditionalDirectiveRecord *PPRec;
  Preprocessor &PP;
  bool IsOutputFile;
  llvm::SmallPtrSet<ObjCProtocolDecl *, 32> ObjCProtocolDecls;
  
  ObjCMigrateASTConsumer(StringRef migrateDir,
                         bool migrateLiterals,
                         bool migrateSubscripting,
                         bool migrateProperty,
                         FileRemapper &remapper,
                         FileManager &fileMgr,
                         const PPConditionalDirectiveRecord *PPRec,
                         Preprocessor &PP,
                         bool isOutputFile = false)
  : MigrateDir(migrateDir),
    MigrateLiterals(migrateLiterals),
    MigrateSubscripting(migrateSubscripting),
    MigrateProperty(migrateProperty),
    Remapper(remapper), FileMgr(fileMgr), PPRec(PPRec), PP(PP),
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
                             bool migrateProperty)
  : WrapperFrontendAction(WrappedAction), MigrateDir(migrateDir),
    MigrateLiterals(migrateLiterals), MigrateSubscripting(migrateSubscripting),
    MigrateProperty(migrateProperty),
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

void ObjCMigrateASTConsumer::migrateObjCInterfaceDecl(ASTContext &Ctx,
                                                      ObjCInterfaceDecl *D) {
  for (ObjCContainerDecl::method_iterator M = D->meth_begin(), MEnd = D->meth_end();
       M != MEnd; ++M) {
    ObjCMethodDecl *Method = (*M);
    if (Method->isPropertyAccessor() ||  Method->param_size() != 0)
      continue;
    // Is this method candidate to be a getter?
    QualType GRT = Method->getResultType();
    if (GRT->isVoidType())
      continue;
    // FIXME. Don't know what todo with attributes, skip for now.
    if (Method->hasAttrs())
      continue;
    
    Selector GetterSelector = Method->getSelector();
    IdentifierInfo *getterName = GetterSelector.getIdentifierInfoForSlot(0);
    Selector SetterSelector =
      SelectorTable::constructSetterSelector(PP.getIdentifierTable(),
                                             PP.getSelectorTable(),
                                             getterName);
    if (ObjCMethodDecl *SetterMethod = D->lookupMethod(SetterSelector, true)) {
      // Is this a valid setter, matching the target getter?
      QualType SRT = SetterMethod->getResultType();
      if (!SRT->isVoidType())
        continue;
      const ParmVarDecl *argDecl = *SetterMethod->param_begin();
      QualType ArgType = argDecl->getType();
      if (!Ctx.hasSameUnqualifiedType(ArgType, GRT) ||
          SetterMethod->hasAttrs())
          continue;
        edit::Commit commit(*Editor);
        edit::rewriteToObjCProperty(Method, SetterMethod, *NSAPIObj, commit);
        Editor->commit(commit);
      }
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
  if (const ObjCProtocolDecl *PDecl = Protocol->getDefinition())
    for (ObjCProtocolDecl::prop_iterator P = PDecl->prop_begin(),
         E = PDecl->prop_end(); P != E; ++P) {
      ObjCPropertyDecl *Property = *P;
      if (Property->getPropertyImplementation() == ObjCPropertyDecl::Optional)
        continue;
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
  if (const ObjCProtocolDecl *PDecl = Protocol->getDefinition()) {
    if (PDecl->meth_begin() == PDecl->meth_end())
      return false;
    for (ObjCContainerDecl::method_iterator M = PDecl->meth_begin(),
         MEnd = PDecl->meth_end(); M != MEnd; ++M) {
      ObjCMethodDecl *MD = (*M);
      if (MD->isImplicit())
        continue;
      if (MD->getImplementationControl() == ObjCMethodDecl::Optional)
        continue;
      bool match = false;
      DeclContext::lookup_const_result R = ImpDecl->lookup(MD->getDeclName());
      if (R.size() == 0)
        return false;
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

  return true;
}

void ObjCMigrateASTConsumer::migrateProtocolConformance(ASTContext &Ctx,
                                            const ObjCImplementationDecl *ImpDecl) {
  const ObjCInterfaceDecl *IDecl = ImpDecl->getClassInterface();
  if (!IDecl || ObjCProtocolDecls.empty())
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
  edit::rewriteToObjCInterfaceDecl(IDecl, MinimalConformingProtocols,
                                   *NSAPIObj, commit);
  Editor->commit(commit);
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
  if (MigrateProperty)
    for (DeclContext::decl_iterator D = TU->decls_begin(), DEnd = TU->decls_end();
         D != DEnd; ++D) {
      if (ObjCInterfaceDecl *CDecl = dyn_cast<ObjCInterfaceDecl>(*D))
        migrateObjCInterfaceDecl(Ctx, CDecl);
      else if (ObjCProtocolDecl *PDecl = dyn_cast<ObjCProtocolDecl>(*D))
        ObjCProtocolDecls.insert(PDecl);
      else if (const ObjCImplementationDecl *ImpDecl =
               dyn_cast<ObjCImplementationDecl>(*D))
        migrateProtocolConformance(Ctx, ImpDecl);
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
                                    Remapper,
                                    CI.getFileManager(),
                                    PPRec,
                                    CI.getPreprocessor(),
                                    /*isOutputFile=*/true); 
}
