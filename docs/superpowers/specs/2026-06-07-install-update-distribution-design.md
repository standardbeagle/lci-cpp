# Install & Update Distribution — Design

Date: 2026-06-07
Status: Approved

## Goal

Add five ways to install `lci` and one way to update it, all fetching the
prebuilt release binaries from GitHub (no source build):

1. `npm i -g @standardbeagle/lci`
2. `uv tool install lci-cli`
3. `curl -fsSL .../install.sh | sh` (Linux/macOS)
4. `irm .../install.ps1 | iex` (Windows)
5. `lci update` — self-update subcommand built into the binary

## The Contract (single source of truth)

Every implementation re-implements the same ~20-line logic in its own
language against this contract:

```
repo        = standardbeagle/lci-cpp
latest API  = https://api.github.com/repos/standardbeagle/lci-cpp/releases/latest
asset match (regex against live release JSON, NOT hardcoded filename):
    Linux   x86_64        → /lci-.*Linux\.tar\.gz$/
    macOS   x86_64/arm64  → /lci-.*Darwin\.tar\.gz$/   (universal binary)
    Windows x86_64        → /lci-.*(win64|Windows)\.tar\.gz$/i
unsupported (e.g. Linux arm64) → FAIL FAST, message names OS+arch
tarball layout: lci-<ver>-<sys>/bin/lci   (lci.exe on Windows)
version override: LCI_VERSION env / --version flag → pin a specific tag
```

Asset is matched by regex against the release JSON so the design survives
the CPack Windows-naming ambiguity (`win64` vs `Windows`) and version bumps.

Decision: macOS arm64 is served by the universal `Darwin` tarball, not an
error. Linux arm64 fails fast (no arm64 release artifact exists today).

## Components

### `lci update` (C++)

`src/update/updater.{h,cpp}` + wiring in `src/cli/main.cpp`.

- `lci update` — resolve latest; if == `LCI_VERSION` → "already on X", exit 0.
- `lci update --check` — report current vs latest, no write.
- `lci update --force` — reinstall even if equal.
- `lci update --version <tag>` — install a specific tag.
- HTTPS download + extract delegated to system `curl` and `tar` (the same
  tools install.sh requires); release JSON parsed natively with nlohmann.
  Deviation from the original "cpp-httplib" plan: httplib is linked WITHOUT
  `CPPHTTPLIB_OPENSSL_SUPPORT`, so enabling native TLS would mean a
  vcpkg+OpenSSL build expansion for one subcommand. Reusing curl/tar keeps
  the dependency surface identical to the install scripts and adds no
  build-time dependency.
- Self-path: `/proc/self/exe` (Linux), `_NSGetExecutablePath` (macOS),
  `GetModuleFileNameW` (Windows).
- Extract via system `tar` (Linux/macOS/Win10+); fail fast if absent.
- Atomic replace: POSIX → write `lci.new` beside self, chmod +x,
  `rename()` over self (legal while running). Windows → rename running
  `.exe` to `.old`, move new in, best-effort delete `.old` next run.
- No write permission → clear error naming the path + sudo/admin hint.
  No silent fallback.

### install.sh / install.ps1 (repo root)

- `install.sh`: POSIX sh. `uname` detect → contract → curl|wget API →
  download → `tar -xzf` → install to `/usr/local/bin` (fallback
  `~/.local/bin` if not writable + PATH hint). `LCI_DRYRUN=1` prints the
  resolved URL and exits (test hook).
- `install.ps1`: mirror. `Invoke-RestMethod` → download → `tar -xf` →
  install to `$env:LOCALAPPDATA\Programs\lci`, add to user PATH.
  `$env:LCI_DRYRUN` test hook.

### npm — `packaging/npm/`, `@standardbeagle/lci`

- `postinstall.js`: detect → download tarball at the package version →
  extract binary into `vendor/`. Fail fast on unsupported/offline.
- `bin/lci.js`: shim, `spawn`s `vendor/lci` with passthrough argv/stdio,
  propagates exit code.

### PyPI/uv — `packaging/python/`, `lci-cli`

- `uv tool install` doesn't reliably run install hooks → download-on-
  first-run. Console-script `lci` → `lci_cli` ensures binary is cached in
  platformdirs cache, downloading on first call, then execs it.
- `pyproject.toml` (hatchling), `lci_cli/__main__.py`.

### Docs

README: npm/uv/one-liner blocks + "Updating" section. `Formula/lci.rb`:
fix stale repo slug (`standardbeagle/lci` → `lci-cpp`); full Homebrew
rework out of scope.

## Testing (real data, no mocks)

- Pure asset-mapping function unit-tested per impl.
- install.sh/ps1: shellcheck + `LCI_DRYRUN` URL assertion vs real release.
- npm/python: integration install into temp dir hitting the real release.
- `lci update`: unit test asset selection; `--check` integration vs real API.

## Out of scope

- Auto-publish to npmjs/PyPI in release.yml (chose GitHub-only fetch).
- SHA256SUMS verification — release doesn't emit checksums today.
  Flagged as a worthwhile follow-up.
