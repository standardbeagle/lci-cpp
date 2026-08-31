#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/analysis/coupling_analyzer.h>
#include <lci/analysis/feature_analyzer.h>
#include <lci/analysis/ci_vocabulary_analyzer.h>
#include <lci/analysis/layer_analyzer.h>
#include <lci/analysis/module_analyzer.h>
#include <lci/analysis/naming_analyzer.h>
#include <lci/analysis/scope_set.h>
#include <lci/analysis/english_words.h>
#include <lci/reference.h>
#include <lci/semantic/synonym_table.h>

namespace lci {
namespace {

// ===========================================================================
// Helpers: create test symbols
// ===========================================================================

EnhancedSymbol make_sym(std::string name, SymbolType type,
                        SymbolID id = 0, FileID fid = 0) {
    EnhancedSymbol es;
    es.symbol.name = std::move(name);
    es.symbol.type = type;
    es.symbol.file_id = fid;
    es.symbol.line = 1;
    es.symbol.end_line = 10;
    es.id = id;
    return es;
}

FileSymbolData make_file(std::string path,
                         std::vector<const EnhancedSymbol*> syms) {
    FileSymbolData fsd;
    fsd.path = std::move(path);
    fsd.symbols = std::move(syms);
    return fsd;
}

// ===========================================================================
// CouplingAnalyzer - is_code_file
// ===========================================================================

TEST(CouplingAnalyzer, IsCodeFileGoTrue) {
    EXPECT_TRUE(CouplingAnalyzer::is_code_file("main.go"));
}

TEST(CouplingAnalyzer, IsCodeFileCppTrue) {
    EXPECT_TRUE(CouplingAnalyzer::is_code_file("src/foo.cpp"));
}

TEST(CouplingAnalyzer, IsCodeFileHeaderTrue) {
    EXPECT_TRUE(CouplingAnalyzer::is_code_file("include/bar.h"));
}

TEST(CouplingAnalyzer, IsCodeFileTxtFalse) {
    EXPECT_FALSE(CouplingAnalyzer::is_code_file("readme.txt"));
}

TEST(CouplingAnalyzer, IsCodeFileNoExtFalse) {
    EXPECT_FALSE(CouplingAnalyzer::is_code_file("Makefile"));
}

TEST(CouplingAnalyzer, IsCodeFilePyTrue) {
    EXPECT_TRUE(CouplingAnalyzer::is_code_file("script.py"));
}

// ===========================================================================
// CouplingAnalyzer - get_package_name
// ===========================================================================

TEST(CouplingAnalyzer, PackageNameRelative) {
    EXPECT_EQ(CouplingAnalyzer::get_package_name("/proj/src/foo/bar.go", "/proj"),
              "src/foo");
}

TEST(CouplingAnalyzer, PackageNameRoot) {
    EXPECT_EQ(CouplingAnalyzer::get_package_name("/proj/bar.go", "/proj"),
              "(root)");
}

TEST(CouplingAnalyzer, PackageNameEmptyRoot) {
    EXPECT_EQ(CouplingAnalyzer::get_package_name("src/foo/bar.go", ""),
              "src/foo");
}

// ===========================================================================
// CouplingAnalyzer - analyze empty
// ===========================================================================

TEST(CouplingAnalyzer, AnalyzeEmpty) {
    CouplingAnalyzer ca;
    auto result = ca.analyze({}, "/proj",
                             [](SymbolID) { return std::vector<SymbolID>{}; });
    EXPECT_DOUBLE_EQ(result.coupling.average_coupling, 0.0);
    EXPECT_DOUBLE_EQ(result.cohesion.average_cohesion, 0.0);
}

// ===========================================================================
// CouplingAnalyzer - analyze with two packages
// ===========================================================================

TEST(CouplingAnalyzer, AnalyzeTwoPackages) {
    EnhancedSymbol s1 = make_sym("FuncA", SymbolType::Function, 1);
    EnhancedSymbol s2 = make_sym("FuncB", SymbolType::Function, 2);

    // s1 references s2 (cross-package)
    auto f1 = make_file("/proj/pkg_a/a.go", {&s1});
    auto f2 = make_file("/proj/pkg_b/b.go", {&s2});

    CouplingAnalyzer ca;
    auto result = ca.analyze({f1, f2}, "/proj", [&](SymbolID id) {
        return id == s1.id ? std::vector<SymbolID>{2}
                           : std::vector<SymbolID>{};
    });

    EXPECT_EQ(result.coupling.efferent_coupling["pkg_a"], 1);
    EXPECT_EQ(result.coupling.afferent_coupling["pkg_b"], 1);
    EXPECT_GT(result.coupling.average_coupling, 0.0);
}

// Ca/Ce are DISTINCT package counts (textbook Martin coupling), not raw
// edge counts — pocketbase reported core depended_on_by=1388 edges against
// ~30 real depending packages.
TEST(CouplingAnalyzer, AfferentCountsDistinctPackagesNotEdges) {
    EnhancedSymbol s1 = make_sym("A1", SymbolType::Function, 1);
    EnhancedSymbol s2 = make_sym("A2", SymbolType::Function, 2);
    EnhancedSymbol t = make_sym("Core", SymbolType::Function, 3);

    auto f1 = make_file("/proj/pkg_a/a.go", {&s1, &s2});
    auto f2 = make_file("/proj/core/c.go", {&t});

    CouplingAnalyzer ca;
    // Both pkg_a symbols call Core twice each: 4 edges, 1 depending package.
    auto result = ca.analyze({f1, f2}, "/proj", [&](SymbolID id) {
        if (id == s1.id || id == s2.id)
            return std::vector<SymbolID>{3, 3};
        return std::vector<SymbolID>{};
    });
    EXPECT_EQ(result.coupling.afferent_coupling["core"], 1);
    EXPECT_EQ(result.coupling.efferent_coupling["pkg_a"], 1);
}

// ===========================================================================
// CouplingAnalyzer - self-references counted as internal
// ===========================================================================

TEST(CouplingAnalyzer, SelfRefsAreCohesive) {
    EnhancedSymbol s1 = make_sym("FuncA", SymbolType::Function, 1);
    EnhancedSymbol s2 = make_sym("FuncB", SymbolType::Function, 2);

    // Both in same package, s1 references s2
    auto f = make_file("/proj/pkg/a.go", {&s1, &s2});

    CouplingAnalyzer ca;
    auto result = ca.analyze({f}, "/proj", [&](SymbolID id) {
        return id == s1.id ? std::vector<SymbolID>{2}
                           : std::vector<SymbolID>{};
    });

    // Internal reference should boost cohesion
    auto it = result.cohesion.relational_cohesion.find("pkg");
    ASSERT_NE(it, result.cohesion.relational_cohesion.end());
    EXPECT_DOUBLE_EQ(it->second, 1.0);  // All refs are internal
}

// ===========================================================================
// FeatureAnalyzer - classify_component_type
// ===========================================================================

TEST(FeatureAnalyzer, ClassifyComponentHandler) {
    auto sym = make_sym("UserHandler", SymbolType::Function);
    EXPECT_EQ(FeatureAnalyzer::classify_component_type(sym), "Controller");
}

TEST(FeatureAnalyzer, ClassifyComponentService) {
    auto sym = make_sym("OrderService", SymbolType::Function);
    EXPECT_EQ(FeatureAnalyzer::classify_component_type(sym), "Service");
}

TEST(FeatureAnalyzer, ClassifyComponentRepository) {
    auto sym = make_sym("UserRepository", SymbolType::Class);
    EXPECT_EQ(FeatureAnalyzer::classify_component_type(sym), "Repository");
}

TEST(FeatureAnalyzer, ClassifyComponentModel) {
    auto sym = make_sym("UserModel", SymbolType::Class);
    EXPECT_EQ(FeatureAnalyzer::classify_component_type(sym), "Model");
}

TEST(FeatureAnalyzer, ClassifyComponentInterface) {
    auto sym = make_sym("Serializer", SymbolType::Interface);
    EXPECT_EQ(FeatureAnalyzer::classify_component_type(sym), "Interface");
}

TEST(FeatureAnalyzer, ClassifyComponentPlainFunction) {
    auto sym = make_sym("doSomething", SymbolType::Function);
    EXPECT_EQ(FeatureAnalyzer::classify_component_type(sym), "Function");
}

// ===========================================================================
// FeatureAnalyzer - classify_feature_type
// ===========================================================================

TEST(FeatureAnalyzer, FeatureTypeUserManagement) {
    EXPECT_EQ(FeatureAnalyzer::classify_feature_type("auth"), "User Management");
}

TEST(FeatureAnalyzer, FeatureTypeEcommerce) {
    EXPECT_EQ(FeatureAnalyzer::classify_feature_type("payment"), "E-commerce");
}

TEST(FeatureAnalyzer, FeatureTypeSearch) {
    EXPECT_EQ(FeatureAnalyzer::classify_feature_type("search"), "Search");
}

TEST(FeatureAnalyzer, FeatureTypeGeneral) {
    EXPECT_EQ(FeatureAnalyzer::classify_feature_type("xyz"), "General Feature");
}

TEST(FeatureAnalyzer, FeatureTypeConfig) {
    EXPECT_EQ(FeatureAnalyzer::classify_feature_type("config"), "Configuration");
}

// ===========================================================================
// FeatureAnalyzer - analyze
// ===========================================================================

// Feature analysis is now Louvain community detection over the symbol
// reference graph, so the tests supply a real call-edge adjacency (the
// `callees_of` boundary) rather than relying on name-keyword matching.
namespace {

// Two densely-connected triangles (auth: 1-2-3, order: 4-5-6) joined by a
// single cross edge 3->4, plus an isolated symbol 7. Louvain must recover the
// two triangles as features and leave 7 as an orphan.
std::function<std::vector<SymbolID>(SymbolID)> two_cluster_callees() {
    absl::flat_hash_map<SymbolID, std::vector<SymbolID>> g = {
        {1, {2}}, {2, {3}}, {3, {1, 4}},  // auth triangle + cross edge to order
        {4, {5}}, {5, {6}}, {6, {4}},      // order triangle
        {7, {}},                            // orphan
    };
    return [g = std::move(g)](SymbolID id) -> std::vector<SymbolID> {
        auto it = g.find(id);
        return it != g.end() ? it->second : std::vector<SymbolID>{};
    };
}

EnhancedSymbol make_fn(std::string name, SymbolID id, int complexity) {
    auto s = make_sym(std::move(name), SymbolType::Function, id);
    s.complexity = complexity;
    return s;
}

}  // namespace

TEST(FeatureAnalyzer, AnalyzeEmpty) {
    FeatureAnalyzer fa;
    auto result = fa.analyze({}, two_cluster_callees());
    EXPECT_EQ(result.metrics.total_features, 0);
}

TEST(FeatureAnalyzer, LouvainRecoversTwoCommunities) {
    EnhancedSymbol s1 = make_fn("Login", 1, 5);
    EnhancedSymbol s2 = make_fn("Register", 2, 5);
    EnhancedSymbol s3 = make_fn("Validate", 3, 5);
    EnhancedSymbol s4 = make_fn("Checkout", 4, 7);
    EnhancedSymbol s5 = make_fn("Pay", 5, 7);
    EnhancedSymbol s6 = make_fn("Invoice", 6, 7);
    EnhancedSymbol s7 = make_fn("Unrelated", 7, 1);

    auto fa_file = make_file("src/auth/auth.go", {&s1, &s2, &s3});
    auto fb_file = make_file("src/order/order.go", {&s4, &s5, &s6});
    auto fc_file = make_file("src/util/util.go", {&s7});

    FeatureAnalyzer fa;
    auto r = fa.analyze({fa_file, fb_file, fc_file}, two_cluster_callees());

    // Two triangles -> two features; the disconnected symbol is an orphan.
    EXPECT_EQ(r.metrics.total_features, 2);
    ASSERT_EQ(r.orphan_components.size(), 1u);
    EXPECT_EQ(r.orphan_components.front().name, "Unrelated");

    // Real graph cohesion: each triangle has internal edges, so cohesion > 0,
    // and confidence mirrors cohesion.
    for (const auto& feat : r.features) {
        EXPECT_GT(feat.confidence, 0.0);
        EXPECT_EQ(feat.components.size(), 3u);
    }
    EXPECT_GT(r.metrics.avg_cohesion, 0.0);

    // avg_complexity is real mean cyclomatic complexity (5 and 7) = 6.
    EXPECT_DOUBLE_EQ(r.metrics.avg_complexity, 6.0);

    // The single cross edge 3->4 surfaces as one directed cross-feature dep.
    ASSERT_EQ(r.cross_feature_deps.size(), 1u);
    EXPECT_EQ(r.cross_feature_deps.front().type, "calls");
    EXPECT_GT(r.cross_feature_deps.front().strength, 0.0);
}

TEST(FeatureAnalyzer, Deterministic) {
    EnhancedSymbol s1 = make_fn("Login", 1, 5);
    EnhancedSymbol s2 = make_fn("Register", 2, 5);
    EnhancedSymbol s3 = make_fn("Validate", 3, 5);
    EnhancedSymbol s4 = make_fn("Checkout", 4, 7);
    EnhancedSymbol s5 = make_fn("Pay", 5, 7);
    EnhancedSymbol s6 = make_fn("Invoice", 6, 7);

    auto fa_file = make_file("src/auth/auth.go", {&s1, &s2, &s3});
    auto fb_file = make_file("src/order/order.go", {&s4, &s5, &s6});

    FeatureAnalyzer fa;
    auto r1 = fa.analyze({fa_file, fb_file}, two_cluster_callees());
    auto r2 = fa.analyze({fa_file, fb_file}, two_cluster_callees());

    ASSERT_EQ(r1.features.size(), r2.features.size());
    for (size_t i = 0; i < r1.features.size(); ++i) {
        EXPECT_EQ(r1.features[i].name, r2.features[i].name);
        EXPECT_EQ(r1.features[i].components.size(),
                  r2.features[i].components.size());
    }
}

// ===========================================================================
// Determinism (Karpathy rule 4): analyzers that walk a hash map must sort
// before they emit, or both the order AND any depth/rank derived from that
// order come out of a per-process hash seed.
// ===========================================================================

TEST(LayerAnalyzer, LayersEmitInSortedOrderWithMatchingDepth) {
    auto s1 = make_sym("UserRepository", SymbolType::Class, 1);
    auto s2 = make_sym("renderPage", SymbolType::Function, 2);
    auto s3 = make_sym("OrderService", SymbolType::Class, 3);
    auto s4 = make_sym("validateInput", SymbolType::Function, 4);
    auto s5 = make_sym("stringUtil", SymbolType::Function, 5);
    auto f = make_file("app.go", {&s1, &s2, &s3, &s4, &s5});

    auto result = LayerAnalyzer().analyze({f}, "");
    ASSERT_GE(result.layers.size(), 2u);

    for (size_t i = 1; i < result.layers.size(); ++i) {
        EXPECT_LT(result.layers[i - 1].name, result.layers[i].name);
    }
    for (size_t i = 0; i < result.layers.size(); ++i) {
        EXPECT_EQ(static_cast<int>(i) + 1, result.layers[i].depth);
    }
}

TEST(CIVocabularyAnalyzer, DomainsAndTermsEmitSorted) {
    auto s1 = make_sym("UserRepository", SymbolType::Class, 1);
    auto s2 = make_sym("OrderRepository", SymbolType::Class, 2);
    auto s3 = make_sym("AccountRepository", SymbolType::Class, 3);
    auto s4 = make_sym("PaymentService", SymbolType::Class, 4);
    auto s5 = make_sym("BillingService", SymbolType::Class, 5);
    auto f = make_file("app.go", {&s1, &s2, &s3, &s4, &s5});

    auto terms = CIVocabularyAnalyzer().extract_domain_terms_from_files({f});
    ASSERT_FALSE(terms.empty());

    for (size_t i = 1; i < terms.size(); ++i) {
        EXPECT_LT(terms[i - 1].domain, terms[i].domain);
    }
    for (const auto& dt : terms) {
        for (size_t i = 1; i < dt.terms.size(); ++i) {
            EXPECT_LT(dt.terms[i - 1], dt.terms[i]) << "domain " << dt.domain;
        }
    }
}

// ===========================================================================
// LayerAnalyzer - classify_symbol_to_layer
// ===========================================================================

TEST(LayerAnalyzer, ClassifyServiceToApplication) {
    auto sym = make_sym("UserService", SymbolType::Class);
    EXPECT_EQ(LayerAnalyzer::classify_symbol_to_layer(sym), "Application Layer");
}

TEST(LayerAnalyzer, ClassifyModelToDomain) {
    auto sym = make_sym("UserModel", SymbolType::Class);
    EXPECT_EQ(LayerAnalyzer::classify_symbol_to_layer(sym), "Domain Layer");
}

TEST(LayerAnalyzer, ClassifyRepositoryToData) {
    auto sym = make_sym("UserRepository", SymbolType::Class);
    EXPECT_EQ(LayerAnalyzer::classify_symbol_to_layer(sym), "Data Layer");
}

TEST(LayerAnalyzer, ClassifyComponentToPresentation) {
    auto sym = make_sym("UserComponent", SymbolType::Class);
    EXPECT_EQ(LayerAnalyzer::classify_symbol_to_layer(sym), "Presentation Layer");
}

TEST(LayerAnalyzer, ClassifyRenderToPresentation) {
    auto sym = make_sym("renderPage", SymbolType::Function);
    EXPECT_EQ(LayerAnalyzer::classify_symbol_to_layer(sym), "Presentation Layer");
}

TEST(LayerAnalyzer, ClassifyValidateToDomain) {
    auto sym = make_sym("validateInput", SymbolType::Function);
    EXPECT_EQ(LayerAnalyzer::classify_symbol_to_layer(sym), "Domain Layer");
}

TEST(LayerAnalyzer, ClassifyUtilToUtility) {
    auto sym = make_sym("stringUtil", SymbolType::Function);
    EXPECT_EQ(LayerAnalyzer::classify_symbol_to_layer(sym), "Utility Layer");
}

TEST(LayerAnalyzer, ClassifyUnknownToUtility) {
    auto sym = make_sym("xyz", SymbolType::Function);
    EXPECT_EQ(LayerAnalyzer::classify_symbol_to_layer(sym), "Utility Layer");
}

// ===========================================================================
// LayerAnalyzer - detect_patterns
// ===========================================================================

TEST(LayerAnalyzer, DetectLayeredArchitecture) {
    std::vector<ArchitecturalLayer> layers;
    for (auto name : {"Presentation Layer", "Application Layer",
                      "Domain Layer", "Data Layer"}) {
        ArchitecturalLayer al;
        al.name = name;
        al.metrics.symbol_count = 10;
        layers.push_back(al);
    }

    auto patterns = LayerAnalyzer::detect_patterns(layers);

    bool found_layered = false;
    for (const auto& p : patterns) {
        if (p.name == "Layered Architecture") found_layered = true;
    }
    EXPECT_TRUE(found_layered);
}

TEST(LayerAnalyzer, DetectEmptyLayers) {
    auto patterns = LayerAnalyzer::detect_patterns({});
    EXPECT_TRUE(patterns.empty());
}

// ===========================================================================
// LayerAnalyzer - analyze
// ===========================================================================

TEST(LayerAnalyzer, AnalyzeEmpty) {
    LayerAnalyzer la;
    auto result = la.analyze({}, "/repo");
    EXPECT_TRUE(result.layers.empty());
}

TEST(LayerAnalyzer, AnalyzeGroupsSymbols) {
    EnhancedSymbol s1 = make_sym("UserService", SymbolType::Class);
    EnhancedSymbol s2 = make_sym("UserModel", SymbolType::Class);
    EnhancedSymbol s3 = make_sym("renderPage", SymbolType::Function);

    auto f = make_file("src/app.go", {&s1, &s2, &s3});

    LayerAnalyzer la;
    auto result = la.analyze({f}, "");

    EXPECT_GE(static_cast<int>(result.layers.size()), 2);
}

TEST(LayerAnalyzer, LayerModulesArePackagesNotSymbols) {
    // The defect this pins: layers emitted one "module" per SYMBOL —
    // "Utility Layer: modules=440917" on a repo with 817 modules. Five
    // symbols across two directories must yield module counts bounded by
    // the directory count, with symbol counts reported separately.
    EnhancedSymbol s1 = make_sym("helperOne", SymbolType::Function, 1);
    EnhancedSymbol s2 = make_sym("helperTwo", SymbolType::Function, 2);
    EnhancedSymbol s3 = make_sym("helperThree", SymbolType::Function, 3);
    auto f1 = make_file("/repo/src/a.go", {&s1, &s2});
    auto f2 = make_file("/repo/lib/b.go", {&s3});

    auto result = LayerAnalyzer().analyze({f1, f2}, "/repo");
    ASSERT_EQ(result.layers.size(), 1u);  // all helpers -> Utility Layer
    const auto& l = result.layers[0];
    EXPECT_EQ(l.name, "Utility Layer");
    EXPECT_EQ(l.modules.size(), 2u);  // src + lib, not 3 symbols
    EXPECT_EQ(l.metrics.module_count, 2);
    EXPECT_EQ(l.metrics.symbol_count, 3);
}

TEST(LayerAnalyzer, PatternConfidenceIsMeasuredNotConstant) {
    // Four core layers present, but only a sliver of the corpus lives in
    // them -> no pattern reported (the old code emitted four patterns at a
    // constant 0.80 for any such corpus). With most symbols in the core
    // layers, Layered Architecture is reported at its measured share.
    auto layer = [](std::string name, int symbols) {
        ArchitecturalLayer l;
        l.name = std::move(name);
        l.metrics.symbol_count = symbols;
        return l;
    };

    auto weak = LayerAnalyzer::detect_patterns(
        {layer("Presentation Layer", 1), layer("Application Layer", 1),
         layer("Domain Layer", 1), layer("Data Layer", 1),
         layer("Utility Layer", 96)});
    EXPECT_TRUE(weak.empty());

    auto strong = LayerAnalyzer::detect_patterns(
        {layer("Presentation Layer", 30), layer("Application Layer", 30),
         layer("Domain Layer", 20), layer("Data Layer", 10),
         layer("Utility Layer", 10)});
    ASSERT_EQ(strong.size(), 1u);
    EXPECT_EQ(strong[0].name, "Layered Architecture");
    EXPECT_NEAR(strong[0].confidence, 0.9, 1e-9);
}

// ===========================================================================
// ModuleAnalyzer must not fabricate numbers it does not compute. The Go port
// carried coupling_score=0.3 and architectural_score=0.8 CONSTANTS, and the
// detailed sub=modules view rendered them raw while overview/statistics
// replaced coupling with the real CouplingAnalyzer value — the same corpus
// answered coupling=0.30 in one mode and 0.02 in another (okhttp audit).
// Unknown is -1; emitters print n/a.
TEST(ModuleAnalyzer, NoFabricatedCouplingOrArchScore) {
    auto s1 = make_sym("A", SymbolType::Function, 1, 1);
    auto s2 = make_sym("B", SymbolType::Function, 2, 2);
    std::vector<FileSymbolData> files = {
        make_file("pkg/a/x.go", {&s1}),
        make_file("pkg/b/y.go", {&s2}),
    };
    auto r = ModuleAnalyzer().analyze(files, "");
    ASSERT_FALSE(r.modules.empty());
    for (const auto& m : r.modules) {
        EXPECT_LT(m.coupling_score, 0.0)
            << m.name << ": placeholder coupling constant must be gone";
    }
    EXPECT_LT(r.metrics.average_coupling, 0.0);
    EXPECT_LT(r.metrics.architectural_score, 0.0)
        << "architectural_score was a 0.8 constant";
}

// ===========================================================================
// ModuleAnalyzer - classify_module_by_path
// ===========================================================================

TEST(ModuleAnalyzer, ClassifyAPILayer) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("src/api/v1"), "API Layer");
}

