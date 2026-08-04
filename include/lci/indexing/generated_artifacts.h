#pragma once

#include <string>
#include <vector>

namespace lci {

/// Derives project-specific generated-output exclude globs by reading the
/// project's own build manifests — the tools that WRITE generated artifacts
/// say where they put them:
///   - tsconfig.json / jsconfig.json  -> compilerOptions.outDir
///   - composer.json                  -> config.vendor-dir
///   - *.csproj (root + one level)    -> OutputPath / BaseOutputPath /
///                                       BaseIntermediateOutputPath
///
/// Returns root-relative globs ("<dir>/**"). Directories already covered by
/// the static default excludes (dist, build, out, bin, obj, vendor, dot-dirs)
/// are filtered out so the list carries only project-specific additions.
/// Unreadable or malformed manifests contribute nothing — the scan must not
/// fail because a manifest is odd; the static excludes still apply.
std::vector<std::string> derive_generated_excludes(const std::string& root);

}  // namespace lci
