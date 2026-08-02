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
if __name__=="__main__": unittest.main()
