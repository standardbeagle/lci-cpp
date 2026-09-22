#!/usr/bin/env python3
"""Deterministically select the stage-2 pilot's claim subset.

A pilot cannot afford the full 30-claim bank, and a hand-picked subset is a
researcher degree of freedom: whoever chooses the claims can choose the
result. So the subset is DERIVED, by a rule fixed before any cell runs, and
this script is the rule -- re-running it must reproduce the committed list
byte for byte.

The rule: one claim per author category (all six), balanced two-per-corpus so
no corpus carries the pilot, chosen by a seed-7 shuffle (the seed the corpora
themselves are pinned at) and the first assignment that satisfies the balance.
Ties are broken by claim id, so the result does not depend on filesystem
order.
"""

import argparse
import glob
import json
import os
import random

BENCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TASKS_DIR = os.path.join(BENCH_ROOT, "exploration", "tasks")
SEED = 7
PER_CORPUS = 2


def load_bank(tasks_dir=TASKS_DIR):
    bank = []
    for path in sorted(glob.glob(os.path.join(tasks_dir, "*.json"))):
        with open(path) as handle:
            task = json.load(handle)
        bank.append({
            "id": task["id"],
            "corpus": task.get("corpus") or task["manifest_ref"]["corpus_id"],
            "category": task["author"]["category"],
            "verdict": task["author"]["verdict"],
        })
    return bank


def select(bank, seed=SEED, per_corpus=PER_CORPUS):
    """First balanced assignment under a seeded shuffle, or ValueError."""
    categories = sorted({row["category"] for row in bank})
    corpora = sorted({row["corpus"] for row in bank})
    if len(categories) * 1 != len(corpora) * per_corpus:
        raise ValueError(
            f"{len(categories)} categories cannot be spread {per_corpus} per corpus "
            f"over {len(corpora)} corpora; the balance rule needs restating")
    rng = random.Random(seed)
    candidates = {}
    for category in categories:
        rows = sorted((r for r in bank if r["category"] == category),
                      key=lambda r: r["id"])
        rng.shuffle(rows)
        candidates[category] = rows

    chosen = {}

    def place(index, used):
        if index == len(categories):
            return True
        category = categories[index]
        for row in candidates[category]:
            if used.get(row["corpus"], 0) >= per_corpus:
                continue
            chosen[category] = row
            used[row["corpus"]] = used.get(row["corpus"], 0) + 1
            if place(index + 1, used):
                return True
            used[row["corpus"]] -= 1
            del chosen[category]
        return False

    if not place(0, {}):
        raise ValueError("no balanced assignment exists for this bank")
    return [chosen[c] for c in categories]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tasks-dir", default=TASKS_DIR)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    picked = select(load_bank(args.tasks_dir))
    if args.json:
        print(json.dumps(picked, indent=2, sort_keys=True))
        return 0
    for row in picked:
        print(f"{row['category']:15} {row['corpus']:13} {row['verdict']:12} {row['id']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
