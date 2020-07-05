//===-------- ModuleInterfaceLoader.h - Loads .swiftinterface files -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file This implements the logic for loading and building module
/// interfaces.
///
/// === Loading Module Interfaces ===
///
/// If there is a .swiftinterface file corresponding to a given module name
/// present in the frontend's search paths, then this module loader will look in
/// the following places for a module:
///
/// - First, look in the module cache (specified by -module-cache-path)
///   - We check here first because an existing .swiftmodule might be
///     out-of-date, necessitating a rebuild. If a cached module is out-of-date,
///     it's simply rebuilt.
/// - Next, look adjacent to the .swiftinterface. If we find a module that's
///   either loadable by this compiler, valid, and up-to-date, or totally
///   unreadable, then delegate to the serialized module loader to either load
///   or diagnose it.
/// - Finally, look in the prebuilt module cache (specified
///   by -prebuilt-module-cache-path)
///
/// If we can't find an appropriate module to load, we can always fall back and
/// recompile the .swiftinterface file.
///
/// === Dependency Checking ===
///
/// Cached modules keep track of their dependencies' last modification time and
/// file size. This means that checking if a module is up-to-date requires
/// `stat`ing the dependencies and comparing the results from the filesystem
/// with the results in the module.
///
/// Prebuilt modules, on the other hand, won't have a reliable modification
/// time, as their dependencies live in the SDK. Prebuilt modules will instead
/// keep track of the size and content hash of their dependencies.
/// In order to avoid constantly re-hashing the dependencies, however, we will
/// install a "forwarding module" in the regular cache.
/// This "forwarding module"
///  - Points to the prebuilt module on disk, and
///  - Lists modification times from the last time we verified the content
///
/// So, to recap, there are 4 kinds of modules:
/// ┌───────────────────────────────┐
/// │     ┌───┐           ┌───┐     │
/// │     │ I │           │ M │     │
/// │     └───┘           └───┘     │
/// │ .swiftinterface  .swiftmodule │
/// │     ┌───┐           ┌───┐     │
/// │     │ P │           │ F │     │
/// │     └───┘           └───┘     │
/// │   Prebuilt        Forwarding  │
/// │ .swiftmodule     .swiftmodule │
/// └───────────────────────────────┘
///
/// - Prebuilt modules have hash-based dependencies, cached modules have
///   mod-time-based dependencies
/// - Forwarding modules point to prebuilt modules and augment them with
///   modification times
///
/// === Example Cache ===
///
/// Here's an example of what's in a prebuilt cache or module cache.
///
/// Say there are 4 frameworks, each exposing a .swiftinterface file.
/// Then, we pre-build 2 of those frameworks and put them in the prebuilt cache.
/// Finally, we import all 4 of those frameworks while building a project.
///
/// For the 2 frameworks with modules in the prebuilt cache, we'll have
/// installed 2 forwarding modules. For the other 2 frameworks, we'll have
/// compiled the interfaces and put them in the module cache.
///
///                   ┌─────┐
///  ┌────────────────┤ SDK ├───────────────┐
///  │                └─────┘               │
///  │          ┌────────────────┐          │  ┌────────────────┐
///  │  ┌───────┤ Framework Dirs ├────────┐ │ ┌┤ Prebuilt Cache ├┐
///  │  │       └────────────────┘        │ │ │└────────────────┘│
///  │  │  ┌───┐   ┌───┐   ┌───┐   ┌───┐  │ │ │   ┌───┐  ┌───┐   │
///  │  │  │ I │   │ I │   │ I │   │ I │◀─┼─┼─┼───│ P │  │ P │◀═╗│
///  │  │  └───┘   └───┘   └───┘   └───┘  │ │ │   └───┘  └───┘  ║│
///  │  │    ▲       ▲       ▲            │ │ │     ▲      │    ║│
///  │  └────┼───────┼───────┼────────────┘ │ └─────╫──────┼────╫┘
///  │       │       │       └──────────────┼───────╫──────┘    ║
///  └───────┼───────┼──────────────────────┘       ║           ║
///          │       │   ┌───────────────┐          ║           ║
///          │  ┌────┼───┤ Module Cache  ├────────┐ ║           ║
///          │  │    │   └───────────────┘        │ ║           ║
///          │  │  ┌───┐   ┌───┐   ┌───┐   ┌───┐  │ ║           ║
///          │  │  │ M │   │ M │   │ F │   │ F │  │ ║           ║
///          │  │  └───┘   └───┘   └───┘   └───┘  │ ║           ║
///          │  │            │       ║       ╚════╪═╝           ║
///          │  └────────────┼───────╫────────────┘             ║
///          └───────────────┘       ╚══════════════════════════╝
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_FRONTEND_MODULEINTERFACELOADER_H
#define SWIFT_FRONTEND_MODULEINTERFACELOADER_H

