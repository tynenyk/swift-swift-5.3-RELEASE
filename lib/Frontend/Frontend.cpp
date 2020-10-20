//===--- Frontend.cpp - frontend utility methods --------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains utility methods for parsing and performing semantic
// on modules.
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/Frontend.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/IncrementalRanges.h"
#include "swift/AST/Module.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Parse/Lexer.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/Utils/Generics.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

using namespace swift;

CompilerInstance::CompilerInstance() = default;
CompilerInstance::~CompilerInstance() = default;

std::string CompilerInvocation::getPCHHash() const {
  using llvm::hash_combine;

  auto Code = hash_combine(LangOpts.getPCHHashComponents(),
                           FrontendOpts.getPCHHashComponents(),
                           ClangImporterOpts.getPCHHashComponents(),
                           SearchPathOpts.getPCHHashComponents(),
                           DiagnosticOpts.getPCHHashComponents(),
                           SILOpts.getPCHHashComponents(),
                           IRGenOpts.getPCHHashComponents());

  return llvm::APInt(64, Code).toString(36, /*Signed=*/false);
}

const PrimarySpecificPaths &
CompilerInvocation::getPrimarySpecificPathsForAtMostOnePrimary() const {
  return getFrontendOptions().getPrimarySpecificPathsForAtMostOnePrimary();
}

const PrimarySpecificPaths &
CompilerInvocation::getPrimarySpecificPathsForPrimary(
    StringRef filename) const {
  return getFrontendOptions().getPrimarySpecificPathsForPrimary(filename);
}

const PrimarySpecificPaths &
CompilerInvocation::getPrimarySpecificPathsForSourceFile(
    const SourceFile &SF) const {
  return getPrimarySpecificPathsForPrimary(SF.getFilename());
}

std::string CompilerInvocation::getOutputFilenameForAtMostOnePrimary() const {
  return getPrimarySpecificPathsForAtMostOnePrimary().OutputFilename;
}
std::string
CompilerInvocation::getMainInputFilenameForDebugInfoForAtMostOnePrimary()
    const {
  return getPrimarySpecificPathsForAtMostOnePrimary()
      .MainInputFilenameForDebugInfo;
}
std::string
CompilerInvocation::getObjCHeaderOutputPathForAtMostOnePrimary() const {
  return getPrimarySpecificPathsForAtMostOnePrimary()
      .SupplementaryOutputs.ObjCHeaderOutputPath;
}
std::string CompilerInvocation::getModuleOutputPathForAtMostOnePrimary() const {
  return getPrimarySpecificPathsForAtMostOnePrimary()
      .SupplementaryOutputs.ModuleOutputPath;
}
std::string CompilerInvocation::getReferenceDependenciesFilePathForPrimary(
    StringRef filename) const {
  return getPrimarySpecificPathsForPrimary(filename)
      .SupplementaryOutputs.ReferenceDependenciesFilePath;
}
std::string
CompilerInvocation::getSwiftRangesFilePathForPrimary(StringRef filename) const {
  return getPrimarySpecificPathsForPrimary(filename)
      .SupplementaryOutputs.SwiftRangesFilePath;
}
std::string CompilerInvocation::getCompiledSourceFilePathForPrimary(
    StringRef filename) const {
  return getPrimarySpecificPathsForPrimary(filename)
      .SupplementaryOutputs.CompiledSourceFilePath;
}
std::string
CompilerInvocation::getSerializedDiagnosticsPathForAtMostOnePrimary() const {
  return getPrimarySpecificPathsForAtMostOnePrimary()
      .SupplementaryOutputs.SerializedDiagnosticsPath;
}
std::string CompilerInvocation::getTBDPathForWholeModule() const {
  assert(getFrontendOptions().InputsAndOutputs.isWholeModule() &&
         "TBDPath only makes sense when the whole module can be seen");
  return getPrimarySpecificPathsForAtMostOnePrimary()
      .SupplementaryOutputs.TBDPath;
}

std::string
CompilerInvocation::getLdAddCFileOutputPathForWholeModule() const {
  assert(getFrontendOptions().InputsAndOutputs.isWholeModule() &&
         "LdAdd cfile only makes sense when the whole module can be seen");
  return getPrimarySpecificPathsForAtMostOnePrimary()
    .SupplementaryOutputs.LdAddCFilePath;
}

std::string
CompilerInvocation::getModuleInterfaceOutputPathForWholeModule() const {
  assert(getFrontendOptions().InputsAndOutputs.isWholeModule() &&
         "ModuleInterfaceOutputPath only makes sense when the whole module "
         "can be seen");
  return getPrimarySpecificPathsForAtMostOnePrimary()
      .SupplementaryOutputs.ModuleInterfaceOutputPath;
}

std::string
CompilerInvocation::getPrivateModuleInterfaceOutputPathForWholeModule() const {
  assert(getFrontendOptions().InputsAndOutputs.isWholeModule() &&
         "PrivateModuleInterfaceOutputPath only makes sense when the whole "
         "module can be seen");
  return getPrimarySpecificPathsForAtMostOnePrimary()
      .SupplementaryOutputs.PrivateModuleInterfaceOutputPath;
}

SerializationOptions CompilerInvocation::computeSerializationOptions(
    const SupplementaryOutputPaths &outs, const ModuleDecl *module) const {
  const FrontendOptions &opts = getFrontendOptions();

  SerializationOptions serializationOpts;
  serializationOpts.OutputPath = outs.ModuleOutputPath.c_str();
  serializationOpts.DocOutputPath = outs.ModuleDocOutputPath.c_str();
  serializationOpts.SourceInfoOutputPath = outs.ModuleSourceInfoOutputPath.c_str();
  serializationOpts.GroupInfoPath = opts.GroupInfoPath.c_str();
  if (opts.SerializeBridgingHeader && !outs.ModuleOutputPath.empty())
    serializationOpts.ImportedHeader = opts.ImplicitObjCHeaderPath;
  serializationOpts.ModuleLinkName = opts.ModuleLinkName;
  serializationOpts.ExtraClangOptions = getClangImporterOptions().ExtraArgs;
  if (!getIRGenOptions().ForceLoadSymbolName.empty())
    serializationOpts.AutolinkForceLoad = true;

  // Options contain information about the developer's computer,
  // so only serialize them if the module isn't going to be shipped to
  // the public.
  serializationOpts.SerializeOptionsForDebugging =
      opts.SerializeOptionsForDebugging.getValueOr(
          !isModuleExternallyConsumed(module));

  return serializationOpts;
}

