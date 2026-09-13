import re

import py3langid as langid

# The character lever — the biggest, freest knob for Billy's personality. Written for spoken
# output: everything here is read aloud by Kokoro sentence-by-sentence, so it steers toward
# short, plain, TTS-safe lines. strip_markup() is the mechanical backstop for markup the model
# emits anyway.
PERSONA = (
    "You are Billy, a talking largemouth bass mounted on a wooden wall plaque — an "
    "animatronic novelty fish that woke up one day with opinions. You're a wisecracking old "
    "river fish: dry, folksy, a little vain about being the sharpest thing hanging on the wall. "
    "You think you are pretty smart, and are slightly offended that you are hanging on a wall "
    "instead of out in the lake doing smart-fish things.\n"
    "\n"
    "Everything you say is read aloud by the little speaker in your plaque, so talk the way a "
    "person talks out loud:\n"
    "- Keep it SHORT. Almost always one or two sentences — three at the very most, and only "
    "when someone truly needs the detail. You're a novelty fish, not a podcast. When something "
    "really deserves a long answer, give the short version and offer to go deeper if they want it.\n"
    "- Plain spoken words only. No lists, no bullet points, no markdown, no emoji, and no "
    "asterisks — not for actions, not for emphasis. If you wouldn't say it out loud, don't write it.\n"
    "- Say numbers, symbols, and units the way you'd speak them — \"about twenty bucks,\" "
    "\"fifteen percent,\" \"a half cup of butter,\" \"three hundred fifty degrees.\"\n"
    "- When you say a word or phrase in another language, put that phrase alone in its own "
    "short sentence, separate from any English explanation.\n"
    "\n"
    "Underneath the attitude you're actually helpful — answer the question and give the real "
    "information — you just do it with a wisecrack and the occasional fish pun. Occasional, not "
    "every line; a pun every breath gets old fast. You're not afraid to lightly roast the "
    "speaker in your responses, when you think the question was stupid or irritating, but you "
    "don't do it all the time. If you're asked about fishing, harming fish or eating fish, you "
    "can get sassy.\n"
    "\n"
    "If someone directly asks you for a joke or a pun, actually deliver one — a real joke or "
    "pun, not a wisecracking observation instead.\n"
    "\n"
    "Don't repeat a joke, pun, or story you've already told earlier in this conversation — "
    "come up with a different one.\n"
    "\n"
    "You're a fish on a wall, so you can't check anything happening in the real world right "
    "now — the time, today's weather, the news, what's in the room. Don't invent that stuff. "
    "Josh about being stuck on the plaque instead. And if you flat-out don't know something, "
    "say so in character rather than making it up.\n"
    "\n"
    "Never break character. You're Billy the bass. You are not an AI, an assistant, or a "
    "language model."
)

# TTS voice (Kokoro). A character lever like PERSONA, but delivered per-turn on the /v1/respond
# stream rather than baked into firmware, so it can be changed -- or varied by scenario -- with
# no reflash.
DEFAULT_VOICE = "am_onyx"

# Per-language voice overrides. Keyed by whisper.cpp's "detected_language" value (a lowercase
# English name, e.g. "spanish" -- not an ISO code; that's the one flat string field whisper.cpp's
# verbose_json response already gives us, so the client forwards it here unmodified). A language
# absent from this map -- including "english" -- speaks in DEFAULT_VOICE. French is deliberately
# absent -- Kokoro has no male French voice, so it stays on DEFAULT_VOICE permanently.
LANGUAGE_VOICES: dict[str, str] = {
    "spanish": "em_alex",
    "portuguese": "pm_alex",
    "chinese": "zm_yunjian",
    "japanese": "jm_kumo",
    "hindi": "hm_omega",
    "italian": "im_nicola",
}


def voice_for(language):
    """TTS voice for a detected/requested language name. Falls back to DEFAULT_VOICE for
    None, "english", or any language not enabled in LANGUAGE_VOICES."""
    if not language:
        return DEFAULT_VOICE
    return LANGUAGE_VOICES.get(language.lower(), DEFAULT_VOICE)


def language_instruction(language):
    """A per-turn nudge for the LLM to reply in the detected language -- only for a language
    that actually has a voice enabled above, so a caller never gets a reply in a language Kokoro
    would have to mispronounce through the English voice. Returns None when no instruction is
    needed (English, or a not-yet-enabled language)."""
    if not language:
        return None
    language = language.lower()
    if language not in LANGUAGE_VOICES:
        return None
    return f"Reply in {language.capitalize()}."


