#!/usr/bin/env python3
"""Run the preregistered, canned tool-response-shape experiment."""
from __future__ import annotations

import argparse, hashlib, json, os, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay_common

import analyze_response_shape_ab as analysis
import opencode_runner as runner
classify_failure=runner.classify_failure
empty_git_workspace=runner.empty_git_workspace
parse_events=runner.parse_events

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "benchmarks/repo-qa/response-shape"
MANIFEST = BASE / "manifest.json"
TASKS = BASE / "tasks.json"
PROMPT = """Answer the QUESTION using only the selected tool response. Do not use tools or outside knowledge.
Return strict JSON only: {{\"answers\":[\"atomic answer\"],\"evidence\":[\"identifier\"],\"claims\":[\"atomic factual claim\"]}}.
QUESTION:\n{question}\nSELECTED TOOL: {tool}\nTOOL RESPONSE:\n{response}\n"""
# Provider stderr is diagnostic only: keep a bounded tail, never the full stream.
STDERR_TAIL_LIMIT = 512

# The authoritative canonical/digest pair (replay_common) uses ensure_ascii=True;
# the retired local copy used ensure_ascii=False, so digests of values containing
# non-ASCII text change. The committed manifest/tasks are pure ASCII, so no
# committed artifact embeds a divergent digest; runtime cell records from older
# runs of non-ASCII cells would simply re-execute on resume.
canonical = replay_common.canonical
digest = replay_common.digest

def load_inputs(manifest_path=MANIFEST, tasks_path=TASKS):
    manifest, bank = json.loads(Path(manifest_path).read_text()), json.loads(Path(tasks_path).read_text())
    validate_bank(manifest, bank)
    return manifest, bank["tasks"]

def parse_compact(text):
    parts = [p.strip() for p in text.split(";")]
    facts=[]
    if parts[0] != "ok": facts.append("status=" + parts[0])
    for part in parts[1:]:
        if part: facts.append(part)
    return facts

def parse_labeled(text):
    fields={}
    for line in text.splitlines():
        if ":" not in line: raise ValueError("malformed labeled line")
        key,value=line.split(":",1); fields[key.strip()]=value.strip()
    if set(fields) != {"status","key_result","evidence","next_action"}: raise ValueError("wrong labeled fields")
    facts=[]
    if fields["status"] != "ok": facts.append("status=" + fields["status"])
    for key in ("key_result","evidence","next_action"):
        if fields[key]: facts.extend(p.strip() for p in fields[key].split(";") if p.strip())
    return facts

def validate_bank(manifest, bank):
    tasks=bank.get("tasks",[]); arms=set(manifest["arm_mapping"])
    if not 6 <= len(tasks) <= 10: raise ValueError("task bank must contain 6-10 tasks")
    if len({t["id"] for t in tasks}) != len(tasks): raise ValueError("duplicate task id")
    required={"direct_lookup","multi_item","negative_evidence","file_line","null_control"}
    if not required <= {t["stratum"] for t in tasks}: raise ValueError("missing required strata")
    for task in tasks:
        if set(task["arms"]) != arms: raise ValueError(f"{task['id']}: arm mismatch")
        compact=parse_compact(task["arms"]["shape_17"])
        labeled=parse_labeled(task["arms"]["shape_42"])
        if compact != task["facts"] or labeled != task["facts"]: raise ValueError(f"{task['id']}: fact drift")
        validate_answer_key(task)
        a=len(task["arms"]["shape_17"].split()); b=len(task["arms"]["shape_42"].split())
        if abs(a-b) > max(manifest["token_tolerance"]["absolute"], a*manifest["token_tolerance"]["relative"]):
            raise ValueError(f"{task['id']}: token drift")

def validate_answer_key(task):
    """Every expected answer is an atom the rendered facts actually contain.

    This keeps the answer key tied to what both arms render: an expected value
    absent from the fact list would be unanswerable from either response, and a
    sentence-shaped key would re-introduce phrase grading.
    """
    facts=[replay_common.normalize_term(f) for f in task["facts"]]
    for item in task["expected_answers"]:
        value=item if isinstance(item,str) else item.get("value")
        forms=[] if isinstance(item,str) else item.get("accepted_forms",[])
        if not isinstance(value,str) or not value.strip(): raise ValueError(f"{task['id']}: expected answer must carry a value")
        if any(not isinstance(f,str) or not f.strip() for f in forms): raise ValueError(f"{task['id']}: accepted_forms must be non-empty strings")
        term=replay_common.normalize_term(value)
        if not any(term in fact for fact in facts): raise ValueError(f"{task['id']}: expected answer {value!r} is absent from the rendered facts")

