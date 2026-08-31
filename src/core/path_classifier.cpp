#include <lci/path_classifier.h>

#include <lci/kdl.h>

#include <algorithm>
#include <cstring>

namespace lci {

namespace {

// The ruleset lci ships with. Written in the same KDL a project writes, so
// what a reader sees here is exactly what a `.lci.kdl` can override, extend,
// or replace.
//
// `rank` decides precedence when several attributes match one path: a test
// file inside vendor/ is vendored, a generated file under tests/ is a test
// fixture. `activates` is what the attribute turns on — an attribute that
// does not activate "analysis" is excluded from code_insight's sections and
// reported as an exclusion instead of silently shrinking the corpus.
constexpr std::string_view kBuiltinAttributeRuleset = R"KDL(
attributes {
    // Third-party trees. Lowest rank: a vendored dependency's own tests are
    // still vendored code, and none of it is ours to analyze.
    vendored rank=0 {
        activates "index" "search"
        dir "vendor" "vendors" "node_modules" "third_party" "thirdparty"
        dir "bower_components" ".yarn"
        // C-project vendoring convention: redis carries jemalloc, lua and
        // hiredis under deps/, and jemalloc's handleOOM owned a finding.
        dir "deps"
        // Built/bundled front-end output. pocketbase's ui/public/libs
        // minified bundles used to own the worst-module error-handling score,
        // which says nothing about the code its authors wrote.
        dir "libs" "public"
        glob "*.min.js" "*.min.mjs" "*.min.css" "*.bundle.js"
        glob "*.iife.js" "*.umd.js"
        content "minified"
    }

    // Machine-written code. References resolve into it (the call sites are
    // real) but its style, error handling, and naming are an emitter's, not
    // this codebase's.
    generated rank=1 {
        activates "index" "search"
        dir "generated" "__generated__"
        glob "*_generated.*" "*.generated.*" "zz_generated*"
        glob "*.pb.go" "*.pb.cc" "*.pb.h" "*_pb2.py" "*_pb2_grpc.py"
        glob "*.g.dart" "*.g.cs" "*.d.ts"
        content "generated-header"
    }

    // Tests. Deliberately no "testing" directory rule — real packages are
    // named that.
    test rank=2 {
        activates "index" "search"
        dir "test" "tests" "__tests__" "testdata" "spec" "specs" "fixtures"
        dir "mock" "mocks" "fakes" "stubs"
        dir "testhelper" "testhelpers" "testutil" "testutils"
        glob "*_test.go" "*_test.py" "*_test.cc" "*_test.cpp" "*_test.rb"
        glob "*_spec.rb" "*_test.exs" "*.test.*" "*.spec.*"
        glob "*Test.php" "*Tests.php" "*TestCase.php"
        glob "*Test.java" "*Tests.java" "*Test.cs" "*Tests.cs" "*Test.kt"
        glob "conftest.py" "test_*.py"
        glob "*_mock.go" "mock_*.go" "*.mock.*"
        // Script-style test entry points: JS/TS has no leading-prefix
        // convention the way Python does, so a `test-resolution.ts` build
        // check reads as product code. zod's worst error-handling module was
        // exactly this file.
        glob "test-*.ts" "test-*.js" "test-*.mjs" "test_*.ts" "test_*.js"
        // C# convention: the test project is a sibling DIRECTORY named
        // *.Tests — Newtonsoft.Json.Tests supplied most of that repo's
        // error-handling findings while the shipped serializer was clean.
        dir "*.Tests" "*.Test"
        // Script-style test entry points: redis has modules/vector-sets/
        // test.py and utils/lru/test-lru.rb.
        glob "test.py" "test.rb" "test-*.py" "test-*.rb"
        // JVM/Gradle source-set and module LAYOUTS (okhttp audit): Kotlin
        // multiplatform <target>Test source sets, Gradle testFixtures, and
        // test-support / *-tests modules ship whole test trees whose files
        // carry no test suffix (AutobahnTester.kt, MockHttp2Peer.kt).
        // "*Test" as a DIR only — bare filenames like Contest.kt never hit
        // a dir rule, so production names containing "test" stay untouched.
        dir "jvmTest" "commonTest" "androidTest" "jsTest" "nativeTest"
        dir "iosTest" "jvmAndroidTest" "testFixtures"
        dir "*-tests" "*-testing-support" "testing-support"
    }

    // Benchmark harnesses and their tooling. Separate from tests because a
    // benchmark is not a correctness gate: a bench script swallowing an
    // exception is not a defect in the product, and counting it as one buries
    // the findings that are. No "benchmarking" rule, for the same reason
    // "testing" is absent above.
    benchmark rank=3 {
        activates "index" "search"
        dir "bench" "benchmark" "benchmarks"
        glob "*_bench.go" "*_bench.cc" "*_bench.cpp" "*_bench.rs" "*_bench.py"
        glob "*_benchmark.go" "*_benchmark.cc" "*_benchmark.cpp"
        glob "*_benchmark.py"
        // Dash spelling: redis tools/array-bench.py drove three exposure
        // paths. "workbench.py" must not match, hence the dash.
        glob "*-bench.py" "*-bench.js" "*-bench.ts" "*-bench.rb"
        glob "*-bench.go" "*-bench.cc" "*-bench.cpp" "*-bench.rs"
    }

    // Examples and demos. Go's toolchain ignores leading-underscore dirs
    // (chi's _examples), but a bare `_*` glob also swallowed Python's
    // production `_compat`/`_internal` packages (fastapi audit: the front
    // door could not rank because fastapi/_compat never entered analysis) —
    // so only the example-shaped underscore spellings match.
    example rank=4 {
        activates "index" "search"
        dir "example" "examples" "demo" "demos" "sample" "samples"
        dir "_example" "_examples" "_demo" "_demos" "_sample" "_samples"
        // Dev playgrounds: not shipped, not tested, and conventionally the
        // laxest code in a repo. axios's worst error-handling module was its
        // sandbox/ server.
        dir "sandbox" "sandboxes" "playground" "playgrounds" "scratch"
    }

    docs rank=5 {
        activates "index" "search"
        // docs_src: the MkDocs convention for runnable documentation
        // snippets (fastapi ships ~1000 tutorial files there; they were
        // its worst error-handling module).
        dir "doc" "docs" "docs_src" "doc_src"
        glob "*.md" "*.markdown" "*.rst" "*.adoc"
    }

    // The fallback: every file no rule claimed. Always attribute id 0.
    production rank=99 {
        activates "index" "search" "refs" "analysis"
    }
}
)KDL";

// Simple glob: '*' matches any run (including '/'), '?' any one char.
// Iterative backtracking — no allocation, no std::regex (karpathy table).
bool glob_match(std::string_view pat, std::string_view text) {
    size_t p = 0, t = 0, star_p = std::string_view::npos, star_t = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == text[t] || pat[p] == '?')) {
            ++p; ++t;
        } else if (p < pat.size() && pat[p] == '*') {
            star_p = p++;
            star_t = t;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

bool has_prefix(std::string_view s, std::string_view pre) {
    return s.compare(0, std::min(pre.size(), s.size()), pre) == 0 &&
           s.size() >= pre.size();
}

// True when a config shorthand pattern matches. Semantics (see header):
//   trailing '/'  -> directory prefix or interior segment sequence
//   contains '/'  -> glob over the whole relative path
//   otherwise     -> glob over the basename
bool shorthand_matches(std::string_view pattern, std::string_view rel_path,
                       std::string_view base) {
    if (pattern.empty()) return false;
    if (pattern.back() == '/') {
        std::string_view dir = pattern.substr(0, pattern.size() - 1);
        if (dir.empty()) return false;
        // Anchored prefix ("src/legacy_tests/") or any interior position
        // preceded by a '/' boundary.
        if (has_prefix(rel_path, dir) && rel_path.size() > dir.size() &&
            rel_path[dir.size()] == '/') {
            return true;
        }
        // Interior: "/<dir>/" occurring anywhere.
        std::string needle;
        needle.reserve(dir.size() + 2);
        needle.push_back('/');
        needle.append(dir);
        needle.push_back('/');
        return rel_path.find(needle) != std::string_view::npos;
    }
    if (pattern.find('/') != std::string_view::npos) {
        return glob_match(pattern, rel_path);
    }
    return glob_match(pattern, base);
}

// Does any `dir` pattern of `def` match a path SEGMENT? The basename is
// excluded — that is what `glob` is for.
bool dirs_match(const AttrDef& def, std::string_view rel_path) {
    if (def.dirs.empty()) return false;
    size_t start = 0;
    while (start < rel_path.size()) {
        auto end = rel_path.find('/', start);
        if (end == std::string_view::npos) break;  // basename
        std::string_view seg = rel_path.substr(start, end - start);
        for (const auto& pat : def.dirs) {
            if (glob_match(pat, seg)) return true;
        }
        start = end + 1;
    }
    return false;
}

bool globs_match(const AttrDef& def, std::string_view rel_path,
                 std::string_view base) {
    for (const auto& pat : def.globs) {
        std::string_view text =
            pat.find('/') != std::string::npos ? rel_path : base;
        if (glob_match(pat, text)) return true;
    }
    return false;
}

// Content sniff: generated-file header convention within the first 512 bytes
// ("Code generated by X. DO NOT EDIT." — Go; "@generated" — JS/facebook).
bool has_generated_header(std::string_view content) {
    std::string_view head =
        content.substr(0, std::min<size_t>(content.size(), 512));
    return head.find("Code generated") != std::string_view::npos ||
           head.find("DO NOT EDIT") != std::string_view::npos ||
           head.find("@generated") != std::string_view::npos;
}

// Content sniff: minified asset — large payload with almost no newlines.
bool looks_minified(std::string_view content) {
    if (content.size() < 4096) return false;
    size_t sample = std::min<size_t>(content.size(), 64 * 1024);
    size_t newlines = 0;
    const char* p = content.data();
    const char* end = p + sample;
    while ((p = static_cast<const char*>(
                memchr(p, '\n', static_cast<size_t>(end - p)))) != nullptr) {
        ++newlines;
        ++p;
        if (p >= end) break;
    }
    // Average line length over the sample; minifiers emit >300-char lines.
    return sample / (newlines + 1) > 300;
}

bool content_matches(const AttrDef& def, std::string_view content) {
    for (const auto& id : def.contents) {
        if (id == "minified" && looks_minified(content)) return true;
        if (id == "generated-header" && has_generated_header(content)) {
            return true;
        }
    }
    return false;
}

constexpr uint8_t cap_bit(Capability cap) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(cap));
}

