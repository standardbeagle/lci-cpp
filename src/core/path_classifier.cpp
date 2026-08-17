#include <lci/path_classifier.h>

namespace lci {

PathClassifier::PathClassifier(std::vector<PathAttrRule> config_rules)
    : config_rules_(std::move(config_rules)) {}

PathAttr PathClassifier::classify(std::string_view /*rel_path*/) const {
    return PathAttr::Production;  // stub — RED
}

PathAttr PathClassifier::classify(std::string_view rel_path,
                                  std::string_view /*content*/) const {
    return classify(rel_path);
}

std::string_view PathClassifier::name(PathAttr attr) {
    switch (attr) {
        case PathAttr::Production: return "production";
        case PathAttr::Test: return "test";
        case PathAttr::Example: return "example";
        case PathAttr::Vendored: return "vendored";
        case PathAttr::Generated: return "generated";
        case PathAttr::Docs: return "docs";
    }
    return "production";
}

bool PathClassifier::parse(std::string_view n, PathAttr& out) {
    if (n == "production" || n == "code") { out = PathAttr::Production; return true; }
    if (n == "test") { out = PathAttr::Test; return true; }
    if (n == "example") { out = PathAttr::Example; return true; }
    if (n == "vendored") { out = PathAttr::Vendored; return true; }
    if (n == "generated") { out = PathAttr::Generated; return true; }
    if (n == "docs") { out = PathAttr::Docs; return true; }
    return false;
}

}  // namespace lci
