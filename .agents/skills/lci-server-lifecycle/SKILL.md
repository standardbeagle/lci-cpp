---
name: lci-server-lifecycle
description: "Use when: tracing the per-root index server (spawn, socket path, registry, idle/RSS/root-gone reaper, eviction, shutdown), the HTTP-over-Unix-socket route table, Client/get_socket_path_for_root, `lci server|servers|shutdown|status|stats`, or comparing lci's daemon model against clangd/gopls, Zoekt, watchman, Serena, ripgrep."
---

# lci server lifecycle

Every `lci` command needing an index talks to a per-user, per-project-root index server over a Unix domain socket (loopback TCP on Windows only). The first command auto-spawns it detached, waits for bind then index-ready, later commands reuse it. It exits on: `idle_timeout_sec` with no real request, one reaper tick after its root is deleted, anonymous RSS over `max_rss_mb` post-`malloc_trim`, or eviction past `max_instances` by a newer server. Index is in-memory only; every start rebuilds it.

## Surfaces

| Surface | Entry point | Notes |
|---|---|---|
| CLI `lci server [-d,--daemon] [--foreground]` | `src/cli/main.cpp:426`; `run_server` `src/cli/server.cpp:289` | `--foreground` overrides `--daemon` with a stderr note. Daemon mode re-execs self with `--foreground` via `subprocess::spawn_detached`. Only this path calls `enable_instance_registry`. |
| CLI `lci servers [-j]` | `src/cli/main.cpp:446`; `run_servers` `src/cli/server.cpp:485` | Reads registry only; never spawns. Prints PID, idle seconds, root, `[root deleted]`. |
| CLI `lci shutdown` / `stop` `[-f,--force] [-a,--all]` | `src/cli/main.cpp:455`; `run_shutdown` `src/cli/server.cpp:421`, `run_shutdown_all` `:517` | `--all` walks the registry. Pre-0.8.0 servers and the `lci mcp` in-process fallback server are not in the registry. |
| CLI `lci status [-j] [-v]` | `src/cli/main.cpp:410`; `run_status` `src/cli/status.cpp:61` | Prints `Status: not running` and spawns nothing when no server (`CliStatusTest.NoServerPrintsNotRunningAndSpawnsNothing`). Threads/RSS come from the server's `/stats`. |
| Auto-spawn from any query command | `ensure_server_running` `src/cli/server.cpp:130` (flag-less overload `:282`) | Callers: `commands.cpp`, `commands_query.cpp`, `search.cpp`, `grep.cpp`, `mcp.cpp:105`. Two-phase wait: 30 s spawn-ready, then `wait_for_ready` bounded by `performance.indexing_timeout_sec` as a stall bound. |
| Socket path | `get_socket_path` `src/server/server.cpp` (decl `include/lci/server/server.h:31`), `get_socket_path_for_root` `src/server/server.cpp:165` | `<tmp>/lci-<uid>.sock` or `<tmp>/lci-<uid>-<hash8>.sock`; Windows `127.0.0.1:<43519 + hash % 1000>`. |
| Instance registry dir | `instance_registry_dir` `src/cli/server.cpp:52` | `$XDG_RUNTIME_DIR/lci` else `<tmp>/lci-registry-<uid>`, chmod 0700. Entries `lci-srv-<uid>-<hash>.json`, mtime = activity, 60 s touch throttle (`kRegistryTouchIntervalNs` `src/server/server.cpp:846`). |
| Route table (POST) | `IndexServer::register_handlers` `src/server/server.cpp:1128`; routes `:1169-1239` | `/ping /status /search /symbol /fileinfo /shutdown /reindex /stats /definition /references /callers /tree /git-analyze /list-symbols /inspect-symbol /browse-file /mcp`. |
| Route table (GET) | `src/server/server.cpp:1256-1264` | `/ping /status /stats` only. |
| `POST /mcp` bridge | `src/server/server.cpp:1239`; dispatcher set in `run_server` `src/cli/server.cpp:363` | `lci mcp` forwards each stdio JSON-RPC frame here so stdio clients share one warmed index. Returns an error body when no dispatcher is installed (embedded/test servers). |
| Listener caps | `src/server/server.cpp:1128-1156` | 16 MiB body, 30 s read/write timeouts, keep-alive 64 requests, pool = max(hw threads, 64) workers, queue cap `kMaxQueuedRequests = 32` (`include/lci/server/server.h:306`). |
| Client API | `class Client` `include/lci/server/client.h:20`; `src/server/client.cpp` | `is_server_running`, `ping`, `get_status`, `get_stats`, `shutdown(force)`, `reindex`, `wait_for_ready` (`:256`, stall-aware), `mcp_dispatch` (`:409`), `set_timeout`. |
| Config `server { }` block | `apply_server` `src/config/config.cpp:309`; `struct ServerConfig` `include/lci/config.h:62` | Keys `idle_timeout_sec`, `max_instances`, `max_rss_mb`. |
| Docs | `docs/src/content/docs/http-socket-api.mdx` (Transport `:12`, Endpoints `:27`, Lifecycle `:67`); `README.md:130` "Server lifecycle" | See drift notes under traps. |