// What an attribute activates when it says nothing. Silence is not "activate
// nothing" — that would drop the files out of the index entirely. A newly
// declared, tagged tree is indexed, searchable, and a valid reference target,
// but out of analysis, matching every shipped non-production attribute.
constexpr uint8_t kDefaultCapabilities =
    cap_bit(Capability::Index) | cap_bit(Capability::Search) |
    cap_bit(Capability::Refs);

}  // namespace

// -- Capabilities --------------------------------------------------------------

std::string_view capability_name(Capability cap) {
    switch (cap) {
        case Capability::Index: return "index";
        case Capability::Search: return "search";
        case Capability::Refs: return "refs";
        case Capability::Analysis: return "analysis";
    }
    return "index";
}

bool parse_capability(std::string_view name, Capability& out) {
    if (name == "index") { out = Capability::Index; return true; }
    if (name == "search") { out = Capability::Search; return true; }
    if (name == "refs") { out = Capability::Refs; return true; }
    if (name == "analysis") { out = Capability::Analysis; return true; }
    return false;
}

// -- Ruleset parsing -----------------------------------------------------------

bool parse_attributes_block(std::string_view kdl_source,
                            std::vector<AttrDef>& defs_out,
                            std::vector<PathAttrRule>& rules_out,
                            std::string& error) {
    std::string parse_error;
    auto nodes = kdl::parse(kdl_source, parse_error);
    if (!parse_error.empty()) {
        error = parse_error;
        return false;
    }
    for (const auto& node : nodes) {
        if (node.name != "attributes") continue;
        for (const auto& attr : node.children) {
            // Shorthand — `test "src/legacy/"` — adds patterns to an
            // attribute without redefining it. This is the historical config
            // syntax and stays valid.
            for (const auto& pattern : attr.string_args()) {
                if (pattern.empty()) {
                    error = "attributes: attribute '" + attr.name +
                            "' has an empty pattern";
                    return false;
                }
                rules_out.push_back({attr.name, pattern});
            }
            if (attr.children.empty()) {
                if (attr.string_args().empty()) {
                    error = "attributes: attribute '" + attr.name +
                            "' declares neither patterns nor a definition"
                            " block";
                    return false;
                }
                continue;
            }
            // Full form — a definition block.
            AttrDef def;
            def.name = attr.name;
            def.rank = attr.int_prop("rank", 50);
            bool saw_activates = false;
            for (const auto& child : attr.children) {
                if (child.name == "activates") {
                    saw_activates = true;
                    for (const auto& c : child.string_args()) {
                        Capability cap{};
                        if (!parse_capability(c, cap)) {
                            error = "attributes: attribute '" + attr.name +
                                    "' activates unknown capability '" + c +
                                    "' (valid: index, search, refs, analysis)";
                            return false;
                        }
                        def.capabilities |= cap_bit(cap);
                    }
                } else if (child.name == "dir") {
                    for (auto& s : child.string_args()) {
                        def.dirs.push_back(std::move(s));
                    }
                } else if (child.name == "glob") {
                    for (auto& s : child.string_args()) {
                        def.globs.push_back(std::move(s));
                    }
                } else if (child.name == "content") {
                    for (auto& s : child.string_args()) {
                        if (s != "minified" && s != "generated-header") {
                            error = "attributes: attribute '" + attr.name +
                                    "' names unknown content heuristic '" + s +
                                    "' (valid: minified, generated-header)";
                            return false;
                        }
                        def.contents.push_back(std::move(s));
                    }
                } else {
                    error = "attributes: attribute '" + attr.name +
                            "' has unknown key '" + child.name +
                            "' (valid: activates, dir, glob, content)";
                    return false;
                }
            }
            // Silence means the defaults; an explicit `activates` with
            // nothing listed means exactly nothing, which is how a project
            // says "do not index this tree at all".
            if (!saw_activates) def.capabilities = kDefaultCapabilities;
            defs_out.push_back(std::move(def));
        }
    }
    return true;
}