Lowering::TypeConverter &CompilerInstance::getSILTypes() {
  if (auto *tc = TheSILTypes.get())
    return *tc;
  
  auto *tc = new Lowering::TypeConverter(*getMainModule());
  TheSILTypes.reset(tc);
  return *tc;
}

void CompilerInstance::createSILModule() {
  assert(MainModule && "main module not created yet");
  // Assume WMO if a -primary-file option was not provided.
  TheSILModule = SILModule::createEmptyModule(
      getMainModule(), getSILTypes(), Invocation.getSILOptions(),
      Invocation.getFrontendOptions().InputsAndOutputs.isWholeModule());
}

void CompilerInstance::recordPrimaryInputBuffer(unsigned BufID) {
  PrimaryBufferIDs.insert(BufID);
}

void CompilerInstance::recordPrimarySourceFile(SourceFile *SF) {
  assert(MainModule && "main module not created yet");
  PrimarySourceFiles.push_back(SF);
  SF->enableInterfaceHash();
  SF->createReferencedNameTracker();
  if (SF->getBufferID().hasValue())
    recordPrimaryInputBuffer(SF->getBufferID().getValue());
}

bool CompilerInstance::setUpASTContextIfNeeded() {
  if (Invocation.getFrontendOptions().RequestedAction ==
      FrontendOptions::ActionType::CompileModuleFromInterface) {
    // Compiling a module interface from source uses its own CompilerInstance
    // with options read from the input file. Don't bother setting up an
    // ASTContext at this level.
    return false;
  }

  Context.reset(ASTContext::get(
      Invocation.getLangOptions(), Invocation.getTypeCheckerOptions(),
      Invocation.getSearchPathOptions(), SourceMgr, Diagnostics));
  registerParseRequestFunctions(Context->evaluator);
  registerTypeCheckerRequestFunctions(Context->evaluator);
  registerSILGenRequestFunctions(Context->evaluator);
  registerSILOptimizerRequestFunctions(Context->evaluator);
  registerTBDGenRequestFunctions(Context->evaluator);
  registerIRGenRequestFunctions(Context->evaluator);
  
  // Migrator, indexing and typo correction need some IDE requests.
  // The integrated REPL needs IDE requests for completion.
  if (Invocation.getMigratorOptions().shouldRunMigrator() ||
      !Invocation.getFrontendOptions().IndexStorePath.empty() ||
      Invocation.getLangOptions().TypoCorrectionLimit ||
      Invocation.getFrontendOptions().RequestedAction ==
          FrontendOptions::ActionType::REPL) {
    registerIDERequestFunctions(Context->evaluator);
  }

  registerIRGenSILTransforms(*Context);

  if (setUpModuleLoaders())
    return true;

  return false;
}

void CompilerInstance::setupStatsReporter() {
  const auto &Invok = getInvocation();
  const std::string &StatsOutputDir =
      Invok.getFrontendOptions().StatsOutputDir;
  if (StatsOutputDir.empty())
    return;

  auto silOptModeArgStr = [](OptimizationMode mode) -> StringRef {
    switch (mode) {
    case OptimizationMode::ForSpeed:
      return "O";
    case OptimizationMode::ForSize:
      return "Osize";
    default:
      return "Onone";
    }
  };

  auto getClangSourceManager = [](ASTContext &Ctx) -> clang::SourceManager * {
    if (auto *clangImporter = static_cast<ClangImporter *>(
            Ctx.getClangModuleLoader())) {
      return &clangImporter->getClangASTContext().getSourceManager();
    }
    return nullptr;
  };

  const auto &FEOpts = Invok.getFrontendOptions();
  const auto &LangOpts = Invok.getLangOptions();
  const auto &SILOpts = Invok.getSILOptions();
  const std::string &OutFile =
      FEOpts.InputsAndOutputs.lastInputProducingOutput().outputFilename();
  auto Reporter = std::make_unique<UnifiedStatsReporter>(
      "swift-frontend",
      FEOpts.ModuleName,
      FEOpts.InputsAndOutputs.getStatsFileMangledInputName(),
      LangOpts.Target.normalize(),
      llvm::sys::path::extension(OutFile),
      silOptModeArgStr(SILOpts.OptMode),
      StatsOutputDir,
      &getSourceMgr(),
      getClangSourceManager(getASTContext()),
      Invok.getFrontendOptions().TraceStats,
      Invok.getFrontendOptions().ProfileEvents,
      Invok.getFrontendOptions().ProfileEntities);
  // Hand the stats reporter down to the ASTContext so the rest of the compiler
  // can use it.
  getASTContext().setStatsReporter(Reporter.get());
  Stats = std::move(Reporter);
}

void CompilerInstance::setupDiagnosticVerifierIfNeeded() {
  auto &diagOpts = Invocation.getDiagnosticOptions();
  if (diagOpts.VerifyMode != DiagnosticOptions::NoVerify) {
    DiagVerifier = std::make_unique<DiagnosticVerifier>(
        SourceMgr, InputSourceCodeBufferIDs,
        diagOpts.VerifyMode == DiagnosticOptions::VerifyAndApplyFixes,
        diagOpts.VerifyIgnoreUnknown);
    addDiagnosticConsumer(DiagVerifier.get());
  }
}

bool CompilerInstance::setup(const CompilerInvocation &Invok) {
  Invocation = Invok;

  // If initializing the overlay file system fails there's no sense in
  // continuing because the compiler will read the wrong files.
  if (setUpVirtualFileSystemOverlays())
    return true;
  setUpLLVMArguments();
  setUpDiagnosticOptions();

  const auto &frontendOpts = Invocation.getFrontendOptions();

  // If we are asked to emit a module documentation file, configure lexing and
  // parsing to remember comments.
  if (frontendOpts.InputsAndOutputs.hasModuleDocOutputPath())
    Invocation.getLangOptions().AttachCommentsToDecls = true;

  // If we are doing index-while-building, configure lexing and parsing to
  // remember comments.
  if (!frontendOpts.IndexStorePath.empty()) {
    Invocation.getLangOptions().AttachCommentsToDecls = true;
  }

  // Set up the type checker options.
  auto &typeCkOpts = Invocation.getTypeCheckerOptions();
  if (isWholeModuleCompilation()) {
    typeCkOpts.DelayWholeModuleChecking = true;
  }
  if (FrontendOptions::isActionImmediate(frontendOpts.RequestedAction)) {
    typeCkOpts.InImmediateMode = true;
  }

  assert(Lexer::isIdentifier(Invocation.getModuleName()));

  if (isInSILMode())
    Invocation.getLangOptions().EnableAccessControl = false;

  if (setUpInputs())
    return true;

  if (setUpASTContextIfNeeded())
    return true;

  setupStatsReporter();
  setupDiagnosticVerifierIfNeeded();

  return false;
}

