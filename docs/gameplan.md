# Murmur — Product & Engineering Plan

## Vision

Build a **best-in-class, fully local Windows dictation application** designed for developers and AI-heavy workflows.

- Run entirely offline (no cloud, no network unless explicitly enabled)
- Use local Whisper models (GPU-accelerated, RTX 4090)
- Type reliably into any application via SendInput Unicode
- Support voice commands (new line, send, clear, stop dictation)
- Optionally enhance output via local LLMs (LM Studio)
- Real-time DSP audio processing (noise gate, compression)
- Feel minimal, premium, and fast — "Afterburner/OBS" tool quality

Not just a dictation tool. A **developer-grade voice interface layer for AI workflows**.

---

## Architecture Decision

**Hybrid approach chosen**: Python engine + C++ ImGui thin-client UI communicating via HTTP.

- Python engine handles: audio capture, DSP processing, Whisper inference, LLM cleanup, voice commands, text injection, profiles, auto-detect
- C++ UI handles: visual controls, DSP sliders, spectrum visualizer, live metrics, transcript preview, approval workflow, engine lifecycle management
- HTTP API on `127.0.0.1:8899` is the contract between them
- This preserves ~1000 lines of battle-tested Python while enabling a native "pro tool" UI
- The HTTP API contract also enables a future full C++ engine swap with zero UI changes

Full C++ rewrite was considered and rejected — see `gameplan-ui-full-cpp.md` for reference.

---

## Completed Work

### Engine Architecture (11 services) — DONE

Modular architecture in `services/`, orchestrator in `app.py`:

| Service | File | Purpose |
|---------|------|---------|
| Config | `config.py` | ConfigManager, CLI args, mode/profile resolution, config cascade |
| Audio | `audio.py` | WASAPI capture (48kHz), DSP integration, resample to 16kHz, FFT spectrum, device management, auto-recovery |
| DSP | `dsp.py` | NoiseGate (expander with hysteresis, calibration), Compressor (feed-forward, RMS envelope), DSPChain |
| Transcriber | `transcriber.py` | faster-whisper large-v3 on CUDA, float16, anti-repetition, Silero VAD |
| Commands | `commands.py` | Voice command detection + action execution |
| Output | `output.py` | `keyboard.write()` with KEYEVENTF_UNICODE, press_key helpers |
| LLM | `llm.py` | LM Studio HTTP cleanup with runtime reconfiguration, reasoning strip, fallback |
| Tray | `tray.py` | pystray system tray icon, Mode/Profile submenus, auto-detect/approval/push-to-talk toggles |
| Window Detect | `window_detect.py` | Foreground window polling via ctypes, regex profile matching |
| Engine State | `engine_state.py` | EnginePhase enum (8 states including PENDING_APPROVAL), LatencyMetrics, EngineState dataclass |
| Server | `server.py` | FastAPI app (create_app factory), APIServer (uvicorn daemon thread), 20+ endpoints |

### Silence-Based Chunking — DONE

- Continuous audio buffer with RMS energy detection
- Configurable energy threshold, silence timeout, max speech duration
- Flushes on natural pauses — no split words, better sentence grouping
- Handles long speech via max_speech_seconds safety cap

### Voice Commands — DONE

- Prefix-gated: requires "command" prefix before any command phrase (configurable via `command_prefix`)
- "command new line" → inject Enter
- "command send" → inject Enter (or Ctrl+Enter in LM Studio profile)
- "command clear" → select all + delete
- "command stop dictation" → stop recording
- Bare phrases without prefix are typed as regular text
- Custom phrases configurable in config.json per profile

### LLM Enhancement (4 modes) — DONE

Pipeline: Whisper → Command Router → (optional) LM Studio cleanup → Output Injector

| Mode | LLM | Behavior |
|------|-----|----------|
| Raw | OFF | Whisper text typed as-is |
| Clean | ON | Remove filler words, fix grammar |
| Prompt | ON | Restructure for LLM-ready prompts |
| Dev | ON | Convert to bullet points / task lists |

Each mode has its own system prompt in `prompts/`, temperature, and max_tokens. Per-profile enablement. Fallback to raw text if LM Studio unreachable. Reasoning strip for thinking models (Qwen).

### Profiles + Auto-Detection — DONE

Profiles: Default, Terminal, LM Studio, VS Code, Meeting. Each defines LLM mode, voice commands, hotkey, approval_mode, push_to_talk overrides.

Auto-detect: polls foreground window title every 500ms via `ctypes.windll.user32.GetForegroundWindow()`, matches regex rules in config.json, auto-switches profile.

Config cascade: `DEFAULTS` ← `config.json flat keys` ← `llm_modes[mode]` ← `profiles[name]`

### Approval Mode — DONE

- Transcribed + cleaned text held in UI before typing
- User can: Approve (type it), Edit (modify in text box then type), Reject (discard)
- API endpoints: `POST /control/approve`, `/control/edit`, `/control/reject`, `/control/set_approval_mode`
- Engine holds text in `pending_text` at `PENDING_APPROVAL` phase until action taken
- Toggle between auto-type and approval mode via UI checkbox, tray, or API

### Push-to-Talk — DONE

- Hold hotkey to record, release to flush
- Alternative to toggle mode
- Toggle via UI button, tray menu, or API (`POST /control/set_push_to_talk`)
- Hotkey re-registration from `add_hotkey` to `on_press_key`/`on_release_key` on toggle

### DSP Audio Pipeline — DONE

Real-time DSP chain running in the sounddevice callback:

