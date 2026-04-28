#include "runner/modes/subst.h"

namespace lci::parity {

std::string substitute(const std::string& s, const std::string& corpus_path) {
    std::string out = s;
    auto replace = [&](const std::string& token, const std::string& with) {
        size_t pos = 0;
        while ((pos = out.find(token, pos)) != std::string::npos) {
            out.replace(pos, token.size(), with);
            pos += with.size();
        }
    };
    replace("${CORPUS}", corpus_path);
    return out;
}

} // namespace lci::parity