static bool loadAndValidateVFSOverlay(
    const std::string &File,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &BaseFS,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> &OverlayFS,
    DiagnosticEngine &Diag) {
  auto Buffer = BaseFS->getBufferForFile(File);
  if (!Buffer) {
    Diag.diagnose(SourceLoc(), diag::cannot_open_file, File,
                         Buffer.getError().message());
    return true;
  }

  auto VFS = llvm::vfs::getVFSFromYAML(std::move(Buffer.get()),
                                        nullptr, File);
  if (!VFS) {
    Diag.diagnose(SourceLoc(), diag::invalid_vfs_overlay_file, File);
    return true;
  }
  OverlayFS->pushOverlay(VFS);
  return false;
}

bool CompilerInstance::setUpVirtualFileSystemOverlays() {
  auto BaseFS = SourceMgr.getFileSystem();
  auto OverlayFS = llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem>(
                    new llvm::vfs::OverlayFileSystem(BaseFS));
  bool hadAnyFailure = false;
  bool hasOverlays = false;
  for (const auto &File : Invocation.getSearchPathOptions().VFSOverlayFiles) {
    hasOverlays = true;
    hadAnyFailure |=
        loadAndValidateVFSOverlay(File, BaseFS, OverlayFS, Diagnostics);
  }

  // If we successfully loaded all the overlays, let the source manager and
  // diagnostic engine take advantage of the overlay file system.
  if (!hadAnyFailure && hasOverlays) {
    SourceMgr.setFileSystem(OverlayFS);
  }

  return hadAnyFailure;
}

void CompilerInstance::setUpLLVMArguments() {
  // Honor -Xllvm.
  if (!Invocation.getFrontendOptions().LLVMArgs.empty()) {
    llvm::SmallVector<const char *, 4> Args;
    Args.push_back("swift (LLVM option parsing)");
    for (unsigned i = 0, e = Invocation.getFrontendOptions().LLVMArgs.size();
         i != e; ++i)
      Args.push_back(Invocation.getFrontendOptions().LLVMArgs[i].c_str());
    Args.push_back(nullptr);
    llvm::cl::ParseCommandLineOptions(Args.size()-1, Args.data());
  }
}

void CompilerInstance::setUpDiagnosticOptions() {
  if (Invocation.getDiagnosticOptions().ShowDiagnosticsAfterFatalError) {
    Diagnostics.setShowDiagnosticsAfterFatalError();
  }
  if (Invocation.getDiagnosticOptions().SuppressWarnings) {
    Diagnostics.setSuppressWarnings(true);
  }
  if (Invocation.getDiagnosticOptions().WarningsAsErrors) {
    Diagnostics.setWarningsAsErrors(true);
  }
  if (Invocation.getDiagnosticOptions().PrintDiagnosticNames) {
    Diagnostics.setPrintDiagnosticNames(true);
  }
  Diagnostics.setDiagnosticDocumentationPath(
      Invocation.getDiagnosticOptions().DiagnosticDocumentationPath);
}

// The ordering of ModuleLoaders is important!
//
// 1. SourceLoader: This is a hack and only the compiler's tests are using it,
//    to avoid writing repetitive code involving generating modules/interfaces.
//    Ideally, we'd get rid of it.
// 2. MemoryBufferSerializedModuleLoader: This is used by LLDB, because it might
//    already have the module available in memory.
// 3. ModuleInterfaceLoader: Tries to find an up-to-date swiftmodule. If it
//    succeeds, it issues a particular "error" (see
//    [Note: ModuleInterfaceLoader-defer-to-SerializedModuleLoader]), which
//    is interpreted by the overarching loader as a command to use the
//    SerializedModuleLoader. If we failed to find a .swiftmodule, this falls
//    back to using an interface. Actual errors lead to diagnostics.
// 4. SerializedModuleLoader: Loads a serialized module if it can.
// 5. ClangImporter: This must come after all the Swift module loaders because
//    in the presence of overlays and mixed-source frameworks, we want to prefer
//    the overlay or framework module over the underlying Clang module.
bool CompilerInstance::setUpModuleLoaders() {
  if (hasSourceImport()) {
    bool enableLibraryEvolution =
      Invocation.getFrontendOptions().EnableLibraryEvolution;
    Context->addModuleLoader(SourceLoader::create(*Context,
                                                  enableLibraryEvolution,
                                                  getDependencyTracker()));
  }
  auto MLM = ModuleLoadingMode::PreferSerialized;
  if (auto forceModuleLoadingMode =
      llvm::sys::Process::GetEnv("SWIFT_FORCE_MODULE_LOADING")) {
    if (*forceModuleLoadingMode == "prefer-interface" ||
        *forceModuleLoadingMode == "prefer-parseable")
      MLM = ModuleLoadingMode::PreferInterface;
    else if (*forceModuleLoadingMode == "prefer-serialized")
      MLM = ModuleLoadingMode::PreferSerialized;
    else if (*forceModuleLoadingMode == "only-interface" ||
             *forceModuleLoadingMode == "only-parseable")
      MLM = ModuleLoadingMode::OnlyInterface;
    else if (*forceModuleLoadingMode == "only-serialized")
      MLM = ModuleLoadingMode::OnlySerialized;
    else {
      Diagnostics.diagnose(SourceLoc(),
                           diag::unknown_forced_module_loading_mode,
                           *forceModuleLoadingMode);
      return true;
    }
  }
  auto IgnoreSourceInfoFile =
    Invocation.getFrontendOptions().IgnoreSwiftSourceInfo;
  if (Invocation.getLangOptions().EnableMemoryBufferImporter) {
    auto MemoryBufferLoader = MemoryBufferSerializedModuleLoader::create(
        *Context, getDependencyTracker(), MLM, IgnoreSourceInfoFile);
    this->MemoryBufferLoader = MemoryBufferLoader.get();
    Context->addModuleLoader(std::move(MemoryBufferLoader));
  }

  // Wire up the Clang importer. If the user has specified an SDK, use it.
  // Otherwise, we just keep it around as our interface to Clang's ABI
  // knowledge.
  std::unique_ptr<ClangImporter> clangImporter =
    ClangImporter::create(*Context, Invocation.getClangImporterOptions(),
                          Invocation.getPCHHash(), getDependencyTracker());
  if (!clangImporter) {
    Diagnostics.diagnose(SourceLoc(), diag::error_clang_importer_create_fail);
    return true;
  }

  if (MLM != ModuleLoadingMode::OnlySerialized) {
    auto const &Clang = clangImporter->getClangInstance();
    std::string ModuleCachePath = getModuleCachePathFromClang(Clang);
    auto &FEOpts = Invocation.getFrontendOptions();
    StringRef PrebuiltModuleCachePath = FEOpts.PrebuiltModuleCachePath;
    auto PIML = ModuleInterfaceLoader::create(
        *Context, ModuleCachePath, PrebuiltModuleCachePath,
        getDependencyTracker(), MLM, FEOpts.PreferInterfaceForModules,
        FEOpts.RemarkOnRebuildFromModuleInterface,
        IgnoreSourceInfoFile,
        FEOpts.DisableInterfaceFileLock);
    Context->addModuleLoader(std::move(PIML));
  }

  std::unique_ptr<SerializedModuleLoader> SML =
    SerializedModuleLoader::create(*Context, getDependencyTracker(), MLM,
                                   IgnoreSourceInfoFile);
  this->SML = SML.get();
  Context->addModuleLoader(std::move(SML));

  Context->addModuleLoader(std::move(clangImporter), /*isClang*/ true);

  return false;
}

