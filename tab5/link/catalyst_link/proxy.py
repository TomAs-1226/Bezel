"""POST /v1/messages: the Messages API, forwarded through the official Python SDK so the key stays on the PC.

The tablet's body goes to the SDK as given, with `stream=True`: parameters the installed SDK's
`beta.messages.create` declares go as keyword arguments, anything it doesn't know yet (e.g. a new
`fallbacks` form) goes through `extra_body`. `anthropic-beta` values become `betas`. The SDK's raw stream
events are re-emitted as SSE in the API's own wire format, so the tablet parses one format whichever
route it takes."""
from __future__ import annotations

import inspect
import json
import os
import queue
import threading
import time
from typing import Any, Mapping, Protocol

try:  # the Link runs without the SDK; it then reports "claude": false
    import anthropic
except ImportError:  # pragma: no cover - exercised only where the SDK is missing
    anthropic = None  # type: ignore[assignment]

from .state import State

# SDK-side arguments of create(); a body key with one of these names is not an API parameter.
_SDK_ONLY = frozenset({"self", "stream", "betas", "extra_headers", "extra_query", "extra_body", "timeout"})
_TYPE_BY_STATUS = {400: "invalid_request_error", 401: "authentication_error", 402: "billing_error",
                   403: "permission_error", 404: "not_found_error", 413: "request_too_large",
                   429: "rate_limit_error", 500: "api_error", 529: "overloaded_error"}


class Responder(Protocol):
    def send_json(self, status: int, obj: dict[str, Any]) -> None: ...
    def start_stream(self) -> None: ...
    def write(self, data: bytes) -> None: ...


def api_error(etype: str, message: str) -> dict[str, Any]:
    return {"type": "error", "error": {"type": etype, "message": message}}


def error_payload(exc: BaseException) -> tuple[int, dict[str, Any]]:
    """(HTTP status, the API's error object) for an exception the SDK raised."""
    if anthropic is not None and isinstance(exc, anthropic.APITimeoutError):
        return 504, api_error("timeout_error", "timed out reaching the Claude API")
    if anthropic is not None and isinstance(exc, anthropic.APIConnectionError):
        return 502, api_error("api_error", f"could not reach the Claude API: {exc}")
    status = getattr(exc, "status_code", None)
    status = status if isinstance(status, int) and status >= 400 else 500
    body = getattr(exc, "body", None)
    if isinstance(body, dict) and isinstance(body.get("error"), dict) and body["error"].get("type"):
        return status, {**body, "type": "error"}  # the API's own error, request_id and all
    etype = getattr(exc, "type", None) or _TYPE_BY_STATUS.get(status, "api_error")
    return status, api_error(str(etype), str(getattr(exc, "message", None) or exc))


def sse(event: str, data: dict[str, Any]) -> bytes:
    return f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'), ensure_ascii=False)}\n\n".encode("utf-8")


def event_dict(event: Any) -> dict[str, Any]:
    """An SDK stream event as its wire JSON (only the fields the API sent, API names)."""
    if isinstance(event, dict):
        data = event
    elif hasattr(event, "to_dict"):
        data = event.to_dict(mode="json")
    else:
        data = event.model_dump(mode="json", by_alias=True, exclude_unset=True)
    # The SDK orders keys by its model's fields; put "type" first as the API does (JSON doesn't care).
    return {"type": data.get("type"), **data} if "type" in data else data


