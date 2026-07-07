#!/usr/bin/env python3
"""Validate repo-QA annotation manifests through the LCI MCP tool.

This checks the real benchmark workspaces, not synthetic unit fixtures:
each manifest entry is queried through semantic_annotations by every declared
label and category, then matched back to the expected symbol/path/metadata.
"""

import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench
import benchlib as bl


def load_manifest_entries(repo):
    root = os.path.join(bl.BENCH_ROOT, "annotations", repo)
    if not os.path.isdir(root):
        return []
    entries = []
    for name in sorted(os.listdir(root)):
        if not name.endswith(".json"):
            continue
        path = os.path.join(root, name)
        with open(path) as f:
            data = json.load(f)
        anns = data if isinstance(data, list) else data.get("annotations", [])
        for item in anns:
            if isinstance(item, dict):
                item = dict(item)
                item["_manifest"] = os.path.relpath(path, bl.BENCH_ROOT)
                entries.append(item)
    return entries


def call_semantic_annotations_batch(lci_bin, workspace, queries):
    requests = []
    for idx, (key, value) in enumerate(queries, start=1):
        requests.append(
            {
                "jsonrpc": "2.0",
                "id": idx,
                "method": "tools/call",
                "params": {
                    "name": "semantic_annotations",
                    "arguments": {key: value},
                },
            }
        )
    proc = subprocess.run(
        [lci_bin, "mcp"],
        cwd=workspace,
        input="".join(json.dumps(request) + "\n" for request in requests),
        capture_output=True,
        text=True,
        timeout=120,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"lci mcp failed in {workspace}: exit={proc.returncode}\n{proc.stderr}"
        )
    lines = [line for line in proc.stdout.splitlines() if line.strip()]
    if len(lines) != len(requests):
        raise RuntimeError(f"lci mcp produced no output in {workspace}")
    responses = {}
    for line in lines:
        outer = json.loads(line)
        if "error" in outer:
            raise RuntimeError(f"MCP error in {workspace}: {outer['error']}")
        content = outer["result"]["content"]
        text = content[0]["text"]
        responses[outer["id"]] = json.loads(text)
    return {
        query: responses[idx]
        for idx, query in enumerate(queries, start=1)
    }


def expected_labels(item):
    labels = item.get("labels", [])
    if isinstance(labels, str):
        return [labels]
    return list(labels)


def expected_tags(item):
    tags = item.get("tags", {})
    return tags if isinstance(tags, dict) else {}


def find_match(response, item):
    wanted_symbol = item.get("symbol") or item.get("name")
    wanted_file = item.get("file") or item.get("path")
    annotations = response.get("annotations", [])
    for ann in annotations:
        if ann.get("symbol_name") != wanted_symbol:
            continue
        if ann.get("file_path") != wanted_file:
            continue
        return ann
    return None


def validate_entry(responses, repo, item):
    labels = expected_labels(item)
    category = item.get("category", "")
    queries = [("label", label) for label in labels]
    if category:
        queries.append(("category", category))
    if not queries:
        return [f"{repo}: {item['_manifest']}: entry has no labels/category"]

    failures = []
    for key, value in queries:
        response = responses[(key, value)]
        match = find_match(response, item)
        where = f"{repo}:{item['_manifest']} {key}={value}"
        if match is None:
            failures.append(
                f"{where}: expected {item.get('symbol') or item.get('name')} "
                f"in {item.get('file') or item.get('path')}; got "
                f"{response.get('annotations', [])}"
            )
            continue

        if category and match.get("category") != category:
            failures.append(
                f"{where}: category {match.get('category')!r} != {category!r}"
            )

        actual_labels = set(match.get("direct_labels", []))
        missing_labels = [label for label in labels if label not in actual_labels]
        if missing_labels:
            failures.append(f"{where}: missing labels {missing_labels}")

        actual_tags = match.get("tags", {})
        for tag_key, expected_value in expected_tags(item).items():
            if actual_tags.get(tag_key) != expected_value:
                failures.append(
                    f"{where}: tag {tag_key!r} {actual_tags.get(tag_key)!r} "
                    f"!= {expected_value!r}"
                )

        line = item.get("line")
        if isinstance(line, int) and line > 0 and match.get("line") != line:
            failures.append(f"{where}: line {match.get('line')} != {line}")

    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repos",
        default=",".join(sorted(os.listdir(os.path.join(bl.BENCH_ROOT, "annotations")))),
        help="comma-separated repo names; defaults to every annotations/<repo>",
    )
    parser.add_argument("--lci-bin", default=None)
    args = parser.parse_args()

    cfg = bl.Config()
    lci_bin = args.lci_bin or cfg.defaults["lci-bin"]
    repos = [repo for repo in args.repos.split(",") if repo]

    total_entries = 0
    total_queries = 0
    failures = []
    for repo in repos:
        entries = load_manifest_entries(repo)
        if not entries:
            failures.append(f"{repo}: no annotation manifest entries found")
            continue
        workspace = bench.ensure_workspace(cfg, repo, "lci-ann")
        unique_queries = []
        seen = set()
        for item in entries:
            for label in expected_labels(item):
                query = ("label", label)
                if query not in seen:
                    seen.add(query)
                    unique_queries.append(query)
            if item.get("category"):
                query = ("category", item["category"])
                if query not in seen:
                    seen.add(query)
                    unique_queries.append(query)
        responses = call_semantic_annotations_batch(lci_bin, workspace, unique_queries)
        for item in entries:
            labels = expected_labels(item)
            total_entries += 1
            total_queries += len(labels) + (1 if item.get("category") else 0)
            failures.extend(validate_entry(responses, repo, item))

    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        print(
            f"annotation validation failed: {len(failures)} failure(s), "
            f"{total_entries} entries, {total_queries} queries"
        )
        return 1

    print(
        f"annotation validation passed: {total_entries} entries, "
        f"{total_queries} semantic_annotations queries across {len(repos)} repos"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
