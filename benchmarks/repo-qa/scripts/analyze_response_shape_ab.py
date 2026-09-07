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

SUPPORTED_ANALYSIS_REVISIONS = {"paired-task-v2"}
ARM_ROLE_BASELINE = "compact"
ARM_ROLE_TREATMENT = "labeled"
HALLUCINATION_CAVEAT = (
    "Hallucination is an UPPER BOUND. A claim is judged unsupported unless a frozen fact is "
    "contained in it, so a supported fact re-expressed as different prose still counts against "
    "the arm. The true rate is at or below every number reported here.")

def arm_roles(manifest):
    """Which opaque arm is the baseline (A) and which the treatment (B)."""
    roles={role:arm for arm,role in manifest["arm_mapping"].items()}
    return roles[ARM_ROLE_BASELINE], roles[ARM_ROLE_TREATMENT]

def usable(rec): return bool(rec) and rec.get("status")=="answered" and isinstance(rec.get("score"),dict)

def paired_metric(by_key, manifest, tasks, models, value, require_usable=True):
    """Per-task Arm B minus Arm A effects, with run-to-run dispersion.

    Arm means hide the pairing the design registered: the same task under both
    renderings in the same repetition is the unit, so a task the model simply
    cannot answer cancels instead of inflating both arms. `spread` is the
    dispersion of the per-repetition mean delta -- the run-to-run noise the
    decision rule compares against -- not the spread across tasks, which
    measures task difficulty rather than instability.
    """
    arm_a,arm_b=arm_roles(manifest)
    rep_means=[]; deltas=[]; baseline_successes=0
    for rep in range(1,manifest["repetitions"]+1):
        values=[]
        for task in tasks:
            for model in models:
                a=by_key.get((task["id"],arm_a,model,rep)); b=by_key.get((task["id"],arm_b,model,rep))
                if require_usable and not (usable(a) and usable(b)): continue
                base=value(a); values.append(value(b)-base); baseline_successes+=base
        if values: rep_means.append(statistics.fmean(values)); deltas.extend(values)
    spread=(max(rep_means)-min(rep_means)) if len(rep_means)>1 else None
    pairs=len(deltas)
    # Rule 15: zero spread means UNMEASURED, not certain, and a baseline pinned at
    # floor or ceiling makes any non-zero treatment result clear the bar for free.
    degenerate=pairs==0 or spread in (None,0.0) or baseline_successes in (0,pairs)
    return {"pairs":pairs,"mean_delta":statistics.fmean(deltas) if deltas else None,
            "spread":spread,"rep_means":rep_means,"min_delta":min(deltas) if deltas else None,
            "max_delta":max(deltas) if deltas else None,
            "baseline_successes":baseline_successes,"degenerate":degenerate}

def decide(manifest, correctness, hallucination, completion):
    """Apply the pre-registered practical rule mechanically -- no post-hoc argument."""
    rule=manifest["practical_rule"]
    if correctness["degenerate"]: verdict="unmeasured"
    elif correctness["mean_delta"]>=rule["correctness_delta"] and correctness["mean_delta"]>correctness["spread"]: verdict="b_better"
    elif -correctness["mean_delta"]>=rule["correctness_delta"] and -correctness["mean_delta"]>correctness["spread"]: verdict="a_better"
    else: verdict="no_effect"
    halluc=("unmeasured" if hallucination["pairs"]==0
            else "violated" if hallucination["mean_delta"]>rule["hallucination_noninferiority"] else "within_bound")
    complete=("unmeasured" if completion["pairs"]==0
              else "violated" if completion["mean_delta"]<-rule["completion_noninferiority"] else "within_bound")
    if verdict=="unmeasured" or "unmeasured" in (halluc,complete): adopt="unmeasured"
    elif "violated" in (halluc,complete): adopt="adopt_neither"
    elif verdict=="b_better": adopt="adopt_labeled"
    elif verdict=="a_better": adopt="adopt_compact"
    else: adopt="adopt_neither"
    return {"correctness":verdict,"hallucination":halluc,"completion":complete,"adopt":adopt}

def paired_group(by_key, manifest, tasks, models):
    correctness=paired_metric(by_key,manifest,tasks,models,lambda r:float(bool(r["score"]["correct"])))
    hallucination=paired_metric(by_key,manifest,tasks,models,lambda r:float(bool(r["score"]["hallucinated"])))
    completion=paired_metric(by_key,manifest,tasks,models,lambda r:float(usable(r)),require_usable=False)
    return {"models":sorted(models),"correctness":correctness,"hallucination":hallucination,
            "completion":completion,"decision":decide(manifest,correctness,hallucination,completion)}

