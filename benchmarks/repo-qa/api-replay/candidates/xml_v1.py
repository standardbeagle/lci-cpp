"""Deterministic, invertible XML representation of arbitrary JSON values.

This is an exploratory candidate codec. It deliberately represents JSON's data
model rather than attempting to infer domain-specific element names.
"""

from __future__ import annotations

import base64
import json
import math
import xml.etree.ElementTree as ET
from typing import Any


_ROOT_TAG = "json"
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


def _encode_text(value: str) -> str:
    # surrogatepass is needed because JSON permits escaped lone surrogates while
    # XML 1.0 cannot contain them directly.
    raw = value.encode("utf-8", errors="surrogatepass")
    return base64.b64encode(raw).decode("ascii")


def _decode_text(value: str | None) -> str:
    try:
        raw = base64.b64decode(value or "", validate=True)
    except ValueError as error:
        raise ValueError("invalid base64 text in XML candidate") from error
    return raw.decode("utf-8", errors="surrogatepass")


def _element(value: Any) -> ET.Element:
    if value is None:
        return ET.Element("null")
    if isinstance(value, bool):
        node = ET.Element("boolean")
        node.text = "true" if value else "false"
        return node
    if isinstance(value, str):
        node = ET.Element("string", {"encoding": "base64"})
        node.text = _encode_text(value)
        return node
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
        for item in value:
            wrapper = ET.SubElement(node, "item")
            wrapper.append(_element(item))
        return node
    if isinstance(value, dict):
        if not all(isinstance(key, str) for key in value):
            raise TypeError("JSON object keys must be strings")
        node = ET.Element("object")
        for key in sorted(value):
            wrapper = ET.SubElement(
                node,
                "member",
                {"key": _encode_text(key), "key-encoding": "base64"},
            )
            wrapper.append(_element(value[key]))
        return node
    raise TypeError(f"unsupported JSON value type: {type(value).__name__}")


def render_value(value: Any) -> str:
    root = ET.Element(_ROOT_TAG, {"version": _VERSION})
    root.append(_element(value))
    return ET.tostring(root, encoding="unicode", short_empty_elements=True)


def render_json(source: str) -> str:
    return render_value(json.loads(source))


def _require_shape(
    node: ET.Element,
    *,
    attributes: set[str] = frozenset(),
    children: int | None = None,
) -> None:
    if set(node.attrib) != attributes:
        raise ValueError(f"unexpected attributes on <{node.tag}>")
    if children is not None and len(node) != children:
        raise ValueError(f"unexpected children on <{node.tag}>")
    if len(node) and (node.text or "").strip():
        raise ValueError(f"unexpected mixed content in <{node.tag}>")
    for child in node:
        if (child.tail or "").strip():
            raise ValueError(f"unexpected mixed content in <{node.tag}>")


def _value(node: ET.Element) -> Any:
    if node.tag == "null":
        _require_shape(node, children=0)
        if node.text:
            raise ValueError("null cannot contain text")
        return None
    if node.tag == "boolean":
        _require_shape(node, children=0)
        if node.text not in {"true", "false"}:
            raise ValueError("invalid boolean")
        return node.text == "true"
    if node.tag == "string":
        _require_shape(node, attributes={"encoding"}, children=0)
        if node.attrib["encoding"] != "base64":
            raise ValueError("unsupported string encoding")
        return _decode_text(node.text)
    if node.tag == "number":
        _require_shape(node, children=0)
        if node.text is None:
            raise ValueError("number cannot be empty")
        try:
            value = json.loads(node.text)
        except json.JSONDecodeError as error:
            raise ValueError("invalid JSON number") from error
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError("number element does not contain a JSON number")
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError("non-finite numbers are not JSON values")
        return value
    if node.tag == "array":
        _require_shape(node)
        values = []
        for item in node:
            if item.tag != "item":
                raise ValueError("array children must be <item>")
            _require_shape(item, children=1)
            values.append(_value(item[0]))
        return values
    if node.tag == "object":
        _require_shape(node)
        value = {}
        for member in node:
            if member.tag != "member":
                raise ValueError("object children must be <member>")
            _require_shape(member, attributes={"key", "key-encoding"}, children=1)
            if member.attrib["key-encoding"] != "base64":
                raise ValueError("unsupported key encoding")
            key = _decode_text(member.attrib["key"])
            if key in value:
                raise ValueError("duplicate object key")
            value[key] = _value(member[0])
        return value
    raise ValueError(f"unknown JSON value element <{node.tag}>")


def invert_to_value(rendered: str) -> Any:
    if "<!DOCTYPE" in rendered.upper() or "<!ENTITY" in rendered.upper():
        raise ValueError("document type and entity declarations are forbidden")
    try:
        root = ET.fromstring(rendered)
    except ET.ParseError as error:
        raise ValueError("invalid XML candidate") from error
    if root.tag != _ROOT_TAG:
        raise ValueError("candidate root must be <json>")
    _require_shape(root, attributes={"version"}, children=1)
    if root.attrib["version"] != _VERSION:
        raise ValueError("unsupported XML candidate version")
    return _value(root[0])


def invert_to_json(rendered: str) -> str:
    return canonical_json(invert_to_value(rendered))


def encode(value: Any) -> str:
    """Shared candidate-harness adapter."""
    return render_value(value)


def decode(text: str) -> Any:
    """Shared candidate-harness adapter."""
    return invert_to_value(text)
