"""Lossless exploratory semi-structured renderings of arbitrary JSON values."""

from __future__ import annotations

import json
import math
from typing import Any


def _json(value: Any) -> str:
    # ASCII escaping keeps every accepted JSON string transportable as UTF-8,
    # including parsed values containing lone UTF-16 surrogate code points.
    return json.dumps(value, ensure_ascii=True, allow_nan=False, separators=(",", ":"))


def _check(value: Any) -> None:
    if value is None or isinstance(value, (str, bool, int)):
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("non-finite numbers are not JSON values")
        return
    if isinstance(value, list):
        for child in value:
            _check(child)
        return
    if isinstance(value, dict):
        if not all(isinstance(key, str) for key in value):
            raise TypeError("JSON object keys must be strings")
        for child in value.values():
            _check(child)
        return
    raise TypeError(f"unsupported JSON value: {type(value).__name__}")


PATH_HEADER = "LCI-PATH-RECORDS/1\n"


def encode_path_records(value: Any) -> str:
    """Flatten a JSON value into deterministic length-framed typed-path records."""
    _check(value)
    records: list[str] = []

    def visit(node: Any, path: list[Any]) -> None:
        path_text = _json(path)
        if isinstance(node, dict):
            tag, payload = "O", str(len(node))
        elif isinstance(node, list):
            tag, payload = "A", str(len(node))
        elif node is None:
            tag, payload = "Z", ""
        elif isinstance(node, bool):
            tag, payload = "B", "true" if node else "false"
        elif isinstance(node, str):
            tag, payload = "S", _json(node)
        else:
            tag, payload = "N", _json(node)
        records.append(f"R{len(path_text)}:{path_text}{tag}{len(payload)}:{payload}\n")
        if isinstance(node, dict):
            for key in sorted(node):
                visit(node[key], [*path, key])
        elif isinstance(node, list):
            for index, child in enumerate(node):
                visit(child, [*path, index])

    visit(value, [])
    return PATH_HEADER + "".join(records)


def decode_path_records(text: str) -> Any:
    if not text.startswith(PATH_HEADER):
        raise ValueError("invalid path-record header")
    offset = len(PATH_HEADER)
    records: dict[tuple[Any, ...], tuple[str, str]] = {}

    def framed(prefix: str) -> str:
        nonlocal offset
        if not text.startswith(prefix, offset):
            raise ValueError(f"expected {prefix!r} at offset {offset}")
        offset += len(prefix)
        colon = text.find(":", offset)
        if colon < 0 or not text[offset:colon].isdigit():
            raise ValueError("invalid frame length")
        length = int(text[offset:colon])
        offset = colon + 1
        result = text[offset:offset + length]
        if len(result) != length:
            raise ValueError("truncated frame")
        offset += length
        return result

    while offset < len(text):
        path_text = framed("R")
        try:
            path = json.loads(path_text)
        except json.JSONDecodeError as exc:
            raise ValueError("invalid record path") from exc
        if not isinstance(path, list) or any(
            not isinstance(part, (str, int)) or isinstance(part, bool) for part in path
        ):
            raise ValueError("path members must be string keys or integer indices")
        if offset >= len(text) or text[offset] not in "OAZBSN":
            raise ValueError("invalid record tag")
        tag = text[offset]
        offset += 1
        payload = framed("")
        if offset >= len(text) or text[offset] != "\n":
            raise ValueError("record lacks newline terminator")
        offset += 1
        key = tuple(path)
        if key in records:
            raise ValueError("duplicate record path")
        records[key] = (tag, payload)

    if () not in records:
        raise ValueError("missing root record")
    consumed: set[tuple[Any, ...]] = set()
    # One O(n) pass instead of rescanning every record per container node.
    children_index: dict[tuple[Any, ...], list[tuple[Any, tuple[Any, ...]]]] = {}
    for p in records:
        if p:
            children_index.setdefault(p[:-1], []).append((p[-1], p))

    def build(path: tuple[Any, ...]) -> Any:
        if path not in records:
            raise ValueError("missing child record")
        consumed.add(path)
        tag, payload = records[path]
        if tag in "OA":
            if not payload.isdigit():
                raise ValueError("container size is not an integer")
            size = int(payload)
            children = children_index.get(path, [])
            if len(children) != size:
                raise ValueError("container child count mismatch")
            if tag == "A":
                child_map = {
                    part: child for part, child in children
                    if isinstance(part, int) and not isinstance(part, bool)
                }
                if set(child_map) != set(range(size)) or len(child_map) != len(children):
                    raise ValueError("array indices must be contiguous integers")
                return [build(child_map[index]) for index in range(size)]
            if any(not isinstance(part, str) for part, _ in children):
                raise ValueError("object paths must use string keys")
            return {part: build(child) for part, child in sorted(children)}
        if path in children_index:
            raise ValueError("scalar record has children")
        if tag == "Z":
            if payload:
                raise ValueError("null record has a payload")
            return None
        try:
            value = json.loads(payload)
        except json.JSONDecodeError as exc:
            raise ValueError("invalid scalar payload") from exc
        if tag == "S" and isinstance(value, str):
            return value
        if tag == "B" and isinstance(value, bool):
            return value
        if tag == "N" and isinstance(value, (int, float)) and not isinstance(value, bool):
            _check(value)
            return value
        raise ValueError("scalar payload does not match its tag")

    result = build(())
    if consumed != set(records):
        raise ValueError("unreachable records")
    return result


