<p align="center">
  <img src="logo-murmur.png" alt="Murmur" width="200">
</p>

# Murmur

So... I broke my wrist.

Typing sucked.

So I built this.

What started as a "quick little Python script" to dictate into whatever window I had open somehow turned into a hybrid C++ / Python voice engine with Whisper, a local LLM, a DSP chain, a real-time spectrum analyzer, profiles, auto-detect, an HTTP API... you know... normal casual overengineering.

Anyway.

Now it's open source.

If you want to talk instead of type — this is for you.

---

## What Is This?

Murmur is a local voice-to-text engine that types directly into whatever window you have focused.

No cloud.
No API keys.
No sending your voice to some mystery server.
No "please subscribe to Pro to unlock punctuation."

It runs:

- **Whisper large-v3** (on your GPU)
- **Optional local LLM cleanup** (via LM Studio)
- **A real-time DSP chain** (noise gate + compressor)
- **A C++ desktop UI** (Dear ImGui + DirectX 11)

Then it types the result straight into your active window using `SendInput` — direct keystroke injection, no clipboard nonsense.

You press a key.
You talk.
It types.

That's it.

---

## How It Works (Quick Version)

Press F1. Talk. It types.

Four modes control how your speech gets processed:

| Mode | What You Get |
|------|-------------|
| **Raw** | Exactly what Whisper hears — no cleanup |
| **Clean** | Grammar fixed, filler words removed. The default. |
| **Prompt** | Your rambling restructured into clear LLM prompts |
| **Dev** | Speech converted into numbered tasks and checklists |

Profiles auto-switch modes based on your active window:

- Open **VS Code** → Dev mode
- Open **Terminal** → Raw mode (commands need exact text)
- Open **LM Studio** → Prompt mode
- Everything else → Clean mode

You can also switch manually via the UI or tray menu. Customize profiles and auto-detect rules in `config.json`.

---

## Why I Made This

I couldn't type comfortably.
I needed speech-to-text.
But normal dictation software either:

- sends everything to the cloud,
- or gives you raw messy transcription,
- or feels like it was designed in 2004.

So I built something for myself.

Originally it was just:

```python
while listening:
    transcribe()
    type_text()
```

Then I added cleanup.
Then modes.
Then profiles.
Then a UI.
Then DSP.
Then I accidentally built an engine.

Classic.

---

## What It Actually Does

### 🎙 Voice → Text (Whisper large-v3)

