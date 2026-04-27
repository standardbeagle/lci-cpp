#include "diff_engine/diff.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <sstream>

namespace lci::parity {

namespace {

void walk(const nlohmann::json& a, const nlohmann::json& b,
          const DiffOptions& opts, std::string path,
          DiffResult& r) {
    auto tier = classify_path(opts.tiers, normalize_indexes(path));

    // Ignore tier: skip outright.
    if (tier == FieldTier::Ignore) return;

    // Both null: equal.
    if (a.is_null() && b.is_null()) return;

    // Type mismatch always fails (unless ignored).
    if (a.type() != b.type()) {
        r.passed = false;
        r.reasons.push_back(path + ": type mismatch (" +
                            std::string(a.type_name()) + " vs " +
                            std::string(b.type_name()) + ")");
        return;
    }

    if (a.is_object()) {
        // Union of keys
        std::vector<std::string> keys;
        for (auto it = a.begin(); it != a.end(); ++it) keys.push_back(it.key());
        for (auto it = b.begin(); it != b.end(); ++it) {
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
                keys.push_back(it.key());
            }
        }
        for (const auto& k : keys) {
            std::string child = path.empty() ? k : path + "." + k;
            const auto& av = a.contains(k) ? a.at(k) : nlohmann::json();
            const auto& bv = b.contains(k) ? b.at(k) : nlohmann::json();
            walk(av, bv, opts, child, r);
        }
        return;
    }

    if (a.is_array()) {
        if (a.size() != b.size()) {
            r.passed = false;
            r.reasons.push_back(path + ": array length mismatch (" +
                                std::to_string(a.size()) + " vs " +
                                std::to_string(b.size()) + ")");
            return;
        }
        std::string child_path = path + "[]";
        for (size_t i = 0; i < a.size(); ++i) {
            walk(a[i], b[i], opts, child_path, r);
        }
        return;
    }

    // Leaf comparison per tier.
    switch (tier) {
        case FieldTier::Stable: {
            if (a != b) {
                r.passed = false;
                r.reasons.push_back(path + ": stable mismatch (" +
                                    a.dump() + " vs " + b.dump() + ")");
            }
            return;
        }
        case FieldTier::Ranked: {
            // Score must be number; absolute diff within tolerance.
            if (!a.is_number() || !b.is_number()) {
                r.passed = false;
                r.reasons.push_back(path + ": ranked tier expects number");
                return;
            }
            double da = a.get<double>(), db = b.get<double>();
            if (std::abs(da - db) > opts.score_abs) {
                r.passed = false;
                std::ostringstream os;
                os << path << ": ranked drift |" << da << " - " << db
                   << "| > " << opts.score_abs;
                r.reasons.push_back(os.str());
            }
            return;
        }
        case FieldTier::Timed: {
            for (const auto& v : {a, b}) {
                if (!v.is_number()) {
                    r.passed = false;
                    r.reasons.push_back(path + ": timed tier expects number");
                    return;
                }
                double dv = v.get<double>();
                if (dv < 0 || dv > static_cast<double>(opts.timed_max_ms)) {
                    r.passed = false;
                    r.reasons.push_back(path + ": timed out of range");
                    return;
                }
            }
            return;
        }
        case FieldTier::Id: {
            if (!a.is_string() || !b.is_string()) {
                r.passed = false;
                r.reasons.push_back(path + ": id tier expects string");
                return;
            }
            if (!opts.id_pattern.empty()) {
                std::regex re(opts.id_pattern);
                if (!std::regex_match(a.get<std::string>(), re) ||
                    !std::regex_match(b.get<std::string>(), re)) {
                    r.passed = false;
                    r.reasons.push_back(path + ": id format mismatch");
                }
            }
            return;
        }
        case FieldTier::Ignore:
            return;
    }
}

std::string make_unified_diff(const std::string& a, const std::string& b) {
    if (a == b) return "";
    std::ostringstream os;
    os << "--- go\n+++ cpp\n";
    std::vector<std::string> al, bl;
    std::istringstream as(a), bs(b);
    std::string line;
    while (std::getline(as, line)) al.push_back(line);
    while (std::getline(bs, line)) bl.push_back(line);
    size_t n = std::max(al.size(), bl.size());
    for (size_t i = 0; i < n; ++i) {
        const std::string& la = i < al.size() ? al[i] : "";
        const std::string& lb = i < bl.size() ? bl[i] : "";
        if (la == lb) {
            os << " " << la << "\n";
        } else {
            os << "-" << la << "\n+" << lb << "\n";
        }
    }
    return os.str();
}

} // namespace

DiffResult compare(const nlohmann::json& go,
                   const nlohmann::json& cpp,
                   const DiffOptions& opts) {
    DiffResult r;
    walk(go, cpp, opts, "", r);
    if (!r.passed) {
        r.unified_diff = make_unified_diff(go.dump(2), cpp.dump(2));
    }
    return r;
}

} // namespace lci::parity
