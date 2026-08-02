"""Readable XML codec with lossless base64 fallback for arbitrary JSON values.

Document structure lives in xml_core; this module supplies the
plain-where-safe / base64-where-needed string and key-encoding policy.
"""

from __future__ import annotations

import importlib.util
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

_SOURCE = Path(__file__).with_name("xml_core.py")
_SPEC = importlib.util.spec_from_file_location("lci_xml_candidate_core", _SOURCE)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load shared XML core: {_SOURCE}")
_core = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_core)

NAME = "readable-xml-v1"


def _xml_char(codepoint: int) -> bool:
    return (
        codepoint in (0x09, 0x0A, 0x0D)
        or 0x20 <= codepoint <= 0xD7FF
        or 0xE000 <= codepoint <= 0xFFFD
        or 0x10000 <= codepoint <= 0x10FFFF
    )


def _safe_text(value: str) -> bool:
    # XML parsers normalize carriage returns even in element content.
    return "\r" not in value and all(_xml_char(ord(character)) for character in value)


def _safe_attribute(value: str) -> bool:
    # Literal XML attribute whitespace is normalized by conforming parsers.
    return not any(character in "\t\n\r" for character in value) and _safe_text(value)


def _string_element(value: str) -> ET.Element:
    if _safe_text(value):
        node = ET.Element("string")
        node.text = value
    else:
        node = ET.Element("string", {"encoding": "base64"})
        node.text = _core.encode_base64(value)
    return node


def _decode_string(node: ET.Element) -> str:
    if len(node):
        raise ValueError("string cannot have children")
    if not node.attrib:
        value = node.text or ""
        if not _safe_text(value):
            raise ValueError("unsafe plain XML string")
        return value
    _core.require_shape(node, {"encoding"}, children=0)
    if node.attrib["encoding"] != "base64":
        raise ValueError("unsupported string encoding")
    return _core.decode_base64(node.text)


def _member_attributes(key: str) -> dict[str, str]:
    if _safe_attribute(key):
        return {"key": key}
    return {"key": _core.encode_base64(key), "key-encoding": "base64"}


def _decode_member_key(member: ET.Element) -> str:
    if set(member.attrib) == {"key"}:
        key = member.attrib["key"]
        if not _safe_attribute(key):
            raise ValueError("unsafe plain XML key")
        return key
    if set(member.attrib) == {"key", "key-encoding"}:
        if member.attrib["key-encoding"] != "base64":
            raise ValueError("unsupported key encoding")
        return _core.decode_base64(member.attrib["key"])
    raise ValueError("invalid member attributes")


def encode(value: Any) -> str:
    return _core.build_document(
        value,
        version=NAME,
        string_element=_string_element,
        member_attributes=_member_attributes,
    )


def decode(text: str) -> Any:
    return _core.parse_document(
        text,
        version=NAME,
        decode_string=_decode_string,
        decode_member_key=_decode_member_key,
    )
