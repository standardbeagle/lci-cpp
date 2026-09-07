#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <lci/analysis/side_effect_analyzer.h>
#include <lci/config.h>
#include <lci/context_manifest.h>
#include <lci/graph_types.h>
#include <lci/idcodec.h>
#include <lci/indexing/master_index.h>
#include <lci/mcp/handlers_side_effects.h>
#include <lci/side_effects.h>

#include "unique_temp.h"

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// SideEffectCategory bitfield: 16 individual flags + kNone
// ---------------------------------------------------------------------------
TEST(SideEffectCategoryTest, NoneIsZero) {
    EXPECT_EQ(side_effect::kNone, 0u);
}

TEST(SideEffectCategoryTest, All16FlagsHaveUniqueBitPositions) {
    EXPECT_EQ(side_effect::kParamWrite, 1u << 0);
    EXPECT_EQ(side_effect::kReceiverWrite, 1u << 1);
    EXPECT_EQ(side_effect::kGlobalWrite, 1u << 2);
    EXPECT_EQ(side_effect::kClosureWrite, 1u << 3);
    EXPECT_EQ(side_effect::kFieldWrite, 1u << 4);
    EXPECT_EQ(side_effect::kIO, 1u << 5);
    EXPECT_EQ(side_effect::kDatabase, 1u << 6);
    EXPECT_EQ(side_effect::kNetwork, 1u << 7);
    EXPECT_EQ(side_effect::kThrow, 1u << 8);
    EXPECT_EQ(side_effect::kChannel, 1u << 9);
    EXPECT_EQ(side_effect::kAsync, 1u << 10);
    EXPECT_EQ(side_effect::kExternalCall, 1u << 11);
    EXPECT_EQ(side_effect::kDynamicCall, 1u << 12);
    EXPECT_EQ(side_effect::kReflection, 1u << 13);
    EXPECT_EQ(side_effect::kUncertain, 1u << 14);
    EXPECT_EQ(side_effect::kIndirectWrite, 1u << 15);
}

TEST(SideEffectCategoryTest, CategoryCountIs16) {
    EXPECT_EQ(side_effect::kCategoryCount, 16);
}

TEST(SideEffectCategoryTest, BitwiseOrCombinesFlags) {
    uint32_t combined = side_effect::kParamWrite | side_effect::kIO;
    EXPECT_TRUE((combined & side_effect::kParamWrite) != 0);
    EXPECT_TRUE((combined & side_effect::kIO) != 0);
    EXPECT_TRUE((combined & side_effect::kNetwork) == 0);
}

TEST(SideEffectCategoryTest, BitwiseAndDetectsFlags) {
    uint32_t categories = side_effect::kGlobalWrite | side_effect::kNetwork | side_effect::kThrow;
    EXPECT_TRUE((categories & side_effect::kWriteMask) != 0);
    EXPECT_TRUE((categories & side_effect::kIOMask) != 0);
    EXPECT_TRUE((categories & side_effect::kUncertaintyMask) == 0);
}

TEST(SideEffectCategoryTest, WriteMaskCoversAllWriteFlags) {
    uint32_t expected = side_effect::kParamWrite | side_effect::kReceiverWrite |
                        side_effect::kGlobalWrite | side_effect::kClosureWrite |
                        side_effect::kFieldWrite | side_effect::kIndirectWrite;
    EXPECT_EQ(side_effect::kWriteMask, expected);
}

TEST(SideEffectCategoryTest, IOMaskCoversAllIOFlags) {
    uint32_t expected = side_effect::kIO | side_effect::kDatabase | side_effect::kNetwork;
    EXPECT_EQ(side_effect::kIOMask, expected);
}

TEST(SideEffectCategoryTest, UncertaintyMaskCoversAllUncertaintyFlags) {
    uint32_t expected = side_effect::kExternalCall | side_effect::kDynamicCall |
                        side_effect::kReflection | side_effect::kUncertain;
    EXPECT_EQ(side_effect::kUncertaintyMask, expected);
}

