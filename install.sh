#!/bin/sh
# lci installer — downloads the matching prebuilt binary from GitHub releases.
#
#   curl -fsSL https://raw.githubusercontent.com/standardbeagle/lci-cpp/main/install.sh | sh
#
# Environment:
#   LCI_VERSION   pin a specific release (e.g. 0.6.0); default: latest
#   LCI_PREFIX    install directory; default: /usr/local/bin (else ~/.local/bin)
#   LCI_DRYRUN    set to 1 to print the resolved download URL and exit
#   LCI_NO_PATH   set to 1 to skip adding the install dir to your shell rc
#   LCI_NO_SLOP   set to 1 to skip registering the lci MCP server with slop-mcp
#   LCI_AUTO_UPDATE  set to 1 to schedule a weekly `lci update` (systemd/cron)
#
# Contract: see docs/superpowers/specs/2026-06-07-install-update-distribution-design.md
set -eu

REPO="standardbeagle/lci-cpp"

err() { printf 'error: %s\n' "$1" >&2; exit 1; }

os="$(uname -s)"
arch="$(uname -m)"

case "$os" in
    Linux)
        case "$arch" in
            x86_64 | amd64) ;;
            *) err "unsupported architecture for Linux: $arch (only x86_64 has a release artifact; build from source)";;
        esac
        pattern="Linux.tar.gz"
        ;;
    Darwin)
        # Universal binary serves both Intel and Apple Silicon.
        pattern="Darwin.tar.gz"
        ;;
    *)
        err "unsupported OS: $os (use install.ps1 on Windows, or build from source)"
        ;;
esac

# HTTP GET to stdout via curl or wget, pinned to https.
http_get() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --proto '=https' "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget --https-only -qO- "$1"
    else
        err "neither curl nor wget found on PATH"
    fi
}

if [ -n "${LCI_VERSION:-}" ]; then
    api="https://api.github.com/repos/$REPO/releases/tags/v$LCI_VERSION"
else
    api="https://api.github.com/repos/$REPO/releases/latest"
fi

json="$(http_get "$api")" || err "failed to query GitHub releases API ($api)"

url="$(printf '%s\n' "$json" \
    | grep -o '"browser_download_url"[ ]*:[ ]*"[^"]*"' \
    | sed 's/.*"browser_download_url"[ ]*:[ ]*"//; s/"$//' \
    | grep -F "$pattern" \
    | head -n1)"

[ -n "$url" ] || err "no release asset matching '$pattern' found at $api"

# Pin downloads to GitHub over https — refuse anything else.
case "$url" in
    https://github.com/* | https://*.githubusercontent.com/* | https://codeload.github.com/*) ;;
    *) err "unexpected download URL (not https GitHub): $url";;
esac

if [ "${LCI_DRYRUN:-}" = "1" ]; then
    printf '%s\n' "$url"
    exit 0
fi

command -v tar >/dev/null 2>&1 || err "tar is required but was not found on PATH"

# Choose an install directory: explicit LCI_PREFIX, then a writable
# /usr/local/bin, otherwise ~/.local/bin.
if [ -n "${LCI_PREFIX:-}" ]; then
    prefix="$LCI_PREFIX"
elif [ -w /usr/local/bin ]; then
    prefix="/usr/local/bin"
else
    prefix="$HOME/.local/bin"
fi
mkdir -p "$prefix" || err "cannot create install directory: $prefix"
[ -w "$prefix" ] || err "no write permission to $prefix (set LCI_PREFIX or re-run with sudo)"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/lci-install.XXXXXX")" || err "cannot create temp dir"
trap 'rm -rf "$tmp"' EXIT INT TERM

asset_name="$(basename "$url")"
tarball="$tmp/$asset_name"
printf 'Downloading %s\n' "$url"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL --proto '=https' -o "$tarball" "$url" || err "download failed"
else
    wget --https-only -qO "$tarball" "$url" || err "download failed"
fi

# Verify integrity against the release SHA256SUMS when present.
sums_url="$(printf '%s\n' "$json" \
    | grep -o '"browser_download_url"[ ]*:[ ]*"[^"]*"' \
    | sed 's/.*"browser_download_url"[ ]*:[ ]*"//; s/"$//' \
    | grep '/SHA256SUMS$' \
    | head -n1)"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        return 1
    fi
}

if [ -n "$sums_url" ]; then
    if http_get "$sums_url" > "$tmp/SHA256SUMS" 2>/dev/null; then
        # Exact filename field match — avoid regex/suffix false positives.
        expected="$(awk -v n="$asset_name" '$2==n {print $1; exit}' "$tmp/SHA256SUMS")"
        if [ -z "$expected" ]; then
            err "SHA256SUMS has no entry for $asset_name"
        fi
        actual="$(sha256_of "$tarball")" \
            || err "no sha256 tool (sha256sum/shasum) found to verify the download"
        if [ "$actual" != "$expected" ]; then
            err "checksum mismatch for $asset_name (expected $expected, got $actual)"
        fi
        printf 'Verified checksum.\n'
    else
        printf 'warning: could not fetch SHA256SUMS; skipping integrity check\n' >&2
    fi
