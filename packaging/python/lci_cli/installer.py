"""Download + cache the prebuilt lci binary for the host platform.

Uses only the standard library (urllib, json, tarfile) — no third-party deps.
Fails fast with a clear message on every error path.
"""

import json
import os
import platform
import shutil
import stat
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO = "standardbeagle/lci-cpp"


class InstallError(RuntimeError):
    pass


def _version() -> str:
    try:
        from importlib.metadata import version

        return version("lci-cli")
    except Exception:
        # Fallback for source checkouts without installed metadata.
        return "0.5.0"


def _cache_dir() -> Path:
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
    elif sys.platform == "darwin":
        base = str(Path.home() / "Library" / "Caches")
    else:
        base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "lci"


def _binary_name() -> str:
    return "lci.exe" if sys.platform == "win32" else "lci"


def _asset_matcher():
    """Return a predicate selecting the release asset for this platform."""
    machine = platform.machine().lower()
    x86 = machine in ("x86_64", "amd64")
    if sys.platform.startswith("linux"):
        if not x86:
            raise InstallError(
                f"unsupported architecture for Linux: {machine} "
                "(only x86_64 has a release artifact; build from source)"
            )
        return lambda name: name.endswith("Linux.tar.gz")
    if sys.platform == "darwin":
        # Universal binary serves both Intel and Apple Silicon.
        return lambda name: name.endswith("Darwin.tar.gz")
    if sys.platform == "win32":
        if not x86:
            raise InstallError(
                f"unsupported architecture for Windows: {machine} "
                "(only x86_64 has a release artifact; build from source)"
            )
        lower = lambda name: name.lower()
        return lambda name: name.endswith(".tar.gz") and (
            "win64" in lower(name) or "windows" in lower(name)
        )
    raise InstallError(f"unsupported platform: {sys.platform}")


def _http_get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "lci-pip-installer"})
    with urllib.request.urlopen(req) as resp:  # follows redirects
        return resp.read()


def _download_binary(dest: Path, version: str) -> None:
    match = _asset_matcher()
    api = f"https://api.github.com/repos/{REPO}/releases/tags/v{version}"
    try:
        release = json.loads(_http_get(api).decode("utf-8"))
    except Exception as exc:  # noqa: BLE001 — surface any fetch/parse failure
        raise InstallError(f"failed to query release v{version}: {exc}") from exc

    asset = next((a for a in release.get("assets", []) if match(a["name"])), None)
    if asset is None:
        raise InstallError(f"no release asset for this platform in v{version}")

    with tempfile.TemporaryDirectory(prefix="lci-pip-") as tmp:
        tarball = Path(tmp) / asset["name"]
        try:
            tarball.write_bytes(_http_get(asset["browser_download_url"]))
        except Exception as exc:  # noqa: BLE001
            raise InstallError(f"download failed: {exc}") from exc

        with tarfile.open(tarball, "r:gz") as tf:
            tf.extractall(tmp)

        want = _binary_name()
        found = next((p for p in Path(tmp).rglob(want) if p.is_file()), None)
        if found is None:
            raise InstallError("extracted archive did not contain the lci binary")

        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(found, dest)
        if sys.platform != "win32":
            dest.chmod(dest.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def ensure_binary() -> Path:
    """Return the path to the cached lci binary, downloading it if absent."""
    version = _version()
    binary = _cache_dir() / version / _binary_name()
    if binary.exists():
        return binary
    try:
        _download_binary(binary, version)
    except InstallError as exc:
        print(f"lci install error: {exc}", file=sys.stderr)
        sys.exit(1)
    return binary