TEST(SideEffectCategoryTest, AllFlagsFitInUint32) {
    uint32_t all = side_effect::kParamWrite | side_effect::kReceiverWrite |
                   side_effect::kGlobalWrite | side_effect::kClosureWrite |
                   side_effect::kFieldWrite | side_effect::kIO | side_effect::kDatabase |
                   side_effect::kNetwork | side_effect::kThrow | side_effect::kChannel |
                   side_effect::kAsync | side_effect::kExternalCall | side_effect::kDynamicCall |
                   side_effect::kReflection | side_effect::kUncertain |
                   side_effect::kIndirectWrite;
    EXPECT_EQ(all, 0xFFFFu);
    static_assert(sizeof(uint32_t) == 4);
}

// ---------------------------------------------------------------------------
// PurityLevel enum
// ---------------------------------------------------------------------------
TEST(PurityLevelTest, ValuesMatch1To5Scale) {
    EXPECT_EQ(static_cast<uint8_t>(PurityLevel::Pure), 1);
    EXPECT_EQ(static_cast<uint8_t>(PurityLevel::InternallyPure), 2);
    EXPECT_EQ(static_cast<uint8_t>(PurityLevel::ObjectState), 3);
    EXPECT_EQ(static_cast<uint8_t>(PurityLevel::ModuleGlobal), 4);
    EXPECT_EQ(static_cast<uint8_t>(PurityLevel::ExternalDependency), 5);
}

TEST(PurityLevelTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(PurityLevel::Pure), "Pure");
    EXPECT_EQ(to_string(PurityLevel::InternallyPure), "InternallyPure");
    EXPECT_EQ(to_string(PurityLevel::ObjectState), "ObjectState");
    EXPECT_EQ(to_string(PurityLevel::ModuleGlobal), "ModuleGlobal");
    EXPECT_EQ(to_string(PurityLevel::ExternalDependency), "ExternalDependency");
}

// ---------------------------------------------------------------------------
// PurityConfidence enum
// ---------------------------------------------------------------------------
TEST(PurityConfidenceTest, ValuesAreOrdered) {
    EXPECT_LT(static_cast<uint8_t>(PurityConfidence::None),
              static_cast<uint8_t>(PurityConfidence::Low));
    EXPECT_LT(static_cast<uint8_t>(PurityConfidence::Low),
              static_cast<uint8_t>(PurityConfidence::Medium));
    EXPECT_LT(static_cast<uint8_t>(PurityConfidence::Medium),
              static_cast<uint8_t>(PurityConfidence::High));
    EXPECT_LT(static_cast<uint8_t>(PurityConfidence::High),
              static_cast<uint8_t>(PurityConfidence::Proven));
}

TEST(PurityConfidenceTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(PurityConfidence::None), "none");
    EXPECT_EQ(to_string(PurityConfidence::Low), "low");
    EXPECT_EQ(to_string(PurityConfidence::Medium), "medium");
    EXPECT_EQ(to_string(PurityConfidence::High), "high");
    EXPECT_EQ(to_string(PurityConfidence::Proven), "proven");
}

// ---------------------------------------------------------------------------
// AccessType enum
// ---------------------------------------------------------------------------
TEST(AccessTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(AccessType::Read), "read");
    EXPECT_EQ(to_string(AccessType::Write), "write");
}

// ---------------------------------------------------------------------------
// AccessTarget enum
// ---------------------------------------------------------------------------
TEST(AccessTargetTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(AccessTarget::Local), "local");
    EXPECT_EQ(to_string(AccessTarget::Parameter), "parameter");
    EXPECT_EQ(to_string(AccessTarget::Receiver), "receiver");
    EXPECT_EQ(to_string(AccessTarget::Global), "global");
    EXPECT_EQ(to_string(AccessTarget::Closure), "closure");
    EXPECT_EQ(to_string(AccessTarget::Field), "field");
    EXPECT_EQ(to_string(AccessTarget::Index), "index");
    EXPECT_EQ(to_string(AccessTarget::Unknown), "unknown");
}

// ---------------------------------------------------------------------------
// AccessPatternType enum
// ---------------------------------------------------------------------------
TEST(AccessPatternTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(AccessPatternType::Pure), "pure");
    EXPECT_EQ(to_string(AccessPatternType::ReadThenWrite), "read-then-write");
    EXPECT_EQ(to_string(AccessPatternType::WriteOnly), "write-only");
    EXPECT_EQ(to_string(AccessPatternType::WriteThenRead), "write-then-read");
    EXPECT_EQ(to_string(AccessPatternType::Interleaved), "interleaved");
    EXPECT_EQ(to_string(AccessPatternType::Unknown), "unknown");
}