- **Noise Gate**: Expander-style with hysteresis, smoothed envelope detector, hold timer, vectorized gain ramp, auto-calibration
- **Compressor**: Feed-forward, block RMS envelope, configurable ratio/makeup gain
- Both use block-corrected one-pole coefficients, pre-allocated buffers, zero-alloc per callback
- Parameters validated with hard bounds, persisted to config.json automatically
- `/dsp/calibrate` endpoint for auto-threshold setting

### FFT Spectrum Analyzer — DONE

- 64-bin log-spaced FFT (50Hz–12kHz) with Hann window and energy compensation
- 20Hz background thread, dual ring buffers (pre/post-DSP toggle)
- C++ UI adds: EMA smoothing, peak hold, noise floor tracking, phase-based coloring, articulation band highlighting

### Reliability Polish — DONE

- **Logging**: stdlib `logging` with RotatingFileHandler (2MB, 3 backups) + console. No `print()`.
- **Error handling**: try/except around transcription and typing. Logs error, sets ERROR state, continues.
- **Audio recovery**: consecutive error tracking in callback (threshold: 50), auto-restart via `restart_stream()`.
- **Thread safety**: `threading.Lock` (`_state_lock`) on all multi-attribute mutations.
- **Shutdown**: idempotent `_quit()` with flag, Windows `SetConsoleCtrlHandler` via ctypes, thread join with 2s timeout.
- **LLM fallback**: if LM Studio is down or returns garbage, raw Whisper text is used silently.
- **DSP validation**: all parameters have hard bounds. Invalid values rejected before application.

### HTTP API (Engine Daemon Mode) — DONE

FastAPI + uvicorn, enabled with `--server` flag. 20+ endpoints for full external control. See CLAUDE.md for the complete endpoint table.

- State machine: IDLE → LISTENING → RECORDING → TRANSCRIBING → CLEANING → TYPING (or PENDING_APPROVAL) → any failure → ERROR
- Latency tracking: `time.perf_counter()` per pipeline stage (record, transcribe, cleanup, type)
- Live audio RMS, FFT bins, DSP state in `/status` response
- CORS enabled for browser/tool debugging
- Lazy import: zero overhead without `--server`

### C++ ImGui UI — DONE

- Dear ImGui + DirectX 11, Win32 window, vsync render loop
- Full control panel: DSP sliders with linked enforcement, auto-calibration, spectrum visualizer
- Mode/profile switching, mic selection, hotkey configuration via Edit menu
- Approval panel: Approve/Edit/Reject with multiline text editor
- Status display: connection state, engine phase (color-coded), profile/mode, audio meters, transcripts, latency breakdown
- Recording banner: clickable toggle + hotkey display
- Feature toggles: Approval mode, Push-to-talk
- Engine auto-launch via `CreateProcessW` (spawns `pythonw.exe` or bundled `app.exe`)
- Background HTTP polling (health + status every 50ms)
- Graceful shutdown (POST /engine/shutdown → wait → TerminateProcess fallback)
- Source: `dictation-ui/src/` (main.cpp, app.cpp, engine_client.cpp, engine_process.cpp, dx11_helpers.cpp)

### Packaging — DONE

- PyInstaller `--onedir` bundle of Python engine → `app.exe` (includes torch + CUDA)
- `--base-dir` CLI flag for path resolution in bundled mode
- Whisper model cache redirected to `Murmur/models/`
- C++ launcher detects bundled vs dev mode automatically
- `build.bat` assembles complete `Murmur/` distribution folder

### Quality Standards — MET

- No duplicated characters or phrase doubling
- No silent crashes — all errors logged and reported via state machine
- Automatic audio stream recovery
- Structured logging with rotation
- No network calls unless explicitly enabled
- Fully local by default

### Performance Targets — MET (RTX 4090)

- Model load: < 3 seconds
- Chunk transcription: < 500ms
- End-to-end latency: < 1 second (balanced)
- Zero typing jitter
- DSP processing: < 1ms per block (zero-alloc)

---

## Remaining Work

### Stabilize

1. **Smarter VAD / Silence Chunking**: Replace static RMS energy threshold with Silero VAD for chunk boundary detection. faster-whisper already bundles it. Benefits: works in noisy environments, no manual threshold tuning.

2. **Backlog Protection**: Bounded queue for audio chunks. If user speaks faster than processing, drop oldest chunks (or warn). Prevents memory growth during long sessions.

### Harden

1. **UI Auto-Reconnect**: If `/health` fails N consecutive times → auto-restart engine process. Show "Reconnecting..." in UI.

2. **Watchdog Logic**: Monitor engine process handle, detect unexpected exit, auto-relaunch with backoff (1s, 2s, 5s, give up after 3).

3. **Memory Guardrails**: Track engine memory, warn if excessive, optional periodic restart.

### Future Enhancements

- **Streaming preview**: partial transcription results shown live
- **Advanced commands**: "undo", "backspace N", custom macros
- **Chunking presets**: Aggressive / Balanced / Thoughtful
- **Confidence scoring**: Whisper confidence visualization
- **Multi-language support**
- **Auto-start**: Task Scheduler entry
- **Model Manager UI**: install/remove/set default Whisper models
- **Plugin system**
- **Update checker**

---

## North Star UX

User clicks Murmur.exe → it immediately launches the Python engine (hidden) → confirms connectivity → shows **READY**.

Hit **F1** (or click the recording banner) → speak naturally → pause → app transcribes → optionally cleans → types into active app.

Voice commands: "new line", "send", "clear", "stop dictation". Profiles auto-switch for VS Code / Terminal / LM Studio. DSP gate handles background noise automatically.

No manual terminal steps. No "is it running?" confusion. Fully local. No cloud. No bloat. Developer-grade precision.
