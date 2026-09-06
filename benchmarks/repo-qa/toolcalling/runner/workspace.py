"""Build the opencode workspace that offers the mock LCI server and nothing else.

Arm disjointness (bench-harness-oracle-independence rule 12) is a property of
this file: the only MCP server enabled is the mock, every other configured
server is explicitly disabled, and the workspace holds no source corpus, so an
opencode native tool (grep/read/glob) has nothing to answer a selection prompt
from. A native call is therefore a real selection event, not a shortcut past
the toolset.

The opencode recipe itself is inherited from scripts/bench.py and
scripts/opencode_runner.py -- a GIT workspace (a bare directory hangs), the
mcp block in opencode.json, and full provider/model ids at the call site.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

BENCH_ROOT = Path(__file__).resolve().parents[2]
MOCK_SCRIPT = BENCH_ROOT / "scripts" / "mock_lci_mcp.py"
MCP_SERVER_NAME = "lci"


def mock_workspace_config(variant_path: Path, call_log: Path | None = None) -> dict:
    """opencode.json contents for one description-variant arm.

    Only `environment` differs between arms: the mock takes its tool NAMES and
    input schema from the committed surface manifest and its DESCRIPTIONS from
    the file named here, so a variant swap moves the wording and nothing else.
    """
    environment = {"LCI_MOCK_DESCRIPTIONS": str(Path(variant_path).resolve())}
    if call_log is not None:
        environment["LCI_MOCK_CALL_LOG"] = str(Path(call_log).resolve())
    return {
        "$schema": "https://opencode.ai/config.json",
        "mcp": {
            MCP_SERVER_NAME: {
                "type": "local",
                "command": [sys.executable, str(MOCK_SCRIPT), "mcp"],
                "environment": environment,
                "enabled": True,
            },
            "slop-mcp": {"enabled": False},
        },
        "permission": {"edit": "deny", "webfetch": "deny"},
    }


def enabled_mcp_servers(config: dict) -> list[str]:
    return sorted(name for name, spec in config.get("mcp", {}).items()
                  if spec.get("enabled", True))


def build_workspace(parent: Path, variant_path: Path,
                    call_log: Path | None = None) -> Path:
    """A committed, corpus-free git workspace wired to the mock server."""
    workspace = Path(tempfile.mkdtemp(prefix="selection-", dir=str(parent)))
    try:
        subprocess.run(["git", "init", "-q"], cwd=workspace, check=True)
        (workspace / ".gitignore").write_text("*\n!.gitignore\n!opencode.json\n")
        (workspace / "opencode.json").write_text(
            json.dumps(mock_workspace_config(variant_path, call_log),
                       indent=2, sort_keys=True) + "\n")
        subprocess.run(["git", "add", ".gitignore", "opencode.json"],
                       cwd=workspace, check=True)
        subprocess.run(
            ["git", "-c", "user.name=Harness", "-c", "user.email=harness@invalid",
             "commit", "-qm", "selection harness workspace"],
            cwd=workspace, check=True)
        return workspace
    except BaseException:
        shutil.rmtree(workspace, ignore_errors=True)
        raise