## Code map

Walk: CLI verb -> `ensure_server_running` / `run_server` -> `IndexServer::start` -> handlers -> `MasterIndex` snapshots.

- `src/cli/main.cpp` — CLI11 subcommand wiring for `server`, `servers`, `shutdown`, `status`, `stats`.
- `src/cli/server.cpp` — `instance_registry_dir`, `build_server_spawn_argv` (`:82`, carries `--root`, config path, include/exclude), `unlink_socket_if_identity` (`:121`, unlink only when inode matches the stale server's socket), `ensure_server_running` (liveness probe, build-id stale detection at `:178`, socket-collision check, spawn, two-phase wait), `run_server` (socket path, registry opt-in, MCP registry + `WarmupLatch` warm thread, SIGINT/SIGTERM handlers, `is_running()` poll loop), `run_shutdown`, `run_servers`, `run_shutdown_all`.
- `src/cli/status.cpp` — `run_status`: `/stats` then `/status`; JSON keys `ready`, `num_threads`, `memory_rss_mb`, `uptime_seconds`, `avg_search_time_ms`.
- `src/cli/cli_core.cpp` — root/config resolution that the spawn argv inherits (absolute root, `:66-78` explains why).
- `include/lci/core/subprocess.h:24` — `spawn_detached` used by both daemon mode and auto-spawn.
- `include/lci/server/server.h` — `IndexServer` (`:319`): `set_socket_path`, `set_build_id_override`, `enable_instance_registry`, `set_search_engine` (atomic publication), `set_mcp_dispatcher`, `set_self_stop_callback`, `start`, `wait`, `shutdown`, `is_running`; `ServerInstance` (`:47`), `list_server_instances` (`:63`), `build_id` (`:41`). Members: `std::atomic<SearchEngine*> search_engine_` (`:441`), `running_`, `listener_bind_failed_`, `indexing_active_`, `bound_socket_ino_`, `last_activity_ns_`, `shared_mutex mu_` (write path only), `lifecycle_mu_`, `shutdown_mu_`.
- `src/server/server.cpp` — `current_user_id`, `read_own_rss_mb` (RssAnon with VmRSS fallback, `:98`), socket path helpers, `unix_socket_alive` raw connect probe, `IndexServer::start` (`:387`: flock on `<sock>.lock` sidecar with `O_NOFOLLOW`, live-listener refusal, background `index_directory` thread, registry publish before listen, `umask(0077)` around `bind_to_port` then `chmod 0600`, reaper thread), `shutdown_locked` (`:699`), `touch_activity` (`:854`), `publish_registry_entry`/`write_registry_file` (`:881/:898`), `evict_excess_peers` (`:927`), `list_server_instances` (`:951`), `request_self_stop` (`:1018`, exactly-once), `reaper_loop` (`:1049`), `register_handlers` (`:1128`), `require_ready` (`:1286`).
- `src/server/server_endpoints.cpp` — `handle_ping` (`:83`, carries `build_id`), `handle_status` (`:106`, `ready`, `file_count`, `symbol_count`, `indexing_active`, `indexing_progress{phase,files_scanned,files_total,percent_complete,elapsed_ms}`), `handle_symbol`, `handle_fileinfo`, `handle_shutdown` (`:246`, replies first, then a detached trigger thread calls `request_self_stop`), `handle_reindex` (`:301`, rejects paths outside root), `handle_stats` (`:447`), `handle_tree`, `handle_git_analyze`.
- `src/server/server_endpoints_symbols.cpp` — `handle_list_symbols` (`:124`), `handle_inspect_symbol` (`:338`), `handle_browse_file` (`:538`).
- `src/server/handlers_search.cpp` — `handle_search` (`:28`), `handle_definition` (`:141`), `handle_references` (`:235`), `handle_callers` (`:301`).
- `src/server/client.cpp` — httplib client over the socket path; `valid_tcp_address` (`:19`) for the Windows form; `post_json`/`get_json` (`:439/:485`).
- `include/lci/server/request_decode.h` — typed request decoding; wrong-typed fields return 400 not 500 (`ServerTest.WrongTypedJsonFieldsReturn400Not500`).
- `src/cli/mcp.cpp:95-125` — `lci mcp` prefers the shared server via `/mcp`; falls back to an in-process `IndexServer` when local overrides (`--config`, include/exclude, `LCI_ERROR_REPORT`) are present or the server cannot be reached.
- Tests: `tests/server_test.cpp` (socket path, lifecycle, reaper, registry, umask, symlink flock, graceful shutdown, concurrent requests); `tests/client_test.cpp` (Client API incl. `wait_for_ready` stall semantics); `tests/cli_test.cpp:2773-2850` (`CliServerSpawnTest`), `:3526-3600` (`CliStatusTest`); `tests/integration/server_lifecycle_test.cpp` (real server end-to-end, `ServerCancellationTest.ShutdownJoinsInFlightIndexing`, `ServerRssCapTest.ExceededCapStopsServer`); `tests/integration/spec_migration_test.cpp:400` (`IntegrationHttpSpec.MatchesGolden`, 14 goldens under `tests/integration/goldens/http/`: `ping status stats reindex search definition references tree fileinfo git-analyze list-symbols list-symbols-receiver inspect-symbol browse-file`); `tests/config_test.cpp:491` (`ParsesServerSection`, `ServerSectionDefaults`).

## Config knobs

| Key | Default | Struct field | Effect |
|---|---|---|---|
| `server { idle_timeout_sec }` | 1800 | `ServerConfig::idle_timeout_sec` `include/lci/config.h:66` | Exit after this long with no non-`/ping` request. 0 disables. |
| `server { max_instances }` | 8 | `ServerConfig::max_instances` `:71` | A starting registry-enabled server shuts down least-recently-active peers beyond the cap. 0 disables. |
| `server { max_rss_mb }` | 4096 | `ServerConfig::max_rss_mb` `:80` | Reaper reads RssAnon each tick; over cap -> `malloc_trim(0)`; still over -> loud exit. Linux only. 0 disables. |
| `performance { indexing_timeout_sec }` | 120 | `include/lci/config.h:58` | Bounds the index-ready wait in `ensure_server_running` as a stall bound (progress resets it). |
| `XDG_RUNTIME_DIR` | unset | `src/cli/server.cpp:61` | Registry lives under `$XDG_RUNTIME_DIR/lci` when set. |
| `TMPDIR` (via `temp_directory_path`) | `/tmp` | `src/server/server.cpp:156-192` | Socket and fallback registry location. |
| `LCI_ERROR_REPORT` | unset | `src/cli/mcp.cpp:102` | Forces `lci mcp` onto the in-process index instead of the shared server. |
| `LCI_UPDATE_GOLDENS=1` | unset | `tests/integration/README.md:15` | Regenerates http goldens. |

Unknown keys inside `server { }` produce a warning, not an error (`warn_unknown` `src/config/config.cpp:325`).

## Invariants and traps

- Exposure posture: loopback only. POSIX binds `AF_UNIX` (`src/server/server.cpp:595`); the only `bind_to_port("127.0.0.1", ...)` is inside `#ifdef _WIN32` (`:571`). No `0.0.0.0`, no `AF_INET` on Unix (grep of `src/server`, `src/cli`). Concrete caps are declared at `src/server/server.cpp:1128-1156`.
- Socket mode: `umask(0077)` wraps `bind_to_port`, then `chmod 0600`. The umask test is a disclosed guard, not a RED (it passes pre-fix via the later chmod): `.claude/rules/worktree-isolation-and-goldens.md` rule 7, `ServerTest.SocketModeIsOwnerOnlyUnderPermissiveUmask`.
- Sidecar flock opens `<sock>.lock` with `O_NOFOLLOW`; a pre-planted symlink is not followed (`ServerTest.SocketLockSymlinkIsNotFollowed`, a true RED). Lock held but no listener after 2 s -> proceed loudly (zombie class).
- `start()` refuses to steal a live listener (`ServerLifecycleTest.StartRefusesToStealALiveListener`); unlink of a stale socket happens only when the inode matches the one captured before the stale server exited (`unlink_socket_if_identity`, `CliServerSpawnTest.SocketUnlinkOnlyWhenInodeMatches`).
- Read path is lock-free: `search_engine_` is an atomic readiness flag, never dereferenced by handlers; index reads travel `MasterIndex` RCU snapshots. `.claude/rules/karpathy-principles.md` rule 3 (criterion-states-the-invariant clause) and `require_ready` `src/server/server.cpp:1286`.
- `/ping` does not stamp activity (`set_pre_routing_handler` `:1161`), so discovery probes and eviction scans cannot keep an idle server alive (`ServerReaperTest.PingDoesNotDeferIdleExit`).
- `POST /shutdown` replies first, then stops from a detached thread; `request_self_stop` is exactly-once per listen and re-arms on restart (`ServerLifecycleTest.ShutdownEndpointCallbackOnceAndRestartRearms`). History: before 2026-08-09 `/shutdown` never cleared `running_`, so CLI servers ignored it; memory note `server-lifecycle-reaper`.
- RSS cap reads RssAnon deliberately (page-cache from mmaps must not trip it). Note the comment at `src/server/server.cpp:98-104` still says the content store retains file-backed mmaps; `karpathy-principles.md` rule 1 records that `a9bf1ac` moved content to heap and filed `01M1PE7698726F1P9QQMTQ5FT1`, so that rationale is stale. Origin incident: memory note `errlookup-lci-oom` (26 GB RSS server took the host down).
- `max_instances` eviction only runs for registry-enabled servers, and only `run_server` enables the registry. Embedded servers (tests, `lci mcp` in-process fallback) neither evict nor are evicted, and `lci servers` / `lci shutdown --all` cannot see them (`README.md:149-153`).
- Root-gone exit is enforced only when the root existed at `start()` (`src/server/server.cpp:671-676`).
- `/reindex` path: `handle_reindex` calls `indexer_->clear()` before `index_directory()`, which `karpathy-principles.md` rule 3 flagged as cancelling the bulk-window fix for that path (filed `01M1PA4DNHVBWX1749E3KQJA5N`). Check current state before assuming it is closed.
- Worker pool sizing is the concurrency cap (`kMaxQueuedRequests`); `/mcp` parks a worker on the warmup latch, so `/ping` and `/shutdown` must stay answerable (`ServerLifecycleTest.PingAnswersWhileMcpCallsParkedOnWarmup`).
- `/stats.num_threads` is `std::thread::hardware_concurrency()` (`src/server/server_endpoints.cpp:462`), not the live thread count. `lci status` labels it "Threads".
- Findings from the 2026-09-08 mapping (fixed or filed): see docs/reviews/2026-09-08-skill-mapping-findings.md.
- Integration goldens embed absolute repo paths (`http_reindex`); a worktree at another path fails them spuriously (`.claude/rules/worktree-isolation-and-goldens.md` rule 2).
- Dogfood: `lci def ensure_server_running` and `lci refs request_self_stop` both worked and were faster than grep for this map. Gap: no way to ask lci for the httplib route table (string literals in lambdas); grep was needed for `svr_.Post(`.

## Probe recipes

Binary: `build/release/src/lci`. Use `-r <root>`.

```
# List and stop servers (registry only, never spawns)
build/release/src/lci servers --json
build/release/src/lci shutdown --all

# Status without spawning; then a query that auto-spawns
build/release/src/lci status --json -r .
build/release/src/lci def IndexServer -r .
build/release/src/lci status -v -r .

# Foreground server for debugging (Ctrl-C or `lci shutdown` stops it)
build/release/src/lci server --foreground -r .

# Talk to the socket directly
ls -l /tmp/lci-$(id -u)-*.sock
curl --unix-socket /tmp/lci-$(id -u)-<hash>.sock http://lci/status
curl --unix-socket /tmp/lci-$(id -u)-<hash>.sock -X POST http://lci/stats -d '{}'

# Verify posture: no TCP listener owned by lci
ss -ltnp | grep -i lci ; echo "expect no output"
ss -lxp | grep lci-

# Registry contents
ls -la ${XDG_RUNTIME_DIR:-/tmp/lci-registry-$(id -u)}/lci 2>/dev/null || ls -la /tmp/lci-registry-$(id -u)

# Short idle timeout to watch the reaper (in a scratch root's .lci.kdl)
printf 'server {\n  idle_timeout_sec 5\n  max_rss_mb 512\n}\n' > <root>/.lci.kdl
```

Targeted tests (build `lci_tests` / `lci_integration_tests` only, per `.claude/rules/test-iteration-discipline.md`):

```
cmake --build build/release --parallel --target lci_tests
build/release/tests/lci_tests --gtest_filter='ServerTest.*:ServerReaperTest.*:ServerRegistryTest.*:ServerLifecycleTest.*:ServerLifecycleFailureTest.*:SocketPathTest.*:ClientTest.*:Client*ConnectionTest.*:ClientWaitForReadyStallTest.*:CliServerSpawnTest.*:CliStatusTest.*:KdlConfigTest.*Server*'
ctest --test-dir build/release -R lci_integration_suite --output-on-failure
build/release/tests/lci_integration_tests --gtest_filter='IntegrationHttpSpec*:ServerLifecycleTest.*:ServerCancellationTest.*:ServerRssCapTest.*'
```

Goldens: `tests/integration/goldens/http/*.json`; refresh with `LCI_UPDATE_GOLDENS=1` (`tests/integration/README.md:30-38`).

## Product comparison

Comparable daemon models (unverified below = from model knowledge, not measured):

- clangd / gopls (LSP): one process per editor session, client-owned over stdio, no cross-client sharing. (unverified)
- Sourcegraph Zoekt: separate `zoekt-webserver` over prebuilt on-disk shards, HTTP/TCP, designed for network exposure with auth in front. (unverified)
- watchman: per-user Unix-socket daemon, auto-started, idle-exits; closest lifecycle analogue. (unverified)
- ripgrep, universal-ctags: no daemon; each invocation rescans or reads a tags file.
- Serena MCP: per-project process that launches language servers, lifetime tied to the MCP client. (unverified)
- Cursor / GitHub code search: remote-hosted indexes, not local daemons.

lci claims in this area and how to check them:

| Claim | Where made | Measure | Existing numbers |
|---|---|---|---|
| Auto-start on first command, reuse after | `README.md:132-134`, `http-socket-api.mdx:73-75` | `lci shutdown --all`, then time two consecutive `lci def X -r <root>`; second should skip indexing (`status.ready` already true) | none committed for spawn latency |
| Servers do not leak: idle exit, root-gone exit, LRU eviction | `README.md:135-142`, memory `server-lifecycle-reaper` | `ServerReaperTest.*`; `lci servers` after deleting a scratch root; set `idle_timeout_sec 5` and watch | 175-orphan incident is the baseline story, not a number |
| Server never becomes the process that kills the host | `include/lci/config.h:72-79` | `ServerRssCapTest.ExceededCapStopsServer`; `lci status -v` RSS vs `max_rss_mb` on a large corpus | memory `errlookup-lci-oom` (26 GB pre-fix, 1.8 GB next.js post-fix); memory `ref-tracker-memory-refactor` (next.js 327 MB RssAnon) |
| Root-gone exit within ~500 ms | `README.md:142` | `kReaperTick = 500 ms` `src/server/server.cpp:843`; `ServerReaperTest.RootDeletionStopsServer` | tick is 500 ms, so worst case is one tick plus the check |
| Loopback-only, owner-only socket | `src/server/server.cpp:1129`, docs Transport | `ss -lxp`, `stat -c %a` on the socket (expect 600), `ServerTest.SocketModeIsOwnerOnlyUnderPermissiveUmask` | test-pinned |
| Sub-millisecond query once warm | `.claude/rules/karpathy-principles.md` mandate; `tests/integration/real_project_performance_test.cpp` | `RealProjectSearchLatencyTest.FastapiSearchUnder5ms` (in-process, not over the socket); over-socket: `time lci search X -r <root>` | `tests/benchmarks/baseline/linux-x64.json` (engine, not transport) |

Comparison axes:

| Axis | lci | Check the competitor | Notes |
|---|---|---|---|
| Spawn model | CLI auto-spawns a detached per-root daemon; `spawn_detached` | watchman `watchman get-sockname` auto-starts; LSP is client-owned | lci and watchman are the only two with CLI-driven implicit start |
| Transport | Unix socket, HTTP/1.1 JSON via httplib; TCP loopback only on Windows | Zoekt TCP HTTP; LSP stdio; watchman Unix socket BSER | lci has no TCP on Unix, so no network exposure question |
| Multi-client sharing | One server per (uid, root); `lci mcp` bridges stdio clients through `/mcp` | LSP: none; Zoekt: yes over network | sharing is what removes per-process re-index |
| Lifetime bounding | idle timeout, root-gone, RSS cap, LRU instance cap | watchman idle exit (unverified); Zoekt/LSP none built in | lci's RSS self-cap is unusual |
| Persistence | none; rebuild on every start (`http-socket-api.mdx:77-79`) | Zoekt shards on disk; ctags file; clangd `.cache/clangd` index | largest gap, see below |
| Warm-up visibility | `/status.indexing_progress{phase,files_scanned,files_total,percent_complete}`; `lci status` | LSP `$/progress`; Zoekt none per query | |
| Readiness gating | 503 until engine published; `wait_for_ready` stall-aware | LSP requests queue; Zoekt serves whatever shards exist | |
| Concurrency caps | 32 queued, 64+ workers, 16 MiB body, 30 s I/O timeouts | check server flags / config | declared in code, not tunable via `.lci.kdl` |
| Discovery of running instances | `lci servers` via registry files | watchman `watch-list`; LSP none | registry misses embedded servers |

Honest gaps vs the field:

- No disk persistence: every server start re-parses the corpus (`http-socket-api.mdx:77-79`). Zoekt, clangd and ctags all survive restarts without a rebuild.
- Idle exit plus no persistence means a 30-minute pause costs a full re-index on the next command; there is no committed spawn-to-ready latency number to size that cost.
- Listener caps are compile-time constants; no `.lci.kdl` knob.
- Windows falls back to loopback TCP with a uid-derived port; port collisions across two roots are avoided only by `hash % 1000` mixing.
- `lci servers` and `shutdown --all` cannot see servers embedded in `lci mcp` fallback processes or pre-0.8.0 binaries.
- No authentication on the socket beyond filesystem mode 0600; acceptable for loopback Unix, but any future TCP posture change would need authn per `# Exposure Posture`.

## Related skills

lci-feature-map (router), lci-search, lci-symbol-navigation, lci-get-context, lci-parsing-languages, lci-code-insight, lci-side-effects, lci-git-analysis, lci-indexing-pipeline, lci-mcp-server, lci-config, lci-ops-diagnostics, lci-benchmarks-evaluation