TEST(AccessPatternTypeTest, IsCleanIdentifiesCleanPatterns) {
    EXPECT_TRUE(is_clean(AccessPatternType::Pure));
    EXPECT_TRUE(is_clean(AccessPatternType::ReadThenWrite));
    EXPECT_TRUE(is_clean(AccessPatternType::WriteOnly));
    EXPECT_FALSE(is_clean(AccessPatternType::WriteThenRead));
    EXPECT_FALSE(is_clean(AccessPatternType::Interleaved));
    EXPECT_FALSE(is_clean(AccessPatternType::Unknown));
}

// ---------------------------------------------------------------------------
// ViolationType enum
// ---------------------------------------------------------------------------
TEST(ViolationTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(ViolationType::WriteBeforeRead), "write-before-read");
    EXPECT_EQ(to_string(ViolationType::ReadAfterWrite), "read-after-write");
    EXPECT_EQ(to_string(ViolationType::InterleavedAccess), "interleaved-access");
    EXPECT_EQ(to_string(ViolationType::SelfInterference), "self-interference");
    EXPECT_EQ(to_string(ViolationType::MutateParameter), "mutate-parameter");
    EXPECT_EQ(to_string(ViolationType::MutateReceiver), "mutate-receiver");
}

// ---------------------------------------------------------------------------
// SideEffectInfo struct
// ---------------------------------------------------------------------------
TEST(SideEffectInfoTest, DefaultConstruction) {
    SideEffectInfo sei;
    EXPECT_TRUE(sei.function_name.empty());
    EXPECT_EQ(sei.categories, 0u);
    EXPECT_EQ(sei.confidence, PurityConfidence::None);
    EXPECT_EQ(sei.purity_level, PurityLevel::InternallyPure);
    EXPECT_FALSE(sei.is_pure);
    EXPECT_EQ(sei.purity_score, 0.0);
    EXPECT_EQ(sei.transitive_categories, 0u);
    EXPECT_TRUE(sei.parameter_writes.empty());
    EXPECT_TRUE(sei.global_writes.empty());
    EXPECT_TRUE(sei.external_calls.empty());
    EXPECT_TRUE(sei.throw_sites.empty());
    EXPECT_TRUE(sei.unresolved_calls.empty());
    EXPECT_TRUE(sei.impurity_reasons.empty());
}

// ---------------------------------------------------------------------------
// ErrorHandlingInfo struct
// ---------------------------------------------------------------------------
TEST(ErrorHandlingInfoTest, DefaultConstruction) {
    ErrorHandlingInfo ehi;
    EXPECT_FALSE(ehi.can_throw);
    EXPECT_FALSE(ehi.returns_error);
    EXPECT_FALSE(ehi.exception_neutral);
    EXPECT_FALSE(ehi.exception_safe);
    EXPECT_EQ(ehi.defer_count, 0);
    EXPECT_EQ(ehi.try_finally_count, 0);
    EXPECT_EQ(ehi.throw_count, 0);
    EXPECT_TRUE(ehi.error_return_lines.empty());
}

// ---------------------------------------------------------------------------
// NodeType enum
// ---------------------------------------------------------------------------
TEST(NodeTypeTest, Has11Variants) {
    EXPECT_EQ(static_cast<uint8_t>(NodeType::Function), 0);
    EXPECT_EQ(static_cast<uint8_t>(NodeType::Recursive), 10);
}

TEST(NodeTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(NodeType::Function), "function");
    EXPECT_EQ(to_string(NodeType::Loop), "loop");
    EXPECT_EQ(to_string(NodeType::Condition), "condition");
    EXPECT_EQ(to_string(NodeType::Switch), "switch");
    EXPECT_EQ(to_string(NodeType::Async), "async");
    EXPECT_EQ(to_string(NodeType::Network), "network");
    EXPECT_EQ(to_string(NodeType::Database), "database");
    EXPECT_EQ(to_string(NodeType::CPUIntensive), "cpu_intensive");
    EXPECT_EQ(to_string(NodeType::FileIO), "file_io");
    EXPECT_EQ(to_string(NodeType::External), "external");
    EXPECT_EQ(to_string(NodeType::Recursive), "recursive");
}