TEST(ModuleAnalyzer, ClassifyServiceLayer) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("src/service"), "Service Layer");
}

TEST(ModuleAnalyzer, ClassifyDataLayer) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("src/model"), "Data Layer");
}

TEST(ModuleAnalyzer, ClassifyRepositoryLayer) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("src/repository"),
              "Repository Layer");
}

TEST(ModuleAnalyzer, ClassifyUtility) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("src/util"), "Utility");
}

TEST(ModuleAnalyzer, ClassifyTest) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("tests/unit"), "Test");
}

TEST(ModuleAnalyzer, ClassifyGeneral) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("src/core"), "General");
}

TEST(ModuleAnalyzer, ClassifyConfig) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("src/config"), "Configuration");
}

TEST(ModuleAnalyzer, ClassifyMiddleware) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("src/middleware"), "Middleware");
}

// Broadened buckets (2026-08-26 field run: every module of four real corpora
// reported "General", a dead column).
TEST(ModuleAnalyzer, ClassifyBroadenedBuckets) {
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("cmd/slop"),
              "Entry Point");
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("website"), "UI");
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("internal/parser"),
              "Language Core");
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("internal/evaluator"),
              "Language Core");
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path(
                  "src/WorkTrack.Core/Migrations"),
              "Data Layer");
    // "Api" outranks "Auth": the api keyword sits earlier in the ladder.
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("src/WorkTrack.Api/Auth"),
              "API Layer");
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("internal/auth"), "Auth");
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("scripts"), "Tooling");
    // No keyword still means General.
    EXPECT_EQ(ModuleAnalyzer::classify_module_by_path("internal/daemon"),
              "General");
}

