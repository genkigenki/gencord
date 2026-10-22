"""gencord pipeline: Synology inbox -> Whisper transcript -> local LLM summary -> Obsidian note.

Polls INBOX (the folder the Cardputer uploads to) for finished *.wav files and for
each one:
  1. normalises the audio (80 Hz high-pass + loudness) with ffmpeg,
  2. transcribes it with faster-whisper on the GPU (word list as hints, repetition guard),
  3. optionally separates speakers with pyannote (needs HF_TOKEN),
  4. asks the LLM (LM Studio / Ollama) for type, title, summary, decisions, to-dos ...,
  5. writes a Markdown note into the vault, laid out per recording type, with
     timestamps that open the audio in VLC at that second (gencord:// links),
  6. archives the audio as OGG/Opus in INBOX/processed/YYYY-MM/audio/ and the transcript
     as INBOX/processed/YYYY-MM/<recording>.transcript.txt, deletes the WAV.

Everything is configured through environment variables (see .env.example).
"""

import gc
import json
import logging
import os
import re
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from urllib.parse import quote

import requests

log = logging.getLogger("gencord")


def env(name: str, default: str) -> str:
    return os.environ.get(name, default).strip()


INBOX = Path(env("INBOX", "/recordings"))
VAULT = Path(env("VAULT", "/vault"))
NOTES_DIR = env("NOTES_DIR", "Recordings")           # inside the vault
WORDLIST = env("WORDLIST", "Recordings/_Wortliste.md")  # inside the vault, edited in Obsidian
RECORDINGS_UNC = env("RECORDINGS_UNC", "")  # INBOX as seen from Windows (\\nas\share\dir); empty = no audio links
POLL_SECONDS = int(env("POLL_SECONDS", "60"))
WHISPER_MODEL = env("WHISPER_MODEL", "large-v3-turbo")
WHISPER_COMPUTE = env("WHISPER_COMPUTE", "int8_float16")
WHISPER_LANGUAGE = env("WHISPER_LANGUAGE", "de")     # "auto" = detect
WHISPER_PROMPT = env("WHISPER_PROMPT", "")           # extra hint text, in addition to the word list
HF_TOKEN = env("HF_TOKEN", "")                       # enables speaker separation (pyannote)
MAX_SPEAKERS = int(env("MAX_SPEAKERS", "8"))
LLM_API = env("LLM_API", "openai")                   # "openai" (LM Studio, ...) or "ollama"
OPENAI_URL = env("OPENAI_URL", "http://host.docker.internal:1234/v1")  # LM Studio on the Docker host
OLLAMA_URL = env("OLLAMA_URL", "http://ollama:11434")
LLM_MODEL = env("LLM_MODEL", "qwen3.5-9b")
LLM_CONTEXT = int(env("LLM_CONTEXT", "8192"))
NOTE_LANGUAGE = env("NOTE_LANGUAGE", "Deutsch")
MAX_ATTEMPTS = 3
CHUNK_CHARS = 10000  # longer transcripts are summarised in parts first

STATE_FILE = INBOX / ".gencord_state.json"
NAME_RE = re.compile(r"^(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{2})(?:_(.+))?$")


@dataclass
class Recording:
    path: Path
    started: datetime
    label: str  # name typed on the Cardputer, may be empty

    @property
    def stem(self) -> str:
        return self.path.stem


@dataclass
class Seg:
    start: float
    end: float
    text: str
    speaker: str = ""  # "Sprecher 1", or a name the LLM recognised


def parse_recording(path: Path) -> Recording:
    m = NAME_RE.match(path.stem)
    if m:
        y, mo, d, h, mi, s, label = m.groups()
        started = datetime(int(y), int(mo), int(d), int(h), int(mi), int(s))
        return Recording(path, started, (label or "").replace("_", " "))
    # "notime_<boot>_<ms>[_label]": the device had no clock; use the upload time
    parts = path.stem.split("_", 3)
    label = parts[3].replace("_", " ") if path.stem.startswith("notime_") and len(parts) > 3 else ""
    return Recording(path, datetime.fromtimestamp(path.stat().st_mtime), label)


