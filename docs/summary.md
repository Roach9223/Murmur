# Murmur — Local Voice-to-Text for Developers

## What It Is

Murmur is a fully local Windows dictation application built for developers and AI power users. Press F1, speak naturally, and cleaned text gets typed into whatever app has focus — VS Code, a terminal, LM Studio, anything.

It runs entirely on your machine: Whisper for transcription on GPU, an optional local LLM (via LM Studio) for cleaning up speech into polished text, and Unicode keystroke injection to type the result. No cloud, no API keys, no data leaves your computer.

It's not just speech-to-text. It's a **voice interface layer** — messy speech goes in, developer-grade text comes out.

---

## Architecture

Murmur uses a **hybrid architecture** with two processes:

```
+---------------------------+          HTTP (127.0.0.1:8899)          +-------------------+
|     Python Engine         | <-------------------------------------> |   C++ ImGui UI    |
|  (audio, DSP, Whisper,    |    GET /health, /status                 |  (DX11 rendering, |
|   LLM, text injection)    |    POST /control/*, /dsp/*, /config     |   DSP controls,   |
|                           |    POST /engine/shutdown                |   spectrum, etc.)  |
+---------------------------+                                         +-------------------+
```

**Python Engine** (`app.py` + `services/`): The heavy lifting. Captures audio via WASAPI, processes it through a real-time DSP chain (noise gate + compressor), runs Whisper large-v3 on CUDA, optionally cleans output through a local LLM, detects voice commands, and injects text via SendInput. Exposes an HTTP API on port 8899 when run with `--server`.

**C++ UI** (`Murmur/Murmur.exe`): A lightweight ImGui + DirectX 11 dashboard. Polls the engine's HTTP API for status, displays real-time state (phase, audio level, DSP meters, spectrum, transcripts, latency), and provides full controls (DSP sliders, mode/profile switching, mic selection, hotkey configuration, approval workflow). Auto-launches the Python engine on startup and shuts it down on exit.

### Why Hybrid?

The Python ecosystem (faster-whisper, PyTorch/CUDA, numpy for DSP) handles the ML and audio workload with minimal code. The C++ UI provides a fast, native Windows GUI with GPU-rendered spectrum and controls. The HTTP API bridges them cleanly — either component can be used independently.

---

## How It Works — End to End

```
1. Launch Murmur.exe
   └─ Auto-spawns: engine/app.exe --server --port 8899 --base-dir <dir>
   └─ UI shows "LAUNCHING ENGINE..." then connects

2. Audio pipeline starts immediately
   └─ Mic stream opens at 48 kHz (always-on, even when not recording)
   └─ DSP chain processes every audio block: noise gate → compressor
   └─ FFT spectrum computed at 20 Hz for visualization
   └─ Live RMS metering always active

3. Press F1 (or click the recording banner)
   └─ Engine: IDLE → LISTENING
   └─ Audio chunks start queuing for transcription

4. Start speaking
   └─ RMS exceeds energy_threshold → LISTENING → RECORDING
   └─ Audio chunks accumulate in memory (post-DSP, post-resample)

5. Pause speaking (silence_timeout seconds)
   └─ Silence detected → RECORDING → TRANSCRIBING
   └─ Audio resampled: 48 kHz → 16 kHz (scipy resample_poly, factor-of-3)
   └─ faster-whisper large-v3 on CUDA transcribes

6. Voice command check (prefix-gated)
   └─ "command new line" → press Enter
   └─ "command stop dictation" → toggle off
   └─ Bare "new line" → typed as text (prefix required)
   └─ Otherwise → continue to cleanup

7. LLM cleanup (if mode != raw)
   └─ TRANSCRIBING → CLEANING
   └─ Raw text sent to LM Studio (localhost:1234)
   └─ System prompt depends on mode (clean/prompt/dev)
   └─ Fallback: if LM Studio is down, use raw text

8. Approval check (if approval_mode is on)
   └─ CLEANING → PENDING_APPROVAL
   └─ Text held for review — approve, edit, or reject via UI/API

9. Type result
   └─ → TYPING
   └─ keyboard.write() → SendInput + KEYEVENTF_UNICODE
   └─ Text appears in the focused application

10. Ready for next chunk
    └─ TYPING → LISTENING
    └─ Latency metrics recorded (record + transcribe + cleanup + type)
```

---

## File Structure

