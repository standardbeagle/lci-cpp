#!/usr/bin/env python3
"""Produce strict JSON and Markdown model response-format scorecards."""
import argparse, json, statistics
from collections import defaultdict
from pathlib import Path

def mean(items, getter): return statistics.fmean(getter(x) for x in items) if items else None

def metrics(items):
    usable=[x for x in items if x.get("status")=="answered" and isinstance(x.get("score"),dict)]
    token=lambda x:x.get("tokens") or {}
    # omission_count is a COUNT; 1-answer_recall is a RATE. Mixing units in one
    # mean is meaningless, so average only records carrying the count and report
    # how many usable records were excluded for lacking it.
    with_omissions=[x for x in usable if "omission_count" in x["score"]]
    return {"planned":len(items),"usable":len(usable),"failures":len(items)-len(usable),
        "completion":len(usable)/len(items) if items else None,
        "correctness":mean(usable,lambda x:x["score"]["correct"]),
        "evidence_use":mean(usable,lambda x:x["score"]["evidence_recall"]),
        "hallucination":mean(usable,lambda x:x["score"]["hallucinated"]),
        "omissions":mean(with_omissions,lambda x:x["score"]["omission_count"]),
        "omissions_excluded_missing_field":len(usable)-len(with_omissions),
        "latency_seconds":mean(usable,lambda x:float(x.get("wall_seconds",0))),
        "input_tokens":mean(usable,lambda x:float(token(x).get("input",0))),
        "output_tokens":mean(usable,lambda x:float(token(x).get("output",0)))}

def analyze(manifest,tasks,records):
    expected={(t["id"],a,m["id"],r) for t in tasks for a in manifest["arm_mapping"] for m in manifest["models"] for r in range(1,manifest["repetitions"]+1)}
    by_key={}
    for rec in records:
        key=(rec["task"],rec["arm"],rec["model"],rec["repetition"])
        if key in by_key: raise ValueError(f"duplicate cell: {key}")
        if key not in expected: raise ValueError(f"unexpected cell: {key}")
        by_key[key]=rec
    missing=sorted(expected-set(by_key)); groups=defaultdict(list)
    classes={m["id"]:m.get("tier","unclassified") for m in manifest["models"]}
    for (_,arm,model,_),rec in by_key.items(): groups[(model,arm)].append(rec)
    cells={f"{model}|{arm}":metrics(items) for (model,arm),items in sorted(groups.items())}
    models={model:{arm:cells.get(f"{model}|{arm}",metrics([])) for arm in manifest["arm_mapping"]} for model in classes}
    class_groups=defaultdict(list)
    for (model,arm),items in groups.items(): class_groups[(classes[model],arm)].extend(items)
    failures=defaultdict(int)
    for rec in records:
        if rec.get("status")!="answered": failures[rec.get("status","unknown")]+=1
    return {"schema":"lci.response-shape.scorecard.v1","complete":not missing,"missing":missing,"cells":cells,"models":models,
        "class_rollups":{f"{c}|{a}":metrics(v) for (c,a),v in sorted(class_groups.items())},
        "failures":dict(sorted(failures.items())),"interpretation_valid":not missing and not failures}

def markdown(report):
    pct=lambda x:"—" if x is None else f"{100*x:.1f}%"
    num=lambda x:"—" if x is None else f"{x:.2f}"
    lines=["# Response-format comprehension scorecard","",f"Complete: **{'yes' if report['complete'] else 'no'}**  ",f"Interpretation valid: **{'yes' if report['interpretation_valid'] else 'no'}**","",
        "| Model | Shape | Cells | Correct | Evidence | Hallucination | Omissions | Completion | Latency (s) | Tokens in/out |","|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for model,arms in report["models"].items():
        for arm,m in arms.items(): lines.append(f"| {model} | {arm} | {m['usable']}/{m['planned']} | {pct(m['correctness'])} | {pct(m['evidence_use'])} | {pct(m['hallucination'])} | {num(m['omissions'])} | {pct(m['completion'])} | {num(m['latency_seconds'])} | {num(m['input_tokens'])}/{num(m['output_tokens'])} |")
    lines += ["","## Model-class rollups",""]
    for key,m in report["class_rollups"].items(): lines.append(f"- `{key}`: correctness {pct(m['correctness'])}, evidence {pct(m['evidence_use'])}, completion {pct(m['completion'])}")
    if report["failures"]: lines += ["","## Failures","",*(f"- `{k}`: {v}" for k,v in report["failures"].items())]
    return "\n".join(lines)+"\n"

def write_model_scorecards(directory,report):
    directory.mkdir(parents=True,exist_ok=True)
    for model,arms in report["models"].items():
        slug=model.replace("/","__")
        value={"schema":"lci.response-shape.model-scorecard.v1","model":model,"complete":report["complete"],"shapes":arms}
        (directory/f"{slug}.json").write_text(json.dumps(value,indent=2,sort_keys=True)+"\n")
        subset={**report,"models":{model:arms},"class_rollups":{}}
        (directory/f"{slug}.md").write_text(markdown(subset))

def main():
    p=argparse.ArgumentParser(); p.add_argument("--manifest",type=Path,required=True); p.add_argument("--tasks",type=Path,required=True); p.add_argument("--records",type=Path,required=True); p.add_argument("--out",type=Path,required=True); p.add_argument("--markdown",type=Path); p.add_argument("--model-scorecards",type=Path); a=p.parse_args()
    manifest=json.loads(a.manifest.read_text()); tasks=json.loads(a.tasks.read_text())["tasks"]
    records=[json.loads(x.read_text()) for x in sorted(a.records.glob("*.json"))]
    result=analyze(manifest,tasks,records); a.out.write_text(json.dumps(result,indent=2,sort_keys=True)+"\n")
    if a.markdown: a.markdown.write_text(markdown(result))
    if a.model_scorecards: write_model_scorecards(a.model_scorecards,result)
if __name__=="__main__": main()
