"""Deterministic, invertible XML representation of arbitrary JSON values.

This is an exploratory candidate codec. It deliberately represents JSON's data
model rather than attempting to infer domain-specific element names. Document
structure lives in xml_core; this module supplies the always-base64
string/key-encoding policy.
"""

from __future__ import annotations

import importlib.util
import json
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

_SOURCE = Path(__file__).with_name("xml_core.py")
_SPEC = importlib.util.spec_from_file_location("lci_xml_candidate_core", _SOURCE)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load shared XML core: {_SOURCE}")
_core = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_core)

_VERSION = "1"
NAME = "typed-xml-v1"


def canonical_json(value: Any) -> str:
    """Return the repository's compact canonical JSON spelling."""
    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    )


def _string_element(value: str) -> ET.Element:
    node = ET.Element("string", {"encoding": "base64"})
    node.text = _core.encode_base64(value)
    return node


def _decode_string(node: ET.Element) -> str:
    _core.require_shape(node, attributes={"encoding"}, children=0)
    if node.attrib["encoding"] != "base64":
        raise ValueError("unsupported string encoding")
    return _core.decode_base64(node.text)


def _member_attributes(key: str) -> dict[str, str]:
    return {"key": _core.encode_base64(key), "key-encoding": "base64"}


def _decode_member_key(member: ET.Element) -> str:
    if set(member.attrib) != {"key", "key-encoding"}:
        raise ValueError("unexpected attributes on <member>")
    if member.attrib["key-encoding"] != "base64":
        raise ValueError("unsupported key encoding")
    return _core.decode_base64(member.attrib["key"])


def render_value(value: Any) -> str:
    return _core.build_document(
        value,
        version=_VERSION,
        string_element=_string_element,
        member_attributes=_member_attributes,
    )


def render_json(source: str) -> str:
    return render_value(json.loads(source))


def invert_to_value(rendered: str) -> Any:
    return _core.parse_document(
        rendered,
        version=_VERSION,
        decode_string=_decode_string,
        decode_member_key=_decode_member_key,
    )


def invert_to_json(rendered: str) -> str:
    return canonical_json(invert_to_value(rendered))


def encode(value: Any) -> str:
    """Shared candidate-harness adapter."""
    return render_value(value)


def decode(text: str) -> Any:
    """Shared candidate-harness adapter."""
    return invert_to_value(text)