// -- Registry ------------------------------------------------------------------

void PathAttrRegistry::finalize() {
    by_rank_.clear();
    by_rank_.reserve(defs_.size());
    for (size_t i = 0; i < defs_.size(); ++i) {
        by_rank_.push_back(static_cast<PathAttrId>(i));
    }
    // Rank, then name: two attributes at the same rank must still classify
    // the same way on every machine (karpathy #4).
    std::sort(by_rank_.begin(), by_rank_.end(),
              [this](PathAttrId a, PathAttrId b) {
                  if (defs_[a].rank != defs_[b].rank) {
                      return defs_[a].rank < defs_[b].rank;
                  }
                  return defs_[a].name < defs_[b].name;
              });
}

bool PathAttrRegistry::find(std::string_view name, PathAttrId& out) const {
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].name == name) {
            out = static_cast<PathAttrId>(i);
            return true;
        }
    }
    return false;
}

std::vector<PathAttrId> PathAttrRegistry::with_capability(
    Capability cap) const {
    std::vector<PathAttrId> out;
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].activates(cap)) out.push_back(static_cast<PathAttrId>(i));
    }
    return out;
}

const PathAttrRegistry& PathAttrRegistry::builtin() {
    static const PathAttrRegistry* registry = [] {
        auto* r = new PathAttrRegistry();
        std::vector<AttrDef> defs;
        std::vector<PathAttrRule> rules;
        std::string error;
        // A broken shipped ruleset is a build defect, not a runtime
        // condition. The synthetic fallback below keeps the binary usable
        // (everything classifies as production) while
        // PathClassifierTest.ShippedRulesetParses fails loudly in CI.
        if (parse_attributes_block(kBuiltinAttributeRuleset, defs, rules,
                                   error)) {
            // The fallback attribute goes first so it holds id 0.
            for (auto& d : defs) {
                if (d.name == "production") r->defs_.push_back(std::move(d));
            }
            for (auto& d : defs) {
                if (!d.name.empty() && d.name != "production") {
                    r->defs_.push_back(std::move(d));
                }
            }
        }
        if (r->defs_.empty() || r->defs_[kFallbackAttr].name != "production") {
            r->defs_.clear();
            AttrDef fallback;
            fallback.name = "production";
            fallback.rank = 99;
            fallback.capabilities = cap_bit(Capability::Index) |
                                    cap_bit(Capability::Search) |
                                    cap_bit(Capability::Refs) |
                                    cap_bit(Capability::Analysis);
            r->defs_.push_back(std::move(fallback));
        }
        r->finalize();
        return r;
    }();
    return *registry;
}