// ===========================================================================
// ModuleAnalyzer - analyze
// ===========================================================================

TEST(ModuleAnalyzer, AnalyzeEmpty) {
    ModuleAnalyzer ma;
    auto result = ma.analyze({});
    EXPECT_EQ(result.metrics.total_modules, 0);
    EXPECT_EQ(result.detection_strategy, "directory_structure");
}

TEST(ModuleAnalyzer, AnalyzeGroupsByDirectory) {
    EnhancedSymbol s1 = make_sym("FuncA", SymbolType::Function);
    EnhancedSymbol s2 = make_sym("FuncB", SymbolType::Function);
    EnhancedSymbol s3 = make_sym("FuncC", SymbolType::Function);

    auto f1 = make_file("src/api/handler.go", {&s1});
    auto f2 = make_file("src/service/logic.go", {&s2});
    auto f3 = make_file("src/service/helper.go", {&s3});

    ModuleAnalyzer ma;
    auto result = ma.analyze({f1, f2, f3});

    EXPECT_EQ(result.metrics.total_modules, 2);
    EXPECT_GT(result.metrics.average_cohesion, 0.0);
}

TEST(ModuleAnalyzer, AnalyzeMetricsCalculated) {
    EnhancedSymbol s1 = make_sym("FuncA", SymbolType::Function);
    EnhancedSymbol s2 = make_sym("FuncA_helper", SymbolType::Function);

    auto f = make_file("src/pkg/code.go", {&s1, &s2});

    ModuleAnalyzer ma;
    auto result = ma.analyze({f});

    EXPECT_EQ(result.metrics.total_modules, 1);
    EXPECT_GT(result.modules[0].cohesion_score, 0.0);
    EXPECT_GT(result.modules[0].stability, 0.0);
    EXPECT_EQ(result.modules[0].function_count, 2);
}

