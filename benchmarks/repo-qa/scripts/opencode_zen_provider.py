#!/usr/bin/env python3
"""Secret-safe OpenCode Zen transport for frozen provider-request replays."""
from __future__ import annotations

import json
import base64
import hashlib
import socket
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Callable


ENDPOINTS = {
    "opencode": "https://opencode.ai/zen/v1/chat/completions",
    "opencode-go": "https://opencode.ai/zen/go/v1/chat/completions",
}
AUTH_REQUIRED = set(ENDPOINTS)
SAFE_RESPONSE_HEADERS = {"content-type", "retry-after", "x-request-id", "request-id", "cf-ray"}


def endpoint_for_model(model: str) -> str:
    provider, separator, _ = model.partition("/")
    if not separator or provider not in ENDPOINTS:
        raise ValueError(f"model provider is not allowlisted: {provider!r}")
    return ENDPOINTS[provider]


def _credential(entry: object) -> str | None:
    if isinstance(entry, str) and entry:
        return entry
    if not isinstance(entry, dict):
        return None
    for field in ("key", "apiKey", "token", "access_token", "accessToken"):
        value = entry.get(field)
        if isinstance(value, str) and value:
            return value
    nested = entry.get("oauth")
    return _credential(nested) if isinstance(nested, dict) else None


def load_auth(model: str, auth_path: Path) -> str | None:
    provider = model.split("/", 1)[0]
    try:
        document = json.loads(auth_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("unable to read configured OpenCode auth") from error
    if not isinstance(document, dict):
        raise ValueError("configured OpenCode auth must be an object")
    token = _credential(document.get(provider))
    if token is None and provider in AUTH_REQUIRED:
        raise ValueError(f"configured OpenCode auth has no credential for {provider!r}")
    return token


def _content(value: object) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "".join(
            part.get("text", "")
            for part in value
            if isinstance(part, dict) and part.get("type") in (None, "text") and isinstance(part.get("text"), str)
        )
    return ""


def _events(raw: str) -> list[str]:
    events: list[str] = []
    data: list[str] = []
    for line in raw.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if not line:
            if data:
                events.append("\n".join(data))
                data = []
            continue
        if line.startswith(":"):
            continue
        field, separator, value = line.partition(":")
        if field == "data":
            data.append(value[1:] if separator and value.startswith(" ") else value)
    if data:
        events.append("\n".join(data))
    return events


def parse_response(raw_bytes: bytes, content_type: str | None) -> dict:
    try:
        raw = raw_bytes.decode("utf-8")
    except UnicodeDecodeError:
        return {"status": "malformed_provider_stream", "final_answer": "", "usage": None,
                "failure": "provider response is not UTF-8"}
    streaming = "text/event-stream" in (content_type or "").lower() or raw.lstrip().startswith(("data:", ":"))
    payloads = _events(raw) if streaming else [raw]
    answer: list[str] = []
    usage = None
    saw_json = False
    try:
        for payload in payloads:
            if payload.strip() == "[DONE]":
                continue
            item = json.loads(payload)
            if not isinstance(item, dict):
                raise ValueError("provider event is not an object")
            saw_json = True
            if item.get("error"):
                return {"status": "provider_error", "final_answer": "", "usage": item.get("usage"),
                        "failure": "provider returned an error event"}
            if isinstance(item.get("usage"), dict):
                usage = item["usage"]
            choices = item.get("choices", [])
            if not isinstance(choices, list):
                raise ValueError("provider choices is not an array")
            for choice in choices:
                if not isinstance(choice, dict):
                    raise ValueError("provider choice is not an object")
                answer.append(_content(choice.get("delta", {}).get("content")) if isinstance(choice.get("delta"), dict)
                              else _content(choice.get("message", {}).get("content")) if isinstance(choice.get("message"), dict)
                              else _content(choice.get("text")))
    except (json.JSONDecodeError, ValueError, TypeError):
        return {"status": "malformed_provider_stream", "final_answer": "", "usage": usage,
                "failure": "provider response is not valid OpenAI-compatible JSON/SSE"}
    final = "".join(answer)
    if not saw_json or not final.strip():
        return {"status": "empty_answer", "final_answer": "", "usage": usage,
                "failure": "provider response contains no assistant text"}
    return {"status": "answered", "final_answer": final, "usage": usage, "failure": None}


def _safe_headers(headers: object) -> dict[str, str]:
    if headers is None:
        return {}
    return {str(key).lower(): str(value) for key, value in headers.items()
            if str(key).lower() in SAFE_RESPONSE_HEADERS}


class OpenCodeZenProvider:
    def __init__(self, auth_path: Path, timeout: float, *, opener: Callable = urllib.request.urlopen,
                 clock: Callable[[], float] = time.monotonic):
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        self.auth_path = auth_path
        self.timeout = timeout
        self.opener = opener
        self.clock = clock

    def complete(self, request: dict, model: str) -> dict:
        endpoint = endpoint_for_model(model)
        token = load_auth(model, self.auth_path)
        body = json.dumps(request, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
        request_headers = {"Content-Type": "application/json", "Accept": "text/event-stream"}
        if token is not None:
            request_headers["Authorization"] = f"Bearer {token}"
        outgoing = urllib.request.Request(endpoint, data=body, method="POST", headers=request_headers)
        started = self.clock()
        status_code = None
        headers: dict[str, str] = {}
        raw = b""
        transport_failure = None
        try:
            with self.opener(outgoing, timeout=self.timeout) as response:
                status_code = response.getcode()
                headers = _safe_headers(response.headers)
                raw = response.read()
        except urllib.error.HTTPError as error:
            status_code = error.code
            headers = _safe_headers(error.headers)
            raw = error.read()
        except (TimeoutError, socket.timeout):
            transport_failure = "provider_timeout"
        except (urllib.error.URLError, OSError):
            transport_failure = "transport_error"
        latency = self.clock() - started
        raw_text = raw.decode("utf-8", errors="replace")
        common = {"http_status": status_code, "response_headers": headers, "raw_provider_stream": raw_text,
                  "raw_provider_stream_base64": base64.b64encode(raw).decode("ascii"),
                  "raw_provider_stream_digest": "sha256:" + hashlib.sha256(raw).hexdigest(),
                  "latency_seconds": latency, "usage": None, "final_answer": ""}
        if transport_failure:
            return {**common, "status": transport_failure, "failure": transport_failure}
        if status_code == 429:
            return {**common, "status": "provider_quota", "failure": "provider returned HTTP 429"}
        if status_code is not None and 500 <= status_code <= 599:
            return {**common, "status": "provider_5xx", "failure": f"provider returned HTTP {status_code}"}
        if status_code is None or not 200 <= status_code <= 299:
            return {**common, "status": "provider_error", "failure": f"provider returned HTTP {status_code}"}
        parsed = parse_response(raw, headers.get("content-type"))
        return {**common, **parsed}