PathAttrRegistry PathAttrRegistry::with_config(
    const std::vector<AttrDef>& config_attrs,
    const std::vector<PathAttrRule>& config_rules, std::string& error) {

    PathAttrRegistry reg = builtin();  // the shipped ruleset is the base
    for (const auto& incoming : config_attrs) {
        if (incoming.name.empty()) {
            error = "attributes: an attribute needs a name";
            return builtin();
        }
        PathAttrId existing{};
        if (reg.find(incoming.name, existing)) {
            // Redefining a shipped attribute REPLACES its patterns and
            // capabilities. A project that writes the block means it, and
            // merging would leave shipped patterns in force invisibly.
            AttrDef& target = reg.defs_[existing];
            target.rank = incoming.rank;
            target.capabilities = incoming.capabilities;
            target.dirs = incoming.dirs;
            target.globs = incoming.globs;
            target.contents = incoming.contents;
        } else {
            reg.defs_.push_back(incoming);
        }
    }
    for (const auto& rule : config_rules) {
        PathAttrId id{};
        if (!reg.find(rule.attr, id)) {
            // A pattern naming an attribute nobody defined declares it, with
            // the same defaults an undecorated definition block would get.
            AttrDef def;
            def.name = rule.attr;
            def.capabilities = kDefaultCapabilities;
            reg.defs_.push_back(std::move(def));
            id = static_cast<PathAttrId>(reg.defs_.size() - 1);
        }
        reg.config_rules_.emplace_back(id, rule.pattern);
    }
    reg.finalize();
    return reg;
}

