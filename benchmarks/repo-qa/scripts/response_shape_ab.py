#!/usr/bin/env python3
"""Run the preregistered, canned tool-response-shape experiment."""
from __future__ import annotations

import argparse, hashlib, json, os, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "benchmarks/repo-qa/response-shape"
MANIFEST = BASE / "manifest.json"
TASKS = BASE / "tasks.json"
PROMPT = """Answer the QUESTION using only the selected tool response. Do not use tools or outside knowledge.
Return strict JSON only: {{\"answers\":[\"atomic answer\"],\"evidence\":[\"identifier\"],\"claims\":[\"atomic factual claim\"]}}.
QUESTION:\n{question}\nSELECTED TOOL: {tool}\nTOOL RESPONSE:\n{response}\n"""
FINAL_STATUSES = {"answered", "malformed_answer"}

def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)

def digest(value):
    return "sha256:" + hashlib.sha256((value if isinstance(value, str) else canonical(value)).encode()).hexdigest()

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

def _norm(xs): return {" ".join(x.casefold().split()) for x in xs}

def score_answer(task, answer):
    predicted, expected=_norm(answer["answers"]),_norm(task["expected_answers"])
    evidence, required=_norm(answer["evidence"]),_norm(task["required_evidence"])
    supported=_norm(task["facts"] + task["expected_answers"] + task["required_evidence"])
    claims=_norm(answer["claims"]); unsupported=sorted(claims-supported)
    tp=len(predicted&expected); ep=len(evidence&required)
    omissions=sorted(expected-predicted)
    return {"correct": predicted==expected, "answer_precision": tp/len(predicted) if predicted else (1.0 if not expected else 0.0),
            "answer_recall": tp/len(expected) if expected else (1.0 if not predicted else 0.0),
            "evidence_precision": ep/len(evidence) if evidence else (1.0 if not required else 0.0),
            "evidence_recall": ep/len(required) if required else (1.0 if not evidence else 0.0),
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

def cell_identity(manifest, task, arm, model, repetition):
    prompt=PROMPT.format(question=task["question"],tool=task["tool"],response=task["arms"][arm])
    identity={"schema":"lci.response-shape.cell.v1","task":task["id"],"arm":arm,"model":model,"repetition":repetition,
              "manifest_digest":digest(manifest),"prompt_digest":digest(prompt),"fixture_digest":digest(task),
              "grading_schema":manifest["grading_schema"],"analysis_revision":manifest["analysis_revision"]}
    identity["cell_key"]=digest(identity)[7:]
    return identity,prompt

def write_atomic(path, value):
    path.parent.mkdir(parents=True,exist_ok=True); fd,tmp=tempfile.mkstemp(prefix=path.name,suffix=".tmp",dir=path.parent)
    try:
        with os.fdopen(fd,"w") as h: json.dump(value,h,sort_keys=True); h.write("\n")
        os.replace(tmp,path)
    except BaseException:
        try: os.unlink(tmp)
        except FileNotFoundError: pass
        raise

def reusable(path, identity):
    if not path.exists(): return False
    try: rec=json.loads(path.read_text())
    except (OSError,json.JSONDecodeError) as exc: raise RuntimeError(f"corrupt record {path}: {exc}") from exc
    if any(rec.get(k)!=v for k,v in identity.items()): return False
    return rec.get("status") in FINAL_STATUSES and isinstance(rec.get("answer"),str) and (rec.get("status")!="answered" or isinstance(rec.get("score"),dict))

def execute(provider, manifest, task, arm, model, repetition, out):
    identity,prompt=cell_identity(manifest,task,arm,model,repetition); path=Path(out)/(identity["cell_key"]+".json")
    if reusable(path,identity): return json.loads(path.read_text())
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
    p=argparse.ArgumentParser(); p.add_argument("--out",type=Path,required=True); p.add_argument("--manifest",type=Path,default=MANIFEST); p.add_argument("--tasks",type=Path,default=TASKS); p.add_argument("--dry-run",action="store_true"); p.add_argument("--fake-provider",action="store_true")
    args=p.parse_args(); manifest,tasks=load_inputs(args.manifest,args.tasks); jobs=planned_grid(manifest,tasks)
    if args.dry_run:
        print(json.dumps({"cells":len(jobs),"order":[[t["id"],a,m,r] for t,a,m,r in jobs]},sort_keys=True)); return 0
    if args.fake_provider:
        records=run_grid(DeterministicModelProvider(),manifest,tasks,args.out)
        print(json.dumps({"cells":len(records),"out":str(args.out)},sort_keys=True)); return 0
    p.error("real provider execution is intentionally guarded; configure and invoke a provider adapter explicitly")
if __name__=="__main__": raise SystemExit(main())