```
ai-text-to-type/
│
├── app.py                          # Main orchestrator — state machine, recording loop,
│                                   # silence detection, pipeline (transcribe → clean → type)
│
├── services/                       # 11 independent service modules
│   ├── config.py                   # ConfigManager — loads config.json, CLI args, mode/profile resolution
│   ├── audio.py                    # AudioCaptureService — WASAPI mic capture, DSP integration,
│   │                               #   48→16 kHz resampling, FFT spectrum, device management
│   ├── dsp.py                      # NoiseGate, Compressor, DSPChain — real-time audio processing
│   │                               #   with validation, calibration, vectorized gain ramps
│   ├── transcriber.py              # TranscriptionEngine — faster-whisper model lifecycle,
│   │                               #   anti-repetition params, CUDA float16
│   ├── commands.py                 # CommandRouter — maps spoken phrases to actions
│   │                               #   (enter, ctrl_enter, select_all_delete, stop)
│   ├── output.py                   # OutputInjector — keyboard.write() with KEYEVENTF_UNICODE,
│   │                               #   press_key() for special keys
│   ├── llm.py                      # LLMEnhancer — HTTP client for LM Studio's OpenAI-compatible API,
│   │                               #   runtime reconfiguration, reasoning strip, fallback to raw
│   ├── tray.py                     # TrayService — pystray system tray icon, mode/profile submenus,
│   │                               #   auto-detect toggle, approval/push-to-talk toggles
│   ├── window_detect.py            # ActiveWindowDetector — polls foreground window title via ctypes,
│   │                               #   regex matching against rules, triggers profile switch
│   ├── engine_state.py             # EnginePhase enum (8 states), LatencyMetrics dataclass,
│   │                               #   EngineState container for API exposure
│   └── server.py                   # FastAPI app factory + APIServer (uvicorn in daemon thread),
│                                   #   20+ REST endpoints for full external control
│
├── config.json                     # All tuning knobs: mic device, Whisper model, energy threshold,
│                                   # silence timeout, LLM modes, profiles, auto-detect rules,
│                                   # voice commands, DSP parameters
│
├── prompts/                        # System prompts for LLM modes
│   ├── clean_system.txt            # Clean mode: remove filler, fix grammar, preserve meaning
│   ├── prompt_system.txt           # Prompt mode: restructure for LLM input
│   └── dev_system.txt              # Dev mode: structured bullet points / task lists
│
├── Murmur/                         # Distributable app folder
│   ├── Murmur.exe                  # C++ ImGui UI — auto-launches engine, full control panel
│   ├── config.json                 # User configuration
│   ├── README.txt                  # User-facing documentation
│   ├── brotlicommon.dll            # Runtime dependency (HTTP compression)
│   ├── brotlidec.dll               # Runtime dependency (HTTP decompression)
│   ├── engine/                     # PyInstaller-bundled Python engine
│   │   ├── app.exe                 # Bundled engine executable
│   │   └── prompts/               # System prompt files
│   ├── models/                     # Whisper model cache
│   └── logs/                       # Runtime logs
│
├── dictation-ui/                   # C++ UI source project
│   ├── CMakeLists.txt              # Build configuration (C++17, vcpkg deps)
│   ├── CMakePresets.json           # Debug/Release presets
│   ├── vcpkg.json                  # Dependencies: imgui, nlohmann-json, cpp-httplib
│   └── src/
│       ├── main.cpp                # Win32 window + DX11 init + ImGui setup + render loop
│       ├── app.cpp / app.h         # DictationApp UI — all panels, DSP controls, spectrum visualizer
│       ├── engine_client.cpp/.h    # HTTP client — background polling (/health, /status),
│       │                           #   19 command methods (toggle, DSP config, calibration, etc.)
│       ├── engine_process.cpp/.h   # EngineProcess — Win32 CreateProcessW to launch engine,
│       │                           #   graceful shutdown + force kill, path discovery
│       └── dx11_helpers.cpp/.h     # DX11 device/swapchain/render target boilerplate
│
├── docs/
│   ├── gameplan.md                 # Product vision + engineering roadmap
│   ├── gameplan-ui-full-cpp.md     # Full C++ rewrite reference (not chosen)
│   └── summary.md                  # This file
│
├── logs/                           # RotatingFileHandler output
│   └── dictation.log              # 2MB max, 3 backups
│
├── models/                         # Whisper model cache (downloaded on first run)
├── build.bat                       # Full build script (PyInstaller + CMake + deploy)
├── murmur-engine.spec              # PyInstaller spec for bundling the engine
├── CLAUDE.md                       # Developer guide — architecture, decisions, conventions
├── README.md                       # GitHub landing page
├── LICENSE                         # MIT License
├── requirements.txt                # Pinned Python dependencies
├── start_dictation.bat             # One-click launcher (python app.py)
├── whisper_toggle_dictation.py     # Legacy monolith (reference only, not used)
└── venv/                           # Python virtual environment
```