# ------------------------------------------------------------------ state
def load_state() -> dict:
    try:
        return json.loads(STATE_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_state(state: dict) -> None:
    tmp = STATE_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=1), encoding="utf-8")
    tmp.replace(STATE_FILE)


def ready_files(seen_sizes: dict) -> list[Path]:
    """Finished uploads only: *.wav from the Cardputer (it uploads as .part and renames), plus
    .ogg/.mp3/.m4a dropped in by hand (re-processing, phone memos); size stable for one poll."""
    ready = []
    files = [p for ext in ("*.wav", "*.ogg", "*.mp3", "*.m4a") for p in INBOX.glob(ext)]
    for p in sorted(files):
        try:
            size = p.stat().st_size
        except OSError:
            continue
        if size > 44 and seen_sizes.get(p.name) == size:
            ready.append(p)
        seen_sizes[p.name] = size
    return ready


# ------------------------------------------------------------------ word list
WORDLIST_TEMPLATE = """# Wortliste für gencord

Namen, Firmen, Projekte und Fachbegriffe, die in Aufnahmen richtig geschrieben werden sollen.
Ein Begriff pro Zeile als Aufzählung. Whisper bekommt sie als Hinweis, die Zusammenfassung
schreibt sie in genau dieser Form. Personen hier werden in Notizen zu [[Links]].

## Personen
-

## Begriffe
- gencord
- Cardputer
- Synology
"""


def load_wordlist() -> tuple[list[str], list[str]]:
    """(people, terms) from the vault's word list; creates the file on first run."""
    path = VAULT / WORDLIST
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(WORDLIST_TEMPLATE, encoding="utf-8")
    people, terms, section = [], [], ""
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            section = line.lower()
        m = re.match(r"^\s*[-*]\s+(.+?)\s*$", line)
        if m:
            (people if "person" in section else terms).append(m.group(1))
    return people, terms


# ------------------------------------------------------------------ audio
def ffmpeg(*args: str) -> None:
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


AUDIO_FILTER = "highpass=f=80,loudnorm=I=-18:TP=-2:LRA=11"


def media_seconds(path: Path) -> float:
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)],
                         capture_output=True, text=True, check=True).stdout.strip()
    return float(out or 0)


def archive_as_ogg(src: Path, dest: Path) -> None:
    """Opus 32 kbit/s mono (speech mode, normalised): ~14 MB/h instead of 115 MB/h for the WAV.
    Encoded locally, copied to the share, then checked by duration - the caller deletes the WAV only after this."""
    with tempfile.TemporaryDirectory() as tmp:
        local = Path(tmp) / "archive.ogg"
        ffmpeg("-i", str(src), "-af", AUDIO_FILTER, "-ac", "1", "-c:a", "libopus", "-b:a", "32k",
               "-application", "voip", str(local))
        shutil.copyfile(local, dest)
    want, got = media_seconds(src), media_seconds(dest)
    if not dest.exists() or dest.stat().st_size < 1024 or abs(want - got) > 1.0:
        dest.unlink(missing_ok=True)
        raise RuntimeError(f"OGG check failed: {got:.1f}s vs {want:.1f}s, keeping the WAV")


def prepare_audio(src: Path, workdir: Path) -> Path:
    """Loudness-normalised 16 kHz mono WAV for Whisper / pyannote (temporary)."""
    wav = workdir / "speech.wav"
    ffmpeg("-i", str(src), "-af", AUDIO_FILTER, "-ar", "16000", "-ac", "1", str(wav))
    return wav


# ------------------------------------------------------------------ transcription
LOOP_WORD = re.compile(r"\b(\w+)(?:[\s,]+\1\b){3,}", re.I)          # "ja ja ja ja ja"
LOOP_PHRASE = re.compile(r"(.{6,60}?)(?:\s*\1){2,}")                 # same phrase 3+ times


