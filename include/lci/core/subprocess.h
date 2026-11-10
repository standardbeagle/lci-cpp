#pragma once

#include <cstddef>
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

/// Size past which spawn_detached truncates a daemon's stderr log before
/// appending to it, so a root restarted many times keeps a bounded log.
inline constexpr std::size_t kMaxDaemonLogBytes = std::size_t{1} << 20;

/// Spawns argv as a fully detached background process (new session on POSIX,
/// no console on Windows; stdio redirected to the null device). Returns true
/// on successful spawn. Used to launch the index server daemon.
///
/// `stderr_log`, when non-empty, receives the daemon's stderr instead, so a
/// daemon's own exit reason (RSS self-cap, bind failure) survives the
/// detach. POSIX: the log is opened O_APPEND|O_NOFOLLOW and made 0600, so
/// concurrent spawns on one root share one inode and a live daemon's log is
/// never orphaned; a planted symlink is replaced, never written through; a
/// log past kMaxDaemonLogBytes is truncated first. If the log cannot be
/// opened the spawn fails. Windows: not yet honoured, stderr stays on the
/// null device.
bool spawn_detached(const std::vector<std::string>& argv,
                    const std::string& stderr_log = std::string());

}  // namespace subprocess
}  // namespace lci
