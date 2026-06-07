#!/bin/sh
# lci installer — downloads the matching prebuilt binary from GitHub releases.
#
#   curl -fsSL https://raw.githubusercontent.com/standardbeagle/lci-cpp/main/install.sh | sh
#
# Environment:
#   LCI_VERSION   pin a specific release (e.g. 0.6.0); default: latest
#   LCI_PREFIX    install directory; default: /usr/local/bin (else ~/.local/bin)
#   LCI_DRYRUN    set to 1 to print the resolved download URL and exit
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

# HTTP GET to stdout via curl or wget.
http_get() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- "$1"
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
    | grep "$pattern" \
    | head -n1)"

[ -n "$url" ] || err "no release asset matching '$pattern' found at $api"

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
    curl -fsSL -o "$tarball" "$url" || err "download failed"
else
    wget -qO "$tarball" "$url" || err "download failed"
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
        expected="$(grep "  $asset_name\$" "$tmp/SHA256SUMS" | awk '{print $1}' | head -n1)"
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
case ":$PATH:" in
    *":$prefix:"*) ;;
    *) printf 'Note: %s is not on your PATH. Add it:\n  export PATH="%s:$PATH"\n' "$prefix" "$prefix";;
esac
"$prefix/lci" --version || true
