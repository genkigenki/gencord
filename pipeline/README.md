# gencord pipeline

Inbox → transcript → speakers → summary → Obsidian note. Runs locally on a PC with an NVIDIA GPU;
tested on an RTX 2080 SUPER (8 GB) with LM Studio on the same machine.

```
recorder ──WebDAV──▶ <NAS>/Recordings/inbox/*.wav          (polled every 60 s)
                            │
              gencord-pipeline (Docker, GPU)
                ffmpeg: 80 Hz high-pass + loudness normalisation
                faster-whisper large-v3-turbo   → transcript (word list as hints, repetition guard)
                pyannote 3.1 (optional)         → speakers "Sprecher 1/2/…", named when addressed by name
                LLM (LM Studio / Ollama)        → type, title, summary, decisions, tasks, questions, tags
                            │
              <vault>/Recordings/2026/2026-10-17 20-55 <title>.md
              <NAS>/Recordings/inbox/processed/2026-10/<recording>.transcript.txt
              <NAS>/Recordings/inbox/processed/2026-10/audio/<recording>.ogg
```

- **Inbox formats:** `.wav` from the recorder. `.ogg`, `.mp3` and `.m4a` dropped in by hand
  (re-processing, phone memos) are processed too.
- **Archive:** after a verified conversion to OGG (~14 MB/h), the WAV is deleted. A file that
  fails stays in the inbox and is retried up to 3 times.
- **Silence:** a recording with no speech gets no note; it is moved to `processed/no-speech/`.

## Note

Each note has frontmatter (`created`, `duration`, `type`, `tags`, `people` as `[[links]]`). The
sections depend on the recording type:

| Type | Sections |
|---|---|
| besprechung (meeting) | Zusammenfassung, Entscheidungen, Aufgaben, Offene Fragen |
| telefonat (call) | Zusammenfassung, Vereinbart, Aufgaben, Offene Fragen |
| gespraech (conversation) | Zusammenfassung, Ideen, Aufgaben, Offene Fragen |
| notiz (memo) | Kernpunkte, Ideen, Aufgaben |
| vortrag (talk) | Zusammenfassung, Wichtige Gedanken, Offene Fragen, Aufgaben |

The LLM picks the type. A word in the recording's name forces it: `meeting`, `call`, `idee`/`notiz`, `vortrag`.

After the sections comes the transcript, in paragraphs with speaker names. With `RECORDINGS_UNC`
set, each timestamp is a `gencord://` link that opens the recording in VLC at that second. Run
`tools/install-gencord-links.ps1` once on the Windows PC to register these links (current user
only; `-Uninstall` removes them).

**Word list:** `Recordings/_Wortliste.md` is created in the vault on the first start. Names under
"Personen" and terms under "Begriffe" are passed to Whisper as hints and to the LLM, so they are
spelled right. People become `[[links]]`.

## Deploy

1. Copy `.env.example` to `.env` and fill in the NAS address, the SMB login, both shares
   and, optionally, `HF_TOKEN`.
2. Start the stack:
   ```
   docker compose -p gencord up -d --build
   docker logs -f gencord-pipeline
   ```
   It then also appears in Portainer.
3. **Or in Portainer:** Stacks → Add stack → Repository, compose path `pipeline/docker-compose.yml`.
   Enter the `.env` values as environment variables.

**Speaker separation:** create a free Hugging Face account and accept the terms of
[pyannote/speaker-diarization-3.1](https://huggingface.co/pyannote/speaker-diarization-3.1) and
[pyannote/segmentation-3.0](https://huggingface.co/pyannote/segmentation-3.0). Then create a read
token and set it as `HF_TOKEN`.

**LLM:** by default LM Studio on the Docker host (`http://host.docker.internal:1234/v1`, model
`qwen3.5-9b`). Without LM Studio, use `LLM_API=ollama`, `LLM_MODEL=qwen2.5:7b` and
`docker compose --profile ollama up -d`.

## Speed (RTX 2080 SUPER)

A 27-minute conversation takes about 9 minutes in total: Whisper about 1 min, speakers about 1 min,
the rest is the summary. A 1-minute memo takes well under a minute plus the summary.
