import importlib.util, json, unittest
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
   identity,_=ab.cell_identity(manifest,task,arm,model,rep); records.append({**identity,"status":"answered","score":{"correct":True,"hallucinated":False}})
  result=analysis.analyze(manifest,tasks,records); self.assertTrue(result["complete"]); self.assertTrue(result["interpretation_valid"])
  self.assertFalse(analysis.analyze(manifest,tasks,records[:-1])["complete"])
  with self.assertRaisesRegex(ValueError,"duplicate"): analysis.analyze(manifest,tasks,records+[records[0]])
if __name__=="__main__": unittest.main()