// -- Classifier ----------------------------------------------------------------

PathClassifier::PathClassifier() : registry_(&PathAttrRegistry::builtin()) {}

PathClassifier::PathClassifier(const PathAttrRegistry& registry)
    : registry_(&registry) {}

PathAttrId PathClassifier::classify_path(std::string_view rel_path,
                                        bool& from_config) const {
    from_config = false;
    auto slash = rel_path.rfind('/');
    std::string_view base = slash == std::string_view::npos
                                ? rel_path
                                : rel_path.substr(slash + 1);

    // Config patterns first, declaration order, first match wins — including
    // one naming the fallback attribute, which un-tags a shipped match.
    for (const auto& [id, pattern] : registry_->config_rules()) {
        if (shorthand_matches(pattern, rel_path, base)) {
            from_config = true;
            return id;
        }
    }

    // Shipped patterns, lowest rank first.
    for (PathAttrId id : registry_->by_rank()) {
        const AttrDef& def = registry_->def(id);
        if (dirs_match(def, rel_path) || globs_match(def, rel_path, base)) {
            return id;
        }
    }
    return kFallbackAttr;
}

PathAttrId PathClassifier::classify(std::string_view rel_path) const {
    bool from_config = false;
    return classify_path(rel_path, from_config);
}

PathAttrId PathClassifier::classify(std::string_view rel_path,
                                    std::string_view content) const {
    bool from_config = false;
    PathAttrId attr = classify_path(rel_path, from_config);
    // An explicit config pattern is authoritative over any content sniff —
    // `production "big/blob.js"` is how a project says "yes, it is minified,
    // and it is still ours". Otherwise heuristics only promote a file that no
    // pattern claimed.
    if (from_config || attr != kFallbackAttr || content.empty()) return attr;
    // Rank order, so "minified" (vendored, rank 0) wins over a later
    // heuristic on the same content.
    for (PathAttrId id : registry_->by_rank()) {
        if (content_matches(registry_->def(id), content)) return id;
    }
    return attr;
}

}  // namespace lci
