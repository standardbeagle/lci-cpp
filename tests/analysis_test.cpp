#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <lci/analysis/coupling_analyzer.h>
#include <lci/analysis/feature_analyzer.h>
#include <lci/analysis/layer_analyzer.h>
#include <lci/analysis/module_analyzer.h>
#include <lci/analysis/naming_analyzer.h>
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
    auto result = ca.analyze({}, "/proj");
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
    Reference ref;
    ref.target_symbol = 2;
    s1.outgoing_refs.push_back(ref);

    auto f1 = make_file("/proj/pkg_a/a.go", {&s1});
    auto f2 = make_file("/proj/pkg_b/b.go", {&s2});

    CouplingAnalyzer ca;
    auto result = ca.analyze({f1, f2}, "/proj");

    EXPECT_EQ(result.coupling.efferent_coupling["pkg_a"], 1);
    EXPECT_EQ(result.coupling.afferent_coupling["pkg_b"], 1);
    EXPECT_GT(result.coupling.average_coupling, 0.0);
}

// ===========================================================================
// CouplingAnalyzer - self-references counted as internal
// ===========================================================================

TEST(CouplingAnalyzer, SelfRefsAreCohesive) {
    EnhancedSymbol s1 = make_sym("FuncA", SymbolType::Function, 1);
    EnhancedSymbol s2 = make_sym("FuncB", SymbolType::Function, 2);

    // Both in same package, s1 references s2
    Reference ref;
    ref.target_symbol = 2;
    s1.outgoing_refs.push_back(ref);

    auto f = make_file("/proj/pkg/a.go", {&s1, &s2});

    CouplingAnalyzer ca;
    auto result = ca.analyze({f}, "/proj");

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
        layers.push_back(al);
    }

    auto patterns = LayerAnalyzer::detect_patterns(layers);

    bool found_layered = false;
    for (const auto& p : patterns) {
        if (p.name == "Layered Architecture") found_layered = true;
    }
    EXPECT_TRUE(found_layered);
}

TEST(LayerAnalyzer, DetectMVCPattern) {
    std::vector<ArchitecturalLayer> layers;
    ArchitecturalLayer pres;
    pres.name = "Presentation Layer";
    ArchitecturalLayer app;
    app.name = "Application Layer";
    layers.push_back(pres);
    layers.push_back(app);

    auto patterns = LayerAnalyzer::detect_patterns(layers);

    bool found_mvc = false;
    for (const auto& p : patterns) {
        if (p.name == "MVC Pattern") found_mvc = true;
    }
    EXPECT_TRUE(found_mvc);
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
    auto result = la.analyze({});
    EXPECT_TRUE(result.layers.empty());
}

TEST(LayerAnalyzer, AnalyzeGroupsSymbols) {
    EnhancedSymbol s1 = make_sym("UserService", SymbolType::Class);
    EnhancedSymbol s2 = make_sym("UserModel", SymbolType::Class);
    EnhancedSymbol s3 = make_sym("renderPage", SymbolType::Function);

    auto f = make_file("src/app.go", {&s1, &s2, &s3});

    LayerAnalyzer la;
    auto result = la.analyze({f});

    EXPECT_GE(static_cast<int>(result.layers.size()), 2);
    EXPECT_FALSE(result.dependency_matrix.empty());
    EXPECT_FALSE(result.layer_metrics.empty());
}

TEST(LayerAnalyzer, AnalyzeViolationsMissingLayers) {
    EnhancedSymbol s1 = make_sym("helperFunc", SymbolType::Function);
    auto f = make_file("src/util.go", {&s1});

    LayerAnalyzer la;
    auto result = la.analyze({f});

    // Missing expected layers should generate violations
    EXPECT_GT(result.violation_count, 0);
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
    es.incoming_refs.resize(static_cast<size_t>(fan_in));
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

TEST(NamingAnalyzer, CommonWordRecognized) {
    EXPECT_TRUE(NamingAnalyzer::is_common_word("handler"));
    EXPECT_TRUE(NamingAnalyzer::is_common_word("logf"));
    EXPECT_FALSE(NamingAnalyzer::is_common_word("frobnicate"));
}

TEST(SynonymTable, PrimaryOfReturnsGroupRepresentative) {
    auto table = SynonymTable::build_default();
    EXPECT_EQ(table.primary_of("explode"), "split");
    EXPECT_EQ(table.primary_of("implode"), "join");
    EXPECT_EQ(table.primary_of("fetch"), "get");
    EXPECT_TRUE(table.primary_of("frobnicate").empty());
}

}  // namespace
}  // namespace lci