TEST(ModuleAnalyzer, AnalyzeFileCountAccurate) {
    EnhancedSymbol s1 = make_sym("FuncA", SymbolType::Function);
    EnhancedSymbol s2 = make_sym("FuncB", SymbolType::Function);

    auto f1 = make_file("src/pkg/a.go", {&s1});
    auto f2 = make_file("src/pkg/b.go", {&s2});

    ModuleAnalyzer ma;
    auto result = ma.analyze({f1, f2});

    EXPECT_EQ(result.metrics.total_modules, 1);
    EXPECT_EQ(result.modules[0].file_count, 2);
    EXPECT_EQ(result.modules[0].function_count, 2);
}

// ===========================================================================
// NamingAnalyzer - low-discoverability vocabulary signal
// ===========================================================================

namespace {
// A symbol with `fan_in` synthetic incoming references.
EnhancedSymbol make_ref_sym(std::string name, int fan_in, SymbolID id) {
    EnhancedSymbol es = make_sym(std::move(name), SymbolType::Function, id);
    es.incoming_ref_count = fan_in;
    return es;
}
}  // namespace

TEST(NamingAnalyzer, FlagsUnknownVerbHighFanIn) {
    auto table = SynonymTable::build_default();
    auto frob = make_ref_sym("frobnicate", 3, 1);
    auto f = make_file("api/legacy.go", {&frob});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");

    ASSERT_EQ(rep.outliers.size(), 1u);
    EXPECT_EQ(rep.outliers[0].name, "frobnicate");
    EXPECT_EQ(rep.outliers[0].reason, "unknown-verb");
    EXPECT_EQ(rep.outliers[0].fan_in, 3);
}

TEST(NamingAnalyzer, DoesNotFlagStandardVerb) {
    auto table = SynonymTable::build_default();
    // "fetch" is a recognized synonym of get; "getUser" leads with a common
    // word. Neither should be flagged regardless of fan-in.
    auto fetch = make_ref_sym("fetchUser", 5, 1);
    auto getu = make_ref_sym("getRecord", 5, 2);
    auto f = make_file("api/user.go", {&fetch, &getu});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_TRUE(rep.outliers.empty());
}

// 2026-08-30 sweep vocabulary FP classes. Established technical terms must
// not flag as obscure/misspelling (okhttp: jvm/bom/idn/localhost; zls:
// comptime, a Zig keyword; sinatra: etag/scss/csp, haml->html, yajl->yaml,
// uname->name).
TEST(NamingAnalyzer, TechTermsAreNotOutliers) {
    auto table = SynonymTable::build_default();
    auto a = make_ref_sym("jvmCheck", 5, 1);
    auto b = make_ref_sym("comptimeEval", 5, 2);
    auto c = make_ref_sym("etagHeader", 5, 3);
    auto d = make_ref_sym("hamlRender", 5, 4);
    auto e = make_ref_sym("unameProbe", 5, 5);
    auto f = make_file("core/terms.go", {&a, &b, &c, &d, &e});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    for (const auto& o : rep.outliers) {
        EXPECT_TRUE(false) << "false positive: " << o.name << " (" << o.reason
                           << ")";
    }
}

// A plural of known vocabulary is not a typo of its singular (sinatra:
// decls -> decl, awaitNanos -> nanos "misspelling" of nano).
TEST(NamingAnalyzer, PluralOfKnownWordIsNotMisspelling) {
    auto table = SynonymTable::build_default();
    // "decl" becomes corpus vocabulary via frequency (>2 distinct symbols).
    auto d1 = make_ref_sym("declParse", 3, 1);
    auto d2 = make_ref_sym("declPrint", 3, 2);
    auto d3 = make_ref_sym("declWalk", 3, 3);
    auto ds = make_ref_sym("declsCollect", 5, 4);
    auto f = make_file("core/decl.go", {&d1, &d2, &d3, &ds});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    for (const auto& o : rep.outliers) {
        EXPECT_NE(o.reason, "misspelling")
            << o.name << ": plural of corpus vocabulary flagged as typo";
    }
}

TEST(NamingAnalyzer, IgnoresLowFanInOutliers) {
    auto table = SynonymTable::build_default();
    auto frob = make_ref_sym("frobnicate", 1, 1);  // fan-in < 2
    auto f = make_file("api/legacy.go", {&frob});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_TRUE(rep.outliers.empty());
}

TEST(NamingAnalyzer, AliasesSurfaceNonPrimarySpelling) {
    // explode is a non-primary member of the split group → should appear in
    // aliases_in_use under the primary "split". A plain "split" symbol must
    // NOT create an alias entry (nothing to learn).
    auto table = SynonymTable::build_default();
    auto explode = make_ref_sym("explode", 0, 1);
    auto split = make_ref_sym("split", 0, 2);
    auto f = make_file("api/str.go", {&explode, &split});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    ASSERT_EQ(rep.aliases_in_use.size(), 1u);
    EXPECT_EQ(rep.aliases_in_use[0].canonical, "split");
    bool has_explode = false;
    for (const auto& [m, n] : rep.aliases_in_use[0].terms) {
        if (m == "explode") has_explode = true;
    }
    EXPECT_TRUE(has_explode);
}