BLOCK_HEADER = "LCI-TAGGED-BLOCKS/1\n"


def encode_tagged_blocks(value: Any) -> str:
    """Encode a JSON value as a deterministic recursive tagged block stream."""
    _check(value)

    def emit(node: Any) -> str:
        if node is None:
            return "NULL\n"
        if isinstance(node, bool):
            return f"BOOL {1 if node else 0}\n"
        if isinstance(node, str):
            atom = _json(node)
            return f"STRING {len(atom)}:{atom}\n"
        if isinstance(node, (int, float)):
            atom = _json(node)
            return f"NUMBER {len(atom)}:{atom}\n"
        if isinstance(node, list):
            return f"ARRAY {len(node)}\n" + "".join(emit(child) for child in node)
        parts = [f"OBJECT {len(node)}\n"]
        for key in sorted(node):
            atom = _json(key)
            parts.append(f"KEY {len(atom)}:{atom}\n")
            parts.append(emit(node[key]))
        return "".join(parts)

    return BLOCK_HEADER + emit(value)


def decode_tagged_blocks(text: str) -> Any:
    if not text.startswith(BLOCK_HEADER):
        raise ValueError("invalid tagged-block header")
    offset = len(BLOCK_HEADER)

    def line() -> str:
        nonlocal offset
        end = text.find("\n", offset)
        if end < 0:
            raise ValueError("unterminated tagged-block line")
        result = text[offset:end]
        offset = end + 1
        return result

    def atom(prefix: str, current: str) -> Any:
        if not current.startswith(prefix):
            raise ValueError(f"expected {prefix.strip()}")
        length_text, separator, payload = current[len(prefix):].partition(":")
        if separator != ":" or not length_text.isdigit() or len(payload) != int(length_text):
            raise ValueError("invalid atom frame")
        try:
            return json.loads(payload)
        except json.JSONDecodeError as exc:
            raise ValueError("invalid atom JSON") from exc

    def count(current: str, prefix: str) -> int:
        raw = current[len(prefix):] if current.startswith(prefix) else ""
        if not raw.isdigit():
            raise ValueError(f"invalid {prefix.strip()} count")
        return int(raw)

    def parse() -> Any:
        current = line()
        if current == "NULL":
            return None
        if current == "BOOL 0":
            return False
        if current == "BOOL 1":
            return True
        if current.startswith("STRING "):
            value = atom("STRING ", current)
            if not isinstance(value, str):
                raise ValueError("STRING payload is not a string")
            return value
        if current.startswith("NUMBER "):
            value = atom("NUMBER ", current)
            if not isinstance(value, (int, float)) or isinstance(value, bool):
                raise ValueError("NUMBER payload is not a number")
            _check(value)
            return value
        if current.startswith("ARRAY "):
            return [parse() for _ in range(count(current, "ARRAY "))]
        if current.startswith("OBJECT "):
            result: dict[str, Any] = {}
            for _ in range(count(current, "OBJECT ")):
                key = atom("KEY ", line())
                if not isinstance(key, str) or key in result:
                    raise ValueError("invalid or duplicate object key")
                result[key] = parse()
            return result
        raise ValueError("unknown tagged-block record")

    result = parse()
    if offset != len(text):
        raise ValueError("trailing tagged-block content")
    return result