// ---------------------------------------------------------------------------
// TreeNode struct
// ---------------------------------------------------------------------------
TEST(TreeNodeTest, DefaultConstruction) {
    TreeNode tn;
    EXPECT_TRUE(tn.name.empty());
    EXPECT_TRUE(tn.file_path.empty());
    EXPECT_EQ(tn.line, 0);
    EXPECT_EQ(tn.depth, 0);
    EXPECT_EQ(tn.node_type, NodeType::Function);
    EXPECT_TRUE(tn.children.empty());
    EXPECT_EQ(tn.edit_risk_score, 0);
    EXPECT_TRUE(tn.stability_tags.empty());
    EXPECT_EQ(tn.dependency_count, 0);
    EXPECT_EQ(tn.dependent_count, 0);
    EXPECT_EQ(tn.impact_radius, 0);
    EXPECT_TRUE(tn.safety_notes.empty());
}

TEST(TreeNodeTest, CanAddChildren) {
    TreeNode parent;
    parent.name = "main";
    TreeNode child_a;
    child_a.name = "helper_a";
    parent.children.push_back(child_a);
    TreeNode child_b;
    child_b.name = "helper_b";
    parent.children.push_back(child_b);
    EXPECT_EQ(parent.children.size(), 2u);
    EXPECT_EQ(parent.children[0].name, "helper_a");
    EXPECT_EQ(parent.children[1].name, "helper_b");
}

// ---------------------------------------------------------------------------
// FunctionTree struct
// ---------------------------------------------------------------------------
TEST(FunctionTreeTest, DefaultConstruction) {
    FunctionTree ft;
    EXPECT_TRUE(ft.root_function.empty());
    EXPECT_EQ(ft.max_depth, 0);
    EXPECT_EQ(ft.total_nodes, 0);
}

// ---------------------------------------------------------------------------
// RelationshipType enum
// ---------------------------------------------------------------------------
TEST(RelationshipTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(RelationshipType::Extends), "extends");
    EXPECT_EQ(to_string(RelationshipType::Implements), "implements");
    EXPECT_EQ(to_string(RelationshipType::Contains), "contains");
    EXPECT_EQ(to_string(RelationshipType::ContainedBy), "contained_by");
    EXPECT_EQ(to_string(RelationshipType::DependsOn), "depends_on");
    EXPECT_EQ(to_string(RelationshipType::DependedOnBy), "depended_on_by");
    EXPECT_EQ(to_string(RelationshipType::Calls), "calls");
    EXPECT_EQ(to_string(RelationshipType::CalledBy), "called_by");
    EXPECT_EQ(to_string(RelationshipType::References), "references");
    EXPECT_EQ(to_string(RelationshipType::ReferencedBy), "referenced_by");
    EXPECT_EQ(to_string(RelationshipType::Imports), "imports");
    EXPECT_EQ(to_string(RelationshipType::ImportedBy), "imported_by");
    EXPECT_EQ(to_string(RelationshipType::FileCoLocated), "file_co_located");
    EXPECT_EQ(to_string(RelationshipType::TypeOf), "type_of");
    EXPECT_EQ(to_string(RelationshipType::HasType), "has_type");
    EXPECT_EQ(to_string(RelationshipType::ParentType), "parent_type");
    EXPECT_EQ(to_string(RelationshipType::ChildType), "child_type");
    EXPECT_EQ(to_string(RelationshipType::CrossLanguage), "cross_language");
}

// ---------------------------------------------------------------------------
// UniversalSymbolNode struct
// ---------------------------------------------------------------------------
TEST(UniversalSymbolNodeTest, DefaultConstruction) {
    UniversalSymbolNode usn;
    EXPECT_TRUE(usn.identity.name.empty());
    EXPECT_EQ(usn.identity.id, 0u);
    EXPECT_EQ(usn.visibility.access, AccessLevel::Unknown);
    EXPECT_FALSE(usn.visibility.is_exported);
    EXPECT_EQ(usn.usage.reference_count, 0);
    EXPECT_EQ(usn.metadata.complexity_score, 0);
    EXPECT_TRUE(usn.relationships.extends.empty());
}