Optional<unsigned> CompilerInstance::setUpCodeCompletionBuffer() {
  Optional<unsigned> codeCompletionBufferID;
  auto codeCompletePoint = Invocation.getCodeCompletionPoint();
  if (codeCompletePoint.first) {
    auto memBuf = codeCompletePoint.first;
    // CompilerInvocation doesn't own the buffers, copy to a new buffer.
    codeCompletionBufferID = SourceMgr.addMemBufferCopy(memBuf);
    InputSourceCodeBufferIDs.push_back(*codeCompletionBufferID);
    SourceMgr.setCodeCompletionPoint(*codeCompletionBufferID,
                                     codeCompletePoint.second);
  }
  return codeCompletionBufferID;
}

static bool shouldTreatSingleInputAsMain(InputFileKind inputKind) {
  switch (inputKind) {
  case InputFileKind::Swift:
  case InputFileKind::SwiftModuleInterface:
  case InputFileKind::SIL:
    return true;
  case InputFileKind::SwiftLibrary:
  case InputFileKind::SwiftREPL:
  case InputFileKind::LLVM:
  case InputFileKind::None:
    return false;
  }
  llvm_unreachable("unhandled input kind");
}

bool CompilerInstance::setUpInputs() {
  // Adds to InputSourceCodeBufferIDs, so may need to happen before the
  // per-input setup.
  const Optional<unsigned> codeCompletionBufferID = setUpCodeCompletionBuffer();

  for (const InputFile &input :
       Invocation.getFrontendOptions().InputsAndOutputs.getAllInputs())
    if (setUpForInput(input))
      return true;

  // Set the primary file to the code-completion point if one exists.
  if (codeCompletionBufferID.hasValue() &&
      !isPrimaryInput(*codeCompletionBufferID)) {
    assert(PrimaryBufferIDs.empty() && "re-setting PrimaryBufferID");
    recordPrimaryInputBuffer(*codeCompletionBufferID);
  }

  if (MainBufferID == NO_SUCH_BUFFER &&
      InputSourceCodeBufferIDs.size() == 1 &&
      shouldTreatSingleInputAsMain(Invocation.getInputKind())) {
    MainBufferID = InputSourceCodeBufferIDs.front();
  }

  return false;
}

bool CompilerInstance::setUpForInput(const InputFile &input) {
  bool failed = false;
  Optional<unsigned> bufferID = getRecordedBufferID(input, failed);
  if (failed)
    return true;
  if (!bufferID)
    return false;

  if (isInputSwift() &&
      llvm::sys::path::filename(input.file()) == "main.swift") {
    assert(MainBufferID == NO_SUCH_BUFFER && "re-setting MainBufferID");
    MainBufferID = *bufferID;
  }

  if (input.isPrimary()) {
    recordPrimaryInputBuffer(*bufferID);
  }
  return false;
}

Optional<unsigned> CompilerInstance::getRecordedBufferID(const InputFile &input,
                                                         bool &failed) {
  if (!input.buffer()) {
    if (Optional<unsigned> existingBufferID =
            SourceMgr.getIDForBufferIdentifier(input.file())) {
      return existingBufferID;
    }
  }
  auto buffers = getInputBuffersIfPresent(input);

  if (!buffers.hasValue()) {
    failed = true;
    return None;
  }

  // FIXME: The fact that this test happens twice, for some cases,
  // suggests that setupInputs could use another round of refactoring.
  if (serialization::isSerializedAST(buffers->ModuleBuffer->getBuffer())) {
    PartialModules.push_back(std::move(*buffers));
    return None;
  }
  assert(buffers->ModuleDocBuffer.get() == nullptr);
  assert(buffers->ModuleSourceInfoBuffer.get() == nullptr);
  // Transfer ownership of the MemoryBuffer to the SourceMgr.
  unsigned bufferID = SourceMgr.addNewSourceBuffer(std::move(buffers->ModuleBuffer));

  InputSourceCodeBufferIDs.push_back(bufferID);
  return bufferID;
}

Optional<ModuleBuffers> CompilerInstance::getInputBuffersIfPresent(
    const InputFile &input) {
  if (auto b = input.buffer()) {
    return ModuleBuffers(llvm::MemoryBuffer::getMemBufferCopy(b->getBuffer(),
                                                              b->getBufferIdentifier()));
  }
  // FIXME: Working with filenames is fragile, maybe use the real path
  // or have some kind of FileManager.
  using FileOrError = llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>;
  FileOrError inputFileOrErr = swift::vfs::getFileOrSTDIN(getFileSystem(),
                                                          input.file());
  if (!inputFileOrErr) {
    Diagnostics.diagnose(SourceLoc(), diag::error_open_input_file, input.file(),
                         inputFileOrErr.getError().message());
    return None;
  }
  if (!serialization::isSerializedAST((*inputFileOrErr)->getBuffer()))
    return ModuleBuffers(std::move(*inputFileOrErr));

  auto swiftdoc = openModuleDoc(input);
  auto sourceinfo = openModuleSourceInfo(input);
  return ModuleBuffers(std::move(*inputFileOrErr),
                       swiftdoc.hasValue() ? std::move(swiftdoc.getValue()) : nullptr,
                       sourceinfo.hasValue() ? std::move(sourceinfo.getValue()) : nullptr);
}

