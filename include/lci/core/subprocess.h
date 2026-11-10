#pragma once

#include <string>
#include <vector>

namespace lci {
namespace subprocess {

/// Runs argv[0] (PATH-searched) with the given args in `cwd` (empty =
/// inherit), capturing stdout into `out`. stderr is discarded. No shell is
/// involved on any platform — args are passed verbatim, so there is no
/// quoting/injection surface. Returns true iff the process spawned and
/// exited 0.
bool run_capture(const std::vector<std::string>& argv, const std::string& cwd,
                 std::string& out);

/// Runs argv inheriting stdio. Returns the child's exit code, or -1 if the
/// process could not be spawned (or died on a signal).
int run_status(const std::vector<std::string>& argv);

/// Spawns argv as a fully detached background process (new session on POSIX,
/// no console on Windows; stdio redirected to the null device). Returns true
/// on successful spawn. Used to launch the index server daemon.
///
/// `stderr_log`, when non-empty, receives the daemon's stderr instead, so a
/// daemon's own exit reason (RSS self-cap, bind failure) survives the
/// detach. POSIX: any existing entry at that path is unlinked and a fresh
/// file is created 0600 with O_EXCL|O_NOFOLLOW, so a planted symlink is
/// never written through and an earlier daemon's log is replaced; if the
/// file cannot be created the spawn fails. Windows: not yet honoured, stderr
/// stays on the null device.
bool spawn_detached(const std::vector<std::string>& argv,
                    const std::string& stderr_log = std::string());

}  // namespace subprocess
}  // namespace lci