TEST(NamingAnalyzer, AmbiguousNamesSurfaceRepeatedDefinitions) {
    auto table = SynonymTable::build_default();
    // "process" defined at 5 sites → ambiguous; "processOrder" at 2 → not.
    std::vector<EnhancedSymbol> syms;
    for (int i = 0; i < 5; ++i)
        syms.push_back(make_ref_sym("process", 0, static_cast<SymbolID>(i + 1)));
    syms.push_back(make_ref_sym("processOrder", 0, 10));
    syms.push_back(make_ref_sym("processOrder", 0, 11));
    std::vector<const EnhancedSymbol*> ptrs;
    for (auto& s : syms) ptrs.push_back(&s);
    auto f = make_file("api/proc.go", ptrs);

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    ASSERT_EQ(rep.ambiguous_names.size(), 1u);
    EXPECT_EQ(rep.ambiguous_names[0].name, "process");
    EXPECT_EQ(rep.ambiguous_names[0].definition_count, 5);
}

TEST(NamingAnalyzer, InformationDistinctiveNamesAreNotVague) {
    auto table = SynonymTable::build_default();
    // Four names, no shared tokens: each name's tokens isolate exactly one
    // symbol, so expected matches ~1 and nothing is vague.
    auto a = make_ref_sym("loadConfig", 1, 1);
    auto b = make_ref_sym("parseDocument", 1, 2);
    auto c = make_ref_sym("renderOutput", 1, 3);
    auto d = make_ref_sym("verifySignature", 1, 4);
    auto f = make_file("api/x.go", {&a, &b, &c, &d});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_EQ(rep.information.total_symbols, 4);
    EXPECT_TRUE(rep.information.vague_names.empty());
    EXPECT_GT(rep.information.median_bits, 0.0);
}

TEST(NamingAnalyzer, InformationSharedTokenNameIsVague) {
    auto table = SynonymTable::build_default();
    // "process" appears in every one of 8 names; the name "process" alone
    // carries ~0 bits and expects ~8 matches -> vague. Two-token names
    // ("processOrder") narrow to ~1 and are not vague.
    std::vector<EnhancedSymbol> syms;
    syms.push_back(make_ref_sym("process", 1, 1));
    const char* seconds[] = {"Order",  "User",  "Refund", "Claim",
                             "Ticket", "Batch", "Login"};
    SymbolID id = 2;
    for (const char* w : seconds)
        syms.push_back(make_ref_sym(std::string("process") + w, 1, id++));
    std::vector<const EnhancedSymbol*> ptrs;
    for (auto& s : syms) ptrs.push_back(&s);
    auto f = make_file("api/proc.go", ptrs);

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    ASSERT_EQ(rep.information.vague_names.size(), 1u);
    EXPECT_EQ(rep.information.vague_names[0].name, "process");
    EXPECT_GE(rep.information.vague_names[0].expected_matches, 5.0);
    EXPECT_NEAR(rep.information.vague_names[0].bits, 0.0, 0.01);
}

TEST(NamingAnalyzer, InformationBitsAddAcrossTokens) {
    auto table = SynonymTable::build_default();
    // 4 symbols; "load" in 2/4 (1 bit), "config" in 1/4 (2 bits).
    // "loadConfig" = 3 bits -> expected matches 4 * 2^-3 = 0.5: not vague.
    auto a = make_ref_sym("loadConfig", 1, 1);
    auto b = make_ref_sym("loadRecord", 1, 2);
    auto c = make_ref_sym("parseDocument", 1, 3);
    auto d = make_ref_sym("renderOutput", 1, 4);
    auto f = make_file("api/x.go", {&a, &b, &c, &d});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_TRUE(rep.information.vague_names.empty());
    // median over {3.0 (loadConfig), 3.0 (loadRecord), 4.0, 4.0} = 4.0
    // (upper median). Sanity-check the scale rather than pin exact layout.
    EXPECT_GE(rep.information.median_bits, 3.0);
    EXPECT_LE(rep.information.median_bits, 4.0);
}

TEST(NamingAnalyzer, InformationSkipsConstructorsAndDestructors) {
    auto table = SynonymTable::build_default();
    // 8 symbols share "file" so any file-token name would be vague, but the
    // ctor/dtor forms are language forms and must not rank.
    std::vector<EnhancedSymbol> syms;
    syms.push_back(make_ref_sym("FileStore::FileStore", 1, 1));
    syms.push_back(make_ref_sym("~FileStore", 1, 2));
    const char* rest[] = {"fileOpen", "fileClose", "fileRead",
                          "fileWrite", "fileSeek", "fileStat"};
    SymbolID id = 3;
    for (const char* w : rest) syms.push_back(make_ref_sym(w, 1, id++));
    std::vector<const EnhancedSymbol*> ptrs;
    for (auto& s : syms) ptrs.push_back(&s);
    auto f = make_file("api/f.go", ptrs);

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    for (const auto& v : rep.information.vague_names) {
        EXPECT_EQ(v.name.find("FileStore"), std::string::npos) << v.name;
    }
}

TEST(NamingAnalyzer, InformationObscureTokensStillReported) {
    auto table = SynonymTable::build_default();
    // "zxq" is gibberish in 2 symbols: below the corpus-vocabulary bar,
    // fails every dictionary -> obscure. It is NOT vague (highly selective).
    auto a = make_ref_sym("loadZxq", 1, 1);
    auto b = make_ref_sym("parseZxq", 1, 2);
    auto c = make_ref_sym("renderOutput", 1, 3);
    auto f = make_file("api/z.go", {&a, &b, &c});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_EQ(rep.information.nonword_tokens, 2);
    ASSERT_EQ(rep.information.top_nonwords.size(), 1u);
    EXPECT_EQ(rep.information.top_nonwords[0].first, "zxq");
    EXPECT_EQ(rep.information.top_nonwords[0].second, 2);
}

TEST(NamingAnalyzer, InitializerCalleeParsing) {
    using NA = NamingAnalyzer;
    EXPECT_EQ(NA::initializer_callee("auto cfg = load_config(path);", 6),
              "load_config");
    EXPECT_EQ(NA::initializer_callee("x := pkg.LoadConfig(p)", 1),
              "LoadConfig");
    EXPECT_EQ(NA::initializer_callee("auto p = obj->fetch_user(id);", 6),
              "fetch_user");
    EXPECT_EQ(NA::initializer_callee(
                  "auto u = std::make_unique<Foo>(1);", 6),
              "make_unique");
    EXPECT_EQ(NA::initializer_callee("int total = base;", 5), "");
    EXPECT_EQ(NA::initializer_callee("if (a == b(c)) {", 5), "");
    EXPECT_EQ(NA::initializer_callee("int x;", 5), "");
}

TEST(NamingAnalyzer, FidelityFlagsMismatchedInitializer) {
    auto table = SynonymTable::build_default();
    // tmp = load_config(): placeholder threw away an informative source
    // name -> mismatch. cfg = load_config(): abbreviation, and not a
    // placeholder anyway. user_record: role naming, never flagged.
    // size = tellg-style role naming is exercised implicitly: only
    // placeholder names are candidates.
    std::string content =
        "void setup() {\n"
        "    auto tmp = load_config(path);\n"
        "    auto cfg = load_config(path);\n"
        "    auto user_record = fetch_user(id);\n"
        "}\n";
    EnhancedSymbol tmp = make_sym("tmp", SymbolType::Variable, 1, 7);
    tmp.symbol.line = 2; tmp.symbol.end_line = 2; tmp.symbol.column = 10;
    EnhancedSymbol cfg = make_sym("cfg", SymbolType::Variable, 2, 7);
    cfg.symbol.line = 3; cfg.symbol.end_line = 3; cfg.symbol.column = 10;
    EnhancedSymbol ur = make_sym("user_record", SymbolType::Variable, 3, 7);
    ur.symbol.line = 4; ur.symbol.end_line = 4; ur.symbol.column = 10;
    auto f = make_file("api/setup.go", {&tmp, &cfg, &ur});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "",
                          [&](FileID) -> std::string_view { return content; });
    EXPECT_EQ(rep.fidelity.checked, 3);
    ASSERT_EQ(rep.fidelity.mismatched, 1);
    EXPECT_EQ(rep.fidelity.mismatches[0].var_name, "tmp");
    EXPECT_EQ(rep.fidelity.mismatches[0].source_name, "load_config");
}

