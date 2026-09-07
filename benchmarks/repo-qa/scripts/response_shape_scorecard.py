#!/usr/bin/env python3
"""Per-model response-format comprehension scorecard.

R1 (`response_shape_ab.py`) froze one prompt, one fact bank, two response
shapes, one grading contract and a resume ledger, and proved them on the two
models named in the manifest. This driver points that same machinery at a
CONFIGURABLE model roster and publishes per-model / per-shape scorecards.

Two properties are load-bearing and are tested rather than asserted here:

* **The frozen manifest does not move.** The roster is a separate file, so a
  cell for a model already in the manifest keeps the byte-identical `cell_key`
  it had under R1 and an already-paid R1 record stays reusable. Only `model` --
  an identity field since R1 -- separates the added cells.
* **The hermetic provider replays real recorded streams.** Rule 16: a fixture
  authored beside the parser agrees with the parser. The fake models' event
  envelopes are the committed `opencode run --format json` captures, verbatim;
  only the model's answer text is synthesised, so a provider-side wire change
  breaks a hermetic test instead of a paid grid.

Real provider execution stays guarded: `run --real-provider` refuses without
`--allow-real-provider`, and writes nothing before it refuses.
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import analyze_response_shape_ab as analysis
import opencode_runner as runner
import response_shape_ab as ab

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "benchmarks/repo-qa/response-shape"
MODELS = BASE / "models.json"
FAKE_MODELS = BASE / "models.fake.json"
RECORDINGS = BASE / "recordings"
SCHEMA = "lci.response-shape.models.v1"


def load_models(path=MODELS):
    """Load and CHECK the model roster.

    A roster is configuration that decides what gets paid for and how results
    are grouped, so every failure mode is refused loudly: a bare model name
    (opencode resolves `-m` differently for a bare name than for a full
    `provider/model` id), an undeclared tier (which would silently create a
    one-model class), a duplicate id (which would double-count a model in its
    rollup), and an unprobed id (rule 10b: a model id claimed from memory is
    not evidence -- cite the `opencode models` probe or the recording).
    """
    roster = json.loads(Path(path).read_text())
    if roster.get("schema") != SCHEMA:
        raise ValueError(f"{path}: expected schema {SCHEMA!r}, got {roster.get('schema')!r}")
    classes, models = roster.get("classes") or [], roster.get("models") or []
    if not models:
        raise ValueError(f"{path}: roster is empty")
    seen = set()
    for model in models:
        identifier = model.get("id")
        if not isinstance(identifier, str) or identifier.count("/") != 1 or not all(identifier.split("/")):
            raise ValueError(f"{path}: {identifier!r} is not a full provider/model id")
        if identifier in seen:
            raise ValueError(f"{path}: duplicate model id {identifier!r}")
        seen.add(identifier)
        if model.get("tier") not in classes:
            raise ValueError(f"{path}: {identifier} declares tier {model.get('tier')!r}, not one of {classes}")
        if not isinstance(model.get("id_probe"), str) or not model["id_probe"].strip():
            raise ValueError(f"{path}: {identifier} cites no id probe")
    return roster


def model_ids(roster):
    return [m["id"] for m in roster["models"]]


def model_classes(roster):
    return {m["id"]: m["tier"] for m in roster["models"]}


def _answer(task, behaviour):
    """The answer text a fake model produces for one task.

    The three behaviours are the answer SHAPE classes rule 16 names -- faithful,
    terse, prose-wrapped -- so the hermetic grid exercises the grader's real
    decision boundary rather than three copies of a correct answer.
    """
    atoms = [label for label, _ in ab.expected_atoms(task)]
    evidence = list(task["required_evidence"])
    if behaviour == "faithful":
        return {"answers": atoms, "evidence": evidence, "claims": atoms}
    if behaviour == "terse":
        kept = atoms[:1]
        return {"answers": kept, "evidence": evidence, "claims": kept}
    if behaviour == "wrapped":
        wrapped = [f"the answer is {atom}" for atom in atoms]
        return {"answers": wrapped, "evidence": evidence,
                "claims": wrapped + ["this looks like a routine configuration value"]}
    raise ValueError(f"unknown fake behaviour {behaviour!r}")


class FakeMultiModelProvider:
    """Hermetic multi-model provider replaying committed real event streams.

    It holds no corpus, no credentials and no subprocess: each cell is the named
    recording's own line sequence with the text part's payload replaced, fed
    through the SAME `opencode_runner.parse_events` the live provider uses.
    """

    def __init__(self, roster, recordings=RECORDINGS):
        self.recordings = Path(recordings)
        self.models = {m["id"]: m for m in roster["models"]}
        for model in self.models.values():
            for field in ("recording", "behaviour"):
                if not model.get(field):
                    raise ValueError(f"fake model {model['id']} declares no {field}")
        self.calls = []

    def stream(self, task, arm, model):
        """The recorded envelope, verbatim, carrying this cell's answer text."""
        spec = self.models[model]
        lines = (self.recordings / spec["recording"]).read_text().splitlines()
        answer = ab.canonical(_answer(task, spec["behaviour"]))
        out, replaced = [], False
        for line in lines:
            event = json.loads(line)
            part = event.get("part") or {}
            if part.get("type") == "text":
                part["text"] = answer if not replaced else ""
                replaced = True
                out.append(json.dumps(event, separators=(",", ":"), sort_keys=False))
            else:
                out.append(line)
        if not replaced:
            raise ValueError(f"recording {spec['recording']} carries no text part")
        return out

    def run(self, *, prompt, task, arm, model, timeout):
        del prompt, timeout
        self.calls.append((task["id"], arm, model))
        answer, metadata = runner.parse_events(self.stream(task, arm, model))
        status = "answered" if answer.strip() else "empty_answer"
        return {"status": status, "answer": answer, "wall_seconds": 0.0,
                "recording": self.models[model]["recording"],
                "failure_reason": None if status == "answered" else status,
                "tokens": metadata["tokens"], "provider_error": metadata["provider_error"],
                "malformed_event": metadata["malformed_event"]}