---

## The 11 Services

Each service is a standalone class in `services/`. They receive configuration via constructor, don't import each other, and are independently testable.

| # | Service | File | Responsibility |
|---|---------|------|---------------|
| 1 | **ConfigManager** | `config.py` | Loads `config.json`, parses CLI args (`--server`, `--port`, `--no-cleanup`, `--base-dir`), resolves mode/profile cascades, injects defaults for missing sections |
| 2 | **AudioCaptureService** | `audio.py` | WASAPI mic capture at 48 kHz, DSP chain integration, dual ring buffers (pre/post-DSP), 48→16 kHz resampling, live RMS metering, 64-bin FFT at 20Hz, device enumeration and hot-swap, stream auto-recovery |
| 3 | **DSPChain** | `dsp.py` | NoiseGate (expander with hysteresis, hold timer, vectorized gain ramp, auto-calibration) → Compressor (feed-forward, RMS envelope, configurable ratio/makeup). Zero-alloc per block. |
| 4 | **TranscriptionEngine** | `transcriber.py` | faster-whisper model lifecycle, GPU inference with anti-repetition (`repetition_penalty=1.2`, `no_repeat_ngram_size=3`), Silero VAD |
| 5 | **CommandRouter** | `commands.py` | Prefix-gated voice commands — requires "command" prefix before phrases ("command new line", "command stop dictation"), runtime-updatable on profile switch |
| 6 | **OutputInjector** | `output.py` | `keyboard.write()` for text, `keyboard.press_and_release()` for special keys — SendInput + KEYEVENTF_UNICODE |
| 7 | **LLMEnhancer** | `llm.py` | HTTP client for LM Studio's OpenAI-compatible API, runtime-reconfigurable (prompt, temp, tokens), reasoning strip for thinking models, fallback to raw on failure |
| 8 | **TrayService** | `tray.py` | System tray icon with mode/profile submenus, auto-detect toggle, approval/push-to-talk toggles, recording state indicator |
| 9 | **ActiveWindowDetector** | `window_detect.py` | Polls foreground window title via ctypes, regex matches against rules, triggers profile switch on match |
| 10 | **EngineState** | `engine_state.py` | 8-phase enum (IDLE through ERROR + PENDING_APPROVAL), latency metrics dataclass, state container with `to_status_dict()` for API |
| 11 | **APIServer** | `server.py` | FastAPI + uvicorn, 20+ REST endpoints, CORS enabled, daemon thread, lazy import |

---

## HTTP API

Enabled with `python app.py --server`. Runs on `127.0.0.1:8899`.

### Health & Status

| Method | Endpoint | Purpose |
|--------|----------|---------|
| GET | `/health` | `{ status, version, uptime_s }` — lightweight heartbeat |
| GET | `/status` | Full state: phase, mode, profile, transcripts, RMS, FFT bins, DSP state, devices, latency |

### Recording Controls

| Method | Endpoint | Body | Purpose |
|--------|----------|------|---------|
| POST | `/control/toggle` | — | Toggle recording on/off |
| POST | `/control/start` | — | Start recording (no-op if already on) |
| POST | `/control/stop` | — | Stop recording (no-op if already off) |

### Mode & Profile

| Method | Endpoint | Body | Purpose |
|--------|----------|------|---------|
| POST | `/control/set_mode` | `{ "mode": "clean" }` | Switch LLM mode |
| POST | `/control/set_profile` | `{ "profile": "VS Code" }` | Switch profile |
| POST | `/control/command` | `{ "cmd": "newline" }` | Execute voice command |

### Approval Workflow

| Method | Endpoint | Body | Purpose |
|--------|----------|------|---------|
| POST | `/control/set_approval_mode` | `{ "enabled": true }` | Toggle approval mode |
| POST | `/control/approve` | — | Type pending text |
| POST | `/control/edit` | `{ "text": "edited" }` | Replace + type pending text |
| POST | `/control/reject` | — | Discard pending text |