TEST(NamingAnalyzer, SynonymSplitFlagsSameConceptDifferentSpelling) {
    auto table = SynonymTable::build_default();
    // fetchUser and loadUser canonicalize to the same concept (fetch/load are
    // members of the "get" group) with different spellings: a search for one
    // misses the other. parseDocument shares no concept and stays silent.
    auto fetch = make_ref_sym("fetchUser", 5, 1);
    auto load = make_ref_sym("loadUser", 2, 2);
    auto parse = make_ref_sym("parseDocument", 4, 3);
    auto f = make_file("api/user.go", {&fetch, &load, &parse});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    ASSERT_EQ(rep.synonym_splits.size(), 1u);
    const auto& sp = rep.synonym_splits[0];
    EXPECT_EQ(sp.canonical, "get_user");
    ASSERT_EQ(sp.members.size(), 2u);
    // Members ranked by fan-in.
    EXPECT_EQ(sp.members[0].name, "fetchUser");
    EXPECT_EQ(sp.members[1].name, "loadUser");
    EXPECT_EQ(sp.total_fan_in, 7);
}

TEST(NamingAnalyzer, SynonymSplitIgnoresStyleOnlyDifference) {
    auto table = SynonymTable::build_default();
    // get_user vs getUser: identical token sequence, only casing differs.
    // That is the convention-mismatch axis, not a vocabulary split.
    auto snake = make_ref_sym("get_user", 3, 1);
    auto camel = make_ref_sym("getUser", 3, 2);
    auto f = make_file("api/user.go", {&snake, &camel});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_TRUE(rep.synonym_splits.empty());
}

TEST(NamingAnalyzer, SynonymSplitIgnoresBareSingleTokenVerbs) {
    auto table = SynonymTable::build_default();
    // add vs push as whole names: per-structure container vocabulary
    // (set.add, queue.push), deliberately distinct — aliases_in_use covers
    // the repo-level lesson. Only multi-token names can split.
    auto add = make_ref_sym("add", 9, 1);
    auto push = make_ref_sym("push", 7, 2);
    auto f = make_file("core/containers.go", {&add, &push});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_TRUE(rep.synonym_splits.empty());
}

TEST(NamingAnalyzer, SynonymSplitIgnoresSingleSpelling) {
    auto table = SynonymTable::build_default();
    // The same spelling at two sites is the ambiguity axis, never a split.
    auto a = make_ref_sym("fetchUser", 3, 1);
    auto b = make_ref_sym("fetchUser", 3, 2);
    auto f = make_file("api/user.go", {&a, &b});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_TRUE(rep.synonym_splits.empty());
}

TEST(NamingAnalyzer, CommonWordRecognized) {
    EXPECT_TRUE(NamingAnalyzer::is_common_word("handler"));
    EXPECT_TRUE(NamingAnalyzer::is_common_word("logf"));
    EXPECT_FALSE(NamingAnalyzer::is_common_word("frobnicate"));
}

namespace {
// A symbol with synthetic fan-in and explicit exported flag.
EnhancedSymbol make_ref_sym_exported(std::string name, int fan_in, SymbolID id,
                                     bool exported) {
    EnhancedSymbol es = make_ref_sym(std::move(name), fan_in, id);
    es.is_exported = exported;
    return es;
}

const VocabularyOutlier* find_outlier(const NamingReport& rep,
                                      std::string_view name) {
    for (const auto& o : rep.outliers) {
        if (o.name == name) return &o;
    }
    return nullptr;
}
}  // namespace

// --- Misspelling detection (judge criteria: nullifyMisingField,
// SupressNotFound, isSeperatorRune, marhshalWithoutEscape) -------------------

TEST(NamingAnalyzer, FlagsMisspellingAgainstCorpusVocabulary) {
    // "missing" is corpus-frequent (3 distinct symbols); "mising" is a rare
    // edit-distance-1 lone spelling of it -> misspelling, suggest "missing".
    auto table = SynonymTable::build_default();
    auto a = make_ref_sym("checkMissingField", 0, 1);
    auto b = make_ref_sym("hasMissingValue", 0, 2);
    auto c = make_ref_sym("missingKeys", 0, 3);
    auto bad = make_ref_sym("nullifyMisingField", 8, 4);
    auto f = make_file("core/record_field_resolver_runner.go",
                       {&a, &b, &c, &bad});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    const auto* o = find_outlier(rep, "nullifyMisingField");
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->reason, "misspelling");
    EXPECT_EQ(o->odd_term, "mising");
    ASSERT_FALSE(o->suggested.empty());
    EXPECT_EQ(o->suggested[0], "missing");
}

TEST(NamingAnalyzer, FlagsMisspelledExportedSymbolDespiteLowFanIn) {
    // SupressNotFound: exported but zero fan-in — exported symbols are part of
    // the API surface, importance gate must admit them.
    auto table = SynonymTable::build_default();
    auto bad = make_ref_sym_exported("SupressNotFound", 0, 1, true);
    auto f = make_file("middleware/supress_notfound.go", {&bad});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    const auto* o = find_outlier(rep, "SupressNotFound");
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->reason, "misspelling");
    EXPECT_EQ(o->odd_term, "supress");
    ASSERT_FALSE(o->suggested.empty());
    EXPECT_EQ(o->suggested[0], "suppress");
}

TEST(NamingAnalyzer, FlagsSeperatorMisspelling) {
    auto table = SynonymTable::build_default();
    auto bad = make_ref_sym("isSeperatorRune", 3, 1);
    auto f = make_file("tools/tokenizer/tokenizer.go", {&bad});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    const auto* o = find_outlier(rep, "isSeperatorRune");
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->reason, "misspelling");
    EXPECT_EQ(o->odd_term, "seperator");
    ASSERT_FALSE(o->suggested.empty());
    EXPECT_EQ(o->suggested[0], "separator");
}

TEST(NamingAnalyzer, MarshalMisspellingLabeledMisspellingNotUnknownVerb) {
    // Previously caught but mislabeled unknown-verb; must classify as a
    // misspelling of "marshal" with the correction suggested.
    auto table = SynonymTable::build_default();
    auto bad = make_ref_sym("marhshalWithoutEscape", 3, 1);
    auto f = make_file("encoding/json.go", {&bad});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    const auto* o = find_outlier(rep, "marhshalWithoutEscape");
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->reason, "misspelling");
    EXPECT_EQ(o->odd_term, "marhshal");
    ASSERT_FALSE(o->suggested.empty());
    EXPECT_EQ(o->suggested[0], "marshal");
}

// --- Convention mixing (guzzle add_* family shape) --------------------------

TEST(NamingAnalyzer, ConventionMismatchFlagsMinoritySnakeCase) {
    auto table = SynonymTable::build_default();
    std::vector<EnhancedSymbol> camel;
    const char* camel_names[] = {"createStream",   "resolveHeaders",
                                 "applyDecoder",   "checkDecode",
                                 "validateVerify", "invokeStats",
                                 "buildRequest",   "startTimer"};
    SymbolID id = 1;
    for (const char* n : camel_names) {
        camel.push_back(make_ref_sym_exported(n, 0, id++, true));
    }
    auto s1 = make_ref_sym_exported("add_proxy", 0, id++, true);
    auto s2 = make_ref_sym_exported("add_timeout", 0, id++, true);
    auto s3 = make_ref_sym_exported("add_headers", 0, id++, true);
    std::vector<const EnhancedSymbol*> ptrs;
    for (const auto& s : camel) ptrs.push_back(&s);
    ptrs.push_back(&s1);
    ptrs.push_back(&s2);
    ptrs.push_back(&s3);
    auto f = make_file("src/Handler/StreamHandler.php", std::move(ptrs));

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    for (const char* n : {"add_proxy", "add_timeout", "add_headers"}) {
        const auto* o = find_outlier(rep, n);
        ASSERT_NE(o, nullptr) << n;
        EXPECT_EQ(o->reason, "convention-mismatch") << n;
        EXPECT_EQ(o->odd_term, "snake_case") << n;
    }
    // No camelCase member is flagged for convention.
    for (const char* n : camel_names) {
        const auto* o = find_outlier(rep, n);
        EXPECT_TRUE(o == nullptr || o->reason != "convention-mismatch") << n;
    }
}

