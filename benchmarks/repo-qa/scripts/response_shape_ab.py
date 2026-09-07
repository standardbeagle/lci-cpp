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
FINAL_STATUSES = {"answered", "malformed_answer"}
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
        a=len(task["arms"]["shape_17"].split()); b=len(task["arms"]["shape_42"].split())
        if abs(a-b) > max(manifest["token_tolerance"]["absolute"], a*manifest["token_tolerance"]["relative"]):
            raise ValueError(f"{task['id']}: token drift")

def arm_order(task_id, model, repetition, arms):
    ordered=sorted(arms)
    return ordered if int(hashlib.sha256(f"{task_id}|{model}|{repetition}".encode()).hexdigest(),16)%2 == 0 else ordered[::-1]

def parse_answer(text):
    value=json.loads(text)
    if not isinstance(value,dict) or set(value) != {"answers","evidence","claims"}: raise ValueError("wrong answer shape")
    if any(not isinstance(value[k],list) or any(not isinstance(x,str) for x in value[k]) for k in value): raise ValueError("answer fields must be string lists")
    return value

def _norm(xs): return {replay_common.normalize_term(x) for x in xs}

def _covers(text, term):
    """A free-text model answer carries `term` when it CONTAINS the fact verbatim.

    Exact set equality graded phrasing, not correctness: a real cell answering
    "The configured timeout is 30 seconds" against the expected
    "timeout is 30 seconds" scored incorrect AND hallucinated, because the
    extra words made the string unequal. Containment credits the added context
    while still rejecting a changed VALUE ("timeout is 99 seconds" does not
    contain the expected term), which the discrimination tests pin in both
    directions. Evidence identifiers stay exact — a file:line is not prose.
    """
    return term in text

def _matched(predicted, truth):
    """Split predicted/truth into matched sets under containment."""
    hit_truth={t for t in truth if any(_covers(p,t) for p in predicted)}
    hit_pred={p for p in predicted if any(_covers(p,t) for t in truth)}
    return hit_pred, hit_truth

def _rates(predicted, truth):
    if not predicted and not truth: return 1.0, 1.0
    hit_pred, hit_truth=_matched(predicted, truth)
    precision=len(hit_pred)/len(predicted) if predicted else 0.0
    recall=len(hit_truth)/len(truth) if truth else 0.0
    return precision, recall

def score_answer(task, answer):
    predicted, expected=_norm(answer["answers"]),_norm(task["expected_answers"])
    evidence, required=_norm(answer["evidence"]),_norm(task["required_evidence"])
    supported=_norm(task["facts"] + task["expected_answers"] + task["required_evidence"])
    claims=_norm(answer["claims"])
    unsupported=sorted(c for c in claims if not any(_covers(c,s) or _covers(s,c) for s in supported))
    answer_precision, answer_recall = _rates(predicted, expected)
    evidence_precision, evidence_recall = replay_common.set_precision_recall(evidence, required)
    _, hit_expected=_matched(predicted, expected)
    omissions=sorted(expected-hit_expected)
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
        answer={"answers":task["expected_answers"],"evidence":task["required_evidence"],"claims":task["expected_answers"]}
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

def reusable(path, identity):
    if not path.exists(): return False
    try: rec=json.loads(path.read_text())
    except (OSError,json.JSONDecodeError) as exc: raise RuntimeError(f"corrupt record {path}: {exc}") from exc
    if any(rec.get(k)!=v for k,v in identity.items()): return False
    return rec.get("status") in FINAL_STATUSES and isinstance(rec.get("answer"),str) and (rec.get("status")!="answered" or isinstance(rec.get("score"),dict))

def execute(provider, manifest, task, arm, model, repetition, out):
    identity,prompt=cell_identity(manifest,task,arm,model,repetition); path=Path(out)/(identity["cell_key"]+".json")
    if reusable(path,identity): return json.loads(path.read_text())
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

def planned_grid(manifest,tasks):
    jobs=[]
    for task in tasks:
        for model in [m["id"] for m in manifest["models"]]:
            for rep in range(1,manifest["repetitions"]+1):
                jobs.extend((task,arm,model,rep) for arm in arm_order(task["id"],model,rep,manifest["arm_mapping"]))
    return jobs

def run_grid(provider, manifest, tasks, out):
    return [execute(provider,manifest,task,arm,model,rep,out) for task,arm,model,rep in planned_grid(manifest,tasks)]

def plan_run_report(provider, manifest, tasks, out):
    """Plan, run and report in one call — the single entry point `--report` uses."""
    out=Path(out); records=run_grid(provider,manifest,tasks,out/"cells")
    report=analysis.analyze(manifest,tasks,records)
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
