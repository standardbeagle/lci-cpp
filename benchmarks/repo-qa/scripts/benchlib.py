"""Shared helpers for the repo-QA benchmark: config parsing, result IO."""

import json
import os
import re
import tempfile

BENCH_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_PATH = os.path.join(BENCH_ROOT, "config.kdl")
QUESTIONS_DIR = os.path.join(BENCH_ROOT, "questions")
RESULTS_DIR = os.path.join(BENCH_ROOT, "results")
WORK_DIR = os.path.join(BENCH_ROOT, ".work")

_TOKEN_RE = re.compile(
    r'"(?:[^"\\]|\\.)*"'   # quoted string
    r"|[^\s{}=]+"           # bare word / number / key
    r"|[{}=]"
)


def _lex(line):
    return _TOKEN_RE.findall(line)


def _val(tok):
    if tok.startswith('"'):
        return json.loads(tok)
    try:
        return int(tok)
    except ValueError:
        return tok


def parse_kdl(path):
    """Parse the flat KDL subset used by config.kdl.

    Returns {section: [node, ...]} where node = {"name", "args", "props"}.
    Top-level nodes with braces become sections; their children are nodes.
    """
    sections = {}
    current = None
    with open(path) as f:
        for raw in f:
            line = raw.split("//")[0].strip()
            if not line:
                continue
            toks = _lex(line)
            if toks[-1] == "{":
                current = toks[0]
                sections[current] = []
                continue
            if toks[0] == "}":
                current = None
                continue
            if current is None:
                raise ValueError(f"stray node outside section: {line}")
            node = {"name": toks[0], "args": [], "props": {}}
            i = 1
            while i < len(toks):
                if i + 2 <= len(toks) and i + 1 < len(toks) and toks[i + 1] == "=":
                    node["props"][toks[i]] = _val(toks[i + 2])
                    i += 3
                else:
                    node["args"].append(_val(toks[i]))
                    i += 1
            sections[current].append(node)
    return sections


class Config:
    def __init__(self, path=CONFIG_PATH):
        s = parse_kdl(path)
        self.defaults = {n["name"]: n["args"][0] for n in s["defaults"]}
        self.repos = {n["args"][0]: n["props"] for n in s["repos"]}
        self.models = {n["args"][0]: n["props"] for n in s["models"]}
        self.tiers = {str(n["args"][0]): n["props"] for n in s["tiers"]}

    def tier(self, name):
        t = self.tiers[str(name)]
        return {
            "repos": t["repos"].split(","),
            "models": t["models"].split(","),
            "difficulties": t["difficulties"].split(","),
            "reps": int(t["reps"]),
        }


def load_questions(repo):
    path = os.path.join(QUESTIONS_DIR, f"{repo}.json")
    with open(path) as f:
        data = json.load(f)
    for q in data["questions"]:
        for field in ("id", "difficulty", "question", "gold_answer", "must_mention"):
            if field not in q:
                raise ValueError(f"{path}: question missing {field}")
    return data["questions"]


def result_name(repo, model_alias, variant, qid, rep):
    return f"{repo}__{model_alias}__{variant}__{qid}__r{rep}.json"


def write_json_atomic(path, obj):
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(obj, f, indent=2)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def iter_results(run_dir):
    for name in sorted(os.listdir(run_dir)):
        if not name.endswith(".json") or name.count("__") != 4:
            continue
        path = os.path.join(run_dir, name)
        with open(path) as f:
            yield path, json.load(f)


def fact_accuracy(answer, must_mention):
    """Fraction of fact groups satisfied; a group matches if any regex hits."""
    if not must_mention:
        return None, []
    hits = []
    for group in must_mention:
        ok = any(re.search(rx, answer, re.IGNORECASE | re.DOTALL) for rx in group)
        hits.append(ok)
    return sum(hits) / len(hits), hits