#include "swift/Basic/LLVM.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/ModuleInterfaceSupport.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "llvm/Support/StringSaver.h"

namespace clang {
class CompilerInstance;
}

namespace unittest {
class ModuleInterfaceLoaderTest;
}

namespace swift {

class LangOptions;
class SearchPathOptions;
class CompilerInvocation;

/// A ModuleLoader that loads explicitly built Swift modules specified via
/// -swift-module-file
class ExplicitSwiftModuleLoader: public SerializedModuleLoaderBase {
  explicit ExplicitSwiftModuleLoader(ASTContext &ctx, DependencyTracker *tracker,
                                     ModuleLoadingMode loadMode,
                                     bool IgnoreSwiftSourceInfoFile);
  std::error_code findModuleFilesInDirectory(
    AccessPathElem ModuleID,
    const SerializedModuleBaseName &BaseName,
    SmallVectorImpl<char> *ModuleInterfacePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer) override;

  bool isCached(StringRef DepPath) override { return false; };

  struct Implementation;
  Implementation &Impl;
public:
  static std::unique_ptr<ExplicitSwiftModuleLoader>
  create(ASTContext &ctx,
         DependencyTracker *tracker, ModuleLoadingMode loadMode,
         ArrayRef<std::string> ExplicitModulePaths,
         StringRef ExplicitSwiftModuleMap,
         bool IgnoreSwiftSourceInfoFile);

  /// Append visible module names to \p names. Note that names are possibly
  /// duplicated, and not guaranteed to be ordered in any way.
  void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const override;
  ~ExplicitSwiftModuleLoader();
};

struct ModuleInterfaceLoaderOptions {
  bool remarkOnRebuildFromInterface = false;
  bool disableInterfaceLock = false;
  bool disableImplicitSwiftModule = false;
  ModuleInterfaceLoaderOptions(const FrontendOptions &Opts):
    remarkOnRebuildFromInterface(Opts.RemarkOnRebuildFromModuleInterface),
    disableInterfaceLock(Opts.DisableInterfaceFileLock),
    disableImplicitSwiftModule(Opts.DisableImplicitModules) {}
  ModuleInterfaceLoaderOptions() = default;
};

/// A ModuleLoader that runs a subordinate \c CompilerInvocation and
/// \c CompilerInstance to convert .swiftinterface files to .swiftmodule
/// files on the fly, caching the resulting .swiftmodules in the module cache
/// directory, and loading the serialized .swiftmodules from there.
class ModuleInterfaceLoader : public SerializedModuleLoaderBase {
  friend class unittest::ModuleInterfaceLoaderTest;
  explicit ModuleInterfaceLoader(
      ASTContext &ctx, StringRef cacheDir, StringRef prebuiltCacheDir,
      DependencyTracker *tracker, ModuleLoadingMode loadMode,
      ArrayRef<std::string> PreferInterfaceForModules,
      bool IgnoreSwiftSourceInfoFile, ModuleInterfaceLoaderOptions Opts)
  : SerializedModuleLoaderBase(ctx, tracker, loadMode,
                               IgnoreSwiftSourceInfoFile),
  CacheDir(cacheDir), PrebuiltCacheDir(prebuiltCacheDir),
  PreferInterfaceForModules(PreferInterfaceForModules),
  Opts(Opts) {}

  std::string CacheDir;
  std::string PrebuiltCacheDir;
  ArrayRef<std::string> PreferInterfaceForModules;
  ModuleInterfaceLoaderOptions Opts;

  std::error_code findModuleFilesInDirectory(
    AccessPathElem ModuleID,
    const SerializedModuleBaseName &BaseName,
    SmallVectorImpl<char> *ModuleInterfacePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer) override;