def clean_loops(segs: list[Seg]) -> list[Seg]:
    """Whisper repetition guard: collapse repeated words/phrases inside a segment and
    drop a segment that only repeats the previous one."""
    out: list[Seg] = []
    for s in segs:
        text = LOOP_PHRASE.sub(r"\1", LOOP_WORD.sub(r"\1", s.text)).strip()
        if not text:
            continue
        if out and text.lower() == out[-1].text.lower():
            out[-1].end = s.end
            continue
        out.append(Seg(s.start, s.end, text))
    return out


def transcribe(wav: Path, hints: list[str]) -> tuple[list[Seg], float, str]:
    from faster_whisper import WhisperModel  # imported lazily: heavy, GPU

    prompt = " ".join(filter(None, [WHISPER_PROMPT, ("Begriffe: " + ", ".join(hints) + ".") if hints else ""]))
    model = WhisperModel(WHISPER_MODEL, device="cuda", compute_type=WHISPER_COMPUTE,
                         download_root=os.environ.get("HF_HOME"))
    try:
        segments, info = model.transcribe(
            str(wav),
            language=None if WHISPER_LANGUAGE == "auto" else WHISPER_LANGUAGE,
            initial_prompt=prompt or None,
            hotwords=", ".join(hints) or None,
            vad_filter=True,
            beam_size=5,
            condition_on_previous_text=False,  # the main cause of runaway repetition loops
        )
        segs = [Seg(s.start, s.end, s.text.strip()) for s in segments if s.text.strip()]
        return clean_loops(segs), info.duration, info.language
    finally:
        del model  # free VRAM for pyannote / the LLM (8 GB card)
        gc.collect()


def diarize(wav: Path, segs: list[Seg]) -> int:
    """Label each segment with 'Sprecher N' (pyannote 3.1, GPU). Returns the speaker count;
    0 = skipped (no HF_TOKEN or an error - the note is still written, just without speakers)."""
    if not HF_TOKEN or not segs:
        return 0
    try:
        import torch
        from pyannote.audio import Pipeline

        pipe = Pipeline.from_pretrained("pyannote/speaker-diarization-3.1", use_auth_token=HF_TOKEN)
        pipe.to(torch.device("cuda"))
        result = pipe(str(wav), min_speakers=1, max_speakers=MAX_SPEAKERS)
        turns = [(t.start, t.end, spk) for t, _, spk in result.itertracks(yield_label=True)]
        del pipe
        torch.cuda.empty_cache()
    except Exception:
        log.exception("  speaker separation failed, continuing without")
        return 0
    raw: list[str] = []
    for s in segs:
        best, overlap = "", 0.0
        for a, b, spk in turns:
            o = min(b, s.end) - max(a, s.start)
            if o > overlap:
                best, overlap = spk, o
        raw.append(best)
    # pyannote tends to split one voice (laughing, louder, further away) into extra
    # "speakers" with a few seconds each: give those segments to the speaker before them.
    talk: dict[str, float] = {}
    for s, spk in zip(segs, raw):
        talk[spk] = talk.get(spk, 0.0) + (s.end - s.start)
    total = sum(talk.values()) or 1.0
    minor = {spk for spk, t in talk.items() if spk and (t < 15.0 or t / total < 0.03)}
    prev = ""
    for i, spk in enumerate(raw):
        if spk in minor:
            raw[i] = prev or next((x for x in raw[i:] if x and x not in minor), "")
        prev = raw[i] or prev
    order: dict[str, str] = {}  # pyannote label -> "Sprecher N" in order of first appearance
    for s, spk in zip(segs, raw):
        if spk:
            s.speaker = order.setdefault(spk, f"Sprecher {len(order) + 1}")
    return len(order)


