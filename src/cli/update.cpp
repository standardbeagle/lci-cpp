#include <lci/cli/commands.h>
#include <lci/update/updater.h>
#include <lci/version.h>

namespace lci {
namespace cli {

int run_update(bool check_only, bool force, const std::string& version) {
    update::UpdateConfig cfg;
    cfg.current_version = kVersion;
    cfg.target_version = version;
    cfg.check_only = check_only;
    cfg.force = force;
    return update::run_update(cfg);
}

}  // namespace cli
}  // namespace lci
