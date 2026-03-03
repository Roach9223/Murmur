# Murmur — Developer-Grade Voice Interface

## What This Is

A fully local Windows dictation app for developers and AI workflows. Press F1, speak naturally, and clean text gets typed into any app. Runs on GPU (RTX 4090), uses Whisper for transcription, and optionally cleans output through a local LLM (LM Studio).

Not just speech-to-text. It's a **voice interface layer** — speech goes in, developer-grade prompts come out.

See `docs/gameplan.md` for the full product vision and phased roadmap.

## Current State

**All core features implemented and deployed:**

- Toggle dictation with F1 (suppressed from other apps) or click the UI banner
- Silence-based chunking (energy threshold + pause detection)
- Voice commands with prefix system ("command new line", "command send", etc.)
- **4 LLM modes**: Raw (no LLM), Clean (filler removal), Prompt (optimize for AI input), Dev (structured tasks)
- **Profile system**: Default, Terminal, LM Studio, VS Code, Meeting — each with its own mode + commands
- **Auto-detect active window**: polls foreground window title, auto-switches profiles via regex rules
- **DSP audio pipeline**: Noise gate (expander-style with hysteresis) + compressor, auto-calibration, all adjustable at runtime
- **Real-time spectrum analyzer**: 64-bin log-spaced FFT with phase-based coloring, pre/post-DSP toggle
- **Approval mode**: Review text before typing — approve, edit, or reject
- **Push-to-talk**: Hold hotkey to record, release to stop
- System tray icon with Mode/Profile submenus, auto-detect toggle, approval/push-to-talk toggles
- Unicode text injection via `keyboard.write()` (SendInput + KEYEVENTF_UNICODE)
- Whisper large-v3 on CUDA with anti-repetition settings
- **11 independent services** — audio, dsp, transcriber, commands, output, llm, config, tray, window_detect, engine_state, server
- **Config-driven** — all tuning knobs in config.json, system prompts in prompts/
- **Reliability**: stdlib logging (RotatingFileHandler), error handling with graceful fallback, audio stream auto-recovery, thread safety (`_state_lock`), idempotent shutdown with Windows console handler
- **HTTP API** (opt-in via `--server`): FastAPI + uvicorn on `127.0.0.1:8899`, 20+ endpoints for full external control
- **Engine state machine**: 8 phases (idle, listening, recording, transcribing, cleaning, typing, pending_approval, error) with latency tracking
- **C++ ImGui + DirectX 11 desktop UI** (Murmur.exe): Full control panel with DSP sliders, spectrum visualizer, approval workflow, latency display
- **Packaged as Murmur/**: Pre-built distributable folder with Murmur.exe + bundled Python engine

## Hardware Context

- **GPU**: NVIDIA RTX 4090 — float16 compute, handles large-v3 easily
- **Mic**: SSL 2 MKII USB audio interface via WASAPI (device index 63)
- **WASAPI quirk**: Device only supports 48 kHz natively. We record at 48 kHz and resample to 16 kHz for Whisper using `scipy.signal.resample_poly` (clean factor-of-3 decimation).

## Project Layout

```
ai-text-to-type/
  app.py                        # entry point (orchestrator + state transitions + latency timing)
  services/
    __init__.py
    config.py                   # ConfigManager — config.json, CLI args, mode/profile resolution
    audio.py                    # AudioCaptureService — mic stream, DSP integration, resampling, FFT, device mgmt
    dsp.py                      # NoiseGate, Compressor, DSPChain — real-time audio processing
    transcriber.py              # TranscriptionEngine — Whisper model lifecycle
    commands.py                 # CommandRouter — prefix-gated voice command detection + execution
    output.py                   # OutputInjector — keyboard.write, press_key
    llm.py                      # LLMEnhancer — LM Studio cleanup API + runtime reconfiguration
    tray.py                     # TrayService — system tray icon with Mode/Profile submenus
    window_detect.py            # ActiveWindowDetector — foreground window polling + profile matching
    engine_state.py             # EnginePhase enum, LatencyMetrics, EngineState dataclass
    server.py                   # FastAPI app (create_app factory) + APIServer (uvicorn daemon thread)
  config.json                   # tuning knobs + profiles + modes + DSP + auto-detect rules
  prompts/
    clean_system.txt            # Clean mode: filler removal, grammar fix
    prompt_system.txt           # Prompt mode: optimize for LLM input
    dev_system.txt              # Dev mode: structured tasks/bullet lists
  logs/                         # RotatingFileHandler output (dictation.log, 2MB, 3 backups)
  models/                       # Whisper model cache (downloaded on first run)
  Murmur/                       # distributable app folder
    Murmur.exe                  # C++ ImGui UI — auto-launches engine on startup
    config.json                 # user configuration
    README.txt                  # user-facing documentation
    brotlicommon.dll            # runtime dependency (Brotli)
    brotlidec.dll               # runtime dependency (Brotli)
    engine/                     # PyInstaller-bundled Python engine
      app.exe                   # bundled engine executable
      prompts/                  # system prompt files
    models/                     # Whisper model cache
    logs/                       # runtime logs
  dictation-ui/                 # C++ UI source (CMake + vcpkg)
    CMakeLists.txt
    CMakePresets.json
    vcpkg.json
    src/                        # main.cpp, app.cpp/h, engine_client, engine_process, dx11_helpers
  docs/
    gameplan.md                 # consolidated product & engineering plan
    gameplan-ui-full-cpp.md     # full C++ rewrite plan (reference, not chosen)
    summary.md                  # detailed project summary
  build.bat                     # full build script (PyInstaller + CMake + deploy)
  murmur-engine.spec            # PyInstaller spec for bundling the engine
  CLAUDE.md                     # this file
  README.md                     # GitHub landing page
  LICENSE                       # MIT License
  requirements.txt              # pinned dependencies
  start_dictation.bat           # one-click launcher (runs app.py)
  whisper_toggle_dictation.py   # legacy monolith (reference only)
  venv/                         # Python virtual environment
```

## LLM Modes

| Mode | LLM | Behavior |
|------|-----|----------|
| Raw | OFF | Whisper text typed as-is, no LLM processing |
| Clean | ON | Remove filler words, fix grammar, preserve meaning |
| Prompt | ON | Restructure speech into clear LLM-ready prompts |
| Dev | ON | Convert speech into bullet points / task lists |

Each mode has its own system prompt in `prompts/`, temperature, and max_tokens. Mode is switchable at runtime via UI, tray menu, or API.

## Profiles

Each profile defines an LLM mode and optional overrides (voice commands, hotkey, approval_mode, push_to_talk). Profiles switch via UI, tray menu, auto-detect, or API.

Config resolution cascade: `DEFAULTS` ← `config.json flat keys` ← `llm_modes[mode]` ← `profiles[name]`

## Auto-Detect

When enabled, polls the foreground window title every 500ms and matches against regex rules in `config.json`. When a rule matches, the corresponding profile is activated. Uses `ctypes.windll.user32.GetForegroundWindow()` — no extra dependencies.

## DSP Audio Pipeline

Real-time DSP chain running in the sounddevice callback (before both spectrum and recording queue):

**Noise Gate** — Expander-style with hysteresis, smoothed envelope detector (instant attack, configurable release), hold timer, vectorized per-sample gain ramp. Attenuates to configurable floor (not full mute). Auto-calibration measures room noise for 1.5s and sets thresholds.

**Compressor** — Feed-forward, block RMS envelope with asymmetric attack/release, gain computer with configurable ratio, vectorized gain ramp, makeup gain. Disabled by default.

Both use block-corrected one-pole coefficients (`a^N` where N = block size) because envelope followers run once per block, not per sample. Gain ramps are per-sample via pre-allocated `a^indices` arrays.

Parameters are validated with hard bounds. All DSP changes persist to config.json automatically.

## FFT Spectrum Analyzer

64-bin log-spaced FFT (50Hz–12kHz) with Hann window, energy compensation, dBFS scaling. Runs at 20Hz in a background thread. Pre/post-DSP ring buffers with toggle. The C++ UI adds EMA smoothing, peak hold, noise floor tracking, phase-based coloring, and articulation band highlighting.

## HTTP API (Engine Daemon Mode)

Enabled with `--server`. Runs FastAPI + uvicorn in a daemon thread on `127.0.0.1:8899`.

### Health & Status

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/health` | `{ status, version, uptime_s }` |
| GET | `/status` | Full state + metrics + audio RMS + FFT bins + DSP state + devices |

### Recording Controls

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/control/toggle` | Toggle recording |
| POST | `/control/start` | Start recording (no-op if already on) |
| POST | `/control/stop` | Stop recording (no-op if already off) |

### Mode & Profile

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/control/set_mode` | `{ "mode": "clean" }` |
| POST | `/control/set_profile` | `{ "profile": "VS Code" }` |
| POST | `/control/command` | `{ "cmd": "newline|send|clear|stop" }` |

### Approval Workflow

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/control/set_approval_mode` | `{ "enabled": true }` |
| POST | `/control/approve` | Type pending text |
| POST | `/control/edit` | `{ "text": "edited" }` — replace + type |
| POST | `/control/reject` | Discard pending text |

### Feature Toggles

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/control/set_push_to_talk` | `{ "enabled": true }` |
| POST | `/control/set_hotkey` | `{ "hotkey": "f2" }` |
| POST | `/control/set_mic` | `{ "device_index": 3 }` |

### DSP

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/dsp/calibrate` | `{ "action": "start" }` or `{ "action": "finish" }` |

### Config & Diagnostics

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/config` | Full config JSON |
| POST | `/config` | Partial config update — merges and applies DSP, modes, thresholds, spectrum source |
| GET | `/logs/tail?n=200` | Last N log lines (1–5000) |
| POST | `/engine/shutdown` | Graceful shutdown |

CORS enabled for browser/tool debugging. Lazy import: FastAPI/uvicorn only loaded when `--server` is passed.

## Reliability

- **Logging**: stdlib `logging` with `RotatingFileHandler` (2MB, 3 backups) to `logs/dictation.log` + console handler. No `print()` anywhere.
- **Error handling**: try/except around `transcriber.transcribe()` and `output.type_text()` — logs error, sets `EnginePhase.ERROR`, continues recording loop.
- **Audio recovery**: sounddevice callback tracks consecutive errors (`_MAX_CONSECUTIVE_ERRORS = 50`). When threshold hit, `needs_restart` flag triggers `restart_stream()` from the transcription loop.
- **Thread safety**: `threading.Lock` (`_state_lock`) wraps multi-attribute mutations in `_apply_mode()`, `switch_profile()`, `toggle_recording()`.
- **Shutdown**: idempotent `_quit()` with `_shutdown_called` flag. Windows `SetConsoleCtrlHandler` via ctypes catches console close, logoff, shutdown events. Transcription thread joined with 2s timeout.
- **LLM fallback**: if LM Studio is down or returns garbage (response > 2x input length), raw Whisper text is used silently.
- **DSP validation**: all parameters have hard bounds. Invalid values are rejected before application.

## Engine State Machine

8 phases tracked in `engine_state.py`, updated at every pipeline stage:

`IDLE` → `LISTENING` → `RECORDING` → `TRANSCRIBING` → `CLEANING` → `TYPING` → back to `LISTENING`

With approval mode: `CLEANING` → `PENDING_APPROVAL` → (approve) → `TYPING` → `LISTENING`

Any failure → `ERROR` (with `last_error` string). Latency breakdown stored in `LatencyMetrics` (record_ms, transcribe_ms, cleanup_ms, type_ms) using `time.perf_counter()`.

## Architecture: Code vs Config

### Hard defaults in code (should never break)
- LM Studio base URL: `http://localhost:1234/v1/chat/completions`
- Timeout: 10 seconds
- Fallback: if LM Studio is down, use raw Whisper text
- Response extraction: `choices[0].message.content`
- Resampling: 48 kHz -> 16 kHz (factor of 3)
- Audio format: float32, mono
- Debounce: 500ms on hotkey

### Tuning knobs in config.json (tweak fast)
- LLM mode, model identifier, temperature, max_tokens
- Profiles with per-profile mode, voice commands, hotkey, approval_mode, push_to_talk
- Auto-detect rules (window title regex → profile name)
- Hotkey, mic device index, Whisper model size
- Energy threshold, silence timeout, max speech duration
- Voice command phrases, their actions, and command prefix
- DSP noise gate and compressor parameters

Config is loaded at startup. If `config.json` is missing, built-in defaults are used. Old flat configs (no `llm_modes`/`profiles`/`auto_detect`/`dsp` sections) still work — defaults are injected automatically. The `--no-cleanup` CLI flag forces Raw mode.

## Key Technical Decisions

| Decision | Why |
|----------|-----|
| `keyboard` library (not pynput) | pynput doubled characters and phrases on this system. `keyboard.write()` uses SendInput with KEYEVENTF_UNICODE — direct character injection, no clipboard, no Ctrl+V |
| `keyboard.add_hotkey(suppress=True)` | Prevents F1 from opening help dialogs in other apps |
| Silence-based chunking (not fixed timer) | Fixed 3-second timer cut words at boundaries. Energy-threshold state machine triggers transcription on natural pauses |
| `resample_poly` (not changing sample rate) | WASAPI device 63 only supports 48 kHz. Recording at native rate and resampling is the cleanest path |
| `requests` (not openai SDK) | Simpler, fewer dependencies. LM Studio exposes a standard OpenAI-compatible endpoint |
| Anti-repetition params | `repetition_penalty=1.2`, `no_repeat_ngram_size=3` reduce Whisper hallucination of repeated words in short chunks |
| `pystray` for tray icon (not tkinter/PyQt) | Lightweight, purpose-built for system tray. Runs in daemon thread. No heavy GUI framework needed |
| `ctypes` for window detection (not pywin32/psutil) | Zero new dependencies. `GetForegroundWindow` + `GetWindowTextW` is all we need |
| LLMEnhancer.configure() (not new instances) | LLM is a thin HTTP client — swapping prompt/temperature is simpler than recreating. GIL makes attribute assignment safe |
| `stdlib logging` (not print) | RotatingFileHandler for file output + console handler for terminal. Structured log rotation, consistent format, no print() anywhere |
| `FastAPI + uvicorn` (not Flask) | Async-ready, Pydantic request validation, runs in daemon thread. Lazy import means zero overhead without `--server` |
| `EnginePhase` state machine | Granular pipeline visibility (8 states) for external UI polling. Each pipeline stage updates phase + metrics |
| `create_app(engine)` factory pattern | Route handlers close over the DictationApp reference. Avoids globals, testable in isolation |
| Hybrid Python+C++ architecture | Python for ML ecosystem (faster-whisper, PyTorch, CUDA), C++ ImGui for fast GPU-rendered UI. HTTP API decouples them |
| Vectorized DSP (numpy in-place) | Pre-allocated buffers, `np.multiply(..., out=)`, `a^indices` gain ramp — zero heap allocation per audio callback |
| Block-corrected envelopes | One-pole coefficients raised to block-size power (`a^N`) because envelopes update once per block, not per sample |
| Expander gate (not hard gate) | Configurable floor instead of full mute preserves room tone and sounds natural |

## Threading Model

```
Main thread:     keyboard hotkey + _stop_event.wait()
Tray thread:     pystray Icon.run()
API thread:      uvicorn server (only with --server)
Transcription:   daemon thread per recording session
Window detect:   daemon thread (polling foreground window)
Audio callback:  sounddevice internal thread
FFT compute:     daemon thread (20Hz background FFT)
C++ poll:        EngineClient::PollLoop() at 50ms (in UI process)
```

## Known Limitations

1. **Chunk boundary on long speech**: If someone talks for 15+ seconds without pausing, the MAX_SPEECH_SEC cap forces a split that can cut mid-word.
2. **LLM cleanup adds latency**: Each chunk round-trips through LM Studio. With a fast local model this is <1s, but slower models will create a noticeable delay.
3. **Windows only**: Uses Windows-specific APIs for text injection, hotkey suppression, and window detection.
4. **English only**: Whisper language is hard-coded to `"en"`.

## How to Run

```bash
# Standard mode (tray icon + hotkey):
python app.py

# With HTTP API server for external control:
python app.py --server
python app.py --server --port 9000

# Skip LLM cleanup (for debugging or when LM Studio isn't running):
python app.py --no-cleanup

# Override base directory (for bundled deployment):
python app.py --server --base-dir /path/to/config

# Or double-click:
start_dictation.bat

# Or run Murmur.exe (auto-launches engine):
Murmur/Murmur.exe
```

**Requirements:**
- LM Studio running at localhost:1234 with a model loaded (for cleanup modes; Raw mode works without it)
- Python 3.11+ with venv activated (for source; Murmur.exe bundles everything)
- NVIDIA GPU with CUDA

## Development Conventions

- **Modular architecture** — 11 services in `services/`, orchestrator in `app.py`. Services receive config via constructor, don't import each other. Each service is independently testable.
- **Config over code** — any value the user might want to change goes in config.json
- **Fail gracefully** — LLM down? Use raw text. Whisper error? Log and continue. Never crash the recording loop.
- **Logging is the dashboard** — all output via stdlib `logging`. Tags: `[REC]`, `[STOP]`, `[cmd]`, `[mode]`, `[profile]`, `[auto-detect]`, `[hotkey]`, `[mic]`, `raw:`, `cleaned:`, `>> typed`. No `print()` calls.
- **Thread safety** — use `_state_lock` for any mutation touching multiple attributes. All control surfaces (hotkey, tray, HTTP API, UI) call the same lock-protected methods.
- **No network calls unless explicitly enabled** — fully local by default. LLM cleanup is opt-in via config. HTTP API is opt-in via `--server`.
- **keyboard library only for typing** — never use pynput, pyperclip, or clipboard-based paste. The `keyboard.write()` + KEYEVENTF_UNICODE path is the only one that doesn't double on this system.
- **Test after every change** — F1 on, speak, F1 off. Check console output matches typed output 1:1.
- **DSP is zero-alloc** — pre-allocated buffers, in-place operations, vectorized gain ramps. No heap allocation in the audio callback path.
