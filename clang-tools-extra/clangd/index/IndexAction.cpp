//===--- IndexAction.cpp -----------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "IndexAction.h"
#include "Headers.h"
#include "Logger.h"
#include "index/Relation.h"
#include "index/SymbolOrigin.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <memory>
#include <utility>

namespace clang {
namespace clangd {
namespace {

llvm::Optional<std::string> toURI(const FileEntry *File) {
  if (!File)
    return llvm::None;
  auto AbsolutePath = File->tryGetRealPathName();
  if (AbsolutePath.empty())
    return llvm::None;
  return URI::create(AbsolutePath).toString();
}

// Collects the nodes and edges of include graph during indexing action.
// Important: The graph generated by those callbacks might contain cycles and
// self edges.
struct IncludeGraphCollector : public PPCallbacks {
public:
  IncludeGraphCollector(const SourceManager &SM, IncludeGraph &IG)
      : SM(SM), IG(IG) {}

  // Populates everything except direct includes for a node, which represents
  // edges in the include graph and populated in inclusion directive.
  // We cannot populate the fields in InclusionDirective because it does not
  // have access to the contents of the included file.
  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind FileType,
                   FileID PrevFID) override {
    // We only need to process each file once. So we don't care about anything
    // but entries.
    if (Reason != FileChangeReason::EnterFile)
      return;

    const auto FileID = SM.getFileID(Loc);
    const auto File = SM.getFileEntryForID(FileID);
    auto URI = toURI(File);
    if (!URI)
      return;
    auto I = IG.try_emplace(*URI).first;

    auto &Node = I->getValue();
    // Node has already been populated.
    if (Node.URI.data() == I->getKeyData()) {
#ifndef NDEBUG
      auto Digest = digestFile(SM, FileID);
      assert(Digest && Node.Digest == *Digest &&
             "Same file, different digest?");
#endif
      return;
    }
    if (auto Digest = digestFile(SM, FileID))
      Node.Digest = std::move(*Digest);
    if (FileID == SM.getMainFileID())
      Node.Flags |= IncludeGraphNode::SourceFlag::IsTU;
    Node.URI = I->getKey();
  }

  // Add edges from including files to includes.
  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          llvm::StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange, const FileEntry *File,
                          llvm::StringRef SearchPath,
                          llvm::StringRef RelativePath, const Module *Imported,
                          SrcMgr::CharacteristicKind FileType) override {
    auto IncludeURI = toURI(File);
    if (!IncludeURI)
      return;

    auto IncludingURI = toURI(SM.getFileEntryForID(SM.getFileID(HashLoc)));
    if (!IncludingURI)
      return;

    auto NodeForInclude = IG.try_emplace(*IncludeURI).first->getKey();
    auto NodeForIncluding = IG.try_emplace(*IncludingURI);

    NodeForIncluding.first->getValue().DirectIncludes.push_back(NodeForInclude);
  }

  // Sanity check to ensure we have already populated a skipped file.
  void FileSkipped(const FileEntry &SkippedFile, const Token &FilenameTok,
                   SrcMgr::CharacteristicKind FileType) override {
#ifndef NDEBUG
    auto URI = toURI(&SkippedFile);
    if (!URI)
      return;
    auto I = IG.try_emplace(*URI);
    assert(!I.second && "File inserted for the first time on skip.");
    assert(I.first->getKeyData() == I.first->getValue().URI.data() &&
           "Node have not been populated yet");
#endif
  }

private:
  const SourceManager &SM;
  IncludeGraph &IG;
};

/// Returns an ASTConsumer that wraps \p Inner and additionally instructs the
/// parser to skip bodies of functions in the files that should not be
/// processed.
static std::unique_ptr<ASTConsumer>
skipProcessedFunctions(std::unique_ptr<ASTConsumer> Inner,
                       std::function<bool(FileID)> ShouldIndexFile) {
  class SkipProcessedFunctions : public ASTConsumer {
  public:
    SkipProcessedFunctions(std::function<bool(FileID)> FileFilter)
        : ShouldIndexFile(std::move(FileFilter)), Context(nullptr) {
      assert(this->ShouldIndexFile);
    }

    void Initialize(ASTContext &Context) override { this->Context = &Context; }
    bool shouldSkipFunctionBody(Decl *D) override {
      assert(Context && "Initialize() was never called.");
      auto &SM = Context->getSourceManager();
      auto FID = SM.getFileID(SM.getExpansionLoc(D->getLocation()));
      if (!FID.isValid())
        return false;
      return !ShouldIndexFile(FID);
    }

  private:
    std::function<bool(FileID)> ShouldIndexFile;
    const ASTContext *Context;
  };
  std::vector<std::unique_ptr<ASTConsumer>> Consumers;
  Consumers.push_back(
      std::make_unique<SkipProcessedFunctions>(ShouldIndexFile));
  Consumers.push_back(std::move(Inner));
  return std::make_unique<MultiplexConsumer>(std::move(Consumers));
}

// Wraps the index action and reports index data after each translation unit.
class IndexAction : public WrapperFrontendAction {
public:
  IndexAction(std::shared_ptr<SymbolCollector> C,
              std::unique_ptr<CanonicalIncludes> Includes,
              const index::IndexingOptions &Opts,
              std::function<void(SymbolSlab)> SymbolsCallback,
              std::function<void(RefSlab)> RefsCallback,
              std::function<void(RelationSlab)> RelationsCallback,
              std::function<void(IncludeGraph)> IncludeGraphCallback)
      : WrapperFrontendAction(index::createIndexingAction(C, Opts, nullptr)),
        SymbolsCallback(SymbolsCallback), RefsCallback(RefsCallback),
        RelationsCallback(RelationsCallback),
        IncludeGraphCallback(IncludeGraphCallback), Collector(C),
        Includes(std::move(Includes)),
        PragmaHandler(collectIWYUHeaderMaps(this->Includes.get())) {}