else
    printf 'warning: release has no SHA256SUMS; skipping integrity check\n' >&2
fi

tar -xzf "$tarball" -C "$tmp" || err "failed to extract archive"

bin="$(find "$tmp" -type f -name lci | head -n1)"
[ -n "$bin" ] || err "extracted archive did not contain the lci binary"

install -m 0755 "$bin" "$prefix/lci" 2>/dev/null || {
    cp "$bin" "$prefix/lci" && chmod 0755 "$prefix/lci";
} || err "failed to install to $prefix"

printf 'Installed lci to %s/lci\n' "$prefix"
"$prefix/lci" --version || true

# Make lci immediately callable. A piped `curl | sh` runs in a subshell and
# cannot mutate the parent shell's PATH, so we persist it for future shells via
# the rc file and print the exact export for the current session. Skip with
# LCI_NO_PATH=1.
case ":$PATH:" in
    *":$prefix:"*)
        : # already on PATH — callable now
        ;;
    *)
        export_line="export PATH=\"$prefix:\$PATH\""
        if [ "${LCI_NO_PATH:-}" != "1" ]; then
            case "${SHELL:-}" in
                *zsh)  rc="$HOME/.zshrc" ;;
                *bash) rc="$HOME/.bashrc" ;;
                *)     rc="$HOME/.profile" ;;
            esac
            if [ -f "$rc" ] && grep -Fq "$prefix" "$rc" 2>/dev/null; then
                :
            elif printf '\n# Added by lci installer\n%s\n' "$export_line" >> "$rc" 2>/dev/null; then
                printf 'Added %s to PATH in %s (new shells).\n' "$prefix" "$rc"
            fi
        fi
        printf 'To use lci in THIS shell now, run:\n  %s\n' "$export_line"
        ;;
esac

# Register the lci MCP server (`lci mcp`, stdio) with slop-mcp when it is
# installed, so the binary is immediately usable as an MCP. User scope = all
# projects. Re-running overwrites the entry (idempotent). Skip with LCI_NO_SLOP=1.
if [ "${LCI_NO_SLOP:-}" != "1" ] && command -v slop-mcp >/dev/null 2>&1; then
    if slop-mcp mcp add --user lci "$prefix/lci" mcp >/dev/null 2>&1; then
        printf 'Registered lci MCP server with slop-mcp (user scope).\n'
    else
        printf 'warning: slop-mcp found but registering lci failed; add it manually:\n  slop-mcp mcp add --user lci "%s/lci" mcp\n' "$prefix" >&2
    fi
fi

# Manual update commands (always shown).
printf 'Update later with:\n  lci update            # install latest release\n  lci update --check    # report current vs latest without installing\n'

# Optional weekly auto-update: a scheduled job that runs `lci update`. Opt in
# with LCI_AUTO_UPDATE=1. Prefer a systemd user timer; fall back to cron.
if [ "${LCI_AUTO_UPDATE:-}" = "1" ]; then
    if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then
        unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
        mkdir -p "$unit_dir"
        printf '[Unit]\nDescription=Update lci to the latest release\n\n[Service]\nType=oneshot\nExecStart=%s/lci update\n' "$prefix" > "$unit_dir/lci-update.service"
        printf '[Unit]\nDescription=Weekly lci auto-update\n\n[Timer]\nOnCalendar=weekly\nPersistent=true\n\n[Install]\nWantedBy=timers.target\n' > "$unit_dir/lci-update.timer"
        systemctl --user daemon-reload >/dev/null 2>&1
        if systemctl --user enable --now lci-update.timer >/dev/null 2>&1; then
            printf 'Auto-update enabled: systemd user timer (weekly).\n  Disable: systemctl --user disable --now lci-update.timer\n'
        else
            printf 'warning: could not enable systemd timer; auto-update not scheduled\n' >&2
        fi
    elif command -v crontab >/dev/null 2>&1; then
        if crontab -l 2>/dev/null | grep -Fq 'lci-auto-update'; then
            printf 'Auto-update already scheduled (crontab).\n'
        elif { crontab -l 2>/dev/null; printf '@weekly %s/lci update >/dev/null 2>&1  # lci-auto-update\n' "$prefix"; } | crontab -; then
            printf 'Auto-update enabled: weekly crontab entry.\n  Disable: crontab -e (remove the lci-auto-update line)\n'
        else
            printf 'warning: could not write crontab; auto-update not scheduled\n' >&2
        fi
    else
        printf 'warning: LCI_AUTO_UPDATE=1 but no systemd --user or crontab found; skipped\n' >&2
    fi
fi
