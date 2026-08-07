#!/usr/bin/env python3
"""Unit tests for the shim's session bookkeeping (idle-based history reset). No pytest
dependency — run directly:

    ./.venv/bin/python test_session.py
"""
import billy_shim as shim


def check(got, want, label):
    assert got == want, f"{label}: expected {want!r}, got {got!r}"


def test_session_persists_within_ttl():
    shim._SESSIONS.clear()
    shim._LAST_SEEN.clear()
    fake_now = [1000.0]
    shim.time.monotonic = lambda: fake_now[0]

    shim._session("s1").append({"role": "user", "content": "hi"})
    fake_now[0] += shim.IDLE_RESET_SECONDS - 1   # still inside the TTL
    msgs = shim._session("s1")
    check(len(msgs), 2, "history kept when the gap is under the TTL")


def test_session_resets_after_idle_gap():
    shim._SESSIONS.clear()
    shim._LAST_SEEN.clear()
    fake_now = [2000.0]
    shim.time.monotonic = lambda: fake_now[0]

    shim._session("s2").append({"role": "user", "content": "hi"})
    fake_now[0] += shim.IDLE_RESET_SECONDS + 1   # past the TTL
    msgs = shim._session("s2")
    check(len(msgs), 1, "history dropped once the gap exceeds the TTL")   # fresh system prompt only


def test_sessions_are_independent():
    shim._SESSIONS.clear()
    shim._LAST_SEEN.clear()
    fake_now = [3000.0]
    shim.time.monotonic = lambda: fake_now[0]

    shim._session("a").append({"role": "user", "content": "hi from a"})
    fake_now[0] += shim.IDLE_RESET_SECONDS + 1
    shim._session("b").append({"role": "user", "content": "hi from b"})   # resets "a", not "b"
    check(len(shim._session("a")), 1, "idle session reset")
    check(len(shim._session("b")), 2, "unrelated session untouched")


if __name__ == "__main__":
    test_session_persists_within_ttl()
    test_session_resets_after_idle_gap()
    test_sessions_are_independent()
    print("ok — all shim session tests passed")