class ClaudeProxy:
    keepalive = 10.0  # seconds of upstream silence before the Link sends the tablet a ping

    def __init__(self, state: State, client: Any | None = None) -> None:
        self.state = state
        self._client = client
        self._lock = threading.Lock()

    @property
    def available(self) -> bool:
        return self._client is not None or (anthropic is not None and bool(os.environ.get("ANTHROPIC_API_KEY")))

    def _get_client(self) -> Any:
        with self._lock:
            if self._client is None:
                # The key comes only from the Link's own environment (ANTHROPIC_API_KEY).
                self._client = anthropic.Anthropic()  # type: ignore[union-attr]
            return self._client

    @staticmethod
    def split(create: Any, body: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
        """(keyword arguments the SDK declares, everything else for extra_body)."""
        try:
            params: Mapping[str, inspect.Parameter] = inspect.signature(create).parameters
        except (TypeError, ValueError):
            params = {}
        if any(p.kind is inspect.Parameter.VAR_KEYWORD for p in params.values()):
            known = {k for k in body if k not in _SDK_ONLY}
        else:
            known = set(params) - _SDK_ONLY
        kwargs = {k: v for k, v in body.items() if k in known}
        extra = {k: v for k, v in body.items() if k not in known and k != "stream"}
        return kwargs, extra

    def serve(self, body: dict[str, Any], betas: list[str], out: Responder) -> None:
        started = time.monotonic()
        rec: dict[str, Any] = {"kind": "claude", "model_requested": body.get("model"), "betas": betas,
                               "ok": False, "fell_back": False, "disconnected": False}
        try:
            self._serve(body, betas, out, rec)
        finally:
            rec["seconds"] = round(time.monotonic() - started, 2)
            self.state.log(rec)

    def _serve(self, body: dict[str, Any], betas: list[str], out: Responder, rec: dict[str, Any]) -> None:
        if not self.available:
            rec["status"] = 503
            out.send_json(503, api_error("api_error", "Catalyst Link has no ANTHROPIC_API_KEY (or no anthropic SDK)"))
            return
        if not (isinstance(body.get("model"), str) and isinstance(body.get("max_tokens"), int)
                and isinstance(body.get("messages"), list)):
            rec["status"] = 400
            out.send_json(400, api_error("invalid_request_error", "model, max_tokens and messages are required"))
            return

        create = self._get_client().beta.messages.create
        kwargs, extra = self.split(create, body)
        call: dict[str, Any] = {**kwargs, "stream": True}
        if betas:
            call["betas"] = betas
        if extra:
            call["extra_body"] = extra
        try:
            stream = create(**call)
        except Exception as exc:  # the stream never started: answer with the API's status and error
            status, payload = error_payload(exc)
            rec.update(status=status, error=payload["error"])
            out.send_json(status, payload)
            return

        rec["status"] = 200
        # The SDK drops the API's `ping`s, so a long silent stretch (thinking with nothing to show) would
        # leave the tablet's socket idle until it times out. Read the SDK on a thread and send our own
        # `event: ping` (the API's own keepalive, which the tablet already ignores) during gaps.
        events: queue.Queue[tuple[str, Any]] = queue.Queue()

        def pump() -> None:
            try:
                for event in stream:
                    events.put(("event", event))
                events.put(("end", None))
            except BaseException as exc:  # an `event: error`, a dropped connection, or our close()
                events.put(("error", exc))

        threading.Thread(target=pump, name="claude-stream", daemon=True).start()
        try:
            out.start_stream()
            while True:
                try:
                    kind, item = events.get(timeout=self.keepalive)
                except queue.Empty:
                    out.write(sse("ping", {"type": "ping"}))
                    continue
                if kind == "end":
                    rec["ok"] = rec.pop("stopped", False)
                    break
                if kind == "error":  # mid-stream: one `event: error`, and the stream ends
                    _, payload = error_payload(item)
                    rec["error"] = payload["error"]
                    out.write(sse("error", payload))
                    break
                data = event_dict(item)
                self._note(data, rec)
                out.write(sse(str(data.get("type", "message")), data))
        except OSError:  # only writes raise OSError here: the tablet went away
            rec["disconnected"] = True
        finally:
            # Closing the SDK stream drops the upstream connection, so the API stops generating (and
            # the pump thread's iteration ends).
            close = getattr(stream, "close", None)
            if callable(close):
                close()
            rec.pop("stopped", None)

    @staticmethod
    def _note(data: dict[str, Any], rec: dict[str, Any]) -> None:
        """Keep what the audit log wants: the model that served, tokens, stop reason, fallbacks."""
        kind = data.get("type")
        if kind == "message_start":
            msg = data.get("message") or {}
            rec["model"] = msg.get("model")
            usage = msg.get("usage") or {}
            for key in ("input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"):
                if usage.get(key) is not None:
                    rec[key] = usage[key]
        elif kind == "message_delta":
            usage = data.get("usage") or {}
            for key in ("input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"):
                if usage.get(key) is not None:
                    rec[key] = usage[key]
            if any(isinstance(it, dict) and it.get("type") == "fallback_message" for it in usage.get("iterations") or []):
                rec["fell_back"] = True
            rec["stop_reason"] = (data.get("delta") or {}).get("stop_reason")
        elif kind == "content_block_start":
            block = data.get("content_block") or {}
            if block.get("type") == "fallback":
                rec["fell_back"] = True
                served = (block.get("to") or {}).get("model")
                if served:
                    rec["model"] = served
        elif kind == "message_stop":
            rec["stopped"] = True
