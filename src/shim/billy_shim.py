#!/usr/bin/env python3
"""Billy backend shim — the application-logic layer in front of llama.cpp.

Owns the persona, conversation history, model quirks (the enable_thinking request flag, <think>
stripping), and markdown/emoji scrubbing, then streams clean spoken sentences to the client.

It is a TEXT service: it never touches the audio streams. The client (CLI today, ESP32 fish
later) calls whisper (STT :8081) and Kokoro (TTS :8880) directly, and routes only the text/brain
hop through here. Contract:

  POST /v1/respond  {session, text}      -> text/event-stream of {"sentence", "voice"} then [DONE]
  POST /v1/reset    {session}            -> {"reset": <session>}
  GET  /v1/config                        -> firmware config overrides (fish_config.py) -- fetched
                                             by the fish every turn loop cycle
  GET  /health                           -> shim + upstream llama.cpp status

Run:  uvicorn billy_shim:app --host 0.0.0.0 --port 8000
Config (env):  BILLY_LLM_URL (default http://localhost:8080), BILLY_LLM_MODEL (default Qwen3.5-9B).
"""
import json
import os
import time

import httpx
from fastapi import FastAPI
from fastapi.responses import StreamingResponse
from pydantic import BaseModel

from text import PERSONA, VOICE, strip_think, sentences_from, strip_markup
from fish_config import OVERRIDES

LLM_URL = os.environ.get("BILLY_LLM_URL", "http://localhost:8080").rstrip("/")
LLM_MODEL = os.environ.get("BILLY_LLM_MODEL", "Qwen3.5-9B")

app = FastAPI(title="Billy shim", version="1.0")

# In-memory conversation history, keyed by session id. Held server-side so the fish never
# accumulates the growing message array. Fine for a single fish; swap for a store if
# multiple sessions ever need to persist across restarts.
_SESSIONS: dict[str, list[dict]] = {}
_LAST_SEEN: dict[str, float] = {}   # session -> monotonic time of its last turn

# The fish never calls /v1/reset on its own, so without this a long-lived session (e.g. left in
# WAKEWORD mode for days) grows forever. A gap this long between turns means the conversation is
# over in any practical sense, so treat it as a fresh start rather than bounding history some
# other way.
IDLE_RESET_SECONDS = 5 * 60


def _session(sid: str) -> list[dict]:
    now = time.monotonic()
    last = _LAST_SEEN.get(sid)
    if last is not None and now - last > IDLE_RESET_SECONDS:
        _SESSIONS.pop(sid, None)
    _LAST_SEEN[sid] = now

    msgs = _SESSIONS.get(sid)
    if msgs is None:
        msgs = [{"role": "system", "content": PERSONA}]
        _SESSIONS[sid] = msgs
    return msgs


def _llm_deltas(client: httpx.Client, messages: list[dict]):
    """Yield assistant text deltas from llama.cpp's streamed OpenAI chat completion."""
    # temperature/top_p kept high on purpose — Billy got repetitive and flat at lower settings.
    #
    # Qwen3.5 dropped Qwen3's "/no_think" text-suffix switch — non-thinking mode is now a
    # request-level flag the model's chat template reads (needs llama-server run with --jinja).
    # enable_thinking is the documented top-level field; chat_template_kwargs is sent too as a
    # fallback in case this llama.cpp build only honors the kwargs-passthrough form. strip_think()
    # below still strips any <think> block that gets through regardless, but that's a correctness
    # net, not a latency one — a leaked thinking pass burns real generation time before it. Verify
    # this actually lands (watch for a <think> block, and that "LLM ttfs" stays in the same range
    # as before) rather than trusting it blind.
    body = {"model": LLM_MODEL, "messages": messages, "stream": True,
            "temperature": 0.9, "top_p": 0.9,
            "enable_thinking": False, "chat_template_kwargs": {"enable_thinking": False}}
    with client.stream("POST", f"{LLM_URL}/v1/chat/completions", json=body, timeout=120) as r:
        r.raise_for_status()
        for line in r.iter_lines():
            if not line.startswith("data: "):
                continue
            data = line[len("data: "):]
            if data == "[DONE]":
                break
            try:
                delta = json.loads(data)["choices"][0]["delta"].get("content")
            except (json.JSONDecodeError, KeyError, IndexError):
                continue
            if delta:
                yield delta


def _sse(obj) -> str:
    # ensure_ascii=False so non-ASCII (em-dashes, curly quotes) go out as raw UTF-8 rather than
    # \uXXXX escapes — the fish passes the bytes straight to TTS. Safe in SSE: UTF-8 never contains
    # a stray newline. (text/event-stream is UTF-8 by spec.)
    return f"data: {json.dumps(obj, ensure_ascii=False)}\n\n"


class RespondReq(BaseModel):
    session: str = "default"
    text: str


class ResetReq(BaseModel):
    session: str = "default"


@app.post("/v1/respond")
def respond(req: RespondReq):
    """Stream Billy's reply as clean, spoken-ready sentences (one SSE event each), each carrying
    the voice it should be spoken in. The client pipelines these into TTS — speech can start on
    sentence 1 while the LLM is still generating."""
    msgs = _session(req.session)
    msgs.append({"role": "user", "content": req.text})

    def gen():
        spoken = []
        try:
            with httpx.Client() as client:
                for sent in sentences_from(strip_think(_llm_deltas(client, msgs))):
                    clean = strip_markup(sent)
                    if not clean:  # sentence was pure markup/emoji — nothing to say
                        continue
                    spoken.append(clean)
                    yield _sse({"sentence": clean, "voice": VOICE})
        except httpx.HTTPError as e:
            yield _sse({"error": f"llm upstream: {e}"})
        # Record what Billy actually said so the next turn has context.
        msgs.append({"role": "assistant", "content": " ".join(spoken)})
        yield "data: [DONE]\n\n"

    return StreamingResponse(gen(), media_type="text/event-stream")


@app.post("/v1/reset")
def reset(req: ResetReq):
    """Forget a session's history (start a fresh conversation)."""
    _SESSIONS.pop(req.session, None)
    _LAST_SEEN.pop(req.session, None)
    return {"reset": req.session}


@app.get("/v1/config")
def config():
    """Runtime-tunable firmware constants the fish doesn't already default to -- the firmware
    fetches this at the top of every turn loop cycle and only overwrites the fields present here,
    so retuning after final assembly is an edit to fish_config.py + a shim restart, not a
    firmware rebuild+reflash. Empty means "use the firmware's own compiled-in defaults"."""
    return OVERRIDES


@app.get("/health")
def health():
    try:
        with httpx.Client() as c:
            reachable = c.get(f"{LLM_URL}/health", timeout=4).status_code == 200
    except httpx.HTTPError:
        reachable = False
    return {"status": "ok", "llm_url": LLM_URL, "llm_model": LLM_MODEL,
            "llm_reachable": reachable, "sessions": len(_SESSIONS)}