  bool isCached(StringRef DepPath) override;
public:
  static std::unique_ptr<ModuleInterfaceLoader>
  create(ASTContext &ctx, StringRef cacheDir, StringRef prebuiltCacheDir,
         DependencyTracker *tracker, ModuleLoadingMode loadMode,
         ArrayRef<std::string> PreferInterfaceForModules = {},
         ModuleInterfaceLoaderOptions Opts = ModuleInterfaceLoaderOptions(),
         bool IgnoreSwiftSourceInfoFile = false) {
    return std::unique_ptr<ModuleInterfaceLoader>(
      new ModuleInterfaceLoader(ctx, cacheDir, prebuiltCacheDir,
                                         tracker, loadMode,
                                         PreferInterfaceForModules,
                                         IgnoreSwiftSourceInfoFile,
                                         Opts));
  }

  /// Append visible module names to \p names. Note that names are possibly
  /// duplicated, and not guaranteed to be ordered in any way.
  void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const override;

  /// Unconditionally build \p InPath (a swiftinterface file) to \p OutPath (as
  /// a swiftmodule file).
  ///
  /// A simplified version of the core logic in #openModuleFiles.
  static bool buildSwiftModuleFromSwiftInterface(
    SourceManager &SourceMgr, DiagnosticEngine &Diags,
    const SearchPathOptions &SearchPathOpts, const LangOptions &LangOpts,
    const ClangImporterOptions &ClangOpts,
    StringRef CacheDir, StringRef PrebuiltCacheDir,
    StringRef ModuleName, StringRef InPath, StringRef OutPath,
    bool SerializeDependencyHashes, bool TrackSystemDependencies,
    ModuleInterfaceLoaderOptions Opts);

  std::string getUpToDateCompiledModuleForInterface(StringRef moduleName,
                                                    StringRef interfacePath) override;
};

struct InterfaceSubContextDelegateImpl: InterfaceSubContextDelegate {
private:
  SourceManager &SM;
  DiagnosticEngine &Diags;
  llvm::BumpPtrAllocator Allocator;
  llvm::StringSaver ArgSaver;
  std::vector<StringRef> GenericArgs;
  CompilerInvocation subInvocation;

  template<typename ...ArgTypes>
  InFlightDiagnostic diagnose(StringRef interfacePath,
                              SourceLoc diagnosticLoc,
                              Diag<ArgTypes...> ID,
                        typename detail::PassArgument<ArgTypes>::type... Args) {
    SourceLoc loc = diagnosticLoc;
    if (diagnosticLoc.isInvalid()) {
      // Diagnose this inside the interface file, if possible.
      loc = SM.getLocFromExternalSource(interfacePath, 1, 1);
    }
    return Diags.diagnose(loc, ID, std::move(Args)...);
  }
  void inheritOptionsForBuildingInterface(const SearchPathOptions &SearchPathOpts,
                                          const LangOptions &LangOpts);
  bool extractSwiftInterfaceVersionAndArgs(SmallVectorImpl<const char *> &SubArgs,
                                           std::string &CompilerVersion,
                                           StringRef interfacePath,
                                           SourceLoc diagnosticLoc);
public:
  InterfaceSubContextDelegateImpl(SourceManager &SM,
                                  DiagnosticEngine &Diags,
                                  const SearchPathOptions &searchPathOpts,
                                  const LangOptions &langOpts,
                                  ModuleInterfaceLoaderOptions LoaderOpts,
                                  ClangModuleLoader *clangImporter,
                                  bool buildModuleCacheDirIfAbsent,
                                  StringRef moduleCachePath,
                                  StringRef prebuiltCachePath,
                                  bool serializeDependencyHashes,
                                  bool trackSystemDependencies);
  bool runInSubContext(StringRef moduleName,
                       StringRef interfacePath,
                       StringRef outputPath,
                       SourceLoc diagLoc,
    llvm::function_ref<bool(ASTContext&, ArrayRef<StringRef>,
                            ArrayRef<StringRef>, StringRef)> action) override;
  bool runInSubCompilerInstance(StringRef moduleName,
                                StringRef interfacePath,
                                StringRef outputPath,
                                SourceLoc diagLoc,
            llvm::function_ref<bool(SubCompilerInstanceInfo&)> action) override;

  ~InterfaceSubContextDelegateImpl() = default;

  /// includes a hash of relevant key data.
  StringRef computeCachedOutputPath(StringRef moduleName,
                                    StringRef UseInterfacePath,
                                    llvm::SmallString<256> &OutPath,
                                    StringRef &CacheHash);
  std::string getCacheHash(StringRef useInterfacePath);
  void addExtraClangArg(StringRef Arg);
};
}

#endif