### Feature Toggles

| Method | Endpoint | Body | Purpose |
|--------|----------|------|---------|
| POST | `/control/set_push_to_talk` | `{ "enabled": true }` | Toggle push-to-talk |
| POST | `/control/set_hotkey` | `{ "hotkey": "f2" }` | Change global hotkey |
| POST | `/control/set_mic` | `{ "device_index": 3 }` | Switch microphone |

### DSP

| Method | Endpoint | Body | Purpose |
|--------|----------|------|---------|
| POST | `/dsp/calibrate` | `{ "action": "start" }` | Begin 1.5s noise capture |
| POST | `/dsp/calibrate` | `{ "action": "finish" }` | Compute and apply thresholds |

### Config & Diagnostics

| Method | Endpoint | Purpose |
|--------|----------|---------|
| GET | `/config` | Full config JSON |
| POST | `/config` | Partial merge — applies DSP, modes, thresholds, spectrum source immediately |
| GET | `/logs/tail?n=200` | Last N log lines (1–5000) |
| POST | `/engine/shutdown` | Graceful shutdown (returns immediately) |

---

## LLM Modes

| Mode | LLM | What It Does |
|------|-----|-------------|
| **Raw** | OFF | Whisper output typed as-is — no processing |
| **Clean** | ON | Removes filler words ("um", "like"), fixes grammar, preserves meaning |
| **Prompt** | ON | Restructures messy speech into clear, concise LLM-ready prompts |
| **Dev** | ON | Converts speech into structured bullet points and task lists |

Each mode has its own system prompt (`prompts/*.txt`), temperature, and max_tokens. Mode is switchable at runtime via UI, tray menu, profile switch, or API.

---

## Profiles

Profiles bundle a mode with optional overrides (voice commands, hotkey, approval_mode, push_to_talk). They switch via UI, tray menu, auto-detect, or API.

| Profile | Default Mode | Notes |
|---------|-------------|-------|
| **Default** | Clean | General-purpose dictation |
| **Terminal** | Raw | No cleanup for shell commands |
| **LM Studio** | Prompt | Ctrl+Enter to send, optimized for AI chat |
| **VS Code** | Dev | Structured output for coding tasks |
| **Meeting** | Clean | Meeting notes |

**Auto-detect**: When enabled, polls the foreground window title every 500ms and regex-matches against rules in `config.json`. Matches trigger automatic profile switching (e.g., focus VS Code → Dev mode).

---

## DSP Audio Pipeline

Real-time DSP chain running in the sounddevice callback, before both the spectrum analyzer and the recording queue:

### Noise Gate

Expander-style gate with hysteresis. Attenuates background noise to a configurable floor (not full mute — preserves room tone).

- **Envelope detector**: smoothed RMS with instant attack, block-corrected release (10ms time constant)
- **Hysteresis state machine**: separate open/close thresholds prevent chattering
- **Hold timer**: keeps gate open during brief pauses between words
- **Gain ramp**: vectorized per-sample exponential ramp (`a^indices`) — no Python loops
- **Auto-calibration**: 1.5s silence measurement → 95th percentile RMS → thresholds set automatically

### Compressor

Feed-forward compressor with RMS envelope follower. Tames loud peaks for consistent level.

- **Block-level RMS** → dBFS envelope with asymmetric attack/release (block-corrected coefficients)
- **Gain computer**: `gr_db = over * (1 - 1/ratio)` above threshold
- **Makeup gain**: compensates for gain reduction
- **Disabled by default** — enable via UI or config

Both stages use pre-allocated buffers and in-place operations. Zero heap allocation in the audio callback path.

---

## Running It

### Option 1: Murmur UI (recommended)

```
Double-click Murmur/Murmur.exe
```

- Auto-launches the Python engine in the background (no console window)
- Shows real-time status: connection, phase, DSP meters, spectrum, transcripts, latency
- Full control: DSP sliders, mode/profile switching, mic selection, hotkey config, approval workflow
- Shuts down the engine cleanly on exit

### Option 2: Python engine directly

```bash
cd ai-text-to-type
venv\Scripts\activate

python app.py                              # Tray icon + F1 hotkey
python app.py --server                     # + HTTP API on :8899
python app.py --server --port 9000         # Custom port
python app.py --no-cleanup                 # Skip LLM (Raw mode)
python app.py --server --base-dir /path    # Override config/models/logs directory
```

### Option 3: Batch file

