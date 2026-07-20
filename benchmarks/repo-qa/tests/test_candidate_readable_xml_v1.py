import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
CANDIDATES = ROOT / "benchmarks/repo-qa/api-replay/candidates"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


codec = load("readable_xml_v1", CANDIDATES / "readable_xml_v1.py")
conformance = load("candidate_conformance_readable_xml", CANDIDATES / "conformance.py")


class ReadableXmlV1Tests(unittest.TestCase):
    def test_shared_value_bank_conformance(self):
        report = conformance.evaluate(codec, conformance.load_bank())
        self.assertTrue(report["summary"]["conformant"], report)

    def test_safe_content_remains_readable_and_escaped(self):
        rendered = codec.encode({"file": "src/a<&b.go", "line": 17, "ok": True})
        self.assertIn('key="file"', rendered)
        self.assertIn("src/a&lt;&amp;b.go", rendered)
        self.assertNotIn("encoding=\"base64\"", rendered)
        self.assertEqual(codec.decode(rendered), {"file": "src/a<&b.go", "line": 17, "ok": True})

    def test_unsafe_strings_and_keys_use_base64_only_where_needed(self):
        value = {
            "readable": "normal\ntext",
            "unsafe\rkey": "safe",
            "unsafe-value": "nul:\x00 lone:\ud800 carriage:\r",
        }
        rendered = codec.encode(value)
        self.assertIn('key="readable"', rendered)
        self.assertIn("normal\ntext", rendered)
        self.assertEqual(rendered.count('key-encoding="base64"'), 1)
        self.assertEqual(rendered.count('encoding="base64"'), 2)
        self.assertEqual(codec.decode(rendered), value)

    def test_root_scalars_empty_containers_and_nested_values(self):
        values = [None, True, False, 0, -0.0, "", [], {}, {"0": [[], {}, None]}]
        for value in values:
            with self.subTest(value=value):
                self.assertEqual(codec.decode(codec.encode(value)), value)

    def test_rejects_malformed_or_ambiguous_xml(self):
        invalid = [
            '<json version="readable-xml-v1"><number>true</number></json>',
            '<json version="readable-xml-v1"><array><member><null /></member></array></json>',
            '<json version="readable-xml-v1"><string encoding="rot13">x</string></json>',
            '<json version="readable-xml-v1"><object><member key="a"><null /></member><member key="a"><null /></member></object></json>',
            '<json version="readable-xml-v1"><null /></json><extra />',
            '<!DOCTYPE json [<!ENTITY x "abc">]><json version="readable-xml-v1"><string>&x;</string></json>',
        ]
        for text in invalid:
            with self.subTest(text=text), self.assertRaises(ValueError):
                codec.decode(text)

    def test_deterministic_key_order(self):
        self.assertEqual(codec.encode({"z": 1, "a": 2}), codec.encode({"a": 2, "z": 1}))


if __name__ == "__main__":
    unittest.main()