  std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &CI, llvm::StringRef InFile) override {
    CI.getPreprocessor().addCommentHandler(PragmaHandler.get());
    addSystemHeadersMapping(Includes.get(), CI.getLangOpts());
    if (IncludeGraphCallback != nullptr)
      CI.getPreprocessor().addPPCallbacks(
          std::make_unique<IncludeGraphCollector>(CI.getSourceManager(), IG));
    return skipProcessedFunctions(
        WrapperFrontendAction::CreateASTConsumer(CI, InFile),
        [this](FileID FID) { return Collector->shouldIndexFile(FID); });
  }

  bool BeginInvocation(CompilerInstance &CI) override {
    // We want all comments, not just the doxygen ones.
    CI.getLangOpts().CommentOpts.ParseAllComments = true;
    // Index the whole file even if there are warnings and -Werror is set.
    // Avoids some analyses too. Set in two places as we're late to the party.
    CI.getDiagnosticOpts().IgnoreWarnings = true;
    CI.getDiagnostics().setIgnoreAllWarnings(true);
    // Instruct the parser to ask our ASTConsumer if it should skip function
    // bodies. The ASTConsumer will take care of skipping only functions inside
    // the files that we have already processed.
    CI.getFrontendOpts().SkipFunctionBodies = true;

    return WrapperFrontendAction::BeginInvocation(CI);
  }

  void EndSourceFileAction() override {
    WrapperFrontendAction::EndSourceFileAction();

    SymbolsCallback(Collector->takeSymbols());
    if (RefsCallback != nullptr)
      RefsCallback(Collector->takeRefs());
    if (RelationsCallback != nullptr)
      RelationsCallback(Collector->takeRelations());
    if (IncludeGraphCallback != nullptr) {
#ifndef NDEBUG
      // This checks if all nodes are initialized.
      for (const auto &Node : IG)
        assert(Node.getKeyData() == Node.getValue().URI.data());
#endif
      IncludeGraphCallback(std::move(IG));
    }
  }

private:
  std::function<void(SymbolSlab)> SymbolsCallback;
  std::function<void(RefSlab)> RefsCallback;
  std::function<void(RelationSlab)> RelationsCallback;
  std::function<void(IncludeGraph)> IncludeGraphCallback;
  std::shared_ptr<SymbolCollector> Collector;
  std::unique_ptr<CanonicalIncludes> Includes;
  std::unique_ptr<CommentHandler> PragmaHandler;
  IncludeGraph IG;
};

} // namespace

std::unique_ptr<FrontendAction> createStaticIndexingAction(
    SymbolCollector::Options Opts,
    std::function<void(SymbolSlab)> SymbolsCallback,
    std::function<void(RefSlab)> RefsCallback,
    std::function<void(RelationSlab)> RelationsCallback,
    std::function<void(IncludeGraph)> IncludeGraphCallback) {
  index::IndexingOptions IndexOpts;
  IndexOpts.SystemSymbolFilter =
      index::IndexingOptions::SystemSymbolFilterKind::All;
  Opts.CollectIncludePath = true;
  if (Opts.Origin == SymbolOrigin::Unknown)
    Opts.Origin = SymbolOrigin::Static;
  Opts.StoreAllDocumentation = false;
  if (RefsCallback != nullptr) {
    Opts.RefFilter = RefKind::All;
    Opts.RefsInHeaders = true;
  }
  auto Includes = std::make_unique<CanonicalIncludes>();
  Opts.Includes = Includes.get();
  return std::make_unique<IndexAction>(
      std::make_shared<SymbolCollector>(std::move(Opts)), std::move(Includes),
      IndexOpts, SymbolsCallback, RefsCallback, RelationsCallback,
      IncludeGraphCallback);
}

} // namespace clangd
} // namespace clang
