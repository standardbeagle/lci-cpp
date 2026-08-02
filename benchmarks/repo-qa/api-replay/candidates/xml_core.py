"""Shared JSON<->XML codec core for the XML candidate family.

Both XML candidates (typed-xml-v1, readable-xml-v1) share the whole document
structure — <json version=...> root, null/boolean/number/array/object shapes,
determinism, doctype/entity hardening — and differ only in their string and
object-key encoding policy. The policy is injected as callables; each candidate
module supplies its own string/key handlers and keeps its public contract.
"""

from __future__ import annotations

import base64
import json
import math
import xml.etree.ElementTree as ET
from typing import Any, Callable

_ROOT_TAG = "json"

StringElement = Callable[[str], ET.Element]
DecodeString = Callable[[ET.Element], str]
MemberAttributes = Callable[[str], dict[str, str]]
DecodeMemberKey = Callable[[ET.Element], str]


def encode_base64(value: str) -> str:
    # surrogatepass is needed because JSON permits escaped lone surrogates
    # while XML 1.0 cannot contain them directly.
    return base64.b64encode(value.encode("utf-8", errors="surrogatepass")).decode("ascii")


def decode_base64(value: str | None) -> str:
    try:
        raw = base64.b64decode(value or "", validate=True)
    except ValueError as error:
        raise ValueError("invalid base64 text in XML candidate") from error
    return raw.decode("utf-8", errors="surrogatepass")


def require_shape(
    node: ET.Element,
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


def _element(
    value: Any,
    string_element: StringElement,
    member_attributes: MemberAttributes,
) -> ET.Element:
    if value is None:
        return ET.Element("null")
    if isinstance(value, bool):
        node = ET.Element("boolean")
        node.text = "true" if value else "false"
        return node
    if isinstance(value, str):
        return string_element(value)
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
            item.append(_element(value_item, string_element, member_attributes))
        return node
    if isinstance(value, dict):
        if not all(isinstance(key, str) for key in value):
            raise TypeError("JSON object keys must be strings")
        node = ET.Element("object")
        for key in sorted(value):
            member = ET.SubElement(node, "member", member_attributes(key))
            member.append(_element(value[key], string_element, member_attributes))
        return node
    raise TypeError(f"unsupported JSON value type: {type(value).__name__}")


def _value(
    node: ET.Element,
    decode_string: DecodeString,
    decode_member_key: DecodeMemberKey,
) -> Any:
    if node.tag == "null":
        require_shape(node, children=0)
        if node.text:
            raise ValueError("null cannot contain text")
        return None
    if node.tag == "boolean":
        require_shape(node, children=0)
        if node.text not in {"true", "false"}:
            raise ValueError("invalid boolean")
        return node.text == "true"
    if node.tag == "string":
        return decode_string(node)
    if node.tag == "number":
        require_shape(node, children=0)
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
        require_shape(node)
        values = []
        for item in node:
            if item.tag != "item":
                raise ValueError("array children must be <item>")
            require_shape(item, children=1)
            values.append(_value(item[0], decode_string, decode_member_key))
        return values
    if node.tag == "object":
        require_shape(node)
        value = {}
        for member in node:
            if member.tag != "member":
                raise ValueError("object children must be <member>")
            require_shape(member, set(member.attrib), children=1)
            key = decode_member_key(member)
            if key in value:
                raise ValueError("duplicate object key")
            value[key] = _value(member[0], decode_string, decode_member_key)
        return value
    raise ValueError(f"unknown JSON value element <{node.tag}>")


def build_document(
    value: Any,
    *,
    version: str,
    string_element: StringElement,
    member_attributes: MemberAttributes,
) -> str:
    root = ET.Element(_ROOT_TAG, {"version": version})
    root.append(_element(value, string_element, member_attributes))
    return ET.tostring(root, encoding="unicode", short_empty_elements=True)


def parse_document(
    rendered: str,
    *,
    version: str,
    decode_string: DecodeString,
    decode_member_key: DecodeMemberKey,
) -> Any:
    upper = rendered.upper()
    if "<!DOCTYPE" in upper or "<!ENTITY" in upper:
        raise ValueError("document type and entity declarations are forbidden")
    try:
        root = ET.fromstring(rendered)
    except ET.ParseError as error:
        raise ValueError("invalid XML candidate") from error
    if root.tag != _ROOT_TAG:
        raise ValueError("candidate root must be <json>")
    require_shape(root, attributes={"version"}, children=1)
    if root.attrib["version"] != version:
        raise ValueError("unsupported XML candidate version")
    return _value(root[0], decode_string, decode_member_key)
