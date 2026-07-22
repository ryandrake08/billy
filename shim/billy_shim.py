#!/usr/bin/env python3
"""Billy backend shim — the application-logic layer in front of llama.cpp (SCOPING.md §8.3).

Owns the persona, conversation history, model quirks (the /no_think soft switch, <think>
stripping), and markdown/emoji scrubbing, then streams clean spoken sentences to the client.

It is a TEXT service: it never touches the audio streams. The client (CLI today, ESP32 fish
later) calls whisper (STT :8081) and Kokoro (TTS :8880) directly, and routes only the text/brain
hop through here. Contract:

  POST /v1/respond  {session, text}      -> text/event-stream of {"sentence": "..."} then [DONE]
  POST /v1/reset    {session}            -> {"reset": <session>}
  GET  /health                           -> shim + upstream llama.cpp status

Run:  uvicorn billy_shim:app --host 0.0.0.0 --port 8000
Config (env):  BILLY_LLM_URL (default http://localhost:8080), BILLY_LLM_MODEL (default Qwen3-8B).
"""
import json
import os

import httpx
from fastapi import FastAPI
from fastapi.responses import StreamingResponse
from pydantic import BaseModel

from text import PERSONA, NO_THINK, strip_think, sentences_from, strip_markup

LLM_URL = os.environ.get("BILLY_LLM_URL", "http://localhost:8080").rstrip("/")
LLM_MODEL = os.environ.get("BILLY_LLM_MODEL", "Qwen3-8B")

app = FastAPI(title="Billy shim", version="1.0")

# In-memory conversation history, keyed by session id. Held server-side so the fish never
# accumulates the growing message array (§8.3). Fine for a single fish; swap for a store if
# multiple sessions ever need to persist across restarts.
_SESSIONS: dict[str, list[dict]] = {}


def _session(sid: str) -> list[dict]:
    msgs = _SESSIONS.get(sid)
    if msgs is None:
        msgs = [{"role": "system", "content": PERSONA + NO_THINK}]
        _SESSIONS[sid] = msgs
    return msgs


def _llm_deltas(client: httpx.Client, messages: list[dict]):
    """Yield assistant text deltas from llama.cpp's streamed OpenAI chat completion."""
    body = {"model": LLM_MODEL, "messages": messages, "stream": True,
            "temperature": 0.7, "top_p": 0.8}
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
    return f"data: {json.dumps(obj)}\n\n"


class RespondReq(BaseModel):
    session: str = "default"
    text: str


class ResetReq(BaseModel):
    session: str = "default"


@app.post("/v1/respond")
def respond(req: RespondReq):
    """Stream Billy's reply as clean, spoken-ready sentences (one SSE event each). The client
    pipelines these into TTS — speech can start on sentence 1 while the LLM is still generating."""
    msgs = _session(req.session)
    msgs.append({"role": "user", "content": req.text + NO_THINK})

    def gen():
        spoken = []
        try:
            with httpx.Client() as client:
                for sent in sentences_from(strip_think(_llm_deltas(client, msgs))):
                    clean = strip_markup(sent)
                    if not clean:  # sentence was pure markup/emoji — nothing to say
                        continue
                    spoken.append(clean)
                    yield _sse({"sentence": clean})
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
    return {"reset": req.session}


@app.get("/health")
def health():
    try:
        with httpx.Client() as c:
            reachable = c.get(f"{LLM_URL}/health", timeout=4).status_code == 200
    except httpx.HTTPError:
        reachable = False
    return {"status": "ok", "llm_url": LLM_URL, "llm_model": LLM_MODEL,
            "llm_reachable": reachable, "sessions": len(_SESSIONS)}