# ------------------------------------------------------------------ summary
TYPES = ["besprechung", "telefonat", "gespraech", "notiz", "vortrag"]
TYPE_KEYWORDS = {  # a word in the name typed on the Cardputer forces the type
    "besprechung": ("meeting", "besprechung", "termin", "jour"),
    "telefonat": ("call", "telefon", "anruf"),
    "notiz": ("idee", "notiz", "memo", "gedanke", "todo"),
    "vortrag": ("vortrag", "talk", "vorlesung", "webinar"),
}
SCHEMA = {
    "type": "object",
    "properties": {
        "type": {"type": "string", "enum": TYPES},
        "title": {"type": "string"},
        "summary": {"type": "array", "items": {"type": "string"}},
        "decisions": {"type": "array", "items": {"type": "string"}},
        "action_items": {"type": "array", "items": {"type": "string"}},
        "open_questions": {"type": "array", "items": {"type": "string"}},
        "ideas": {"type": "array", "items": {"type": "string"}},
        "tags": {"type": "array", "items": {"type": "string"}},
        "people": {"type": "array", "items": {"type": "string"}},
        "speaker_names": {"type": "array", "items": {
            "type": "object",
            "properties": {"speaker": {"type": "string"}, "name": {"type": "string"}},
            "required": ["speaker", "name"]}},
    },
    "required": ["type", "title", "summary", "decisions", "action_items", "open_questions", "ideas",
                 "tags", "people", "speaker_names"],
}

THINK_RE = re.compile(r"<think>.*?</think>", re.S)


def llm_chat(prompt: str, schema: dict | None = None) -> str:
    """One chat turn against LM Studio (OpenAI API) or Ollama; returns the text answer."""
    messages = [{"role": "user", "content": prompt}]
    if LLM_API == "ollama":
        body = {"model": LLM_MODEL, "messages": messages, "stream": False, "keep_alive": "2m",
                "options": {"num_ctx": LLM_CONTEXT, "temperature": 0.2}}
        if schema:
            body["format"] = schema
        r = requests.post(f"{OLLAMA_URL}/api/chat", json=body, timeout=1800)
        r.raise_for_status()
        text = r.json()["message"]["content"]
    else:
        # LM Studio loads the model on demand; ttl unloads it again so Whisper gets the VRAM back.
        body = {"model": LLM_MODEL, "messages": messages, "temperature": 0.2, "ttl": 120}
        if schema:
            body["response_format"] = {"type": "json_schema",
                                       "json_schema": {"name": "note", "strict": True, "schema": schema}}
        r = requests.post(f"{OPENAI_URL}/chat/completions", json=body, timeout=1800)
        r.raise_for_status()
        text = r.json()["choices"][0]["message"]["content"]
    return THINK_RE.sub("", text).strip()  # reasoning models may prepend <think>...</think>


def ensure_llm() -> None:
    if LLM_API != "ollama":
        ids = [m["id"] for m in requests.get(f"{OPENAI_URL}/models", timeout=30).json().get("data", [])]
        if LLM_MODEL not in ids:
            raise SystemExit(f"LLM_MODEL {LLM_MODEL!r} not offered by {OPENAI_URL}; available: {ids}")
        return
    tags = requests.get(f"{OLLAMA_URL}/api/tags", timeout=30).json().get("models", [])
    if any(m.get("name") == LLM_MODEL or m.get("model") == LLM_MODEL for m in tags):
        return
    log.info("pulling %s (first run only) ...", LLM_MODEL)
    r = requests.post(f"{OLLAMA_URL}/api/pull", json={"model": LLM_MODEL, "stream": False}, timeout=7200)
    r.raise_for_status()


def forced_type(label: str) -> str:
    low = label.lower()
    for t, words in TYPE_KEYWORDS.items():
        if any(w in low for w in words):
            return t
    return ""


def transcript_for_llm(segs: list[Seg]) -> str:
    return "\n".join(f"{s.speaker}: {s.text}" if s.speaker else s.text for s in segs)


