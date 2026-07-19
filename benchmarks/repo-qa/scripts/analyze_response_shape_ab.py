#!/usr/bin/env python3
"""Strict paired analysis for response_shape_ab records."""
import argparse, json, statistics
from collections import defaultdict
from pathlib import Path

def analyze(manifest, tasks, records):
    expected={(t["id"],a,m["id"],r) for t in tasks for a in manifest["arm_mapping"] for m in manifest["models"] for r in range(1,manifest["repetitions"]+1)}
    by_key={}
    for rec in records:
        key=(rec["task"],rec["arm"],rec["model"],rec["repetition"])
        if key in by_key: raise ValueError(f"duplicate cell: {key}")
        if key not in expected: raise ValueError(f"unexpected cell: {key}")
        by_key[key]=rec
    missing=sorted(expected-set(by_key)); groups=defaultdict(list)
    for (task,arm,model,rep),rec in by_key.items(): groups[(model,arm)].append(rec)
    summary={}
    for (model,arm),items in sorted(groups.items()):
        usable=[x for x in items if x.get("status")=="answered" and isinstance(x.get("score"),dict)]
        summary[f"{model}|{arm}"]={"planned":len(items),"usable":len(usable),"completion":len(usable)/len(items),
            "correctness":statistics.fmean(x["score"]["correct"] for x in usable) if usable else None,
            "hallucination":statistics.fmean(x["score"]["hallucinated"] for x in usable) if usable else None}
    return {"schema":"lci.response-shape.analysis.v1","complete":not missing,"missing":missing,"arms":summary,
            "interpretation_valid":not missing and all(x.get("status")=="answered" for x in records)}

def main():
    p=argparse.ArgumentParser(); p.add_argument("--manifest",type=Path,required=True); p.add_argument("--tasks",type=Path,required=True); p.add_argument("--records",type=Path,required=True); p.add_argument("--out",type=Path,required=True); a=p.parse_args()
    manifest=json.loads(a.manifest.read_text()); tasks=json.loads(a.tasks.read_text())["tasks"]
    records=[json.loads(x.read_text()) for x in sorted(a.records.glob("*.json"))]
    result=analyze(manifest,tasks,records); a.out.write_text(json.dumps(result,indent=2,sort_keys=True)+"\n")
if __name__=="__main__": main()