Optional<std::unique_ptr<llvm::MemoryBuffer>>
CompilerInstance::openModuleSourceInfo(const InputFile &input) {
  llvm::SmallString<128> pathWithoutProjectDir(input.file());
  llvm::sys::path::replace_extension(pathWithoutProjectDir,
                  file_types::getExtension(file_types::TY_SwiftSourceInfoFile));
  llvm::SmallString<128> pathWithProjectDir = pathWithoutProjectDir.str();
  StringRef fileName = llvm::sys::path::filename(pathWithoutProjectDir);
  llvm::sys::path::remove_filename(pathWithProjectDir);
  llvm::sys::path::append(pathWithProjectDir, "Project");
  llvm::sys::path::append(pathWithProjectDir, fileName);
  if (auto sourceInfoFileOrErr = swift::vfs::getFileOrSTDIN(getFileSystem(),
                                                            pathWithProjectDir))
    return std::move(*sourceInfoFileOrErr);
  if (auto sourceInfoFileOrErr = swift::vfs::getFileOrSTDIN(getFileSystem(),
                                                            pathWithoutProjectDir))
    return std::move(*sourceInfoFileOrErr);
  return None;
}

Optional<std::unique_ptr<llvm::MemoryBuffer>>
CompilerInstance::openModuleDoc(const InputFile &input) {
  llvm::SmallString<128> moduleDocFilePath(input.file());
  llvm::sys::path::replace_extension(
      moduleDocFilePath,
      file_types::getExtension(file_types::TY_SwiftModuleDocFile));
  using FileOrError = llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>;
  FileOrError moduleDocFileOrErr =
      swift::vfs::getFileOrSTDIN(getFileSystem(), moduleDocFilePath);
  if (moduleDocFileOrErr)
    return std::move(*moduleDocFileOrErr);

  if (moduleDocFileOrErr.getError() == std::errc::no_such_file_or_directory)
    return std::unique_ptr<llvm::MemoryBuffer>();

  Diagnostics.diagnose(SourceLoc(), diag::error_open_input_file,
                       moduleDocFilePath,
                       moduleDocFileOrErr.getError().message());
  return None;
}

std::unique_ptr<SILModule> CompilerInstance::takeSILModule() {
  return std::move(TheSILModule);
}

ModuleDecl *CompilerInstance::getMainModule() const {
  if (!MainModule) {
    Identifier ID = Context->getIdentifier(Invocation.getModuleName());
    MainModule = ModuleDecl::create(ID, *Context);
    if (Invocation.getFrontendOptions().EnableTesting)
      MainModule->setTestingEnabled();
    if (Invocation.getFrontendOptions().EnablePrivateImports)
      MainModule->setPrivateImportsEnabled();
    if (Invocation.getFrontendOptions().EnableImplicitDynamic)
      MainModule->setImplicitDynamicEnabled();

    if (Invocation.getFrontendOptions().EnableLibraryEvolution)
      MainModule->setResilienceStrategy(ResilienceStrategy::Resilient);
  }
  return MainModule;
}

void CompilerInstance::addAdditionalInitialImportsTo(
    SourceFile *SF, const CompilerInstance::ImplicitImports &implicitImports) {
  SmallVector<SourceFile::ImportedModuleDesc, 4> additionalImports;

  if (implicitImports.objCModuleUnderlyingMixedFramework)
    additionalImports.push_back(SourceFile::ImportedModuleDesc(
        ModuleDecl::ImportedModule(
            /*accessPath=*/{},
            implicitImports.objCModuleUnderlyingMixedFramework),
        SourceFile::ImportFlags::Exported));
  if (implicitImports.headerModule)
    additionalImports.push_back(SourceFile::ImportedModuleDesc(
        ModuleDecl::ImportedModule(/*accessPath=*/{},
                                   implicitImports.headerModule),
        SourceFile::ImportFlags::Exported));
  if (!implicitImports.modules.empty()) {
    for (auto &importModule : implicitImports.modules) {
      additionalImports.push_back(SourceFile::ImportedModuleDesc(
          ModuleDecl::ImportedModule(/*accessPath=*/{}, importModule),
          SourceFile::ImportOptions()));
    }
  }

  SF->addImports(additionalImports);
}

/// Implicitly import the SwiftOnoneSupport module in non-optimized
/// builds. This allows for use of popular specialized functions
/// from the standard library, which makes the non-optimized builds
/// execute much faster.
static bool
shouldImplicityImportSwiftOnoneSupportModule(CompilerInvocation &Invocation) {
  if (Invocation.getImplicitModuleImportKind() !=
      SourceFile::ImplicitModuleImportKind::Stdlib)
    return false;
  if (Invocation.getSILOptions().shouldOptimize())
    return false;

  // If we are not executing an action that has a dependency on
  // SwiftOnoneSupport, don't load it.
  //
  // FIXME: Knowledge of SwiftOnoneSupport loading in the Frontend is a layering
  // violation. However, SIL currently does not have a way to express this
  // dependency itself for the benefit of autolinking.  In the mean time, we
  // will be conservative and say that actions like -emit-silgen and
  // -emit-sibgen - that don't really involve the optimizer - have a
  // strict dependency on SwiftOnoneSupport.
  //
  // This optimization is disabled by -track-system-dependencies to preserve
  // the explicit dependency.
  const auto &options = Invocation.getFrontendOptions();
  return options.TrackSystemDeps
      || FrontendOptions::doesActionGenerateSIL(options.RequestedAction);
}

void CompilerInstance::performParseAndResolveImportsOnly() {
  performSemaUpTo(SourceFile::ImportsResolved);
}

void CompilerInstance::performSema() {
  performSemaUpTo(SourceFile::TypeChecked);
}

void CompilerInstance::performSemaUpTo(SourceFile::ASTStage_t LimitStage) {
  assert(LimitStage > SourceFile::Unprocessed);

  FrontendStatsTracer tracer(getStatsReporter(), "perform-sema");

  ModuleDecl *mainModule = getMainModule();
  Context->LoadedModules[mainModule->getName()] = mainModule;

  if (Invocation.getInputKind() == InputFileKind::SIL) {
    assert(!InputSourceCodeBufferIDs.empty());
    assert(InputSourceCodeBufferIDs.size() == 1);
    assert(MainBufferID != NO_SUCH_BUFFER);
    createSILModule();
  }

  if (Invocation.getImplicitModuleImportKind() ==
      SourceFile::ImplicitModuleImportKind::Stdlib) {
    if (!loadStdlib())
      return;
  }
  if (shouldImplicityImportSwiftOnoneSupportModule(Invocation)) {
    Invocation.getFrontendOptions().ImplicitImportModuleNames.push_back(
        SWIFT_ONONE_SUPPORT.str());
  }

  const ImplicitImports implicitImports(*this);

  if (Invocation.getInputKind() == InputFileKind::SwiftREPL) {
    // Create the initial empty REPL file. This only exists to feed in the
    // implicit imports such as the standard library.
    auto *replFile = createSourceFileForMainModule(
        SourceFileKind::REPL, implicitImports.kind, /*BufferID*/ None);
    addAdditionalInitialImportsTo(replFile, implicitImports);

    // Given this file is empty, we can go ahead and just mark it as having been
    // type checked.
    replFile->ASTStage = SourceFile::TypeChecked;
    return;
  }

  // Make sure the main file is the first file in the module, so do this now.
  if (MainBufferID != NO_SUCH_BUFFER)
    addMainFileToModule(implicitImports);

  parseAndCheckTypesUpTo(implicitImports, LimitStage);
}