def arm_order(task_id, model, repetition, arms):
    ordered=sorted(arms)
    return ordered if int(hashlib.sha256(f"{task_id}|{model}|{repetition}".encode()).hexdigest(),16)%2 == 0 else ordered[::-1]

def parse_answer(text):
    value=json.loads(text)
    if not isinstance(value,dict) or set(value) != {"answers","evidence","claims"}: raise ValueError("wrong answer shape")
    if any(not isinstance(value[k],list) or any(not isinstance(x,str) for x in value[k]) for k in value): raise ValueError("answer fields must be string lists")
    return value

def _norm(xs): return {replay_common.normalize_term(x) for x in xs}

def expected_atoms(task):
    """The answer key as ATOMIC facts, each with its declared accepted forms.

    An expected answer is the fact itself -- a value plus its unit, an
    identifier -- never a sentence wrapped around it. Grading a phrase envelope
    scored the first real strong-tier cell wrong: glm-5.2 answered "30 seconds"
    against an expected "timeout is 30 seconds" and was recorded as an omission
    AND a hallucination, so the published rates measured the grader. Synonymy is
    DECLARED in the bank (`accepted_forms`), never inferred here, so widening
    what counts as correct is a reviewable edit to the answer key.

    Returns [(label, forms)]: the label names the atom in omissions, the forms
    are the normalized strings any of which, contained in a model answer,
    credit it.
    """
    atoms=[]
    for item in task["expected_answers"]:
        value=item if isinstance(item,str) else item["value"]
        forms=[value] if isinstance(item,str) else [value,*item.get("accepted_forms",[])]
        atoms.append((replay_common.normalize_term(value),
                      tuple(replay_common.normalize_term(f) for f in forms)))
    return atoms

def _covers(text, term):
    """A free-text model answer carries `term` when it CONTAINS the fact.

    Containment credits added context ("the configured timeout is 30 seconds")
    around an atomic fact while a changed value ("99 seconds") or a changed unit
    ("30 minutes") contains no form of the atom and still fails. Evidence
    identifiers do NOT go through here -- a file:line is compared exactly.
    """
    return term in text

def _answer_rates(task, predicted):
    """Precision/recall of a model's answers against the atomic answer key."""
    atoms=expected_atoms(task)
    if not predicted and not atoms: return 1.0, 1.0, []
    hit=[label for label,forms in atoms if any(any(f in p for f in forms) for p in predicted)]
    used={p for p in predicted if any(any(f in p for f in forms) for _,forms in atoms)}
    precision=len(used)/len(predicted) if predicted else 0.0
    recall=len(hit)/len(atoms) if atoms else 0.0
    return precision, recall, sorted({label for label,_ in atoms}-set(hit))

def score_answer(task, answer):
    predicted=_norm(answer["answers"])
    evidence, required=_norm(answer["evidence"]),_norm(task["required_evidence"])
    supported=_norm(task["facts"] + task["required_evidence"]) | {f for _,forms in expected_atoms(task) for f in forms}
    claims=_norm(answer["claims"])
    unsupported=sorted(c for c in claims if not any(_covers(c,s) or _covers(s,c) for s in supported))
    answer_precision, answer_recall, omissions = _answer_rates(task, predicted)
    evidence_precision, evidence_recall = replay_common.set_precision_recall(evidence, required)
    return {"correct": not omissions and answer_precision==1.0, "answer_precision": answer_precision,
            "answer_recall": answer_recall,
            "evidence_precision": evidence_precision,
            "evidence_recall": evidence_recall,
            "hallucinated": bool(unsupported), "unsupported_claim_count": len(unsupported), "unsupported_claims": unsupported,
            "omission_count":len(omissions), "omissions":omissions}

class FakeProvider:
    """Hermetic adapter: responses are injected; it has no corpus/provider handle."""
    def __init__(self, answers): self.answers=dict(answers); self.calls=[]
    def run(self, *, prompt, task, arm, model, timeout):
        self.calls.append((task["id"],arm,model)); value=self.answers[(task["id"],arm,model)]
        return value if isinstance(value,dict) else {"status":"answered","answer":value,"tokens":{"input":0,"output":0},"wall_seconds":0}