TEST(NamingAnalyzer, NoConventionMismatchInConsistentFile) {
    auto table = SynonymTable::build_default();
    std::vector<EnhancedSymbol> syms;
    const char* names[] = {"add_proxy", "add_timeout", "check_stream",
                           "build_request", "start_timer", "stop_timer"};
    SymbolID id = 1;
    for (const char* n : names) {
        syms.push_back(make_ref_sym_exported(n, 0, id++, true));
    }
    std::vector<const EnhancedSymbol*> ptrs;
    for (const auto& s : syms) ptrs.push_back(&s);
    auto f = make_file("src/handler.py", std::move(ptrs));

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    for (const auto& o : rep.outliers) {
        EXPECT_NE(o.reason, "convention-mismatch") << o.name;
    }
}

// --- Anti-signal regressions (chi Use/Mount/Group/Tee, domain words) --------

TEST(NamingAnalyzer, CoreApiVerbsHighFanInNotFlagged) {
    auto table = SynonymTable::build_default();
    auto use = make_ref_sym_exported("Use", 40, 1, true);
    auto mount = make_ref_sym_exported("Mount", 25, 2, true);
    auto group = make_ref_sym_exported("Group", 18, 3, true);
    auto tee = make_ref_sym_exported("Tee", 12, 4, true);
    auto f = make_file("mux.go", {&use, &mount, &group, &tee});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_TRUE(rep.outliers.empty())
        << (rep.outliers.empty() ? "" : rep.outliers[0].name);
}

TEST(NamingAnalyzer, CommonEnglishDerivedWordsNotObscure) {
    auto table = SynonymTable::build_default();
    auto a = make_ref_sym_exported("seekableStream", 5, 1, true);
    auto b = make_ref_sym_exported("effectiveURL", 5, 2, true);
    auto c = make_ref_sym_exported("discardBody", 5, 3, true);
    auto f = make_file("stream.go", {&a, &b, &c});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    EXPECT_TRUE(rep.outliers.empty())
        << (rep.outliers.empty() ? "" : rep.outliers[0].name);
}

// --- Real English is never broken vocabulary (SCOWL dictionary gate) --------
// Field-run FPs (2026-08-26 four-repo rating): fail -> tail, constant ->
// content, external -> internal as "misspellings"; opacity, less, expect,
// matching, lenient, enclosed as "obscure-token". All are real English and
// must never be outliers.

TEST(EnglishWords, DictionaryAndMorphology) {
    using analysis::is_english_like_token;
    using analysis::is_english_word;
    // Exact SCOWL words.
    EXPECT_TRUE(is_english_word("fail"));
    EXPECT_TRUE(is_english_word("opacity"));
    EXPECT_TRUE(is_english_word("lenient"));
    EXPECT_TRUE(is_english_word("external"));
    // Derived forms: un+scoped (prefix), serializer -> serial (Porter2 stem).
    EXPECT_TRUE(is_english_like_token("unscoped"));
    EXPECT_TRUE(is_english_like_token("serializer"));
    EXPECT_TRUE(is_english_like_token("deserializer"));
    // Genuine misspellings and jargon stay out.
    EXPECT_FALSE(is_english_like_token("mising"));
    EXPECT_FALSE(is_english_like_token("supress"));
    EXPECT_FALSE(is_english_like_token("seperator"));
    EXPECT_FALSE(is_english_like_token("errf"));
    EXPECT_FALSE(is_english_like_token("mpsc"));
}

TEST(NamingAnalyzer, RealEnglishTokenIsNeverAMisspelling) {
    // "tail" is corpus-frequent; "fail" is a rare lone token at distance 1.
    // Both are real English — fail must NOT be corrected to tail.
    auto table = SynonymTable::build_default();
    auto a = make_ref_sym("readTailBytes", 3, 1);
    auto b = make_ref_sym("tailWindow", 3, 2);
    auto c = make_ref_sym("tailChunk", 3, 3);
    auto bad = make_ref_sym_exported("failGate", 27, 4, true);
    auto f = make_file("bench/bench_gate.py", {&a, &b, &c, &bad});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    const auto* o = find_outlier(rep, "failGate");
    if (o != nullptr) {
        EXPECT_NE(o->reason, "misspelling") << o->odd_term;
    }
}

TEST(NamingAnalyzer, RealEnglishRareTokensAreNotObscure) {
    // Corpus-rare but real English words (the agnt/slop field-run FPs).
    auto table = SynonymTable::build_default();
    auto a = make_ref_sym_exported("opacityValue", 35, 1, true);
    auto b = make_ref_sym_exported("addLenientTool", 24, 2, true);
    auto c = make_ref_sym_exported("NewEnclosedScope", 8, 3, true);
    auto d = make_ref_sym_exported("unscopedQuery", 28, 4, true);
    auto e = make_ref_sym_exported("NewSerializer", 19, 5, true);
    auto f = make_file("scope.go", {&a, &b, &c, &d, &e});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    for (const auto& o : rep.outliers) {
        EXPECT_NE(o.reason, "obscure-token") << o.name << " / " << o.odd_term;
        EXPECT_NE(o.reason, "misspelling") << o.name << " / " << o.odd_term;
    }
}

// --- PHP triad: magic methods + repo-level convention (re-panel R4) ---------

TEST(NamingAnalyzer, MagicMethodsAreNeverOutliers) {
    // __construct/__call/__destruct are language-mandated; flagging them as
    // snake_case convention breaks was 14/15 of guzzle's outliers.
    auto table = SynonymTable::build_default();
    auto a = make_ref_sym_exported("__construct", 12, 1, true);
    auto b = make_ref_sym_exported("__call", 8, 2, true);
    auto c = make_ref_sym_exported("__destruct", 5, 3, true);
    auto d = make_ref_sym_exported("sendAsync", 9, 4, true);
    auto e = make_ref_sym_exported("requestAsync", 9, 5, true);
    auto g = make_ref_sym_exported("buildUri", 9, 6, true);
    auto h = make_ref_sym_exported("applyOptions", 9, 7, true);
    auto f = make_file("src/Client.php", {&a, &b, &c, &d, &e, &g, &h});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    for (const auto& o : rep.outliers) {
        EXPECT_TRUE(o.name.rfind("__", 0) != 0) << o.name << " " << o.reason;
    }
}

TEST(NamingAnalyzer, RepoLevelConventionCatchesMixedFile) {
    // StreamHandler shape: a file with ~equal snake/camel defeats the
    // file-local 2x gate, but the repo (same language) is overwhelmingly
    // camelCase — snake_case members must still flag.
    auto table = SynonymTable::build_default();
    std::vector<EnhancedSymbol> mixed;
    SymbolID id = 1;
    for (const char* n : {"add_cert", "add_proxy", "add_timeout", "add_debug",
                          "add_verify", "parse_proxy"})
        mixed.push_back(make_ref_sym_exported(n, 4, id++, true));
    for (const char* n : {"invokeStats", "checkDecode", "createStream",
                          "resolveHost", "applyConfig"})
        mixed.push_back(make_ref_sym_exported(n, 4, id++, true));
    std::vector<const EnhancedSymbol*> mixed_ptrs;
    for (auto& s : mixed) mixed_ptrs.push_back(&s);
    auto f1 = make_file("src/StreamHandler.php", mixed_ptrs);

    // The rest of the repo: solidly camelCase PHP.
    std::vector<EnhancedSymbol> camel;
    for (const char* n : {"sendAsync", "requestAsync", "buildUri",
                          "applyOptions", "prepareDefaults", "invalidBody",
                          "transferStats", "resolveHandler", "createResponse",
                          "mapRequest", "mapResponse", "withOptions",
                          "getConfig", "setConfig", "readTimeout",
                          "writeTimeout", "connectTimeout", "proxyHost",
                          "verifyPeer", "streamBody"})
        camel.push_back(make_ref_sym_exported(n, 4, id++, true));
    std::vector<const EnhancedSymbol*> camel_ptrs;
    for (auto& s : camel) camel_ptrs.push_back(&s);
    auto f2 = make_file("src/Client.php", camel_ptrs);

    NamingAnalyzer na;
    auto rep = na.analyze({f1, f2}, table, "");
    bool snake_flagged = false;
    for (const auto& o : rep.outliers) {
        if (o.reason == "convention-mismatch" &&
            o.name.rfind("add_", 0) == 0)
            snake_flagged = true;
        // The majority style never flags via the repo fallback.
        EXPECT_FALSE(o.reason == "convention-mismatch" &&
                     o.odd_term == "camelCase")
            << o.name;
    }
    EXPECT_TRUE(snake_flagged);
}