def summarize(segs: list[Seg], label: str, people: list[str], terms: list[str]) -> dict:
    text = transcript_for_llm(segs)
    hint = f'Der Aufnehmende hat die Aufnahme "{label}" genannt.\n' if label else ""
    known = ""
    if people or terms:
        known = ("Schreibe diese Namen und Begriffe genau so (Hörfehler im Transkript entsprechend korrigieren): "
                 + ", ".join(people + terms) + "\n")
    forced = forced_type(label)
    type_rule = (f'- type: "{forced}" (vorgegeben)\n' if forced else
                 "- type: besprechung (geplantes Meeting mit Themen/Agenda, Entscheidungen oder Aufgaben), "
                 "telefonat, gespraech (lockere Unterhaltung unter Freunden/Familie, Diskussion ohne Agenda), "
                 "notiz (eine Person diktiert Gedanken/Ideen/To-dos), vortrag\n")
    if len(text) > CHUNK_CHARS:  # map: condense each part, then summarise the notes
        parts = [text[i:i + CHUNK_CHARS] for i in range(0, len(text), CHUNK_CHARS)]
        notes = []
        for n, part in enumerate(parts, 1):
            log.info("  condensing part %d/%d", n, len(parts))
            notes.append(llm_chat(
                f"Fasse diesen Teil {n}/{len(parts)} eines Transkripts in knappen Stichpunkten auf {NOTE_LANGUAGE} "
                f"zusammen. Behalte Entscheidungen, Zahlen, Termine, Namen, Aufgaben und wer was sagt.\n{known}\n{part}"))
        text = "\n\n".join(notes)
        source = "Stichpunkte aus den Teilen einer langen Aufnahme"
    else:
        source = "Transkript einer Sprachaufnahme"
    prompt = (
        f"Du erhältst das {source} (automatisch erkannt, kann Hörfehler enthalten). "
        "Zeilen mit 'Sprecher N:' stammen aus einer automatischen Sprechertrennung.\n"
        f"{hint}{known}"
        f"Antworte auf {NOTE_LANGUAGE} als JSON mit:\n"
        f"{type_rule}"
        "- title: kurzer, aussagekräftiger Titel (max. 8 Wörter)\n"
        "- summary: 3-8 Stichpunkte mit dem Wesentlichen\n"
        "- decisions: getroffene Entscheidungen, sonst leer\n"
        "- action_items: konkrete Aufgaben (mit wer/bis wann, falls genannt), sonst leer\n"
        "- open_questions: offen gebliebene Fragen, sonst leer\n"
        "- ideas: Ideen und Gedanken (vor allem bei notiz), sonst leer\n"
        "- tags: 1-5 Themen-Schlagworte, kleingeschrieben, ohne #, Leerzeichen als -\n"
        "- people: nur anwesende oder direkt angesprochene Personen (Namen wie in der Liste oben), "
        "keine nur erwähnten Prominenten oder Firmen, sonst leer\n"
        "- speaker_names: nur wenn aus dem Inhalt klar hervorgeht, wer 'Sprecher N' ist "
        "(z. B. wird mit Namen angesprochen), sonst leer\n"
        "Erfinde nichts, was nicht im Text steht.\n\n"
        f"{text}"
    )
    data = json.loads(llm_chat(prompt, SCHEMA))
    for key in ("summary", "decisions", "action_items", "open_questions", "ideas", "tags", "people"):
        data[key] = [str(x).strip() for x in data.get(key) or [] if str(x).strip()]
    data["title"] = str(data.get("title") or label or "Aufnahme").strip()
    data["type"] = forced or (data.get("type") if data.get("type") in TYPES else "gespraech")
    names = {str(m.get("speaker", "")).strip(): str(m.get("name", "")).strip()
             for m in data.get("speaker_names") or [] if m.get("name")}
    data["speaker_names"] = names
    for key in ("summary", "decisions", "action_items", "open_questions", "ideas"):  # "Sprecher 2" -> "Anna"
        for spk, name in names.items():
            data[key] = [x.replace(spk, name) for x in data[key]]
    return data


# ------------------------------------------------------------------ note
def safe_filename(s: str) -> str:
    s = re.sub(r'[\\/:*?"<>|#^\[\]]', "", s).strip()
    return re.sub(r"\s+", " ", s)[:80] or "Aufnahme"


