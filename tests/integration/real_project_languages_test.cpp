// Real-project call-graph resolution across all newly-wired languages.
//
// Each of these seven languages had NO call graph before scope-based
// receiver-type resolution was added. These tests index a real upstream repo
// per language and assert that (a) symbols are extracted and (b) a known
// method call resolves to a receiver-type-qualified target (`Type.method`),
// i.e. the SCIP-base-case resolution fires on real code, not just the
// controlled corpora in the unit suite.
//
// Skips gracefully when the corpus is absent (run ./scripts/add-real-projects.sh).

#include <gtest/gtest.h>

#include <lci/config.h>
#include <lci/core/reference_tracker.h>
#include <lci/indexing/master_index.h>

#include <string>
#include <vector>

#include "helpers/real_project_helpers.h"

namespace lci {
namespace {

// Indexes <lang>/<repo>, asserts symbols were extracted, then confirms the
// sentinel method's callee set contains the expected receiver-type-qualified
// edge — proving type resolution works for the language on real source.
void expect_qualified_callee(const std::string& lang, const std::string& repo,
                             const std::string& sentinel,
                             const std::string& qualified_callee) {
    auto path = testing::find_real_project(lang, repo);
    if (!path) {
        GTEST_SKIP() << "Real project not found: " << lang << "/" << repo
                     << ". Run ./scripts/add-real-projects.sh";
    }
    auto ctx = testing::setup_real_project(*path, repo);
    ASSERT_TRUE(ctx.valid()) << "Failed to index " << lang << "/" << repo;
    EXPECT_GT(ctx.indexer->file_count(), 0);

    const auto& rt = ctx.indexer->ref_tracker();
    auto snapshot = rt.pin();
    auto syms = snapshot->find_symbols_by_name(sentinel);
    ASSERT_FALSE(syms.empty())
        << "sentinel symbol '" << sentinel << "' not extracted in " << repo;

    bool found = false;
    for (const auto* s : syms) {
        for (const auto& callee : rt.get_callee_names(s->id)) {
            if (callee == qualified_callee) {
                found = true;
                break;
            }
        }
        if (found) break;
    }
    EXPECT_TRUE(found) << lang << "/" << repo << ": " << sentinel
                       << " should have a receiver-type-qualified callee '"
                       << qualified_callee << "'";
}

TEST(RealProjectLanguages, JavaGsonResolvesReceiverType) {
    expect_qualified_callee("java", "gson", "toJson", "Gson.toJson");
}

TEST(RealProjectLanguages, CSharpSerilogResolvesReceiverType) {
    expect_qualified_callee("csharp", "serilog", "Write", "Logger.IsEnabled");
}

TEST(RealProjectLanguages, RustRipgrepResolvesReceiverType) {
    expect_qualified_callee("rust", "ripgrep", "build", "GlobSetBuilder.add");
}

TEST(RealProjectLanguages, PhpGuzzleResolvesReceiverType) {
    expect_qualified_callee("php", "guzzle", "send", "Client.sendAsync");
}

TEST(RealProjectLanguages, KotlinOkhttpResolvesReceiverType) {
    expect_qualified_callee("kotlin", "okhttp", "intercept", "Chain.request");
}

TEST(RealProjectLanguages, RubySinatraResolvesReceiverType) {
    expect_qualified_callee("ruby", "sinatra", "call", "ExtendedRack.setup_close");
}

TEST(RealProjectLanguages, ZigZlsResolvesReceiverType) {
    expect_qualified_callee("zig", "zls", "resolveTypeOfNode",
                            "Analyser.resolveBindingOfNode");
}

}  // namespace
}  // namespace lci
