#!/usr/bin/env python3
"""Multi-language end-to-end integration test -- the real backend, not shortcuts.

For each language: POST the golden/<lang>.wav fixture to the real whisper.cpp /inference, forward
its transcript + detected_language to the real shim /v1/respond, and check the reply comes back
in the matching voice and language. Exercises the actual deployed STT->shim->LLM chain against
gpu-host, unlike src/shim/test_*.py (pure in-process logic, no network).

Every golden fixture is the same instruction -- "Repeat back to me: I am Billy, a fish on the
wall, and I'm here to tell jokes." -- translated per language (see generate_golden.py), so Qwen
has something concrete to comply with instead of a free-form question; this narrows, but does not
eliminate, the LLM's normal reply variance. Assertions are on structural/objective properties
(voice routing, reply language via the shim's own sentence_language()) rather than exact wording,
since the model may still add a short in-character remark alongside repeating the phrase.

Needs `gpu-host` (or BILLY_STT_URL) reachable for whisper -- this is a live network test, not
part of the fast default suite. golden/*.wav are gitignored, not committed: Microsoft's edge-tts
(generate_golden.py uses it) is an unofficial, reverse-engineered API with no clear license to
redistribute its output audio, so this test generates them itself on first run if missing --
needing internet for that one-time step only, on whatever machine hasn't generated them yet.

By default this runs the shim straight from *this checkout's* src/shim source (a throwaway local
subprocess against gpu-host's real llama.cpp), not whatever happens to be deployed on gpu-host --
deploy and source can drift (found the hard way: gpu-host's deployed shim didn't yet have several
already-landed language voices enabled when this test was first run against it), and this test
should catch a regression in the code, not merely reflect however stale the last deploy happens
to be. Set
BILLY_SHIM_URL to point at a real deployment instead (e.g. to smoke-test gpu-host itself after a
deploy) -- doing so skips spawning the local subprocess entirely. Run directly, plain asserts, no
pytest (needs src/shim/.venv already set up per its own README section):

    python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
    ./.venv/bin/python test_language_integration.py
"""
import json
import os
import pathlib
import subprocess
import sys
import time

import httpx

SHIM_DIR = pathlib.Path(__file__).parent.parent / "shim"
sys.path.insert(0, str(SHIM_DIR))
from text import sentence_language  # noqa: E402 -- same classifier the shim itself uses

GOLDEN_DIR = pathlib.Path(__file__).parent / "golden"
STT_URL = os.environ.get("BILLY_STT_URL", "http://gpu-host:8081")
LOCAL_SHIM_HOST, LOCAL_SHIM_PORT = "127.0.0.1", 8020

# Ground truth, independent of text.py's LANGUAGE_VOICES -- this test should catch the deployed
# shim actually routing wrong, not just agree with its own internal map.
EXPECTED_VOICE = {
    "english": "am_onyx",
    "spanish": "em_alex",
    "portuguese": "pm_alex",
    "chinese": "zm_yunjian",
    "japanese": "jm_kumo",
    "hindi": "hm_omega",
    "italian": "im_nicola",
}


def check(got, want, label):
    assert got == want, f"{label}: expected {want!r}, got {got!r}"


def transcribe(client, lang):
    wav_path = GOLDEN_DIR / f"{lang}.wav"
    with open(wav_path, "rb") as f:
        r = client.post(f"{STT_URL}/inference",
                         files={"file": ("rec.wav", f, "audio/wav")},
                         data={"response_format": "verbose_json", "temperature": "0.0",
                               "language": "auto"},
                         timeout=60)
    r.raise_for_status()
    body = r.json()
    return body.get("text", "").strip(), body.get("detected_language", "english")


def respond(client, shim_url, session, text, language):
    client.post(f"{shim_url}/v1/reset", json={"session": session}, timeout=10)
    sentences, voices = [], []
    body = {"session": session, "text": text, "language": language}
    with client.stream("POST", f"{shim_url}/v1/respond", json=body, timeout=90) as r:
        r.raise_for_status()
        for line in r.iter_lines():
            if not line.startswith("data:"):
                continue
            payload = line[len("data:"):].strip()
            if payload in ("", "[DONE]"):
                continue
            obj = json.loads(payload)
            if "sentence" in obj:
                sentences.append(obj["sentence"])
                voices.append(obj.get("voice"))
            elif "error" in obj:
                raise RuntimeError(f"shim error: {obj['error']}")
    return sentences, voices


