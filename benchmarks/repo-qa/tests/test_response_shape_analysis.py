import importlib.util, json, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path); mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod
ab=load("rsab",ROOT/"benchmarks/repo-qa/scripts/response_shape_ab.py")
analysis=load("rsa",ROOT/"benchmarks/repo-qa/scripts/analyze_response_shape_ab.py")
class AnalysisTest(unittest.TestCase):
 def test_complete_grid_and_missing_failure(self):
  manifest,tasks=ab.load_inputs(); records=[]
  for task,arm,model,rep in ab.planned_grid(manifest,tasks):
   identity,_=ab.cell_identity(manifest,task,arm,model,rep); records.append({**identity,"status":"answered","score":{"correct":True,"hallucinated":False,"evidence_recall":1.0,"answer_recall":1.0,"omission_count":0},"tokens":{"input":10,"output":4},"wall_seconds":0.1})
  result=analysis.analyze(manifest,tasks,records); self.assertTrue(result["complete"]); self.assertTrue(result["interpretation_valid"])
  self.assertEqual(set(result["models"]),{m["id"] for m in manifest["models"]}); self.assertIn("Model-class rollups",analysis.markdown(result))
  with tempfile.TemporaryDirectory() as d:
   analysis.write_model_scorecards(Path(d),result); self.assertEqual(len(list(Path(d).glob("*.json"))),2)
  self.assertFalse(analysis.analyze(manifest,tasks,records[:-1])["complete"])
  with self.assertRaisesRegex(ValueError,"duplicate"): analysis.analyze(manifest,tasks,records+[records[0]])
 def test_omissions_mean_uses_only_counts_and_notes_dropped_records(self):
  with_count={"status":"answered","score":{"correct":True,"hallucinated":False,"evidence_recall":1.0,"answer_recall":1.0,"omission_count":2},"tokens":{},"wall_seconds":0.1}
  without_count={"status":"answered","score":{"correct":True,"hallucinated":False,"evidence_recall":1.0,"answer_recall":0.5},"tokens":{},"wall_seconds":0.1}
  m=analysis.metrics([with_count,without_count])
  self.assertEqual(m["omissions"],2.0)  # never blended with the 0.5 recall RATE
  self.assertEqual(m["omissions_excluded_missing_field"],1)
  empty=analysis.metrics([without_count]); self.assertIsNone(empty["omissions"])

def build(manifest,tasks,verdict):
 """Synthetic ledger: `verdict(task_id,arm,model,rep)` -> (correct,hallucinated) or None for a provider failure."""
 records=[]
 for task,arm,model,rep in ab.planned_grid(manifest,tasks):
  identity,_=ab.cell_identity(manifest,task,arm,model,rep); value=verdict(task["id"],arm,model,rep)
  if value is None:
   records.append({**identity,"status":"provider_timeout","score":None,"tokens":{},"wall_seconds":0.0}); continue
  correct,hallucinated=value
  records.append({**identity,"status":"answered","tokens":{"input":10,"output":4},"wall_seconds":0.1,
   "score":{"correct":correct,"hallucinated":hallucinated,"evidence_recall":1.0,"answer_recall":1.0,"answer_precision":1.0,"omission_count":0}})
 return records