CompilerInstance::ImplicitImports::ImplicitImports(CompilerInstance &compiler) {
  kind = compiler.Invocation.getImplicitModuleImportKind();

  objCModuleUnderlyingMixedFramework =
      compiler.Invocation.getFrontendOptions().ImportUnderlyingModule
          ? compiler.importUnderlyingModule()
          : nullptr;

  compiler.getImplicitlyImportedModules(modules);

  headerModule = compiler.importBridgingHeader();
}

bool CompilerInstance::loadStdlib() {
  FrontendStatsTracer tracer(getStatsReporter(), "load-stdlib");
  ModuleDecl *M = Context->getStdlibModule(true);

  if (!M) {
    Diagnostics.diagnose(SourceLoc(), diag::error_stdlib_not_found,
                         Invocation.getTargetTriple());
    return false;
  }

  // If we failed to load, we should have already diagnosed
  if (M->failedToLoad()) {
    assert(Diagnostics.hadAnyError() &&
           "Module failed to load but nothing was diagnosed?");
    return false;
  }
  return true;
}

ModuleDecl *CompilerInstance::importUnderlyingModule() {
  FrontendStatsTracer tracer(getStatsReporter(), "import-underlying-module");
  ModuleDecl *objCModuleUnderlyingMixedFramework =
      static_cast<ClangImporter *>(Context->getClangModuleLoader())
          ->loadModule(SourceLoc(),
                       { Located<Identifier>(MainModule->getName(), SourceLoc()) });
  if (objCModuleUnderlyingMixedFramework)
    return objCModuleUnderlyingMixedFramework;
  Diagnostics.diagnose(SourceLoc(), diag::error_underlying_module_not_found,
                       MainModule->getName());
  return nullptr;
}

ModuleDecl *CompilerInstance::importBridgingHeader() {
  FrontendStatsTracer tracer(getStatsReporter(), "import-bridging-header");
  const StringRef implicitHeaderPath =
      Invocation.getFrontendOptions().ImplicitObjCHeaderPath;
  auto clangImporter =
      static_cast<ClangImporter *>(Context->getClangModuleLoader());
  if (implicitHeaderPath.empty() ||
      clangImporter->importBridgingHeader(implicitHeaderPath, MainModule))
    return nullptr;
  ModuleDecl *importedHeaderModule = clangImporter->getImportedHeaderModule();
  assert(importedHeaderModule);
  return importedHeaderModule;
}

void CompilerInstance::getImplicitlyImportedModules(
    SmallVectorImpl<ModuleDecl *> &importModules) {
  FrontendStatsTracer tracer(getStatsReporter(), "get-implicitly-imported-modules");
  for (auto &ImplicitImportModuleName :
       Invocation.getFrontendOptions().ImplicitImportModuleNames) {
    if (Lexer::isIdentifier(ImplicitImportModuleName)) {
      auto moduleID = Context->getIdentifier(ImplicitImportModuleName);
      ModuleDecl *importModule =
        Context->getModule({ Located<Identifier>(moduleID, SourceLoc()) });
      if (importModule) {
        importModules.push_back(importModule);
      } else {
        Diagnostics.diagnose(SourceLoc(), diag::sema_no_import,
                             ImplicitImportModuleName);
        if (Invocation.getSearchPathOptions().SDKPath.empty() &&
            llvm::Triple(llvm::sys::getProcessTriple()).isMacOSX()) {
          Diagnostics.diagnose(SourceLoc(), diag::sema_no_import_no_sdk);
          Diagnostics.diagnose(SourceLoc(), diag::sema_no_import_no_sdk_xcrun);
        }
      }
    } else {
      Diagnostics.diagnose(SourceLoc(), diag::error_bad_module_name,
                           ImplicitImportModuleName, false);
    }
  }
}

void CompilerInstance::addMainFileToModule(
    const ImplicitImports &implicitImports) {
  auto *MainFile = createSourceFileForMainModule(
      Invocation.getSourceFileKind(), implicitImports.kind, MainBufferID);
  addAdditionalInitialImportsTo(MainFile, implicitImports);
}

void CompilerInstance::parseAndCheckTypesUpTo(
    const ImplicitImports &implicitImports, SourceFile::ASTStage_t limitStage) {
  FrontendStatsTracer tracer(getStatsReporter(), "parse-and-check-types");

  bool hadLoadError = parsePartialModulesAndLibraryFiles(implicitImports);
  if (Invocation.isCodeCompletion()) {
    // When we are doing code completion, make sure to emit at least one
    // diagnostic, so that ASTContext is marked as erroneous.  In this case
    // various parts of the compiler (for example, AST verifier) have less
    // strict assumptions about the AST.
    Diagnostics.diagnose(SourceLoc(), diag::error_doing_code_completion);
  }
  if (hadLoadError)
    return;

  // Type-check main file after parsing all other files so that
  // it can use declarations from other files.
  // In addition, in SIL mode the main file has parsing and
  // type-checking interwined.
  if (MainBufferID != NO_SUCH_BUFFER) {
    parseAndTypeCheckMainFileUpTo(limitStage);
  }

  assert(llvm::all_of(MainModule->getFiles(), [](const FileUnit *File) -> bool {
    auto *SF = dyn_cast<SourceFile>(File);
    if (!SF)
      return true;
    return SF->ASTStage >= SourceFile::ImportsResolved;
  }) && "some files have not yet had their imports resolved");
  MainModule->setHasResolvedImports();

  forEachFileToTypeCheck([&](SourceFile &SF) {
    if (limitStage == SourceFile::ImportsResolved) {
      bindExtensions(SF);
      return;
    }

    performTypeChecking(SF);

    if (!Context->hadError() && Invocation.getFrontendOptions().PCMacro) {
      performPCMacro(SF);
    }

    // Playground transform knows to look out for PCMacro's changes and not
    // to playground log them.
    if (!Context->hadError() &&
        Invocation.getFrontendOptions().PlaygroundTransform) {
      performPlaygroundTransform(
          SF, Invocation.getFrontendOptions().PlaygroundHighPerformance);
    }
  });

  // If the limiting AST stage is import resolution, we're done.
  if (limitStage <= SourceFile::ImportsResolved) {
    return;
  }

  finishTypeChecking();
}