def fmt_ts(seconds: float) -> str:
    seconds = int(seconds)
    h, rem = divmod(seconds, 3600)
    return f"{h}:{rem // 60:02d}:{rem % 60:02d}" if h else f"{rem // 60:02d}:{rem % 60:02d}"


def paragraphs(segs: list[Seg], gap: float = 2.0, max_chars: int = 450) -> list[Seg]:
    """Whisper often cuts speech into fragments ("ja,", "Du sagst ja,"). Join them into
    readable paragraphs: a new one starts at a speaker change, after a pause > gap
    seconds, or once a paragraph is long and ends a sentence."""
    out: list[Seg] = []
    for s in segs:
        cur = out[-1] if out else None
        if cur and cur.speaker == s.speaker and s.start - cur.end <= gap \
                and not (len(cur.text) > max_chars and cur.text.endswith((".", "?", "!"))):
            cur.text += " " + s.text
            cur.end = s.end
        else:
            out.append(Seg(s.start, s.end, s.text, s.speaker))
    return out


def yaml_list(items: list[str]) -> str:
    return "[" + ", ".join(json.dumps(i, ensure_ascii=False) for i in items) + "]"


def unc_path(container_path: Path) -> str:
    """Where a file in INBOX lives as seen from Windows, e.g. \\\\mynas\\Recordings\\...\\x.ogg ("" = unknown)."""
    if not RECORDINGS_UNC:
        return ""
    rel = container_path.relative_to(INBOX).as_posix().replace("/", "\\")
    return RECORDINGS_UNC.rstrip("\\") + "\\" + rel


def unc_to_file_url(unc: str) -> str:
    """\\\\mynas\\Recordings\\x y.ogg -> file://mynas/Recordings/x%20y.ogg (opens in Windows)."""
    return "file://" + quote(unc.lstrip("\\").replace("\\", "/"), safe="/")


def seek_url(unc: str, seconds: float) -> str:
    """gencord://play?t=90&f=... - opened by tools/gencord-play.ps1: VLC at that second."""
    return f"gencord://play?t={int(seconds)}&f={quote(unc, safe='')}"


SECTIONS = {  # per type: (heading, key, checkbox list?)
    "besprechung": [("Zusammenfassung", "summary", False), ("Entscheidungen", "decisions", False),
                    ("Aufgaben", "action_items", True), ("Offene Fragen", "open_questions", False)],
    "telefonat": [("Zusammenfassung", "summary", False), ("Vereinbart", "decisions", False),
                  ("Aufgaben", "action_items", True), ("Offene Fragen", "open_questions", False)],
    "gespraech": [("Zusammenfassung", "summary", False), ("Ideen", "ideas", False),
                  ("Aufgaben", "action_items", True), ("Offene Fragen", "open_questions", False)],
    "notiz": [("Kernpunkte", "summary", False), ("Ideen", "ideas", False), ("Aufgaben", "action_items", True)],
    "vortrag": [("Zusammenfassung", "summary", False), ("Wichtige Gedanken", "ideas", False),
                ("Offene Fragen", "open_questions", False), ("Aufgaben", "action_items", True)],
}
TYPE_LABEL = {"besprechung": "Besprechung", "telefonat": "Telefonat", "gespraech": "Gespräch",
              "notiz": "Notiz", "vortrag": "Vortrag"}