def plan(roster, manifest=None, tasks=None):
    manifest, tasks = (manifest, tasks) if manifest else ab.load_inputs()
    jobs = ab.planned_grid(manifest, tasks, models=model_ids(roster))
    return {"cells": len(jobs), "models": model_ids(roster),
            "order": [[t["id"], a, m, r] for t, a, m, r in jobs]}


def run_report(provider, roster, out, manifest=None, tasks=None):
    """Run every planned cell, analyse, and publish the scorecards."""
    manifest, tasks = (manifest, tasks) if manifest else ab.load_inputs()
    out = Path(out)
    result = ab.plan_run_report(provider, manifest, tasks, out,
                               models=model_ids(roster), classes=model_classes(roster))
    analysis.write_scorecards(out / "scorecards", result["report"])
    return result


def plan_run_report_roster(models_path=FAKE_MODELS, out=None):
    """The whole fake pipeline in one call; `out=None` keeps it in a tempdir."""
    roster = load_models(models_path)
    provider = FakeMultiModelProvider(roster)
    if out is not None:
        return run_report(provider, roster, out)
    with tempfile.TemporaryDirectory(prefix="response-shape-scorecard-") as directory:
        return run_report(provider, roster, directory)


def report_only(cells, out, roster):
    manifest, tasks = ab.load_inputs()
    records = [json.loads(p.read_text()) for p in sorted(Path(cells).glob("*.json"))]
    report = analysis.analyze(manifest, tasks, records,
                             models=model_ids(roster), classes=model_classes(roster))
    analysis.write_scorecards(Path(out), report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("plan", "run", "report"):
        child = sub.add_parser(name)
        child.add_argument("--models", type=Path, default=MODELS)
        if name != "plan":
            child.add_argument("--out", type=Path, required=True)
        if name == "run":
            child.add_argument("--fake-provider", action="store_true",
                               help="hermetic multi-model provider replaying committed recordings")
            child.add_argument("--real-provider", action="store_true",
                               help="call the real provider (requires --allow-real-provider)")
            child.add_argument("--allow-real-provider", action="store_true",
                               help="explicit guard: this run may spend real provider calls")
            child.add_argument("--opencode", default="opencode")
        if name == "report":
            child.add_argument("--cells", type=Path, required=True)
    args = parser.parse_args(argv)
    roster = load_models(args.models)

    if args.command == "plan":
        print(json.dumps(plan(roster), sort_keys=True))
        return 0
    if args.command == "report":
        report = report_only(args.cells, args.out, roster)
        print(json.dumps({"complete": report["complete"], "accounting": report["accounting"]}, sort_keys=True))
        return 0

    if args.real_provider and not args.allow_real_provider:
        # Refuse BEFORE creating an output tree: a guarded run that has already
        # written a workspace is indistinguishable from a partial paid run.
        parser.error("real provider execution is guarded; pass --allow-real-provider to spend calls")
    if args.real_provider:
        provider = ab.OpenCodeProvider(args.opencode)
    elif args.fake_provider:
        provider = FakeMultiModelProvider(roster)
    else:
        parser.error("choose a provider: --fake-provider, or --real-provider with --allow-real-provider")
    result = run_report(provider, roster, args.out)
    print(json.dumps({"cells": result["cells"], "out": result["out"],
                      "complete": result["report"]["complete"],
                      "accounting": result["report"]["accounting"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
