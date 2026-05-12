#!/usr/bin/env python3
"""Generate tests/parity/MASK_AUDIT.md by scanning every descriptor's
`ignore` tier and classifying each masked field path."""

import json
import pathlib
import sys
from collections import defaultdict

ROOT = pathlib.Path(__file__).resolve().parents[1]
DESC_DIR = ROOT / "descriptors"
OUT = ROOT / "MASK_AUDIT.md"

# Classification rules. First match wins. Order matters.
RULES_A = {  # non-determinism / RPC envelope
    "exact": {
        "id", "jsonrpc", "pid", "version", "timestamp", "start_time",
        "request_id", "uptime_ms", "elapsed_ms", "schema_version",
        "uptime_seconds", "build_id", "host", "ready", "build_time",
        "progress", "index_version",
    },
    "suffix": ("_ms", ".ms", "_at", "_pid", ".id", ".jsonrpc",
               "_version"),
}
RULES_B = {  # intentional C++ enrichment / runtime metric
    "substr": [
        "signature", "block_name", "indexing_progress",
        "tree.root.file_path", "stats",
        "memory_", "num_threads", "num_goroutines", "max_goroutines",
        "ContentHash", "FastHash", "Checksum", "CharMask",
    ],
}
RULES_C = {  # workspace-state / content-dependent
    "substr": [
        "metrics_issues", "naming_issues", "duplicates",
        ".summary", "_issues_found", "files_changed",
        "symbols_added", "symbols_deleted", "symbols_modified",
        "top_recommendation", "risk_score",
        "file_info.", "summary", "files", "refs", "dependencies",
        "extractors", "resolvers", "search_count",
        "result.content",
    ],
}
RULES_D = {  # ranking / scanner-order divergence
    "substr": ["file_id", "score", "rank", "trigram_count",
               "symbol_count", "symbols", "file_count",
               "index_size_bytes", "result.context",
               "results[].context", "results[].result.context",
               "root"],
}


def classify(p):
    b = p.replace("[]", "")
    if b in RULES_A["exact"] or any(b.endswith(s) for s in RULES_A["suffix"]):
        return "a"
    if any(s in b for s in RULES_B["substr"]):
        return "b"
    if any(s in b for s in RULES_C["substr"]):
        return "c"
    if any(s in b for s in RULES_D["substr"]):
        return "d"
    return "?"


def collect():
    rows = []
    for f in sorted(DESC_DIR.rglob("*.parity.json")):
        try:
            d = json.loads(f.read_text())
        except json.JSONDecodeError:
            continue
        rows.append({
            "id": d.get("id", str(f.relative_to(DESC_DIR))),
            "ignore": (d.get("tiers", {}) or {}).get("ignore", []) or [],
            "rationale": d.get("_rationale", ""),
            "path": str(f.relative_to(ROOT)),
        })
    return rows


def main():
    rows = collect()
    cat_counts = defaultdict(int)
    total = 0
    unclassified = []

    lines = [
        "# MASK_AUDIT.md",
        "",
        "**Auto-generated** by `python3 tests/parity/scripts/mask_audit.py`.",
        "Run after editing any descriptor and check in the result.",
        "",
        f"Coverage: {len(rows)} parity descriptors.",
        "",
        "Bucket meanings:",
        "- **(a) Non-determinism / RPC envelope.** Always safe to ignore.",
        "- **(b) Intentional C++ enrichment / runtime difference.**",
        "  Documented `_rationale` required.",
        "- **(c) Workspace-state / content-dependent.** Varies with WIP",
        "  or repo content under test.",
        "- **(d) Ranking / scanner-order divergence.** Same multiset,",
        "  different order.",
        "- **(?) Unclassified.** Update rules or per-descriptor `_rationale`.",
        "",
        "## Per-descriptor mask summary",
        "",
        "| Descriptor | # | _rationale | a | b | c | d | ? |",
        "|---|---:|:---:|---:|---:|---:|---:|---:|",
    ]
    for r in rows:
        counts = defaultdict(int)
        for path in r["ignore"]:
            c = classify(path)
            counts[c] += 1
            cat_counts[c] += 1
            total += 1
            if c == "?":
                unclassified.append(f"{r['id']}: {path}")
        ind = "✓" if r["rationale"] else " "
        lines.append(
            f"| `{r['id']}` | {len(r['ignore'])} | {ind} | "
            f"{counts['a']} | {counts['b']} | {counts['c']} | "
            f"{counts['d']} | {counts['?']} |"
        )

    lines += ["",
              f"**Totals:** {total} masks across {len(rows)} descriptors. "
              f"a={cat_counts['a']}, b={cat_counts['b']}, "
              f"c={cat_counts['c']}, d={cat_counts['d']}, "
              f"?={cat_counts['?']}.",
              ""]

    lines += ["## Unclassified fields needing manual review", ""]
    if not unclassified:
        lines.append("None. ✓")
    else:
        for u in sorted(set(unclassified)):
            lines.append(f"- {u}")
    lines.append("")

    need = [r for r in rows if r["ignore"] and not r["rationale"]]
    lines += ["## Descriptors with masks but no `_rationale`", ""]
    if not need:
        lines.append("None. ✓")
    else:
        for r in sorted(need, key=lambda x: x["id"]):
            lines.append(f"- `{r['id']}` ({len(r['ignore'])} masks)")
    lines.append("")

    OUT.write_text("\n".join(lines))
    print(f"wrote {OUT}: {len(rows)} descriptors, {total} masks, "
          f"{len(set(unclassified))} unclassified.")
    return 0 if cat_counts["?"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