```
Double-click start_dictation.bat
```

---

## Configuration

All tuning in `config.json`. No code changes needed for normal adjustments.

### Audio
```json
{
  "mic_device_index": 63,
  "energy_threshold": 0.01,
  "silence_timeout": 1.5,
  "max_speech_seconds": 15
}
```

### Whisper
```json
{
  "whisper_model": "large-v3",
  "hotkey": "f1"
}
```

### LLM
```json
{
  "llm_cleanup": true,
  "llm_model": "lmstudio-community/Qwen2.5-7B-Instruct-GGUF",
  "llm_mode": "clean"
}
```

### Voice Commands
```json
{
  "command_prefix": "command",
  "voice_commands": {
    "new line": "enter",
    "newline": "enter",
    "send": "enter",
    "clear": "select_all_delete",
    "stop dictation": "stop",
    "stop dictating": "stop"
  }
}
```

Voice commands require the `command_prefix` to be spoken before the phrase (e.g., "command new line"). This prevents accidental triggering during normal speech. Set `command_prefix` to `""` to disable the prefix.

### DSP
```json
{
  "dsp": {
    "noise_gate": {
      "enabled": true,
      "open_threshold_dbfs": -45.0,
      "close_threshold_dbfs": -50.0,
      "floor_db": -25.0,
      "hold_ms": 100.0,
      "attack_ms": 5.0,
      "release_ms": 150.0
    },
    "compressor": {
      "enabled": false,
      "threshold_dbfs": -15.0,
      "ratio": 2.0,
      "attack_ms": 5.0,
      "release_ms": 100.0,
      "makeup_gain_db": 0.0
    }
  }
}
```

---

## Building the C++ UI

### Prerequisites
- Visual Studio 2022+ with MSVC C++ toolchain
- CMake 3.21+
- vcpkg (set `VCPKG_ROOT` environment variable)

### Build Steps

```bash
cd dictation-ui

# Configure (first time — installs vcpkg deps automatically)
cmake --preset release

# Build
cmake --build build/release --config Release

# Deploy
copy build\release\Release\Murmur.exe ..\Murmur\Murmur.exe
```

### Dependencies (auto-installed by vcpkg)
- **Dear ImGui** (docking branch) — immediate-mode GUI with DX11+Win32 backends
- **nlohmann/json** — JSON parsing for HTTP responses
- **cpp-httplib** — HTTP client for engine API communication
- **brotli** — HTTP compression (transitive dependency)

### Full Build (build.bat)

`build.bat` automates the full pipeline: PyInstaller engine build, CMake UI build, and assembly of the `Murmur/` distributable folder.

---

## Hardware Requirements

- **GPU**: NVIDIA GPU with CUDA support (tested on RTX 4090; any modern NVIDIA GPU with 4GB+ VRAM)
- **Mic**: Any Windows-compatible audio input device
- **OS**: Windows 10/11
- **LM Studio**: Running at localhost:1234 with a model loaded (only needed for clean/prompt/dev modes)
- **Python**: 3.11+ with venv (for source; Murmur.exe bundles everything)

---

## Threading Model

```
Python Engine:
  Main thread       →  keyboard hotkey listener + stop_event.wait()
  Tray thread       →  pystray Icon.run() (system tray UI)
  API thread         →  uvicorn server (only with --server)
  Transcription      →  daemon thread per recording session
  Window detect      →  daemon thread polling foreground window
  Audio callback     →  sounddevice internal thread (WASAPI)
  FFT compute        →  daemon thread (20Hz background FFT)

C++ UI:
  Main thread       →  Win32 message pump + DX11 render loop
  Poll thread        →  HTTP polling (/health + /status every 50ms)
```

---

## Engine State Machine

8 phases tracked in real-time, visible via API and UI:

```
IDLE ──F1──→ LISTENING ──speech──→ RECORDING ──pause──→ TRANSCRIBING
                 ↑                                          │
                 │                                     ┌────┴────┐
                 │                                     │         │
                 ←──────── TYPING ←── CLEANING ←───────┘    (raw mode)
                 │              │                              │
                 │              └──────────────────────────→   ↑
                 │
                 ←── TYPING ←── PENDING_APPROVAL ←── CLEANING
                                (approval mode only)
                 │
            any failure → ERROR (logged, loop continues)
```

Each stage updates `EngineState` with phase + latency metrics. The API exposes this as JSON, and the C++ UI polls it every 50ms to display live status.