// ---------------------------------------------------------------------------
// ContextManifest struct
// ---------------------------------------------------------------------------
TEST(ContextManifestTest, DefaultConstruction) {
    ContextManifest cm;
    EXPECT_TRUE(cm.task.empty());
    EXPECT_TRUE(cm.version.empty());
    EXPECT_TRUE(cm.project_root.empty());
    EXPECT_TRUE(cm.refs.empty());
    EXPECT_EQ(cm.stats.ref_count, 0);
}

TEST(ContextRefTest, DefaultConstruction) {
    ContextRef cr;
    EXPECT_TRUE(cr.file.empty());
    EXPECT_TRUE(cr.symbol.empty());
    EXPECT_FALSE(cr.has_line_range);
    EXPECT_TRUE(cr.expansions.empty());
    EXPECT_TRUE(cr.role.empty());
    EXPECT_TRUE(cr.note.empty());
}

// ---------------------------------------------------------------------------
// HydratedContext struct
// ---------------------------------------------------------------------------
TEST(HydratedContextTest, DefaultConstruction) {
    HydratedContext hc;
    EXPECT_TRUE(hc.task.empty());
    EXPECT_TRUE(hc.refs.empty());
    EXPECT_EQ(hc.stats.refs_loaded, 0);
    EXPECT_FALSE(hc.stats.truncated);
    EXPECT_TRUE(hc.warnings.empty());
}

// ---------------------------------------------------------------------------
// CallType enum
// ---------------------------------------------------------------------------
TEST(CallTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(CallType::Direct), "direct");
    EXPECT_EQ(to_string(CallType::Method), "method");
    EXPECT_EQ(to_string(CallType::Callback), "callback");
    EXPECT_EQ(to_string(CallType::Dynamic), "dynamic");
    EXPECT_EQ(to_string(CallType::Recursive), "recursive");
    EXPECT_EQ(to_string(CallType::Virtual), "virtual");
    EXPECT_EQ(to_string(CallType::Interface), "interface");
    EXPECT_EQ(to_string(CallType::Async), "async");
    EXPECT_EQ(to_string(CallType::Deferred), "deferred");
}

// ---------------------------------------------------------------------------
// DependencyType enum
// ---------------------------------------------------------------------------
TEST(DependencyTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(DependencyType::Unknown), "unknown");
    EXPECT_EQ(to_string(DependencyType::Import), "import");
    EXPECT_EQ(to_string(DependencyType::Inheritance), "inheritance");
    EXPECT_EQ(to_string(DependencyType::Composition), "composition");
    EXPECT_EQ(to_string(DependencyType::Aggregation), "aggregation");
    EXPECT_EQ(to_string(DependencyType::Association), "association");
    EXPECT_EQ(to_string(DependencyType::Usage), "usage");
    EXPECT_EQ(to_string(DependencyType::Configuration), "configuration");
    EXPECT_EQ(to_string(DependencyType::Runtime), "runtime");
}

// ---------------------------------------------------------------------------
// DependencyStrength enum
// ---------------------------------------------------------------------------
TEST(DependencyStrengthTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(DependencyStrength::Weak), "weak");
    EXPECT_EQ(to_string(DependencyStrength::Moderate), "moderate");
    EXPECT_EQ(to_string(DependencyStrength::Strong), "strong");
    EXPECT_EQ(to_string(DependencyStrength::Critical), "critical");
}

// ---------------------------------------------------------------------------
// AccessLevel enum
// ---------------------------------------------------------------------------
TEST(AccessLevelTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(AccessLevel::Unknown), "unknown");
    EXPECT_EQ(to_string(AccessLevel::Public), "public");
    EXPECT_EQ(to_string(AccessLevel::Private), "private");
    EXPECT_EQ(to_string(AccessLevel::Protected), "protected");
    EXPECT_EQ(to_string(AccessLevel::Internal), "internal");
    EXPECT_EQ(to_string(AccessLevel::Package), "package");
}