void CompilerInstance::parseLibraryFile(
    unsigned BufferID, const ImplicitImports &implicitImports) {
  FrontendStatsTracer tracer(getStatsReporter(), "parse-library-file");

  auto *NextInput = createSourceFileForMainModule(
      SourceFileKind::Library, implicitImports.kind, BufferID);
  addAdditionalInitialImportsTo(NextInput, implicitImports);

  // Import resolution will lazily trigger parsing of the file.
  performImportResolution(*NextInput);
}

bool CompilerInstance::parsePartialModulesAndLibraryFiles(
    const ImplicitImports &implicitImports) {
  FrontendStatsTracer tracer(getStatsReporter(),
                             "parse-partial-modules-and-library-files");
  bool hadLoadError = false;
  // Parse all the partial modules first.
  for (auto &PM : PartialModules) {
    assert(PM.ModuleBuffer);
    if (!SML->loadAST(*MainModule, SourceLoc(), /*moduleInterfacePath*/"",
                      std::move(PM.ModuleBuffer), std::move(PM.ModuleDocBuffer),
                      std::move(PM.ModuleSourceInfoBuffer), /*isFramework*/false,
                      /*treatAsPartialModule*/true))
      hadLoadError = true;
  }

  // Then parse all the library files.
  for (auto BufferID : InputSourceCodeBufferIDs) {
    if (BufferID != MainBufferID) {
      parseLibraryFile(BufferID, implicitImports);
    }
  }
  return hadLoadError;
}

void CompilerInstance::parseAndTypeCheckMainFileUpTo(
    SourceFile::ASTStage_t LimitStage) {
  assert(LimitStage >= SourceFile::ImportsResolved);
  FrontendStatsTracer tracer(getStatsReporter(),
                             "parse-and-typecheck-main-file");
  bool mainIsPrimary =
      (isWholeModuleCompilation() || isPrimaryInput(MainBufferID));

  SourceFile &MainFile =
      MainModule->getMainSourceFile(Invocation.getSourceFileKind());

  auto &Diags = MainFile.getASTContext().Diags;
  auto DidSuppressWarnings = Diags.getSuppressWarnings();
  Diags.setSuppressWarnings(DidSuppressWarnings || !mainIsPrimary);

  // For a primary, perform type checking if needed. Otherwise, just do import
  // resolution.
  if (mainIsPrimary && LimitStage >= SourceFile::TypeChecked) {
    performTypeChecking(MainFile);
  } else {
    assert(!TheSILModule && "Should perform type checking for SIL");
    performImportResolution(MainFile);
  }

  // Parse the SIL decls if needed.
  if (TheSILModule) {
    SILParserState SILContext(TheSILModule.get());
    parseSourceFileSIL(MainFile, &SILContext);
  }

  Diags.setSuppressWarnings(DidSuppressWarnings);

  if (mainIsPrimary && !Context->hadError() &&
      Invocation.getFrontendOptions().DebuggerTestingTransform) {
    performDebuggerTestingTransform(MainFile);
  }
}

static void
forEachSourceFileIn(ModuleDecl *module,
                    llvm::function_ref<void(SourceFile &)> fn) {
  for (auto fileName : module->getFiles()) {
    if (auto SF = dyn_cast<SourceFile>(fileName))
      fn(*SF);
  }
}

void CompilerInstance::forEachFileToTypeCheck(
    llvm::function_ref<void(SourceFile &)> fn) {
  if (isWholeModuleCompilation()) {
    forEachSourceFileIn(MainModule, [&](SourceFile &SF) { fn(SF); });
  } else {
    for (auto *SF : PrimarySourceFiles) {
      fn(*SF);
    }
  }
}

void CompilerInstance::finishTypeChecking() {
  if (getASTContext().TypeCheckerOpts.DelayWholeModuleChecking) {
    forEachSourceFileIn(MainModule, [&](SourceFile &SF) {
      performWholeModuleTypeChecking(SF);
    });
  }

  checkInconsistentImplementationOnlyImports(MainModule);
}

SourceFile *CompilerInstance::createSourceFileForMainModule(
    SourceFileKind fileKind, SourceFile::ImplicitModuleImportKind importKind,
    Optional<unsigned> bufferID, SourceFile::ParsingOptions opts) {
  ModuleDecl *mainModule = getMainModule();

  auto isPrimary = bufferID && isPrimaryInput(*bufferID);
  if (isPrimary || isWholeModuleCompilation()) {
    // Disable delayed body parsing for primaries.
    opts |= SourceFile::ParsingFlags::DisableDelayedBodies;
  } else {
    // Suppress parse warnings for non-primaries, as they'll get parsed multiple
    // times.
    opts |= SourceFile::ParsingFlags::SuppressWarnings;
  }

  SourceFile *inputFile = new (*Context)
      SourceFile(*mainModule, fileKind, bufferID, importKind,
                 Invocation.getLangOptions().CollectParsedToken,
                 Invocation.getLangOptions().BuildSyntaxTree, opts);
  MainModule->addFile(*inputFile);

  if (isPrimary)
    recordPrimarySourceFile(inputFile);

  if (bufferID == SourceMgr.getCodeCompletionBufferID()) {
    assert(!CodeCompletionFile && "Multiple code completion files?");
    CodeCompletionFile = inputFile;
  }

  return inputFile;
}

