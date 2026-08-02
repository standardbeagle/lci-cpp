import importlib.util
import json
import math
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "benchmarks/repo-qa/api-replay/candidates/semi_structured.py"
SPEC = importlib.util.spec_from_file_location("semi_structured", MODULE)
codec = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(codec)


ADVERSARIAL_VALUES = [
    None, True, False, 0, -17, 1.5, -0.0, "",
    "line one\nline two\t:[]{}\\\"", "lone surrogates \ud800 \udfff",
    [], {}, [None, [], {}, "0"],
    {"0": "numeric key", "01": 1, "": "empty key",
     "a/b~c": "path syntax", "\n:": "delimiters"},
    {"nested": [{"0": [True, False]}, {}, []],
     "unicode": "snowman ☃ and astral 🚀"},
]


class SemiStructuredCandidateTest(unittest.TestCase):
    def test_both_codecs_round_trip_adversarial_json_values(self):
        pairs = [
            (codec.encode_path_records, codec.decode_path_records),
            (codec.encode_tagged_blocks, codec.decode_tagged_blocks),
        ]
        for encode, decode in pairs:
            for value in ADVERSARIAL_VALUES:
                with self.subTest(codec=encode.__name__, value=value):
                    self.assertEqual(decode(encode(value)), value)

    def test_encodings_are_deterministic_across_object_insertion_order(self):
        left = {"z": 1, "a": {"y": 2, "x": 3}}
        right = {"a": {"x": 3, "y": 2}, "z": 1}
        self.assertEqual(codec.encode_path_records(left), codec.encode_path_records(right))
        self.assertEqual(codec.encode_tagged_blocks(left), codec.encode_tagged_blocks(right))

    def test_numeric_object_key_remains_distinct_from_array_index(self):
        value = {"0": ["object-key then array-index"]}
        rendered = codec.encode_path_records(value)
        self.assertIn('["0",0]', rendered)
        self.assertEqual(codec.decode_path_records(rendered), value)

    def test_path_decoder_rejects_missing_and_unreachable_records(self):
        rendered = codec.encode_path_records([1])
        with self.assertRaises(ValueError):
            codec.decode_path_records(rendered.replace("A1:1", "A1:2", 1))
        with self.assertRaises(ValueError):
            codec.decode_path_records(rendered + 'R5:["x"]Z0:\n')

    def test_tagged_decoder_rejects_trailing_or_bad_counts(self):
        rendered = codec.encode_tagged_blocks([])
        with self.assertRaises(ValueError):
            codec.decode_tagged_blocks(rendered + "NULL\n")
        with self.assertRaises(ValueError):
            codec.decode_tagged_blocks(rendered.replace("ARRAY 0", "ARRAY 1"))

    def test_decoders_reject_non_ascii_and_non_canonical_integer_frames(self):
        # str.isdigit() accepts non-ASCII digits and leading zeros; framing
        # integers must be canonical ASCII so every text has one valid spelling.
        rendered_path = codec.encode_path_records({"a": [None]})
        for mutated in (
            rendered_path.replace("R2:", "R٢:", 1),   # Arabic-Indic digit
            rendered_path.replace("R2:", "R02:", 1),        # leading zero frame
            rendered_path.replace("A1:1", "A2:01", 1),      # leading-zero size
            rendered_path.replace("A1:1", "A1:١", 1),  # non-ASCII size
        ):
            self.assertNotEqual(mutated, rendered_path)
            with self.assertRaises(ValueError):
                codec.decode_path_records(mutated)
        rendered_blocks = codec.encode_tagged_blocks({"a": [None]})
        for mutated in (
            rendered_blocks.replace("ARRAY 1", "ARRAY 01", 1),
            rendered_blocks.replace("ARRAY 1", "ARRAY ١", 1),
            rendered_blocks.replace("KEY 3:", "KEY 03:", 1),
            rendered_blocks.replace("KEY 3:", "KEY ٣:", 1),
        ):
            self.assertNotEqual(mutated, rendered_blocks)
            with self.assertRaises(ValueError):
                codec.decode_tagged_blocks(mutated)

    def test_rejects_values_outside_json_domain(self):
        for encode in (codec.encode_path_records, codec.encode_tagged_blocks):
            with self.subTest(codec=encode.__name__):
                with self.assertRaises(ValueError):
                    encode(math.inf)
                with self.assertRaises(ValueError):
                    encode(math.nan)
                with self.assertRaises(TypeError):
                    encode({1: "integer key"})
                with self.assertRaises(TypeError):
                    encode((1, 2))

    def test_both_adapters_pass_the_shared_conformance_bank(self):
        harness = ROOT / "benchmarks/repo-qa/api-replay/candidates/conformance.py"
        directory = MODULE.parent
        for name in ("path_records_v1.py", "tagged_blocks_v1.py"):
            with self.subTest(candidate=name):
                completed = subprocess.run(
                    ["python3", str(harness), str(directory / name)],
                    check=True, capture_output=True, text=True,
                )
                report = json.loads(completed.stdout)
                self.assertTrue(report["summary"]["conformant"])


if __name__ == "__main__":
    unittest.main()
