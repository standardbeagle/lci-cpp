#include "runner/modes/index.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lci::parity {

CapturedOutput run_index_export(const std::string& binary_path,
                                const Descriptor&  d,
                                const std::string& corpus_path) {
    // Both the Go and C++ binaries write progress messages to stdout when
    // --output=/dev/stdout is used, interleaving them with the JSON stream.
    // Writing to a temp file and reading it back gives clean JSON.
    char tmp_template[] = "/tmp/lci-parity-index-XXXXXX";
    int  fd = mkstemp(tmp_template);
    if (fd < 0) throw std::runtime_error("mkstemp failed");
    close(fd);
    std::string tmp_path(tmp_template);

    Invocation inv;
    inv.args = {"debug", "export", "--output=" + tmp_path};
    inv.cwd  = corpus_path;
    inv.env  = d.invocation.env;

    // Index export over real-repo corpora can take >60s on the Go side:
    // Go's `debug export` re-indexes from scratch with SymbolLinkerEngine
    // (not the running server) and takes ~4m15s on the Go-self repo
    // (~600 files). Lift the per-binary cap to 360s so lci-go-repo
    // doesn't trip the default 60s timeout. The outer CTest TIMEOUT is
    // set to 420s for index/* in tests/parity/CMakeLists.txt.
    constexpr int kIndexExportTimeoutSeconds = 360;
    CapturedOutput cap = run_cli(binary_path, inv, corpus_path,
                                 kIndexExportTimeoutSeconds);

    if (cap.exit_code == 0) {
        std::ifstream f(tmp_path, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        cap.stdout_data = ss.str();
    }

    fs::remove(tmp_path);
    return cap;
}

} // namespace lci::parity
