#!/usr/bin/env python3
"""Re-pin every committed task bank's manifest_ref.forge_version.

One-shot, deterministic rewrite used for the direction-(b) decision on
worktrack task 01M1VMC1DEDYTPXRYNK1VA2CG5: the banks' anchors were authored
against forge_version 1 corpora, so the label must say 1. The edit is textual
and surgical -- it rewrites ONLY the forge_version line inside the top-level
"manifest_ref" block, leaving all other bytes (ordering, spacing) untouched.
Any file whose manifest_ref does not hold exactly one forge_version line with
the expected old value is a hard error, never a silent skip.

Usage: pin_bank_forge_version.py [--from 2] [--to 1]  (defaults shown)
"""

import argparse
import glob
import json
import os
import re
import sys

BENCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BANK_DIRS = (
    os.path.join(BENCH_ROOT, "exploration", "tasks"),
    os.path.join(BENCH_ROOT, "edits", "tasks"),
)


def repin_file(path, old, new):
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r'"manifest_ref"\s*:\s*\{', text)
    if match is None:
        raise SystemExit(f"{path}: no manifest_ref block")
    end = text.index("\n  }", match.end())
    block = text[match.start() : end]
    lines = re.findall(r'"forge_version"\s*:\s*"([^"]+)"', block)
    if lines != [old]:
        raise SystemExit(
            f"{path}: manifest_ref forge_version lines {lines!r} != [{old!r}]"
        )
    # Confirm against the parsed structure too: the textual block must be the
    # real manifest_ref, not a string literal lookalike.
    task = json.loads(text)
    if task["manifest_ref"]["forge_version"] != old:
        raise SystemExit(
            f"{path}: parsed manifest_ref.forge_version "
            f"{task['manifest_ref']['forge_version']!r} != {old!r}"
        )
    updated = text[: match.start()] + block.replace(
        f'"forge_version": "{old}"', f'"forge_version": "{new}"'
    ) + text[end:]
    if updated == text:
        raise SystemExit(f"{path}: rewrite produced no change")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(updated)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--from", dest="old", default="2")
    parser.add_argument("--to", dest="new", default="1")
    args = parser.parse_args(argv)
    paths = []
    for directory in BANK_DIRS:
        paths.extend(glob.glob(os.path.join(directory, "*.json")))
    if not paths:
        raise SystemExit("no bank files found; wrong bench root?")
    for path in sorted(paths):
        repin_file(path, args.old, args.new)
    print(f"re-pinned {len(paths)} bank files {args.old} -> {args.new}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
