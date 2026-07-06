#include "runner/descriptor.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace lci::parity {

namespace {

Mode parse_mode(const std::string& s) {
    if (s == "cli")   return Mode::Cli;
    if (s == "mcp")   return Mode::Mcp;
    if (s == "http")  return Mode::Http;
    if (s == "index") return Mode::Index;
    throw std::runtime_error("invalid mode: " + s);
}

ParseStyle parse_style(const std::string& s) {
    if (s == "json")        return ParseStyle::Json;
    if (s == "text")        return ParseStyle::Text;
    if (s == "exit-only")   return ParseStyle::ExitOnly;
    throw std::runtime_error("invalid parse style: " + s);
}

std::vector<std::string> str_array(const nlohmann::json& j) {
    std::vector<std::string> out;
    for (const auto& e : j) out.push_back(e.get<std::string>());
    return out;
}

void load_tiers(const nlohmann::json& j, TierMap& m) {
    if (!j.is_object()) return;
    if (j.contains("stable")) m.stable = str_array(j.at("stable"));
    if (j.contains("ranked")) m.ranked = str_array(j.at("ranked"));
    if (j.contains("timed"))  m.timed  = str_array(j.at("timed"));
    if (j.contains("ids"))    m.ids    = str_array(j.at("ids"));
    if (j.contains("ignore")) m.ignore = str_array(j.at("ignore"));
    if (j.contains("sort_arrays")) m.sort_arrays = str_array(j.at("sort_arrays"));
}

void load_text_normalize(const nlohmann::json& j, DescriptorTextNormalize& tn) {
    if (!j.is_object()) return;
    tn.explicitly_set = true;
    if (j.contains("scrub_timing"))        tn.scrub_timing        = j.at("scrub_timing").get<bool>();
    if (j.contains("rewrite_corpus_path")) tn.rewrite_corpus_path = j.at("rewrite_corpus_path").get<bool>();
    if (j.contains("strip_emoji_prefix"))  tn.strip_emoji_prefix  = j.at("strip_emoji_prefix").get<bool>();
    if (j.contains("strip_lines"))         tn.strip_lines         = str_array(j.at("strip_lines"));
    if (j.contains("sort_lines"))          tn.sort_lines          = j.at("sort_lines").get<bool>();
    if (j.contains("collapse_blank_lines")) tn.collapse_blank_lines = j.at("collapse_blank_lines").get<bool>();
    if (j.contains("replace")) {
        const auto& arr = j.at("replace");
        if (!arr.is_array()) {
            throw std::runtime_error("text_normalize.replace must be array");
        }
        for (const auto& e : arr) {
            if (!e.is_object() || !e.contains("pattern") || !e.contains("with")) {
                throw std::runtime_error(
                    "text_normalize.replace entries must be {pattern, with}");
            }
            tn.replace.emplace_back(e.at("pattern").get<std::string>(),
                                    e.at("with").get<std::string>());
        }
    }
}

void require(const nlohmann::json& j, const std::string& key) {
    if (!j.contains(key)) {
        throw std::runtime_error("descriptor missing required field: " + key);
    }
}

} // namespace

Descriptor parse_descriptor(const std::string& json_text) {
    auto j = nlohmann::json::parse(json_text);
    require(j, "id");
    require(j, "mode");
    require(j, "corpus");
    require(j, "invocation");

    Descriptor d;
    d.id     = j.at("id").get<std::string>();
    d.mode   = parse_mode(j.at("mode").get<std::string>());
    d.corpus = j.at("corpus").get<std::string>();
    d.go_binary  = j.value("go_binary",  std::string("${LCI_GO}"));
    d.cpp_binary = j.value("cpp_binary", std::string("${LCI_CPP}"));

    const auto& inv = j.at("invocation");
    if (inv.contains("args"))  d.invocation.args = str_array(inv.at("args"));
    if (inv.contains("stdin") && inv.at("stdin").is_string()) {
        d.invocation.stdin_data = inv.at("stdin").get<std::string>();
    }
    if (inv.contains("env")) {
        for (auto it = inv.at("env").begin(); it != inv.at("env").end(); ++it) {
            d.invocation.env[it.key()] = it.value().get<std::string>();
        }
    }
    d.invocation.cwd = inv.value("cwd", std::string("${CORPUS}"));

    if (j.contains("capture")) d.capture = str_array(j.at("capture"));
    else d.capture = {"stdout", "exit"};

    d.parse = parse_style(j.value("parse", std::string("json")));

    if (j.contains("tiers")) load_tiers(j.at("tiers"), d.tiers);

    if (j.contains("tolerances")) {
        const auto& t = j.at("tolerances");
        d.tolerances.score_abs    = t.value("score_abs",    0.01);
        d.tolerances.timed_max_ms = t.value("timed_max_ms", 60000LL);
    }
    d.expect_exit = j.value("expect_exit", 0);
    d.id_pattern  = j.value("id_pattern",  std::string());
    d.wait_for_ready = j.value("wait_for_ready", false);
    d.git_fixture    = j.value("git_fixture", std::string());

    if (j.contains("text_normalize")) {
        load_text_normalize(j.at("text_normalize"), d.text_normalize);
    }
    return d;
}

} // namespace lci::parity