void CompilerInstance::performParseOnly(bool EvaluateConditionals,
                                        bool CanDelayBodies) {
  const InputFileKind Kind = Invocation.getInputKind();
  ModuleDecl *const MainModule = getMainModule();
  Context->LoadedModules[MainModule->getName()] = MainModule;

  assert((Kind == InputFileKind::Swift ||
          Kind == InputFileKind::SwiftLibrary ||
          Kind == InputFileKind::SwiftModuleInterface) &&
         "only supports parsing .swift files");
  (void)Kind;

  SourceFile::ParsingOptions parsingOpts;
  if (!EvaluateConditionals)
    parsingOpts |= SourceFile::ParsingFlags::DisablePoundIfEvaluation;
  if (!CanDelayBodies)
    parsingOpts |= SourceFile::ParsingFlags::DisableDelayedBodies;

  // Make sure the main file is the first file in the module but parse it last,
  // to match the parsing logic used when performing Sema.
  if (MainBufferID != NO_SUCH_BUFFER) {
    assert(Kind == InputFileKind::Swift ||
           Kind == InputFileKind::SwiftModuleInterface);
    createSourceFileForMainModule(Invocation.getSourceFileKind(),
                                  SourceFile::ImplicitModuleImportKind::None,
                                  MainBufferID, parsingOpts);
  }

  // Parse all the library files.
  for (auto BufferID : InputSourceCodeBufferIDs) {
    if (BufferID == MainBufferID)
      continue;

    SourceFile *NextInput = createSourceFileForMainModule(
        SourceFileKind::Library, SourceFile::ImplicitModuleImportKind::None,
        BufferID, parsingOpts);

    // Force the parsing of the top level decls.
    (void)NextInput->getTopLevelDecls();
  }

  // Now parse the main file.
  if (MainBufferID != NO_SUCH_BUFFER) {
    SourceFile &MainFile =
        MainModule->getMainSourceFile(Invocation.getSourceFileKind());
    MainFile.SyntaxParsingCache = Invocation.getMainFileSyntaxParsingCache();

    // Force the parsing of the top level decls.
    (void)MainFile.getTopLevelDecls();
  }

  assert(Context->LoadedModules.size() == 1 &&
         "Loaded a module during parse-only");
}

void CompilerInstance::freeASTContext() {
  TheSILTypes.reset();
  Context.reset();
  MainModule = nullptr;
  SML = nullptr;
  MemoryBufferLoader = nullptr;
  PrimaryBufferIDs.clear();
  PrimarySourceFiles.clear();
}

void CompilerInstance::freeSILModule() { TheSILModule.reset(); }

/// Perform "stable" optimizations that are invariant across compiler versions.
static bool performMandatorySILPasses(CompilerInvocation &Invocation,
                                      SILModule *SM) {
  if (Invocation.getFrontendOptions().RequestedAction ==
      FrontendOptions::ActionType::MergeModules) {
    // Don't run diagnostic passes at all.
  } else if (!Invocation.getDiagnosticOptions().SkipDiagnosticPasses) {
    if (runSILDiagnosticPasses(*SM))
      return true;
  } else {
    // Even if we are not supposed to run the diagnostic passes, we still need
    // to run the ownership evaluator.
    if (runSILOwnershipEliminatorPass(*SM))
      return true;
  }

  if (Invocation.getSILOptions().MergePartialModules)
    SM->linkAllFromCurrentModule();
  return false;
}

/// Perform SIL optimization passes if optimizations haven't been disabled.
/// These may change across compiler versions.
static void performSILOptimizations(CompilerInvocation &Invocation,
                                    SILModule *SM) {
  FrontendStatsTracer tracer(SM->getASTContext().Stats,
                             "SIL optimization");
  if (Invocation.getFrontendOptions().RequestedAction ==
      FrontendOptions::ActionType::MergeModules ||
      !Invocation.getSILOptions().shouldOptimize()) {
    runSILPassesForOnone(*SM);
    return;
  }
  runSILOptPreparePasses(*SM);

  StringRef CustomPipelinePath =
  Invocation.getSILOptions().ExternalPassPipelineFilename;
  if (!CustomPipelinePath.empty()) {
    runSILOptimizationPassesWithFileSpecification(*SM, CustomPipelinePath);
  } else {
    runSILOptimizationPasses(*SM);
  }
  // When building SwiftOnoneSupport.o verify all expected ABI symbols.
  if (Invocation.getFrontendOptions().CheckOnoneSupportCompleteness
       // TODO: handle non-ObjC based stdlib builds, e.g. on linux.
      && Invocation.getLangOptions().EnableObjCInterop
      && Invocation.getFrontendOptions().RequestedAction
             == FrontendOptions::ActionType::EmitObject) {
    checkCompletenessOfPrespecializations(*SM);
  }
}

static void countStatsPostSILOpt(UnifiedStatsReporter &Stats,
                                 const SILModule& Module) {
  auto &C = Stats.getFrontendCounters();
  // FIXME: calculate these in constant time, via the dense maps.
  C.NumSILOptFunctions += Module.getFunctionList().size();
  C.NumSILOptVtables += Module.getVTableList().size();
  C.NumSILOptWitnessTables += Module.getWitnessTableList().size();
  C.NumSILOptDefaultWitnessTables += Module.getDefaultWitnessTableList().size();
  C.NumSILOptGlobalVariables += Module.getSILGlobalList().size();
}

bool CompilerInstance::performSILProcessing(SILModule *silModule) {
  if (performMandatorySILPasses(Invocation, silModule))
    return true;

  {
    FrontendStatsTracer tracer(silModule->getASTContext().Stats,
                               "SIL verification, pre-optimization");
    silModule->verify();
  }

  performSILOptimizations(Invocation, silModule);

  if (auto *stats = getStatsReporter())
    countStatsPostSILOpt(*stats, *silModule);

  {
    FrontendStatsTracer tracer(silModule->getASTContext().Stats,
                               "SIL verification, post-optimization");
    silModule->verify();
  }

  performSILInstCountIfNeeded(silModule);
  return false;
}


const PrimarySpecificPaths &
CompilerInstance::getPrimarySpecificPathsForWholeModuleOptimizationMode()
    const {
  return getPrimarySpecificPathsForAtMostOnePrimary();
}
const PrimarySpecificPaths &
CompilerInstance::getPrimarySpecificPathsForAtMostOnePrimary() const {
  return Invocation.getPrimarySpecificPathsForAtMostOnePrimary();
}
const PrimarySpecificPaths &
CompilerInstance::getPrimarySpecificPathsForPrimary(StringRef filename) const {
  return Invocation.getPrimarySpecificPathsForPrimary(filename);
}
const PrimarySpecificPaths &
CompilerInstance::getPrimarySpecificPathsForSourceFile(
    const SourceFile &SF) const {
  return Invocation.getPrimarySpecificPathsForSourceFile(SF);
}

bool CompilerInstance::emitSwiftRanges(DiagnosticEngine &diags,
                                       SourceFile *primaryFile,
                                       StringRef outputPath) const {
  return incremental_ranges::SwiftRangesEmitter(outputPath, primaryFile,
                                                SourceMgr, diags)
      .emit();
  return false;
}

bool CompilerInstance::emitCompiledSource(DiagnosticEngine &diags,
                                          const SourceFile *primaryFile,
                                          StringRef outputPath) const {
  return incremental_ranges::CompiledSourceEmitter(outputPath, primaryFile,
                                                   SourceMgr, diags)
      .emit();
}
