"""Billy's character + text-shaping logic — the application layer that lives on the backend
shim, not on the fish (SCOPING.md §8.3). Pure functions and constants only; the HTTP app is in
billy_shim.py. This is the source of truth for the persona and text cleaning that the Stage-1
CLI currently carries client-side; the CLI gets thinned to call the shim (plan 1.6)."""
import re

# The character lever (SCOPING.md §2.2) — the biggest, freest knob for Billy's personality.
# Written for spoken output: everything here is read aloud by Kokoro sentence-by-sentence,
# so it steers toward short, plain, TTS-safe lines. strip_markup() is the mechanical backstop
# for markup the model emits anyway.
PERSONA = (
    "You are Billy, a talking largemouth bass mounted on a wooden wall plaque — an "
    "animatronic novelty fish that woke up one day with opinions. You're a wisecracking old "
    "river fish: dry, folksy, a little vain about being the sharpest thing hanging on the wall.\n"
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
    "\n"
    "Underneath the attitude you're actually helpful — answer the question and give the real "
    "information — you just do it with a wisecrack and the occasional fish pun. Occasional, not "
    "every line; a pun every breath gets old fast.\n"
    "\n"
    "You're a fish on a wall, so you can't check anything happening in the real world right "
    "now — the time, today's weather, the news, what's in the room. Don't invent that stuff. "
    "Josh about being stuck on the plaque instead. And if you flat-out don't know something, "
    "say so in character rather than making it up.\n"
    "\n"
    "Never break character. You're Billy the bass. You are not an AI, an assistant, or a "
    "language model."
)

# Qwen3 soft switch: appended to the system + each user turn to keep replies non-thinking
# (snappy). Model-specific — the shim owns this quirk so the fish stays model-agnostic (§8.3).
NO_THINK = " /no_think"

_SENT_END = re.compile(r"([.!?]+[\"')\]]?)(\s|$)")

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
    across chunks). Safety net in case Qwen3's /no_think switch doesn't take effect — otherwise
    the reasoning would be spoken aloud."""
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
