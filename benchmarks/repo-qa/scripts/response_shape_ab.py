#!/usr/bin/env python3
"""Run the preregistered, canned tool-response-shape experiment."""
from __future__ import annotations

import argparse, hashlib, importlib.util, json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay_common

_COMPREHENSION_PATH=Path(__file__).with_name("comprehension_ab.py")
_COMPREHENSION_SPEC=importlib.util.spec_from_file_location("response_shape_comprehension_helpers",_COMPREHENSION_PATH)
if _COMPREHENSION_SPEC is None or _COMPREHENSION_SPEC.loader is None: raise ImportError(f"cannot load {_COMPREHENSION_PATH}")
_COMPREHENSION=importlib.util.module_from_spec(_COMPREHENSION_SPEC); _COMPREHENSION_SPEC.loader.exec_module(_COMPREHENSION)
classify_failure=_COMPREHENSION.classify_failure
empty_git_workspace=_COMPREHENSION.empty_git_workspace
parse_events=_COMPREHENSION.parse_events

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

def score_answer(task, answer):
    predicted, expected=_norm(answer["answers"]),_norm(task["expected_answers"])
    evidence, required=_norm(answer["evidence"]),_norm(task["required_evidence"])
    supported=_norm(task["facts"] + task["expected_answers"] + task["required_evidence"])
    claims=_norm(answer["claims"]); unsupported=sorted(claims-supported)
    answer_precision, answer_recall = replay_common.set_precision_recall(predicted, expected)
    evidence_precision, evidence_recall = replay_common.set_precision_recall(evidence, required)
    omissions=sorted(expected-predicted)
    return {"correct": predicted==expected, "answer_precision": answer_precision,
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

class OpenCodeProvider:
    """Run one isolated, tool-disabled OpenCode session per experimental cell."""
    def __init__(self, executable="opencode"):
        self.executable=executable

    def run(self, *, prompt, task, arm, model, timeout):
        del task, arm
        started=time.monotonic()
        with tempfile.TemporaryDirectory(prefix="response-shape-parent-") as parent:
            workspace=empty_git_workspace(Path(parent))
            environment=os.environ.copy()
            environment["OPENCODE_CONFIG"]=str(workspace/"opencode.json")
            environment["XDG_CONFIG_HOME"]=str(workspace/".xdg-config")
            environment["XDG_STATE_HOME"]=str(workspace/".xdg-state")
            environment["XDG_DATA_HOME"]=str(workspace/".xdg-data")
            environment["XDG_CACHE_HOME"]=str(workspace/".xdg-cache")
            shared_cache=Path(os.environ.get("XDG_CACHE_HOME",Path.home()/".cache"))/"opencode"/"models.json"
            shared_auth=Path(os.environ.get("XDG_DATA_HOME",Path.home()/".local/share"))/"opencode"/"auth.json"
            isolated_cache=workspace/".xdg-cache/opencode"; isolated_cache.mkdir(parents=True,exist_ok=True)
            isolated_data=workspace/".xdg-data/opencode"; isolated_data.mkdir(parents=True,exist_ok=True)
            if shared_cache.exists(): shutil.copy2(shared_cache,isolated_cache/"models.json")
            if shared_auth.exists(): shutil.copy2(shared_auth,isolated_data/"auth.json")
            try:
                proc=subprocess.run([self.executable,"run","--format","json","-m",model,prompt],cwd=workspace,
                    env=environment,text=True,capture_output=True,timeout=timeout)
                raw_stdout=proc.stdout; raw_stderr=proc.stderr
                answer,metadata=parse_events(raw_stdout.splitlines())
                failure_text=json.dumps(metadata.get("provider_error"))+"\n"+raw_stderr
                classified=classify_failure(failure_text)
                # Same ladder as comprehension_ab.run_model: rc 124 is the wrapper
                # timeout, any other nonzero rc keeps its code instead of collapsing.
                if metadata.get("malformed_event"): status="malformed_provider_stream"
                elif classified: status=classified
                elif metadata.get("provider_error"): status="provider_error"
                elif proc.returncode == 124: status="provider_timeout"
                elif proc.returncode != 0: status=f"exit_{proc.returncode}"
                elif not answer.strip(): status="empty_answer"
                else: status="answered"
            except subprocess.TimeoutExpired as exc:
                raw_stdout=exc.stdout if isinstance(exc.stdout,str) else ""
                raw_stderr=exc.stderr if isinstance(exc.stderr,str) else ""
                answer,metadata=parse_events(raw_stdout.splitlines())
                status="provider_timeout"
            finally:
                shutil.rmtree(workspace,ignore_errors=True)
        return {"status":status,"answer":answer,"raw_provider_stdout":raw_stdout,
            "provider_stderr_tail":None if status=="answered" else raw_stderr[-STDERR_TAIL_LIMIT:],
            "wall_seconds":round(time.monotonic()-started,3),"failure_reason":None if status=="answered" else status,**metadata}

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

def main():
    p=argparse.ArgumentParser(); p.add_argument("--out",type=Path,required=True); p.add_argument("--manifest",type=Path,default=MANIFEST); p.add_argument("--tasks",type=Path,default=TASKS); p.add_argument("--dry-run",action="store_true"); p.add_argument("--fake-provider",action="store_true"); p.add_argument("--run-provider",action="store_true"); p.add_argument("--opencode",default="opencode")
    args=p.parse_args(); manifest,tasks=load_inputs(args.manifest,args.tasks); jobs=planned_grid(manifest,tasks)
    if args.dry_run:
        print(json.dumps({"cells":len(jobs),"order":[[t["id"],a,m,r] for t,a,m,r in jobs]},sort_keys=True)); return 0
    if args.fake_provider:
        records=run_grid(DeterministicModelProvider(),manifest,tasks,args.out)
        print(json.dumps({"cells":len(records),"out":str(args.out)},sort_keys=True)); return 0
    if args.run_provider:
        records=run_grid(OpenCodeProvider(args.opencode),manifest,tasks,args.out)
        print(json.dumps({"cells":len(records),"out":str(args.out)},sort_keys=True)); return 0
    p.error("real provider execution is intentionally guarded; configure and invoke a provider adapter explicitly")
if __name__=="__main__": raise SystemExit(main())