Runs locally on your GPU using [faster-whisper](https://github.com/SYSTRAN/faster-whisper). CUDA, float16, anti-repetition params, Silero VAD — the works.

You speak normally.
It segments on silence (configurable threshold + timeout).
It transcribes fast (under 500ms per chunk on an RTX 4090).

No internet required.

### 🧠 Cleanup Modes (Optional)

Because raw dictation sounds like this:

> "uh yeah so basically I was thinking that maybe we could refactor like the auth thing"

And that's not how you want your messages or prompts to look.

Four modes — each with its own system prompt, temperature, and token limit:

| Mode | LLM | What it does |
|------|-----|-------------|
| **Raw** | OFF | Exactly what Whisper hears. No processing. |
| **Clean** | ON | Removes filler words, fixes grammar. Default. |
| **Prompt** | ON | Turns speech into structured, LLM-ready prompts. |
| **Dev** | ON | Turns rambling into numbered tasks and checklists. |

Cleanup runs through a local LLM via [LM Studio](https://lmstudio.ai) at `localhost:1234`. If LM Studio is down, it falls back to raw text silently. No crash, no hang.

Switch modes whenever you want — UI, tray menu, or API.

### 🎚 Real-Time DSP (Because Noise Is Annoying)

There's a noise gate.
There's an optional compressor.
There's a spectrum analyzer.

Did this need to exist?

Probably not.

Did I build it anyway?

Yes.

**The noise gate:**
- Uses hysteresis (separate open/close thresholds — no chattering)
- Has attack / release / hold time constants
- Auto-calibrates to your room (stay silent 1.5s, it figures out the thresholds)
- Doesn't hard-mute — it attenuates smoothly to a configurable floor (preserves room tone)
- Vectorized gain ramp — no Python loops in the audio path

**The compressor:**
- Feed-forward, RMS-based envelope
- Gentle defaults that don't smash your voice into oblivion
- Configurable ratio, threshold, makeup gain
- Disabled by default

**The spectrum analyzer:**
- 64-bin log-spaced FFT (50Hz–12kHz)
- EMA smoothing, peak hold, noise floor tracking
- Phase-based coloring that changes with engine state
- Toggle pre/post-DSP to see what the gate is doing

Because why not.

### 🖥 C++ Desktop UI

I didn't want a clunky Python GUI.
So I built a Dear ImGui + DirectX 11 desktop app.

The Python engine runs separately.
They talk over HTTP on localhost.

Why?

Because Python is great for ML.
C++ is great for UI.
And I didn't want to fight tkinter.

The UI gives you:
- Clickable recording banner (or use the hotkey)
- DSP sliders with live spectrum
- Mode/profile switching
- Mic selection + hotkey configuration
- Approval mode (review text before it's typed)
- Latency breakdown — Record, Transcribe, Cleanup, Type, plus Generation (processing-only) and Total
- Push-to-talk toggle

### 🎯 Profiles + Auto-Detect

Switch profiles automatically based on the active window.

Open VS Code? It switches to Dev mode.
Open Terminal? Raw mode.
Open LM Studio? Prompt mode.

It just adapts. Five profiles out of the box:

| Profile | Mode | Notes |
|---------|------|-------|
| **Default** | Clean | General use |
| **Terminal** | Raw | No LLM — shell commands need exact text |
| **LM Studio** | Prompt | "Send" triggers Ctrl+Enter |
| **VS Code** | Dev | Structured task output |
| **Meeting** | Clean | Note-taking during calls |

Auto-detect polls the foreground window title every 500ms with regex rules. Customize in `config.json`.

### 🗣 Voice Commands

Say "command" followed by a phrase:

| You say | What happens |
|---------|-------------|
| "command new line" | Presses Enter |
| "command send" | Presses Enter (Ctrl+Enter in LM Studio profile) |
| "command clear" | Select All + Delete |
| "command stop dictation" | Stops recording |

Without the prefix, phrases are typed as regular text. Saying "new line" by itself just types "new line." The prefix is configurable in `config.json` — set `command_prefix` to `""` to disable it.

### ✅ Approval Mode

When enabled, text is held for review instead of being typed immediately. You can:
- **Approve** — type it as-is
- **Edit** — modify in a text box, then send
- **Reject** — discard and keep listening

### 🎤 Push-to-Talk

Hold the hotkey to record, release to stop. Alternative to the default toggle mode. Useful in noisy environments or for short commands.

---

## What This Is NOT

- Not a SaaS.
- Not monetized.
- Not harvesting your voice.
- Not polished corporate software.
- Not guaranteed to never break.

It's a tool I built for myself that turned into something useful.

---

## Who This Is For

- People with injuries who don't want to type.
- Developers who talk faster than they type.
- People who like local-first software.
- People who like overbuilt personal tools.
- People who don't trust cloud dictation.

---

## The Stack (Because You'll Ask)

- **Python 3.11+** — engine, audio pipeline, Whisper, LLM client
- **faster-whisper** — Whisper large-v3 on CUDA, float16
- **LM Studio** — local LLM for cleanup (optional)
- **FastAPI + uvicorn** — HTTP API between engine and UI
- **sounddevice / PortAudio** — WASAPI audio capture at 48kHz
- **numpy / scipy** — DSP processing, resampling (48kHz → 16kHz)
- **Dear ImGui + DirectX 11** — C++ desktop UI
- **cpp-httplib** — HTTP client in the UI
- **keyboard** — SendInput-based keystroke injection (KEYEVENTF_UNICODE)
- **pystray** — system tray icon (when running from source)

Yes, it's a hybrid.
Yes, it's slightly ridiculous.
Yes, it works.

---

## Requirements

### Hardware
- **GPU**: NVIDIA with CUDA support (tested on RTX 4090 — any modern NVIDIA GPU with 4GB+ VRAM should work)
- **Mic**: Any Windows-compatible audio input
- **OS**: Windows 10/11

### Software
- **LM Studio** at localhost:1234 with a model loaded (for Clean/Prompt/Dev modes — Raw mode works without it)
- **Python 3.11+** with venv (for running from source — `Murmur.exe` bundles everything)

---

## Running It

### Option 1: Murmur.exe (Recommended)

Download the `Murmur/` folder. Run `Murmur.exe`.

It auto-launches the engine in the background. Press F1 (or click the banner). Talk. Pause. It types.

### Option 2: From Source

```bash
git clone https://github.com/YOUR_USERNAME/murmur.git
cd murmur

python -m venv venv
venv\Scripts\activate
pip install -r requirements.txt

# With HTTP API (required for Murmur.exe UI):
python app.py --server

# Headless with just tray icon + hotkey:
python app.py

# Skip LLM cleanup:
python app.py --no-cleanup
```

### CLI Flags

| Flag | Default | What it does |
|------|---------|-------------|
| `--server` | off | Start HTTP API on 127.0.0.1:8899 |
| `--port N` | 8899 | Custom API port |
| `--no-cleanup` | off | Force Raw mode (no LLM) |
| `--base-dir PATH` | script dir | Override base directory for config/prompts/models/logs |

---

## Configuration

Everything is in `config.json`. If it's missing, defaults are used. Old configs without newer sections (DSP, auto-detect, profiles) are backfilled automatically.

See the [config.json](config.json) in this repo for the full example with all options.

Key settings:

```json
{
  "whisper_model": "large-v3",
  "mic_device_index": 63,
  "hotkey": "f1",
  "energy_threshold": 0.01,
  "silence_timeout": 1.5,
  "llm_mode": "clean",
  "command_prefix": "command",
  "voice_commands": { ... },
  "llm_modes": { ... },
  "profiles": { ... },
  "auto_detect": { ... },
  "dsp": { ... }
}
```

DSP slider changes save automatically. Other config changes take effect on engine restart.

---

## Building from Source

### Python Engine (PyInstaller)

```bash
venv\Scripts\activate
pyinstaller murmur-engine.spec --noconfirm --distpath F:\tmp\murmur_dist --workpath F:\tmp\murmur_build
```

### C++ UI (CMake + vcpkg)

Requires Visual Studio 2022+, CMake 3.21+, vcpkg with `VCPKG_ROOT` set.

```bash
cd dictation-ui
cmake --preset release
cmake --build build/release --config Release
```

### Full Build

`build.bat` automates everything: PyInstaller engine, CMake UI, assembly of the `Murmur/` folder.

---

## HTTP API

The engine exposes a REST API on `127.0.0.1:8899` (when run with `--server`). 20+ endpoints for full external control — recording, modes, profiles, DSP calibration, config, approval workflow, diagnostics. CORS enabled.

See [CLAUDE.md](CLAUDE.md) for the complete endpoint reference.

---

## Architecture

```
Python Engine (audio, DSP, Whisper, LLM, text injection)
        ↕  HTTP on 127.0.0.1:8899
C++ UI (ImGui + DX11 — controls, spectrum, status)
```

11 independent Python services in `services/`. Each handles one thing. They don't import each other. The orchestrator (`app.py`) wires them together.

8-phase state machine: IDLE → LISTENING → RECORDING → TRANSCRIBING → CLEANING → TYPING (or PENDING_APPROVAL) → back to LISTENING. Any failure → ERROR (logged, loop continues).

See [CLAUDE.md](CLAUDE.md) for the deep technical docs, and [docs/gameplan.md](docs/gameplan.md) for the engineering roadmap.

---

## Why It's Open Source

Because:

- I built it for me.
- It might help someone else.
- Local-first tools should exist.
- If I ever stop maintaining it, someone else can keep it alive.

If you improve it — awesome.
If you fork it — awesome.
If you rip parts out for your own project — awesome.

---

## A Quick Note on Overengineering

Yes, this started as:

> "I just need speech-to-text."

It ended up with:

- Engine phases
- Latency metrics
- DSP chain
- HTTP API
- Profiles
- Auto-detect
- C++ UI
- Tray integration
- Spectrum analyzer
- And way too much config

This is what happens when a dev says "I'll just add one more feature."

---

## If You're Reading This

Hi.

Thanks for checking it out.

If it helps you — that's the win.

If it inspires you to build your own weird hyper-personal tool — even better.

— Josh

---

## License

MIT License. See [LICENSE](LICENSE) for details.