def paired_analysis(by_key, manifest, tasks, classes):
    arm_a,arm_b=arm_roles(manifest)
    tiers=defaultdict(list)
    for model,tier in classes.items(): tiers[tier].append(model)
    # Tiers are reported separately and never pooled: the design forbids a single
    # decision over mixed capability.
    return {"arm_a":arm_a,"arm_b":arm_b,"caveat":HALLUCINATION_CAVEAT,
            "by_model":{model:paired_group(by_key,manifest,tasks,[model]) for model in sorted(classes)},
            "by_tier":{tier:paired_group(by_key,manifest,tasks,sorted(models)) for tier,models in sorted(tiers.items())}}

def analyze(manifest,tasks,records):
    revision=manifest.get("analysis_revision")
    if revision not in SUPPORTED_ANALYSIS_REVISIONS:
        raise ValueError(f"unsupported analysis_revision {revision!r}; this analyzer implements {sorted(SUPPORTED_ANALYSIS_REVISIONS)}")
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
        "analysis_revision":revision,"practical_rule":manifest["practical_rule"],
        "paired":paired_analysis(by_key,manifest,tasks,classes),
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
    paired=report.get("paired")
    if paired:
        lines += ["",f"## Paired effects (Arm B `{paired['arm_b']}` minus Arm A `{paired['arm_a']}`)","",
            f"Decision rule applied as pre-registered: {json.dumps(report['practical_rule'],sort_keys=True)}  ",
            f"Analysis revision: `{report.get('analysis_revision')}`","",
            "| Group | Pairs | Correctness Δ | Spread | Hallucination Δ | Completion Δ | Decision |","|---|---:|---:|---:|---:|---:|---|"]
        for scope,rows in (("model",paired["by_model"]),("tier",paired["by_tier"])):
            for name,g in rows.items():
                d=g["decision"]
                lines.append(f"| {scope} `{name}` | {g['correctness']['pairs']} | {num(g['correctness']['mean_delta'])} | "
                             f"{num(g['correctness']['spread'])} | {num(g['hallucination']['mean_delta'])} | "
                             f"{num(g['completion']['mean_delta'])} | {d['adopt']} (correctness {d['correctness']}, "
                             f"hallucination {d['hallucination']}, completion {d['completion']}) |")
        unmeasured=[n for n,g in paired["by_model"].items() if g["correctness"]["degenerate"]]
        if unmeasured:
            lines += ["",f"Degenerate spread or a floor/ceiling-pinned baseline makes these unmeasured, not certain: "
                         f"{', '.join('`'+x+'`' for x in unmeasured)}."]
        lines += ["",f"> {paired['caveat']}"]
    if report["failures"]: lines += ["","## Failures","",*(f"- `{k}`: {v}" for k,v in report["failures"].items())]
    return "\n".join(lines)+"\n"

def write_model_scorecards(directory,report):
    directory.mkdir(parents=True,exist_ok=True)
    for model,arms in report["models"].items():
        slug=model.replace("/","__")
        value={"schema":"lci.response-shape.model-scorecard.v1","model":model,"complete":report["complete"],"shapes":arms}
        (directory/f"{slug}.json").write_text(json.dumps(value,indent=2,sort_keys=True)+"\n")
        paired=report.get("paired") or {}
        subset={**report,"models":{model:arms},"class_rollups":{},
                "paired":{**paired,"by_model":{model:paired.get("by_model",{}).get(model)} if paired else {},"by_tier":{}} if paired else None}
        (directory/f"{slug}.md").write_text(markdown(subset))

def main():
    p=argparse.ArgumentParser(); p.add_argument("--manifest",type=Path,required=True); p.add_argument("--tasks",type=Path,required=True); p.add_argument("--records",type=Path,required=True); p.add_argument("--out",type=Path,required=True); p.add_argument("--markdown",type=Path); p.add_argument("--model-scorecards",type=Path); a=p.parse_args()
    manifest=json.loads(a.manifest.read_text()); tasks=json.loads(a.tasks.read_text())["tasks"]
    records=[json.loads(x.read_text()) for x in sorted(a.records.glob("*.json"))]
    result=analyze(manifest,tasks,records); a.out.write_text(json.dumps(result,indent=2,sort_keys=True)+"\n")
    if a.markdown: a.markdown.write_text(markdown(result))
    if a.model_scorecards: write_model_scorecards(a.model_scorecards,result)
if __name__=="__main__": main()
