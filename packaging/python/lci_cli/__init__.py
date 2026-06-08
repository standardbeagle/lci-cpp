"""lci-cli — thin Python wrapper that fetches and runs the prebuilt lci binary.

``uv tool install`` / ``pipx`` do not reliably run install hooks, so the binary
is downloaded on first run and cached per version. Subsequent runs exec the
cached binary directly.

Contract: see
docs/superpowers/specs/2026-06-07-install-update-distribution-design.md
"""

from .installer import ensure_binary

__all__ = ["main", "ensure_binary"]


def main() -> None:
    import subprocess
    import sys

    binary = ensure_binary()
    result = subprocess.run([str(binary), *sys.argv[1:]])
    sys.exit(result.returncode)