TEST(NamingAnalyzer, AcronymsAreNotObscureTokens) {
    // PKCE/HMAC-class acronyms — either list-known or spelled ALL-CAPS in
    // the symbol name — are deliberate vocabulary, not jargon.
    auto table = SynonymTable::build_default();
    auto a = make_ref_sym_exported("generatePKCE", 8, 1, true);
    auto b = make_ref_sym_exported("hmacSHA256", 8, 2, true);
    auto c = make_ref_sym_exported("parseNoProxyCidrRule", 8, 3, true);
    auto d = make_ref_sym_exported("signHKDF", 8, 4, true);  // ALL-CAPS spell
    auto f = make_file("auth.go", {&a, &b, &c, &d});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    for (const auto& o : rep.outliers) {
        EXPECT_NE(o.reason, "obscure-token") << o.name << " / " << o.odd_term;
    }
}

// --- Location must always carry the filename --------------------------------

TEST(NamingAnalyzer, LocationIncludesFilenameForEveryOutlier) {
    // Regression: every symbol after the first in a file lost its basename
    // ("(:220)"), because the per-file basename was moved out on first use.
    auto table = SynonymTable::build_default();
    auto o1 = make_ref_sym("frobnicate", 3, 1);
    auto o2 = make_ref_sym("zorble", 3, 2);
    auto f = make_file("api/legacy.go", {&o1, &o2});

    NamingAnalyzer na;
    auto rep = na.analyze({f}, table, "");
    ASSERT_EQ(rep.outliers.size(), 2u);
    for (const auto& o : rep.outliers) {
        EXPECT_EQ(o.location.rfind("legacy.go:", 0), 0u) << o.location;
    }
}

TEST(SynonymTable, PrimaryOfReturnsGroupRepresentative) {
    auto table = SynonymTable::build_default();
    EXPECT_EQ(table.primary_of("explode"), "split");
    EXPECT_EQ(table.primary_of("implode"), "join");
    EXPECT_EQ(table.primary_of("fetch"), "get");
    EXPECT_TRUE(table.primary_of("frobnicate").empty());
}

// ===========================================================================
// ScopeSet - selection algebra + populators
// ===========================================================================

TEST(ScopeSet, DefaultMatchesNothingAllMatchesEverything) {
    ScopeSet none = ScopeSet::none();
    EXPECT_FALSE(none.contains_file("a.go"));
    EXPECT_TRUE(none.empty());

    ScopeSet all = ScopeSet::all();
    EXPECT_TRUE(all.contains_file("a.go"));
    EXPECT_TRUE(all.contains_lines("a.go", 1, 5));
}

TEST(ScopeSet, LineRangesMergeAndOverlap) {
    ScopeSet s;
    s.add_lines("a.go", {10, 20});
    s.add_lines("a.go", {21, 30});  // adjacent -> merged
    s.add_lines("a.go", {50, 60});
    EXPECT_TRUE(s.contains_lines("a.go", 5, 10));    // touches 10
    EXPECT_TRUE(s.contains_lines("a.go", 25, 26));   // inside merged
    EXPECT_FALSE(s.contains_lines("a.go", 31, 49));  // gap
    EXPECT_FALSE(s.contains_lines("b.go", 10, 20));  // other file
    EXPECT_TRUE(s.contains_file("a.go"));
}

TEST(ScopeSet, WholeFileAbsorbsRanges) {
    ScopeSet s;
    s.add_lines("a.go", {10, 20});
    s.add_file("a.go");
    EXPECT_TRUE(s.contains_lines("a.go", 1000, 1000));
    s.add_lines("a.go", {1, 2});  // must stay whole-file
    EXPECT_TRUE(s.contains_lines("a.go", 500, 500));
}

TEST(ScopeSet, IntersectKeepsRangeOverlapsOnly) {
    ScopeSet a;
    a.add_lines("f.go", {10, 30});
    a.add_file("whole.go");
    ScopeSet b;
    b.add_lines("f.go", {25, 40});
    b.add_lines("whole.go", {5, 6});
    b.add_file("only-b.go");

    ScopeSet i = a.intersect(b);
    EXPECT_TRUE(i.contains_lines("f.go", 25, 30));
    EXPECT_FALSE(i.contains_lines("f.go", 10, 24));
    EXPECT_TRUE(i.contains_lines("whole.go", 5, 6));
    EXPECT_FALSE(i.contains_lines("whole.go", 7, 9));
    EXPECT_FALSE(i.contains_file("only-b.go"));

    // all() is the identity.
    ScopeSet j = ScopeSet::all().intersect(b);
    EXPECT_TRUE(j.contains_file("only-b.go"));
}

TEST(ScopeSet, UniteMergesFilesAndRanges) {
    ScopeSet a;
    a.add_lines("f.go", {1, 5});
    ScopeSet b;
    b.add_lines("f.go", {100, 110});
    b.add_file("g.go");
    ScopeSet u = a.unite(b);
    EXPECT_TRUE(u.contains_lines("f.go", 1, 1));
    EXPECT_TRUE(u.contains_lines("f.go", 105, 105));
    EXPECT_TRUE(u.contains_file("g.go"));
    EXPECT_TRUE(ScopeSet::all().unite(a).is_all());
}

TEST(ScopeSet, PopulatorsGlobRegexSymbols) {
    std::vector<std::string> paths = {"src/a.go", "src/b.py", "docs/c.md"};
    auto g = scope_from_globs({"src/*.go"}, paths);
    EXPECT_TRUE(g.contains_file("src/a.go"));
    EXPECT_FALSE(g.contains_file("src/b.py"));

    std::string err;
    auto r = scope_from_regex(R"(\.(go|py)$)", paths, err);
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(r.contains_file("src/b.py"));
    EXPECT_FALSE(r.contains_file("docs/c.md"));
    auto bad = scope_from_regex("([", paths, err);
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(bad.empty());

    EnhancedSymbol sym = make_sym("f", SymbolType::Function);
    sym.symbol.line = 40;
    sym.symbol.end_line = 60;
    auto ss = scope_from_symbols({{"src/a.go", &sym}});
    EXPECT_TRUE(ss.contains_lines("src/a.go", 55, 55));
    EXPECT_FALSE(ss.contains_lines("src/a.go", 61, 61));
    EXPECT_TRUE(ss.contains_symbol("src/a.go", sym));
}

TEST(ScopeSet, ParsesUnifiedDiffNewSideRanges) {
    const char* diff =
        "diff --git a/src/a.go b/src/a.go\n"
        "--- a/src/a.go\n"
        "+++ b/src/a.go\n"
        "@@ -10,2 +12,3 @@ func x() {\n"
        "+one\n+two\n+three\n"
        "@@ -40 +45 @@\n"
        "+line\n"
        "diff --git a/gone.go b/gone.go\n"
        "--- a/gone.go\n"
        "+++ /dev/null\n"
        "@@ -1,9 +0,0 @@\n"
        "diff --git a/del.go b/del.go\n"
        "--- a/del.go\n"
        "+++ b/del.go\n"
        "@@ -7,3 +6,0 @@\n";
    auto s = scope_from_unified_diff(diff);
    EXPECT_TRUE(s.contains_lines("src/a.go", 12, 12));
    EXPECT_TRUE(s.contains_lines("src/a.go", 14, 14));
    EXPECT_FALSE(s.contains_lines("src/a.go", 15, 20));
    EXPECT_TRUE(s.contains_lines("src/a.go", 45, 45));
    // Pure file deletion contributes nothing on the new side.
    EXPECT_FALSE(s.contains_file("gone.go"));
    // Pure hunk deletion anchors to the boundary line.
    EXPECT_TRUE(s.contains_lines("del.go", 6, 6));
}

}  // namespace
}  // namespace lci