// ---------------------------------------------------------------------------
// handle_side_effects: empty analyzer must fail loud, not fabricate
// ---------------------------------------------------------------------------

// Karpathy #6 (no fabricated data): summary mode over an analyzer with zero
// per-function records must surface an explicit analysis_unavailable error.
// The pre-fix code counted callable symbols in the index and reported
// pure_count == total (an invented 100% purity ratio).
TEST(SideEffectsHandlerTest, EmptyAnalyzerYieldsAnalysisUnavailable) {
    auto dir = lci::test::unique_temp_dir("lci_se_empty_test_");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "main.go");
        f << "package main\n\nfunc main() {}\n\nfunc helper() {}\n";
    }

    Config config;
    config.project.root = dir.string();
    MasterIndex indexer(config);
    ASSERT_TRUE(indexer.index_directory(dir.string()));

    SideEffectAnalyzer analyzer("go");  // zero records: nothing analyzed
    nlohmann::json params = {{"mode", "summary"}};
    auto result = mcp::handle_side_effects(params, analyzer, &indexer);
    ASSERT_FALSE(result.is_error) << result.text;
    auto j = nlohmann::json::parse(result.text);

    ASSERT_TRUE(j.contains("error")) << j.dump();
    EXPECT_EQ(j["error"].get<std::string>(), "analysis_unavailable");
    // No invented purity: the summary must not claim every function is pure.
    if (j.contains("summary") && j["summary"].contains("purity_ratio")) {
        EXPECT_NE(j["summary"]["purity_ratio"].get<double>(), 1.0);
    }

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// handle_side_effects: capped modes must be deterministic
// ---------------------------------------------------------------------------