def test_language(client, shim_url, lang):
    text, detected = transcribe(client, lang)
    print(f"  STT text: {text}")
    print(f"  STT detected_language: {detected}")
    check(detected, lang, f"{lang}: STT detected_language")
    assert text, f"{lang}: STT returned an empty transcript"

    sentences, voices = respond(client, shim_url, f"integration-test-{lang}", text, detected)
    reply = " ".join(sentences)
    print(f"  reply: {reply}")
    print(f"  voices: {voices}")
    assert sentences, f"{lang}: shim returned no sentences"
    for v in voices:
        check(v, EXPECTED_VOICE[lang], f"{lang}: per-sentence voice")

    reply_lang = sentence_language(reply)
    check(reply_lang, lang, f"{lang}: reply language (via sentence_language on the full reply)")


def ensure_golden():
    """golden/*.wav are gitignored (see .gitignore) -- generate any missing ones now via
    generate_golden.py. Regenerates everything if any language is missing rather than trying to
    generate just the gap: simpler, and it's a ~1 minute one-time cost per machine."""
    import generate_golden
    missing = [lang for lang in generate_golden.PHRASES if not (GOLDEN_DIR / f"{lang}.wav").exists()]
    if not missing:
        return
    print(f"Golden fixtures missing for: {', '.join(missing)} -- generating via edge-tts "
          f"(one-time, needs internet)...")
    generate_golden.main()


def start_local_shim():
    """Launch this checkout's src/shim source as a throwaway subprocess, using its own venv
    (must already be set up: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt,
    per src/shim's own README section). BILLY_LLM_URL defaults to gpu-host's real llama.cpp
    unless already set in the environment."""
    uvicorn = SHIM_DIR / ".venv" / "bin" / "uvicorn"
    assert uvicorn.exists(), f"{uvicorn} missing -- set up src/shim/.venv first (see AGENTS.md)"
    env = os.environ.copy()
    env.setdefault("BILLY_LLM_URL", "http://gpu-host:8080")
    proc = subprocess.Popen(
        [str(uvicorn), "billy_shim:app", "--host", LOCAL_SHIM_HOST, "--port", str(LOCAL_SHIM_PORT)],
        cwd=str(SHIM_DIR), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    url = f"http://{LOCAL_SHIM_HOST}:{LOCAL_SHIM_PORT}"
    deadline = time.time() + 15
    with httpx.Client() as c:
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(f"local shim subprocess exited early (code {proc.returncode})")
            try:
                if c.get(f"{url}/health", timeout=2).status_code == 200:
                    return proc, url
            except httpx.HTTPError:
                pass
            time.sleep(0.5)
    proc.terminate()
    raise RuntimeError("local shim didn't become healthy within 15s")


def main():
    ensure_golden()
    langs = sorted(EXPECTED_VOICE)
    shim_url = os.environ.get("BILLY_SHIM_URL")
    local_proc = None
    if shim_url:
        print(f"Using existing shim at {shim_url} (BILLY_SHIM_URL set)")
    else:
        print(f"Starting local shim from {SHIM_DIR} against gpu-host's llama.cpp ...")
        local_proc, shim_url = start_local_shim()
        print(f"  up at {shim_url}")

    try:
        with httpx.Client() as client:
            r = client.get(f"{shim_url}/health", timeout=5)
            r.raise_for_status()
            assert r.json().get("llm_reachable"), (
                f"shim at {shim_url} can't reach its LLM -- is gpu-host's llama.cpp up?")

            failures = []
            for lang in langs:
                print(f"--- {lang} ---")
                try:
                    test_language(client, shim_url, lang)
                    print(f"  ok — {lang}")
                except Exception as e:  # noqa: BLE001 -- collect all failures, report together
                    print(f"  FAILED — {lang}: {e}")
                    failures.append((lang, e))
    finally:
        if local_proc:
            local_proc.terminate()
            local_proc.wait(timeout=5)

    if failures:
        raise SystemExit(f"{len(failures)}/{len(langs)} language(s) failed: "
                          f"{', '.join(l for l, _ in failures)}")
    print(f"ok — all {len(langs)} languages passed end-to-end")


if __name__ == "__main__":
    main()
