#!/usr/bin/env python3
"""Unit tests for the shim's text-shaping core. No pytest dependency — run directly:

    ./.venv/bin/python test_text.py
"""
from text import (strip_markup, strip_think, sentences_from, voice_for, language_instruction,
                   sentence_language, voice_for_sentence, DEFAULT_VOICE, LANGUAGE_VOICES)


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


def test_voice_for():
    check(voice_for(None), DEFAULT_VOICE, "no language -> default voice")
    check(voice_for("english"), DEFAULT_VOICE, "english -> default voice")
    check(voice_for("klingon"), DEFAULT_VOICE, "unrecognized language -> default voice")
    check(voice_for("french"), DEFAULT_VOICE,
          "recognized but permanently unenabled (no Kokoro male voice) -> default voice")
    check(voice_for("ENGLISH"), DEFAULT_VOICE, "case-insensitive")
    check(voice_for("spanish"), "em_alex", "spanish (P1) -> em_alex")
    check(voice_for("portuguese"), "pm_alex", "portuguese (P1) -> pm_alex")
    check(voice_for("chinese"), "zm_yunjian", "chinese (P1) -> zm_yunjian")
    check(voice_for("japanese"), "jm_kumo", "japanese (P2) -> jm_kumo")
    check(voice_for("hindi"), "hm_omega", "hindi (P2) -> hm_omega")
    check(voice_for("italian"), "im_nicola", "italian (P2) -> im_nicola")
    check(voice_for("SPANISH"), "em_alex", "case-insensitive for an enabled language too")


def test_language_instruction():
    check(language_instruction(None), None, "no language -> no instruction")
    check(language_instruction("english"), None, "english -> no instruction")
    check(language_instruction("klingon"), None, "unrecognized language -> no instruction")
    check(language_instruction("french"), None,
          "recognized but permanently unenabled (no Kokoro male voice) -> no instruction")
    check(language_instruction("spanish"), "Reply in Spanish.", "enabled (P1) -> instruction")
    check(language_instruction("italian"), "Reply in Italian.", "enabled (P2) -> instruction")


def test_sentence_language():
    check(sentence_language("Well hello there, I am Billy the smartest fish on this wall."),
          "english", "clear english sentence")
    check(sentence_language("Vaya, miren quien vino a visitarme."), "spanish", "clear spanish sentence")
    check(sentence_language("哎呀,看看是谁来了。"), "chinese", "clear chinese sentence")
    check(sentence_language("OK."), None, "too short to carry a signal -> None")


def test_voice_for_sentence():
    check(voice_for_sentence("Sure thing, pal.", DEFAULT_VOICE), DEFAULT_VOICE,
          "english sentence keeps the turn voice")
    check(voice_for_sentence("OK.", "em_alex"), "em_alex",
          "low-confidence sentence keeps the turn voice, not DEFAULT_VOICE")
    check(voice_for_sentence("Vaya, miren quien vino a visitarme.", DEFAULT_VOICE),
          "em_alex", "enabled-language (P1 spanish) sentence overrides the turn voice")
    check(voice_for_sentence("Ciao, come stai?", "em_alex"), "im_nicola",
          "enabled-language (P2 italian) sentence overrides the turn voice")
    # All 6 non-English target languages are enabled now, so there's no real language left to
    # exercise the not-yet-enabled/turn-voice-fallback branch against -- disable one temporarily
    # (restored after) to keep that branch covered for whenever a future language lands disabled.
    saved = LANGUAGE_VOICES.pop("italian")
    try:
        check(voice_for_sentence("Ciao, come stai?", "em_alex"), "em_alex",
              "not-yet-enabled sentence keeps the turn voice, not DEFAULT_VOICE")
    finally:
        LANGUAGE_VOICES["italian"] = saved


if __name__ == "__main__":
    test_strip_markup()
    test_strip_think()
    test_sentences_from()
    test_voice_for()
    test_language_instruction()
    test_sentence_language()
    test_voice_for_sentence()
    print("ok — all shim text tests passed")