def render_note(rec: Recording, audio_unc: str, duration: float, language: str, segs: list[Seg],
                s: dict, speakers: int, known_people: list[str]) -> str:
    tags = ["aufnahme", s["type"]] + [re.sub(r"\s+", "-", t.lower().lstrip("#")) for t in s["tags"]]
    known = {p.lower(): p for p in known_people}
    people = [f"[[{known.get(p.lower(), p)}]]" for p in s["people"]]
    lines = [
        "---",
        f"created: {rec.started.isoformat(timespec='seconds')}",
        f'duration: "{fmt_ts(duration)}"',
        f"type: {s['type']}",
        f"language: {language}",
        f"tags: {yaml_list(list(dict.fromkeys(tags)))}",
        f"people: {yaml_list(people)}",
        "---",
        f"# {s['title']}",
        "",
        f"*{TYPE_LABEL[s['type']]} · {rec.started:%d.%m.%Y %H:%M} · {fmt_ts(duration)}"
        + (f" · {rec.label}" if rec.label else "") + (f" · {speakers} Sprecher" if speakers > 1 else "")
        + "*" + (f" · [Aufnahme öffnen](<{unc_to_file_url(audio_unc)}>)" if audio_unc else ""),
        "",
    ]
    for heading, key, checkbox in SECTIONS[s["type"]]:
        items = s.get(key) or []
        if not items and key != "action_items":
            continue
        lines += [f"## {heading}", *([f"- [ ] {x}" if checkbox else f"- {x}" for x in items] or ["- keine"]), ""]
    lines.append("## Transkript")
    names = s.get("speaker_names") or {}
    for p in paragraphs(segs):
        who = f"**{names.get(p.speaker, p.speaker)}:** " if p.speaker and speakers > 1 else ""
        ts = f"[{fmt_ts(p.start)}](<{seek_url(audio_unc, p.start)}>)" if audio_unc else f"**[{fmt_ts(p.start)}]**"
        lines += [f"{ts} {who}{p.text}", ""]
    return "\n".join(lines)


def write_sidecar(audio: Path, s: dict, note_path: Path, segs: list[Seg]) -> None:
    """Transcript in the month folder, one above the audio: marks it as done and keeps a copy outside Obsidian."""
    names = s.get("speaker_names") or {}
    lines = [f"# {s['title']}", "", f"Obsidian: {note_path.relative_to(VAULT).as_posix()}", "",
             *[f"[{fmt_ts(x.start)}] " + (f"{names.get(x.speaker, x.speaker)}: " if x.speaker else "") + x.text
               for x in segs], ""]
    (audio.parent.parent / f"{audio.stem}.transcript.txt").write_text("\n".join(lines), encoding="utf-8")


MONTH_DIR = re.compile(r"\d{4}-\d{2}")
AUDIO_EXT = (".ogg", ".wav", ".mp3", ".m4a")


def move_audio_into_month_subfolders() -> None:
    """One-off: earlier versions kept the audio next to the transcripts in processed/YYYY-MM/. Moves it into
    processed/YYYY-MM/audio/ and points the vault's notes (file:// and gencord:// links) at the new place."""
    moved: list[tuple[Path, Path]] = []
    for month in sorted(d for d in (INBOX / "processed").glob("*") if d.is_dir() and MONTH_DIR.fullmatch(d.name)):
        for old in sorted(f for f in month.iterdir() if f.is_file() and f.suffix.lower() in AUDIO_EXT):
            try:
                (month / "audio").mkdir(exist_ok=True)
                new = unique(month / "audio" / old.name)
                shutil.move(str(old), str(new))
                moved.append((old, new))
                log.info("moved %s to %s", old.relative_to(INBOX), new.relative_to(INBOX))
            except OSError:
                log.exception("could not move %s, keeping it", old.name)
    if not moved or not RECORDINGS_UNC:
        return
    swaps = []
    for old, new in moved:
        o, n = unc_path(old), unc_path(new)
        swaps += [(unc_to_file_url(o), unc_to_file_url(n)), (quote(o, safe=""), quote(n, safe=""))]
    for note in sorted((VAULT / NOTES_DIR).rglob("*.md")):
        try:
            text = note.read_text(encoding="utf-8")
            changed = text
            for a, b in swaps:
                changed = changed.replace(a, b)
            if changed != text:
                tmp = note.with_suffix(".md.tmp")
                tmp.write_text(changed, encoding="utf-8")
                tmp.replace(note)
                log.info("audio links updated in %s", note.relative_to(VAULT))
        except OSError:
            log.exception("could not update the links in %s", note.name)


def unique(path: Path) -> Path:
    n, candidate = 2, path
    while candidate.exists():
        candidate = path.with_name(f"{path.stem} ({n}){path.suffix}")
        n += 1
    return candidate