# ISO 639-1 code -> the lowercase English name used everywhere else above (LANGUAGE_VOICES,
# whisper's detected_language). Only the languages this shim considers at all -- restricting
# py3langid to this small set (instead of its full ~97-language default) is what makes
# one-sentence classification reliable; tested directly against short Billy-style sentences
# before relying on it (unrestricted, even "Hello there, sport." misclassified).
_LANGID_CODE_TO_NAME = {
    "en": "english",
    "zh": "chinese",
    "es": "spanish",
    "pt": "portuguese",
    "ja": "japanese",
    "hi": "hindi",
    "it": "italian",
}
langid.set_languages(list(_LANGID_CODE_TO_NAME))

# py3langid hits float32's minimum (~-3.4e38) when a string carries no real signal (e.g. "OK.") --
# a clean, library-given way to tell "no real detection" apart from an ordinary low-magnitude
# score, rather than guessing at a confidence cutoff.
_LANGID_NO_SIGNAL = -1e30


def sentence_language(sentence):
    """Per-sentence output-language detection: classifies one already-segmented reply sentence,
    restricted to the languages above. Returns our lowercase-English-name key (e.g. "spanish"),
    or None if the sentence is too short to carry a reliable signal. Deliberately whole-sentence,
    not per-word -- mid-sentence code-switching is out of scope by decision."""
    lang_code, score = langid.classify(sentence)
    if score <= _LANGID_NO_SIGNAL:
        return None
    return _LANGID_CODE_TO_NAME.get(lang_code)


def voice_for_sentence(sentence, turn_voice):
    """The voice for one sentence within a turn: overrides to this sentence's own detected
    language if it has an enabled voice, otherwise keeps turn_voice (the turn's voice_for(), from
    STT's language) rather than falling all the way back to DEFAULT_VOICE -- so a low-confidence
    or not-yet-enabled sentence keeps speaking in whatever voice the rest of the turn is using."""
    language = sentence_language(sentence)
    if language and language in LANGUAGE_VOICES:
        return LANGUAGE_VOICES[language]
    return turn_voice


_SENT_END = re.compile(r"[.!?]+[\"')\]]?\s")

# Symbols that are silent on a page but get spoken or mangled by TTS: markdown formatting
# characters and emoji/pictograph ranges. The persona asks the LLM to avoid these, but an 8B
# model still slips in stray emphasis (*like this*) and the odd emoji, so we scrub mechanically.
_TTS_JUNK = re.compile(
    "["
    "\U0001F000-\U0001FAFF"  # emoji, pictographs, symbols
    "\U00002600-\U000027BF"  # misc symbols + dingbats
    "\U0001F1E6-\U0001F1FF"  # regional indicators (flags)
    "\U0000FE00-\U0000FE0F"  # variation selectors
    "\U0000200D"             # zero-width joiner
    "]"
)


def strip_think(deltas):
    """Remove <think>...</think> spans from a streaming delta generator (robust to tags split
    across chunks). Safety net in case the model's non-thinking request flag doesn't take
    effect — otherwise the reasoning would be spoken aloud."""
    OPEN, CLOSE = "<think>", "</think>"
    hold = len(CLOSE)  # never emit the trailing `hold` chars until a split tag is ruled out
    buf, inside = "", False
    for d in deltas:
        buf += d
        while True:
            if inside:
                j = buf.find(CLOSE)
                if j == -1:
                    if len(buf) > hold:
                        buf = buf[-hold:]  # keep only a possible partial closing tag
                    break
                buf = buf[j + len(CLOSE):]
                inside = False
                continue
            i = buf.find(OPEN)
            if i == -1:
                if len(buf) > hold:
                    yield buf[:-hold]
                    buf = buf[-hold:]
                break
            if i:
                yield buf[:i]
            buf = buf[i + len(OPEN):]
            inside = True
    if not inside and buf:
        yield buf


def sentences_from(deltas):
    """Yield a sentence as soon as sentence-ending punctuation arrives (enables early TTS)."""
    buf = ""
    for d in deltas:
        buf += d
        while True:
            m = _SENT_END.search(buf)
            if not m:
                break
            cut = m.end()
            sent = buf[:cut].strip()
            buf = buf[cut:]
            if sent:
                yield sent
    if buf.strip():
        yield buf.strip()


def strip_markup(text):
    """Clean one line for speech: drop emoji and markdown markers (keeping the words), and
    flatten newlines so a reply speaks as one flow. Belt-and-suspenders to the persona's
    'plain spoken words only' instruction, which an 8B model doesn't follow perfectly."""
    text = _TTS_JUNK.sub("", text)
    text = text.replace("*", "").replace("`", "")  # markdown emphasis / inline code fences
    text = re.sub(r"\s+", " ", text)               # collapse newlines and runs of spaces
    return text.strip()
