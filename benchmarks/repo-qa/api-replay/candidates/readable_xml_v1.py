"""Readable XML codec with lossless base64 fallback for arbitrary JSON values."""

from __future__ import annotations

import base64
import json
import math
import xml.etree.ElementTree as ET
from typing import Any


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


def _base64(value: str) -> str:
    return base64.b64encode(value.encode("utf-8", errors="surrogatepass")).decode("ascii")


def _unbase64(value: str | None) -> str:
    try:
        raw = base64.b64decode(value or "", validate=True)
    except ValueError as error:
        raise ValueError("invalid base64 text") from error
    return raw.decode("utf-8", errors="surrogatepass")


def _string_element(value: str) -> ET.Element:
    if _safe_text(value):
        node = ET.Element("string")
        node.text = value
    else:
        node = ET.Element("string", {"encoding": "base64"})
        node.text = _base64(value)
    return node


def _element(value: Any) -> ET.Element:
    if value is None:
        return ET.Element("null")
    if isinstance(value, bool):
        node = ET.Element("boolean")
        node.text = "true" if value else "false"
        return node
    if isinstance(value, str):
        return _string_element(value)
    if isinstance(value, int):
        node = ET.Element("number")
        node.text = str(value)
        return node
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("non-finite numbers are not JSON values")
        node = ET.Element("number")
        node.text = json.dumps(value, allow_nan=False)
        return node
    if isinstance(value, list):
        node = ET.Element("array")
        for value_item in value:
            item = ET.SubElement(node, "item")
            item.append(_element(value_item))
        return node
    if isinstance(value, dict):
        if not all(isinstance(key, str) for key in value):
            raise TypeError("JSON object keys must be strings")
        node = ET.Element("object")
        for key in sorted(value):
            if _safe_attribute(key):
                attributes = {"key": key}
            else:
                attributes = {"key": _base64(key), "key-encoding": "base64"}
            member = ET.SubElement(node, "member", attributes)
            member.append(_element(value[key]))
        return node
    raise TypeError(f"unsupported JSON value type: {type(value).__name__}")


def encode(value: Any) -> str:
    root = ET.Element("json", {"version": NAME})
    root.append(_element(value))
    return ET.tostring(root, encoding="unicode", short_empty_elements=True)


def _shape(node: ET.Element, attributes: set[str], children: int | None = None) -> None:
    if set(node.attrib) != attributes:
        raise ValueError(f"unexpected attributes on <{node.tag}>")
    if children is not None and len(node) != children:
        raise ValueError(f"unexpected children on <{node.tag}>")
    if len(node) and (node.text or "").strip():
        raise ValueError(f"mixed content in <{node.tag}>")
    if any((child.tail or "").strip() for child in node):
        raise ValueError(f"mixed content in <{node.tag}>")


def _decode_string(node: ET.Element) -> str:
    if len(node):
        raise ValueError("string cannot have children")
    if not node.attrib:
        value = node.text or ""
        if not _safe_text(value):
            raise ValueError("unsafe plain XML string")
        return value
    _shape(node, {"encoding"}, children=0)
    if node.attrib["encoding"] != "base64":
        raise ValueError("unsupported string encoding")
    return _unbase64(node.text)


def _decode_value(node: ET.Element) -> Any:
    if node.tag == "null":
        _shape(node, set(), children=0)
        if node.text:
            raise ValueError("null cannot contain text")
        return None
    if node.tag == "boolean":
        _shape(node, set(), children=0)
        if node.text not in {"true", "false"}:
            raise ValueError("invalid boolean")
        return node.text == "true"
    if node.tag == "string":
        return _decode_string(node)
    if node.tag == "number":
        _shape(node, set(), children=0)
        if node.text is None:
            raise ValueError("number cannot be empty")
        try:
            value = json.loads(node.text)
        except json.JSONDecodeError as error:
            raise ValueError("invalid JSON number") from error
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError("number element must contain a JSON number")
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError("non-finite numbers are not JSON values")
        return value
    if node.tag == "array":
        _shape(node, set())
        result = []
        for item in node:
            if item.tag != "item":
                raise ValueError("array children must be <item>")
            _shape(item, set(), children=1)
            result.append(_decode_value(item[0]))
        return result
    if node.tag == "object":
        _shape(node, set())
        result = {}
        for member in node:
            if member.tag != "member" or len(member) != 1:
                raise ValueError("object children must be single-value <member> elements")
            if set(member.attrib) == {"key"}:
                key = member.attrib["key"]
                if not _safe_attribute(key):
                    raise ValueError("unsafe plain XML key")
            elif set(member.attrib) == {"key", "key-encoding"}:
                if member.attrib["key-encoding"] != "base64":
                    raise ValueError("unsupported key encoding")
                key = _unbase64(member.attrib["key"])
            else:
                raise ValueError("invalid member attributes")
            _shape(member, set(member.attrib), children=1)
            if key in result:
                raise ValueError("duplicate object key")
            result[key] = _decode_value(member[0])
        return result
    raise ValueError(f"unknown JSON value element <{node.tag}>")


def decode(text: str) -> Any:
    upper = text.upper()
    if "<!DOCTYPE" in upper or "<!ENTITY" in upper:
        raise ValueError("document type and entity declarations are forbidden")
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        raise ValueError("invalid XML candidate") from error
    _shape(root, {"version"}, children=1)
    if root.tag != "json" or root.attrib["version"] != NAME:
        raise ValueError("unsupported XML candidate root or version")
    return _decode_value(root[0])
