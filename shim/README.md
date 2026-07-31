# Billy shim — backend application-logic layer

The **brain layer** that sits in front of `llama.cpp`. It holds the application logic that
should live on the backend rather than in the fish, so the fish (CLI today, ESP32 later)
stays a thin client:

- the **persona** / system prompt — edit `text.py`, reload, no reflash;
- **conversation history / session state** — kept server-side, keyed by session id;
- **model quirks** — the Qwen3 `/no_think` soft switch and `<think>`-block stripping, so the
  fish stays model-agnostic;
- **markdown/emoji scrubbing** — an 8B model still emits stray `*emphasis*`/emoji that TTS
  would mangle;
- **sentence splitting + streaming** — emits clean sentences as they're ready so the client
  can pipeline TTS for low first-audio latency.

**It is a text service — it does not touch audio.** STT (whisper `:8081`) and TTS (Kokoro
`:8880`) are dumb audio↔text transforms the client calls **directly**; only the text/brain hop
goes through the shim.

## Protocol

| Call | Request | Response |
|---|---|---|
| `POST /v1/respond` | `{"session": "default", "text": "<user utterance>"}` | `text/event-stream`: one `data: {"sentence": "..."}` per spoken sentence, then `data: [DONE]` |
| `POST /v1/reset` | `{"session": "default"}` | `{"reset": "<session>"}` |
| `GET /v1/config` | — | Runtime overrides for firmware-tunable constants (`fish_config.py`); the fish fetches this each turn-loop cycle and applies only the fields present, falling back to its own compiled-in defaults otherwise |
| `GET /health` | — | `{status, llm_url, llm_model, llm_reachable, sessions}` |

The client sends STT text to `/v1/respond`, and pipelines each returned sentence into TTS.

## Run

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/uvicorn billy_shim:app --host 0.0.0.0 --port 8000
```

Port **8000** keeps the shim (application layer) clear of the heavy engines on 8080/8081/8880.

Config (env): `BILLY_LLM_URL` (default `http://localhost:8080` — the shim runs alongside
llama.cpp), `BILLY_LLM_MODEL` (default `Qwen3-8B`).

Tests (no pytest dependency): `.venv/bin/python test_text.py`.

## Deploy

Lives under `/opt/billy-shim` on the backend host, managed by the `billy-shim.service` systemd
unit (`deploy/systemd/billy-shim.service`, enabled at boot). Redeploy after code changes:

```bash
rsync -a --exclude '.venv' --exclude '__pycache__' shim/ your-host-or-ip:/opt/billy-shim/
ssh your-host-or-ip systemctl restart billy-shim
```
