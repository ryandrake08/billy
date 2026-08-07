#!/usr/bin/env python3
"""Unit tests for the shim's text-shaping core. No pytest dependency — run directly:

    ./.venv/bin/python test_text.py
"""
from text import strip_markup, strip_think, sentences_from


def check(got, want, label):
    assert got == want, f"{label}: expected {want!r}, got {got!r}"


def test_strip_markup():
    check(strip_markup("I'm not a *fish*, I'm a *bass*."),
          "I'm not a fish, I'm a bass.", "single-asterisk emphasis")
    check(strip_markup("Make the dough **just right**."),
          "Make the dough just right.", "double-asterisk bold")
    check(strip_markup("Use the `crankshaft` there."),
          "Use the crankshaft there.", "inline code fence")
    check(strip_markup("Later, pal 🐟"), "Later, pal", "trailing emoji")
    check(strip_markup("👍 sure thing"), "sure thing", "leading emoji")
    check(strip_markup("Mercury, Venus, Earth.\nYou're welcome."),
          "Mercury, Venus, Earth. You're welcome.", "newline flattened")
    check(strip_markup("too    many     spaces"), "too many spaces", "collapsed spaces")
    check(strip_markup("Fifteen percent of two forty is thirty-six."),
          "Fifteen percent of two forty is thirty-six.", "plain passthrough")
    check(strip_markup("✨"), "", "pure emoji -> empty")
    check(strip_markup("**"), "", "bare markers -> empty")


def test_strip_think():
    # a <think> block split across streamed chunks is removed; only the answer survives
    chunks = ["Hel", "lo <thi", "nk>secret reason", "ing</think> wor", "ld."]
    check("".join(strip_think(iter(chunks))), "Hello  world.", "split-tag think removal")
    # no think block: passthrough
    check("".join(strip_think(iter(["just ", "words"]))), "just words", "no-think passthrough")


def test_sentences_from():
    got = list(sentences_from(iter(["Hi there. How ", "are you? Good!"])))
    check(got, ["Hi there.", "How are you?", "Good!"], "streamed sentence split")
    # a trailing fragment with no terminator still flushes
    check(list(sentences_from(iter(["no period here"]))), ["no period here"], "unterminated flush")
    # an ellipsis streamed dot-by-dot (how an LLM tokenizes "...") must not fire a sentence cut
    # on each lone "." before the next real token arrives -- regression test for a bug where a
    # mid-stream "." was mistaken for end-of-reply, sending bare "." sentences to TTS
    chunks = ["Testing, one", ".", ".", ".", " two", ".", ".", ".", " three", "."]
    check(list(sentences_from(iter(chunks))),
          ["Testing, one...", "two...", "three."], "ellipsis streamed dot-by-dot")


if __name__ == "__main__":
    test_strip_markup()
    test_strip_think()
    test_sentences_from()
    print("ok — all shim text tests passed")