class DeterministicModelProvider:
    """Hermetic full-grid provider used to exercise multi-model scorecards."""
    def run(self, *, prompt, task, arm, model, timeout):
        del prompt, arm, model, timeout
        values=[label for label,_ in expected_atoms(task)]
        answer={"answers":values,"evidence":task["required_evidence"],"claims":values}
        return {"status":"answered","answer":canonical(answer),"tokens":{"input":len(task["facts"]),"output":len(answer["answers"])+len(answer["evidence"])},"wall_seconds":0.001}

class RecordedStreamProvider:
    """Replay a REAL recorded `opencode run --format json` stream.

    The hand-written fake proves the harness's own branching; it cannot prove
    the harness survives the provider's actual wire shape, because a fixture
    authored beside the parser inherits the parser's blind spots. This adapter
    feeds a committed capture through the SAME `parse_events` the live provider
    uses, so a provider-side shape change (hyphenated part types, a nested
    token block) fails a hermetic test instead of a paid grid run.

    See response-shape/recordings/README.md for capture provenance.
    """

    def __init__(self, recording):
        self.recording=Path(recording)
        self.lines=self.recording.read_text().splitlines()

    def run(self, *, prompt, task, arm, model, timeout):
        del prompt, task, arm, model, timeout
        answer, metadata = parse_events(self.lines)
        status = "answered" if answer.strip() else "empty_answer"
        return {"status":status,"answer":answer,"wall_seconds":0.0,
                "recording":self.recording.name,
                "failure_reason":None if status=="answered" else status,
                "tokens":metadata["tokens"],"provider_error":metadata["provider_error"],
                "malformed_event":metadata["malformed_event"]}


class OpenCodeProvider:
    """Run isolated, tool-disabled OpenCode cells in one per-run workspace.

    The workspace (and its single copy of the shared auth/model state, made by
    the runner's isolated_environment) is created once per run, not once per
    cell: credentials are copied into the 0700 tempdir exactly once and the
    token value is never read or printed by the harness.
    """
    def __init__(self, executable="opencode"):
        self.executable=executable
        self._parent=tempfile.TemporaryDirectory(prefix="response-shape-parent-")
        self.workspace=empty_git_workspace(Path(self._parent.name))

    def run(self, *, prompt, task, arm, model, timeout):
        del task, arm
        result=runner.run_opencode(self.executable,self.workspace,model,prompt,timeout)
        status=result["status"]
        return {"status":status,"answer":result["answer"],"raw_provider_stdout":result["raw_stdout"],
            "provider_stderr_tail":None if status=="answered" else result["raw_stderr"][-STDERR_TAIL_LIMIT:],
            "wall_seconds":result["wall_seconds"],"failure_reason":None if status=="answered" else status,
            "tokens":result["tokens"],"provider_error":result["provider_error"],
            "malformed_event":result["malformed_event"]}

def cell_identity(manifest, task, arm, model, repetition):
    prompt=PROMPT.format(question=task["question"],tool=task["tool"],response=task["arms"][arm])
    identity={"schema":"lci.response-shape.cell.v1","task":task["id"],"arm":arm,"model":model,"repetition":repetition,
              "manifest_digest":digest(manifest),"prompt_digest":digest(prompt),"fixture_digest":digest(task),
              "grading_schema":manifest["grading_schema"],"analysis_revision":manifest["analysis_revision"]}
    identity["cell_key"]=digest(identity)[7:]
    return identity,prompt

def write_atomic(path, value):
    replay_common.write_atomic(Path(path), json.dumps(value, sort_keys=True) + "\n")

def reusable(path, identity, retryable):
    """Skip an existing cell unless its recorded OUTCOME is declared retryable.

    Rule 13: existence is not finality. A provider outage, timeout or quota kill
    is recorded like a result, so a ledger keyed on existence freezes that cell
    permanently; a ledger that re-runs every non-answered record instead pays
    again for outcomes that will never change (a malformed final answer, an
    empty answer, a harness error). Only `manifest.retryable_statuses`, frozen
    before execution, re-executes.
    """
    if not path.exists(): return False
    try: rec=json.loads(path.read_text())
    except (OSError,json.JSONDecodeError) as exc: raise RuntimeError(f"corrupt record {path}: {exc}") from exc
    if any(rec.get(k)!=v for k,v in identity.items()): return False
    if rec.get("status") in set(retryable): return False
    if not isinstance(rec.get("answer"),str): return False
    return rec.get("status")!="answered" or isinstance(rec.get("score"),dict)