class PairedAnalysisTest(unittest.TestCase):
 def setUp(self): self.manifest,self.tasks=ab.load_inputs(); self.ids=[t["id"] for t in self.tasks]
 def _paired(self,verdict,manifest=None):
  manifest=manifest or self.manifest
  return analysis.analyze(manifest,self.tasks,build(manifest,self.tasks,verdict))["paired"]

 def _lift(self,per_rep):
  """Arm A correct on the first two tasks; arm B additionally correct on `per_rep[rep]` more."""
  def verdict(task_id,arm,model,rep):
   index=self.ids.index(task_id); baseline=index<2
   if arm=="shape_17": return (baseline,False)
   return (baseline or index<2+per_rep[rep],False)
  return verdict

 def test_paired_effect_beyond_run_to_run_spread_is_credited(self):
  paired=self._paired(self._lift({1:4,2:3}))
  for model,cell in paired["by_model"].items():
   self.assertEqual(cell["correctness"]["pairs"],12,model)
   self.assertGreater(cell["correctness"]["spread"],0.0)
   self.assertEqual(cell["decision"]["correctness"],"b_better",model)
   self.assertEqual(cell["decision"]["adopt"],"adopt_labeled",model)
  self.assertEqual(set(paired["by_tier"]),{"weak","strong"})

 def test_gap_inside_the_run_to_run_spread_is_not_a_lift(self):
  paired=self._paired(self._lift({1:1,2:0}))
  for model,cell in paired["by_model"].items():
   self.assertLess(cell["correctness"]["mean_delta"],cell["correctness"]["spread"])
   self.assertEqual(cell["decision"]["correctness"],"no_effect",model)
   self.assertEqual(cell["decision"]["adopt"],"adopt_neither",model)

 def test_degenerate_spread_refuses_a_decision(self):
  paired=self._paired(self._lift({1:2,2:2}))
  for model,cell in paired["by_model"].items():
   self.assertEqual(cell["correctness"]["spread"],0.0)
   self.assertTrue(cell["correctness"]["degenerate"])
   self.assertEqual(cell["decision"]["correctness"],"unmeasured",model)
   self.assertEqual(cell["decision"]["adopt"],"unmeasured",model)

 def test_floor_pinned_baseline_refuses_a_decision(self):
  def verdict(task_id,arm,model,rep):
   return (False,False) if arm=="shape_17" else (self.ids.index(task_id)<(4 if rep==1 else 3),False)
  paired=self._paired(verdict)
  for model,cell in paired["by_model"].items():
   self.assertEqual(cell["correctness"]["baseline_successes"],0,model)
   self.assertEqual(cell["decision"]["correctness"],"unmeasured",model)

 def test_hallucination_non_inferiority_blocks_adoption(self):
  lift=self._lift({1:4,2:3})
  def verdict(task_id,arm,model,rep):
   correct,_=lift(task_id,arm,model,rep)
   return (correct, arm=="shape_42" and self.ids.index(task_id)<3)
  paired=self._paired(verdict)
  for model,cell in paired["by_model"].items():
   self.assertEqual(cell["decision"]["correctness"],"b_better",model)
   self.assertEqual(cell["decision"]["hallucination"],"violated",model)
   self.assertEqual(cell["decision"]["adopt"],"adopt_neither",model)

 def test_report_applies_the_manifest_rule_and_prints_the_upper_bound_caveat(self):
  report=analysis.analyze(self.manifest,self.tasks,build(self.manifest,self.tasks,self._lift({1:4,2:3})))
  self.assertEqual(report["practical_rule"],self.manifest["practical_rule"])
  self.assertEqual(report["analysis_revision"],self.manifest["analysis_revision"])
  text=analysis.markdown(report)
  self.assertIn("Paired effects",text)
  self.assertIn(analysis.HALLUCINATION_CAVEAT,text)
  self.assertIn("adopt_labeled",text)

 def test_unknown_analysis_revision_is_rejected(self):
  manifest=dict(self.manifest,analysis_revision="paired-task-v0")
  with self.assertRaisesRegex(ValueError,"analysis_revision"):
   analysis.analyze(manifest,self.tasks,[])

 def test_provider_failures_leave_the_pair_unusable_not_incorrect(self):
  def verdict(task_id,arm,model,rep):
   if task_id==self.ids[0] and arm=="shape_42": return None
   return (True,False) if self.ids.index(task_id)%2 else (rep==1,False)
  paired=self._paired(verdict)
  for cell in paired["by_model"].values():
   self.assertEqual(cell["correctness"]["pairs"],10)
   self.assertLess(cell["completion"]["mean_delta"],0.0)

if __name__=="__main__": unittest.main()
