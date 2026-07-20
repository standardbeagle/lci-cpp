import importlib.util
import json
import random
import unittest
from pathlib import Path


SCRIPT = (
    Path(__file__).parents[1]
    / "api-replay"
    / "candidates"
    / "xml_v1.py"
)
SPEC = importlib.util.spec_from_file_location("candidate_xml_v1", SCRIPT)
xml_v1 = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(xml_v1)


class XmlV1CandidateTests(unittest.TestCase):
    def assert_round_trip(self, value):
        rendered = xml_v1.render_value(value)
        self.assertEqual(xml_v1.invert_to_value(rendered), value)
        self.assertEqual(
            xml_v1.invert_to_json(rendered),
            xml_v1.canonical_json(value),
        )

    def test_root_scalars_and_empty_containers(self):
        for value in [None, True, False, 0, -7, 1.5, -0.0, "", [], {}]:
            with self.subTest(value=value):
                self.assert_round_trip(value)

    def test_nested_values_numeric_and_escaped_keys(self):
        value = {
            "0": "numeric key",
            "": {"quote\"slash\\": [None, True, 2, 3.25]},
            "line\nbreak": {"nested": [[], {}, {"01": False}]},
        }
        self.assert_round_trip(value)

    def test_xml_hostile_and_unicode_strings(self):
        value = {
            "<&\"'": "<&\"'>",
            "control": "\u0000\u0001\u001f",
            "unicode": "snowman ☃ astral \U0001f680",
            "surrogate": "\ud800",
        }
        self.assert_round_trip(value)

    def test_rendering_is_deterministic_for_object_order(self):
        left = xml_v1.render_value({"z": 1, "a": 2})
        right = xml_v1.render_value({"a": 2, "z": 1})
        self.assertEqual(left, right)

    def test_render_json_accepts_every_root_kind(self):
        for source in ["null", '"text"', "42", "[]", '{"1":{}}']:
            with self.subTest(source=source):
                rendered = xml_v1.render_json(source)
                self.assertEqual(
                    json.loads(xml_v1.invert_to_json(rendered)),
                    json.loads(source),
                )

    def test_rejects_ambiguous_or_extended_documents(self):
        invalid = [
            '<json version="1"><object><member key="YQ==" key-encoding="base64"><null /></member><member key="YQ==" key-encoding="base64"><null /></member></object></json>',
            '<json version="1"><string encoding="plain">abc</string></json>',
            '<json version="1"><array><member><null /></member></array></json>',
            '<json version="1"><number>true</number></json>',
            '<json version="1" extra="x"><null /></json>',
            '<!DOCTYPE json [<!ENTITY x "value">]><json version="1"><string encoding="base64">&x;</string></json>',
        ]
        for rendered in invalid:
            with self.subTest(rendered=rendered):
                with self.assertRaises(ValueError):
                    xml_v1.invert_to_value(rendered)

    def test_rejects_non_json_values(self):
        for value in [float("nan"), float("inf"), {1: "bad"}, (1, 2)]:
            with self.subTest(value=value):
                with self.assertRaises((TypeError, ValueError)):
                    xml_v1.render_value(value)

    def test_deterministic_adversarial_value_bank(self):
        rng = random.Random(20260720)
        scalar_bank = [
            None,
            True,
            False,
            0,
            -1,
            2**80,
            -0.0,
            1.25e-40,
            "",
            "0",
            "<&\u0000\udfff\U0001f680",
        ]
        key_bank = ["", "0", "01", "a/b", "a~b", "<&\"", "\u0000", "\ud800"]

        def generate(depth):
            if depth == 0 or rng.randrange(3) == 0:
                return rng.choice(scalar_bank)
            if rng.randrange(2) == 0:
                return [generate(depth - 1) for _ in range(rng.randrange(5))]
            keys = rng.sample(key_bank, rng.randrange(0, 5))
            return {key: generate(depth - 1) for key in keys}

        for index in range(500):
            value = generate(4)
            with self.subTest(index=index):
                self.assert_round_trip(value)


if __name__ == "__main__":
    unittest.main()
