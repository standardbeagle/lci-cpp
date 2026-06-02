#include <lci/semantic/synonym_table.h>

#include <algorithm>
#include <cctype>

namespace lci {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

// Built-in curated dev-verb groups (design §2). Disjoint by construction:
// every word appears in exactly one group.
const std::vector<std::vector<std::string>>& default_groups() {
    static const std::vector<std::vector<std::string>> groups = {
        {"add", "insert", "append", "push"},
        {"delete", "remove", "erase", "destroy", "drop"},
        {"update", "modify", "edit", "upsert"},
        {"get", "fetch", "retrieve", "load", "read"},
        {"set", "store", "save", "write", "put"},
        {"login", "signin", "authenticate"},
        {"logout", "signout"},
        {"create", "make", "new", "build"},
        {"find", "search", "lookup", "query"},
        {"start", "begin", "init"},
        {"stop", "end", "halt", "terminate"},
        {"check", "validate", "verify"},
        {"parse", "decode", "deserialize"},
        {"encode", "serialize", "marshal"},
        {"connect", "open"},
        {"disconnect", "close"},
        {"enable", "activate"},
        {"disable", "deactivate"},
        // Cross-language aliases for common operations whose names vary by
        // ecosystem. These cut wasted searches: e.g. PHP's `explode`/`implode`
        // are string split/join. Disjoint from the groups above.
        {"split", "explode", "tokenize", "divide"},
        {"join", "implode", "concat", "concatenate"},
        {"map", "transform", "convert", "cast"},
        {"filter", "select", "reject"},
        {"reduce", "fold", "aggregate", "accumulate"},
        {"list", "enumerate", "glob", "scan"},
        {"copy", "clone", "duplicate", "dup"},
        {"format", "render", "stringify"},
        {"compare", "diff", "equals"},
        {"hash", "digest", "checksum"},
        {"send", "emit", "publish", "dispatch"},
        {"receive", "consume", "subscribe"},
        {"lock", "acquire"},
        {"unlock", "release"},
    };
    return groups;
}

// Index of the group currently containing `word` (lowercased), or npos.
constexpr std::size_t kNoGroup = static_cast<std::size_t>(-1);
std::size_t find_group(const std::vector<std::vector<std::string>>& groups,
                       std::string_view word) {
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        for (const auto& w : groups[gi]) {
            if (w == word) return gi;
        }
    }
    return kNoGroup;
}

}  // namespace

void SynonymTable::reindex() {
    word_to_group_.clear();
    others_.clear();
    word_to_group_.reserve(groups_.size() * 4);
    others_.reserve(groups_.size() * 4);

    for (uint32_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& group = groups_[gi];
        for (const auto& w : group) {
            word_to_group_[w] = gi;
        }
        for (const auto& w : group) {
            std::vector<std::string> rest;
            rest.reserve(group.size() - 1);
            for (const auto& other : group) {
                if (other != w) rest.push_back(other);
            }
            others_.emplace(w, std::move(rest));
        }
    }
}

SynonymTable SynonymTable::build_default() {
    SynonymTable table;
    table.groups_ = default_groups();
    table.reindex();
    return table;
}

Result<SynonymTable> SynonymTable::build_from_ops(std::span<const SynonymOp> ops) {
    // Baseline: built-in groups, unless a leading clear-all replaces it with
    // empty. clear-all is only valid as the first op.
    std::vector<std::vector<std::string>> groups;
    std::size_t start = 0;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].kind == SynonymOp::Kind::ClearAll) {
            if (i != 0) {
                return make_config_error(
                    "synonyms", "clear-all",
                    "clear-all must be the first child of the synonyms block");
            }
            start = 1;  // baseline stays empty
        }
    }
    if (start == 0) {
        groups = default_groups();
    }

    // Words declared by `group` ops in THIS file. A word appearing in two
    // distinct group ops is ambiguous and rejected.
    absl::flat_hash_map<std::string, bool> declared;

    for (std::size_t i = start; i < ops.size(); ++i) {
        const auto& op = ops[i];
        switch (op.kind) {
            case SynonymOp::Kind::ClearAll:
                // Already validated as first op above; nothing to apply.
                break;

            case SynonymOp::Kind::Clear: {
                if (op.words.size() != 1) {
                    return make_config_error(
                        "synonyms", "clear",
                        "clear takes exactly one word");
                }
                std::string w = to_lower(trim(op.words[0]));
                if (w.empty()) {
                    return make_config_error("synonyms", "clear",
                                             "clear word must not be empty");
                }
                std::size_t gi = find_group(groups, w);
                if (gi != kNoGroup) {
                    groups.erase(groups.begin() +
                                 static_cast<std::ptrdiff_t>(gi));
                }
                break;
            }

            case SynonymOp::Kind::Group: {
                std::vector<std::string> members;
                members.reserve(op.words.size());
                for (const auto& raw : op.words) {
                    std::string w = to_lower(trim(raw));
                    if (w.empty()) {
                        return make_config_error(
                            "synonyms", "group",
                            "group words must not be empty or whitespace");
                    }
                    // Dedupe within the group, preserving order.
                    if (std::find(members.begin(), members.end(), w) ==
                        members.end()) {
                        members.push_back(std::move(w));
                    }
                }
                if (members.size() < 2) {
                    return make_config_error(
                        "synonyms", "group",
                        "a synonym group needs at least 2 distinct words");
                }
                for (const auto& w : members) {
                    if (declared.contains(w)) {
                        return make_config_error(
                            "synonyms", w,
                            "word '" + w +
                                "' appears in more than one synonym group");
                    }
                    declared.emplace(w, true);
                }
                // Override: any existing group containing a listed word is
                // removed first, preserving the disjointness invariant with
                // the KDL group winning.
                for (const auto& w : members) {
                    std::size_t gi = find_group(groups, w);
                    if (gi != kNoGroup) {
                        groups.erase(groups.begin() +
                                     static_cast<std::ptrdiff_t>(gi));
                    }
                }
                groups.push_back(std::move(members));
                break;
            }
        }
    }

    SynonymTable table;
    table.groups_ = std::move(groups);
    table.reindex();
    return table;
}

std::span<const std::string> SynonymTable::synonyms_of(
    std::string_view word) const {
    auto it = others_.find(word);
    if (it == others_.end()) return {};
    return std::span<const std::string>(it->second);
}

bool SynonymTable::in_same_group(std::string_view a, std::string_view b) const {
    if (a == b) return false;
    auto ia = word_to_group_.find(a);
    if (ia == word_to_group_.end()) return false;
    auto ib = word_to_group_.find(b);
    if (ib == word_to_group_.end()) return false;
    return ia->second == ib->second;
}

std::string_view SynonymTable::primary_of(std::string_view word) const {
    auto it = word_to_group_.find(word);
    if (it == word_to_group_.end()) return {};
    const auto& group = groups_[it->second];
    return group.empty() ? std::string_view{} : std::string_view(group.front());
}

}  // namespace lci