// Runs one side_effects query in a fresh child process (fresh absl hash salt
// -> fresh flat_hash_map iteration order) and returns the raw response text.
static std::string run_impure_query_in_child() {
    int fds[2];
    if (pipe(fds) != 0) return {};
    pid_t pid = fork();
    if (pid == 0) {
        // Child: build the fixture fresh, run the handler, write the text.
        close(fds[0]);
        SideEffectAnalyzer analyzer("go");
        // 10 impure functions (global writes) spread over 4 files.
        for (int i = 0; i < 10; ++i) {
            char name[32];
            snprintf(name, sizeof(name), "impureFn%d", i);
            char file[32];
            snprintf(file, sizeof(file), "file%d.go", i % 4);
            int line = 10 + i;
            analyzer.begin_function(name, file, line, line + 5);
            analyzer.record_access("globalVar", {}, AccessType::Write,
                                   line + 1, 1);
            analyzer.end_function();
        }
        nlohmann::json params = {{"mode", "impure"}, {"max_results", 3}};
        auto result = mcp::handle_side_effects(params, analyzer, nullptr);
        const std::string& text = result.text;
        size_t off = 0;
        while (off < text.size()) {
            ssize_t n = write(fds[1], text.data() + off, text.size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        _exit(0);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return out;
}

// Karpathy #4 (determinism): mode=impure with max_results=3 on 10 impure
// functions must return the SAME 3 results in every process. The pre-fix code
// iterated analyzer.results() (flat_hash_map) and stopped at the cap, so the
// emitted subset depended on per-process hash order.
TEST(SideEffectsHandlerTest, ImpureModeDeterministicAcrossProcesses) {
    std::string reference;
    for (int run = 0; run < 5; ++run) {
        std::string text = run_impure_query_in_child();
        ASSERT_FALSE(text.empty()) << "child process produced no output";
        if (run == 0) {
            reference = text;
        } else {
            EXPECT_EQ(text, reference) << "run " << run << " diverged";
        }
    }

    // And the emitted rows are the (path, line)-sorted first three.
    auto j = nlohmann::json::parse(reference);
    ASSERT_EQ(j["total_count"].get<int>(), 10);
    const auto& results = j["results"];
    ASSERT_EQ(results.size(), 3u);
    for (size_t i = 1; i < results.size(); ++i) {
        const auto& a = results[i - 1];
        const auto& b = results[i];
        auto key_a = std::make_pair(a["file_path"].get<std::string>(),
                                    a["line"].get<int>());
        auto key_b = std::make_pair(b["file_path"].get<std::string>(),
                                    b["line"].get<int>());
        EXPECT_LT(key_a, key_b);
    }
    // Exact sorted head: file0.go lines 10, 14, 18 (i%4 spreads the 10
    // functions across file0..file3, lines 10..19).
    EXPECT_EQ(results[0]["file_path"].get<std::string>(), "file0.go");
    EXPECT_EQ(results[0]["line"].get<int>(), 10);
    EXPECT_EQ(results[1]["file_path"].get<std::string>(), "file0.go");
    EXPECT_EQ(results[1]["line"].get<int>(), 14);
    EXPECT_EQ(results[2]["file_path"].get<std::string>(), "file0.go");
    EXPECT_EQ(results[2]["line"].get<int>(), 18);
}

// ---------------------------------------------------------------------------
// handle_side_effects symbol mode: file_path disambiguation + symbol_id
// ---------------------------------------------------------------------------

// Two files each defining `dup` with distinct side-effect records; symbol
// mode must honor file_path and symbol_id instead of taking the first
// find_symbols_by_name hit.
class SideEffectsSymbolModeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = lci::test::unique_temp_dir("lci_se_symbol_test_");
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        // `dup` sits on line 3 in both files.
        const char* src = "package a\n\nfunc dup() int { return 1 }\n";
        {
            std::ofstream fa(dir_ / "a.go");
            fa << src;
        }
        {
            std::ofstream fb(dir_ / "b.go");
            fb << src;
        }

        Config config;
        config.project.root = dir_.string();
        indexer_ = std::make_unique<MasterIndex>(config);
        ASSERT_TRUE(indexer_->index_directory(dir_.string()));

        analyzer_ = std::make_unique<SideEffectAnalyzer>("go");
        for (const char* f : {"a.go", "b.go"}) {
            auto abs = (dir_ / f).string();
            analyzer_->begin_function("dup", abs, 3, 3);
            // Distinct markers: a.go writes a global (impure), b.go is pure.
            if (f[0] == 'a') {
                analyzer_->record_access("g", {}, AccessType::Write, 3, 1);
            }
            analyzer_->end_function();
        }

        // Locate the SymbolID of the b.go definition.
        auto rt_snap = indexer_->ref_tracker().pin();
        auto syms = rt_snap->find_symbols_by_name("dup");
        ASSERT_EQ(syms.size(), 2u);
        for (const auto& s : syms) {
            auto path = indexer_->get_file_path(s->symbol.file_id);
            if (path.size() >= 4 &&
                path.compare(path.size() - 4, 4, "b.go") == 0) {
                b_symbol_id_ = encode_symbol_id(s->id);
            }
        }
        ASSERT_FALSE(b_symbol_id_.empty());
    }

    void TearDown() override {
        indexer_.reset();
        std::filesystem::remove_all(dir_);
    }

    std::filesystem::path dir_;
    std::unique_ptr<MasterIndex> indexer_;
    std::unique_ptr<SideEffectAnalyzer> analyzer_;
    std::string b_symbol_id_;
};

TEST_F(SideEffectsSymbolModeTest, FilePathDisambiguates) {
    nlohmann::json params = {{"mode", "symbol"},
                             {"symbol_name", "dup"},
                             {"file_path", "b.go"}};
    auto result = mcp::handle_side_effects(params, *analyzer_, indexer_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    auto j = nlohmann::json::parse(result.text);
    ASSERT_EQ(j["results"].size(), 1u) << j.dump();
    EXPECT_EQ(j["results"][0]["file_path"].get<std::string>(), "b.go");
    EXPECT_TRUE(j["results"][0]["is_pure"].get<bool>());
}

TEST_F(SideEffectsSymbolModeTest, SymbolIdSelectsExactSymbol) {
    nlohmann::json params = {{"mode", "symbol"}, {"symbol_id", b_symbol_id_}};
    auto result = mcp::handle_side_effects(params, *analyzer_, indexer_.get());
    ASSERT_FALSE(result.is_error) << result.text;
    auto j = nlohmann::json::parse(result.text);
    ASSERT_EQ(j["results"].size(), 1u) << j.dump();
    EXPECT_EQ(j["results"][0]["file_path"].get<std::string>(), "b.go");
}

}  // namespace
}  // namespace lci