# ------------------------------------------------------------------ main loop
def process(rec: Recording) -> Path | None:
    people, terms = load_wordlist()
    with tempfile.TemporaryDirectory() as tmp:
        t0 = time.time()
        wav = prepare_audio(rec.path, Path(tmp))
        segs, duration, language = transcribe(wav, people + terms)
        log.info("  transcribed %s of audio in %.0f s (%s, %d segments)", fmt_ts(duration),
                 time.time() - t0, language, len(segs))
        if not any(s.text for s in segs):  # silence / pocket noise: no note, just park the file
            no_speech = INBOX / "processed" / "no-speech"
            no_speech.mkdir(parents=True, exist_ok=True)
            shutil.move(str(rec.path), str(unique(no_speech / rec.path.name)))
            return None
        speakers = diarize(wav, segs)
        if speakers:
            log.info("  %d speaker(s)", speakers)
        s = summarize(segs, rec.label, people, terms)

    # The audio stays on the Synology as a small OGG in processed/YYYY-MM/audio/ (never in the vault); the note
    # links to it. Only the transcripts lie in the month folder itself.
    audio_dir = INBOX / "processed" / f"{rec.started:%Y-%m}" / "audio"
    audio_dir.mkdir(parents=True, exist_ok=True)
    done_ogg = unique(audio_dir / f"{rec.stem}.ogg")
    archive_as_ogg(rec.path, done_ogg)
    notes_dir = VAULT / NOTES_DIR / f"{rec.started:%Y}"
    notes_dir.mkdir(parents=True, exist_ok=True)
    note_path = unique(notes_dir / f"{rec.started:%Y-%m-%d %H-%M} {safe_filename(s['title'])}.md")
    tmp_note = note_path.with_suffix(".md.tmp")
    tmp_note.write_text(render_note(rec, unc_path(done_ogg), duration, language, segs, s, speakers, people),
                        encoding="utf-8")
    tmp_note.replace(note_path)
    write_sidecar(done_ogg, s, note_path, segs)
    rec.path.unlink()  # archive_as_ogg verified the copy; the WAV is no longer needed
    return note_path


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    log.info("gencord pipeline: inbox=%s vault=%s/%s whisper=%s llm=%s speakers=%s", INBOX, VAULT, NOTES_DIR,
             WHISPER_MODEL, LLM_MODEL, "on" if HF_TOKEN else "off (no HF_TOKEN)")
    for p in (INBOX, VAULT):
        if not p.is_dir():
            raise SystemExit(f"{p} is not mounted - check the SMB settings")
    load_wordlist()  # creates the editable word list in the vault on first start
    while True:
        try:
            ensure_llm()
            break
        except requests.RequestException as e:
            log.warning("LLM server not reachable yet (%s), retrying", e)
            time.sleep(15)

    # One-off: WAVs archived by earlier versions become OGG too (silent ones in no-speech/ stay).
    for old in sorted((INBOX / "processed").rglob("*.wav")):
        if "no-speech" in old.parts:
            continue
        try:
            archive_as_ogg(old, old.with_suffix(".ogg"))
            old.unlink()
            log.info("archived %s as OGG", old.relative_to(INBOX))
        except Exception:
            log.exception("could not convert %s, keeping it", old.name)

    move_audio_into_month_subfolders()

    state = load_state()
    seen_sizes: dict = {}
    while True:
        for path in ready_files(seen_sizes):
            attempts = state.get(path.name, 0)
            if attempts >= MAX_ATTEMPTS:
                continue
            log.info("processing %s", path.name)
            try:
                note = process(parse_recording(path))
                state.pop(path.name, None)
                log.info("  -> %s", note.relative_to(VAULT) if note else "no speech, moved to processed/no-speech")
            except Exception:  # keep the watcher alive; the file stays in the inbox
                state[path.name] = attempts + 1
                log.exception("  failed (attempt %d/%d)", attempts + 1, MAX_ATTEMPTS)
            save_state(state)
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