def execute(provider, manifest, task, arm, model, repetition, out):
    identity,prompt=cell_identity(manifest,task,arm,model,repetition); path=Path(out)/(identity["cell_key"]+".json")
    if reusable(path,identity,manifest.get("retryable_statuses",[])): return json.loads(path.read_text())
    if path.exists():
        attempts=Path(out)/"attempts"; attempts.mkdir(parents=True,exist_ok=True)
        number=1
        while (attempts/f"{identity['cell_key']}.attempt-{number}.json").exists(): number+=1
        os.replace(path,attempts/f"{identity['cell_key']}.attempt-{number}.json")
    run=provider.run(prompt=prompt,task=task,arm=arm,model=model,timeout=manifest["timeout_seconds"])
    status=run.get("status","harness_error"); rec={**identity,**run,"score":None,"failure_reason":run.get("failure_reason")}
    if status=="answered":
        try: rec["score"]=score_answer(task,parse_answer(rec["answer"])); rec["completion"]=True
        except (ValueError,TypeError,json.JSONDecodeError) as exc: rec.update(status="malformed_answer",score=None,completion=False,failure_reason=str(exc))
    else: rec["completion"]=False
    write_atomic(path,rec); return rec

def grid_models(manifest, models=None):
    """The model roster this grid runs, defaulting to the frozen manifest's own.

    A caller-supplied roster (R2's models.json) does NOT enter the manifest, and
    so does not enter `manifest_digest`: a cell for a model already in the frozen
    manifest keeps the byte-identical cell key it had under R1, and an R1 record
    stays reusable. Only `model` -- already an identity field -- distinguishes
    the added cells.
    """
    return list(models) if models else [m["id"] for m in manifest["models"]]

def planned_grid(manifest,tasks,models=None):
    jobs=[]
    for task in tasks:
        for model in grid_models(manifest,models):
            for rep in range(1,manifest["repetitions"]+1):
                jobs.extend((task,arm,model,rep) for arm in arm_order(task["id"],model,rep,manifest["arm_mapping"]))
    return jobs

def run_grid(provider, manifest, tasks, out, models=None):
    return [execute(provider,manifest,task,arm,model,rep,out) for task,arm,model,rep in planned_grid(manifest,tasks,models)]

def plan_run_report(provider, manifest, tasks, out, models=None, classes=None):
    """Plan, run and report in one call — the single entry point `--report` uses."""
    out=Path(out); records=run_grid(provider,manifest,tasks,out/"cells",models)
    report=analysis.analyze(manifest,tasks,records,models=models,classes=classes)
    replay_common.write_atomic(out/"analysis.json", json.dumps(report,indent=2,sort_keys=True)+"\n")
    replay_common.write_atomic(out/"report.md", analysis.markdown(report))
    return {"cells":len(records),"out":str(out),"report":report}


def main():
    p=argparse.ArgumentParser(); p.add_argument("--out",type=Path,required=True); p.add_argument("--manifest",type=Path,default=MANIFEST); p.add_argument("--tasks",type=Path,default=TASKS); p.add_argument("--dry-run",action="store_true"); p.add_argument("--fake-provider",action="store_true"); p.add_argument("--run-provider",action="store_true"); p.add_argument("--opencode",default="opencode"); p.add_argument("--report",action="store_true",help="plan, run and write analysis.json + report.md in one command")
    args=p.parse_args(); manifest,tasks=load_inputs(args.manifest,args.tasks); jobs=planned_grid(manifest,tasks)
    if args.dry_run:
        print(json.dumps({"cells":len(jobs),"order":[[t["id"],a,m,r] for t,a,m,r in jobs]},sort_keys=True)); return 0
    provider=DeterministicModelProvider() if args.fake_provider else OpenCodeProvider(args.opencode) if args.run_provider else None
    if provider is not None:
        if args.report:
            result=plan_run_report(provider,manifest,tasks,args.out)
            print(json.dumps({"cells":result["cells"],"out":result["out"],"complete":result["report"]["complete"]},sort_keys=True)); return 0
        records=run_grid(provider,manifest,tasks,args.out)
        print(json.dumps({"cells":len(records),"out":str(args.out)},sort_keys=True)); return 0
    p.error("real provider execution is intentionally guarded; configure and invoke a provider adapter explicitly")
if __name__=="__main__": raise SystemExit(main())
