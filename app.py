import json
import os
import logging
import logging.handlers
import time
import threading
import queue
import numpy as np
import keyboard
import mouse
import sounddevice as sd

from services.config import ConfigManager, LLM_URL, LLM_TIMEOUT, WHISPER_RATE, RECORD_RATE, COMPUTE_TYPE, DEBOUNCE_SEC
from services.audio import AudioCaptureService
from services.dsp import NoiseGate, Compressor, DSPChain
from services.transcriber import TranscriptionEngine
from services.commands import CommandRouter
from services.output import OutputInjector
from services.llm import LLMEnhancer
from services.tray import TrayService
from services.window_detect import ActiveWindowDetector
from services.engine_state import EngineState, EnginePhase, LatencyMetrics

PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))
logger = logging.getLogger(__name__)

# ImGui key name (lowercase) → Python keyboard library name
_IMGUI_TO_KEYBOARD = {
    "leftarrow": "left", "rightarrow": "right",
    "uparrow": "up", "downarrow": "down",
    "pageup": "page up", "pagedown": "page down",
    "escape": "esc",
    "leftctrl": "left ctrl", "leftshift": "left shift",
    "leftalt": "left alt", "leftsuper": "left windows",
    "rightctrl": "right ctrl", "rightshift": "right shift",
    "rightalt": "right alt", "rightsuper": "right windows",
    "graveaccent": "`", "apostrophe": "'",
    "leftbracket": "[", "rightbracket": "]", "backslash": "\\",
    "capslock": "caps lock", "scrolllock": "scroll lock",
    "numlock": "num lock", "printscreen": "print screen",
    **{f"keypad{i}": f"num {i}" for i in range(10)},
    "keypaddecimal": "decimal", "keypaddivide": "num /",
    "keypadmultiply": "num *", "keypadsubtract": "num -",
    "keypadadd": "num +", "keypadenter": "num enter",
}


def _setup_logging(base_dir: str):
    log_dir = os.path.join(base_dir, "logs")
    os.makedirs(log_dir, exist_ok=True)

    file_handler = logging.handlers.RotatingFileHandler(
        os.path.join(log_dir, "dictation.log"),
        maxBytes=2 * 1024 * 1024,
        backupCount=3,
        encoding="utf-8",
    )
    file_handler.setFormatter(logging.Formatter(
        "%(asctime)s  %(levelname)-5s  %(message)s", datefmt="%H:%M:%S"
    ))

    console_handler = logging.StreamHandler()
    console_handler.setFormatter(logging.Formatter("%(message)s"))

    root = logging.getLogger()
    root.setLevel(logging.DEBUG)
    root.addHandler(file_handler)
    root.addHandler(console_handler)

    # Silence noisy third-party loggers
    for name in ("PIL", "httpcore", "httpx", "urllib3", "huggingface_hub"):
        logging.getLogger(name).setLevel(logging.WARNING)


class DictationApp:
    def __init__(self):
        # Config (--base-dir override resolved inside ConfigManager)
        self.config = ConfigManager(PROJECT_DIR)

        # Services
        self.output = OutputInjector()

        # DSP chain (noise gate + compressor)
        dsp_cfg = self.config.cfg.get("dsp", {})
        gate = NoiseGate(sample_rate=RECORD_RATE, **dsp_cfg.get("noise_gate", {}))
        compressor = Compressor(sample_rate=RECORD_RATE, **dsp_cfg.get("compressor", {}))
        self.dsp_chain = DSPChain(gate, compressor)

        mic_index = self._resolve_mic_device()
        self.audio = AudioCaptureService(mic_index, dsp_chain=self.dsp_chain)
        model_dir = os.path.join(self.config.project_dir, "models")
        self.transcriber = TranscriptionEngine(
            self.config.get("whisper_model"), "cuda", COMPUTE_TYPE,
            model_dir=model_dir,
        )
        self.commands = CommandRouter(
            self.config.get("voice_commands"),
            self.output,
            prefix=self.config.get("command_prefix"),
        )

        # LLM — always create (it's just an HTTP client), mode controls whether it's used
        self.llm = LLMEnhancer(
            url=LLM_URL,
            model=self.config.get("llm_model"),
            system_prompt="",
            temperature=0.1,
            max_tokens=500,
            timeout=LLM_TIMEOUT,
        )

        # State
        self.recording = False
        self._last_toggle = 0.0
        self._stop_event = threading.Event()
        self._state_lock = threading.Lock()
        self._transcription_thread: threading.Thread | None = None
        self._shutdown_called = False
        self._hotkey_hooks = []
        self.current_mode = self.config.get("llm_mode")
        self.current_profile = next(iter(self.config.get_profile_names()), "Default")
        self.llm_enabled = False
        self.newline_key = "enter"

        # Feature toggles
        self.approval_mode = self.config.get("approval_mode")
        self.push_to_talk = self.config.get("push_to_talk")

        # Engine state (for API)
        self.engine_state = EngineState()

        # Apply initial mode
        self._apply_mode(self.current_mode, quiet=True)

        # Tray
        auto_detect_cfg = self.config.get_auto_detect_config()
        self.tray = TrayService(
            toggle_callback=self.toggle_recording,
            is_recording_callback=lambda: self.recording,
            quit_callback=self._quit,
            hotkey_label=self.config.get("hotkey"),
            mode_names=self.config.get_mode_names(),
            profile_names=self.config.get_profile_names(),
            current_mode=self.current_mode,
            current_profile=self.current_profile,
            on_mode_changed=self._apply_mode,
            on_profile_changed=self.switch_profile,
            auto_detect_enabled=auto_detect_cfg.get("enabled", False),
            on_auto_detect_toggled=self._toggle_auto_detect,
            approval_mode=self.approval_mode,
            push_to_talk=self.push_to_talk,
            on_approval_mode_toggled=self.set_approval_mode,
            on_push_to_talk_toggled=self.set_push_to_talk,
        )

        # Window auto-detect
        self.window_detector = None
        if auto_detect_cfg.get("enabled", False):
            self._start_detector(auto_detect_cfg)

        # Silence detection settings
        self.energy_threshold = self.config.get("energy_threshold")
        self.silence_timeout = self.config.get("silence_timeout")
        self.max_speech_sec = self.config.get("max_speech_seconds")
        self.toggle_key = self.config.get("hotkey")

        # Windows console close handler
        self._install_console_handler()

        # HTTP API server (optional, only with --server flag)
        self.api_server = None
        if self.config.args.server:
            from services.server import APIServer
            self.api_server = APIServer(
                engine=self,
                host="127.0.0.1",
                port=self.config.args.port,
            )

    # --- Mic device resolution ---

    def _resolve_mic_device(self) -> int:
        """Resolve mic device index with name-based fallback for USB replug resilience."""
        mic_index = self.config.get("mic_device_index")
        mic_name = self.config.cfg.get("mic_device_name", "")

        try:
            actual_name = sd.query_devices(mic_index)['name']
            if mic_name and mic_name != actual_name:
                resolved = self._resolve_mic_by_name(mic_name)
                if resolved is not None:
                    logger.info("Mic '%s' moved to index %d", mic_name, resolved)
                    return resolved
            return mic_index
        except Exception:
            if mic_name:
                resolved = self._resolve_mic_by_name(mic_name)
                if resolved is not None:
                    logger.info("Saved mic index %d invalid, found '%s' at index %d",
                                mic_index, mic_name, resolved)
                    return resolved
            fallback = sd.default.device[0]
            logger.warning("Saved mic device %d not found, falling back to default (%d)",
                           mic_index, fallback)
            return fallback

    @staticmethod
    def _resolve_mic_by_name(name: str) -> int | None:
        """Find an input device by name. Returns index or None."""
        devices = sd.query_devices()
        for i, dev in enumerate(devices):
            if dev['max_input_channels'] > 0 and dev['name'] == name:
                return i
        return None

    # --- Console handler ---

    def _install_console_handler(self):
        """Register a Windows console control handler for clean shutdown on close/logoff."""
        import ctypes
        kernel32 = ctypes.windll.kernel32

        @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_uint)
        def handler(event):
            # CTRL_C_EVENT=0, CTRL_CLOSE_EVENT=2, CTRL_LOGOFF_EVENT=5, CTRL_SHUTDOWN_EVENT=6
            if event in (0, 2, 5, 6):
                self._quit()
                return True
            return False

        kernel32.SetConsoleCtrlHandler(handler, True)
        self._console_handler = handler  # prevent GC

    # --- Mode / Profile switching ---

    def _apply_mode(self, mode_name: str, quiet: bool = False):
        """Apply an LLM mode. Updates llm_enabled and reconfigures LLM."""
        with self._state_lock:
            mode_cfg = self.config.resolve_mode(mode_name)
            self.current_mode = mode_name
            self.llm_enabled = mode_cfg.get("llm_cleanup", False)
            self.newline_key = mode_cfg.get("newline_key", "enter")

            if self.llm_enabled:
                prompt_file = mode_cfg.get("llm_system_prompt_file", "prompts/clean_system.txt")
                system_prompt = self.config.load_system_prompt(prompt_file)
                self.llm.configure(
                    system_prompt=system_prompt,
                    temperature=mode_cfg.get("llm_temperature", 0.1),
                    max_tokens=mode_cfg.get("llm_max_tokens", 500),
                )

        if hasattr(self, "tray"):
            self.tray.set_mode(mode_name)
        if not quiet:
            logger.info("  [mode] %s", mode_name)

    def switch_profile(self, profile_name: str):
        """Switch to a named profile. Reconfigures mode, voice commands, hotkey."""
        profile_cfg = self.config.resolve_profile(profile_name)

        with self._state_lock:
            self.current_profile = profile_name

        # Switch LLM mode (acquires _state_lock internally)
        new_mode = profile_cfg.get("llm_mode", "clean")
        self._apply_mode(new_mode)

        # Update voice commands
        if "voice_commands" in profile_cfg:
            self.commands.update_commands(profile_cfg["voice_commands"])

        # Update hotkey if profile overrides it
        new_hotkey = profile_cfg.get("hotkey", self.toggle_key)
        if new_hotkey != self.toggle_key:
            self._unregister_hotkey()
            self.toggle_key = new_hotkey
            self._register_hotkey()

        # Update feature toggles if profile overrides them
        if "approval_mode" in profile_cfg:
            self.set_approval_mode(profile_cfg["approval_mode"])
        if "push_to_talk" in profile_cfg:
            self.set_push_to_talk(profile_cfg["push_to_talk"])

        if hasattr(self, "tray"):
            self.tray.set_profile(profile_name)
        logger.info("  [profile] %s (mode: %s)", profile_name, new_mode)

    def _toggle_auto_detect(self, enabled: bool):
        """Start or stop the window auto-detector from tray toggle."""
        if enabled and not self.window_detector:
            auto_detect_cfg = self.config.get_auto_detect_config()
            self._start_detector(auto_detect_cfg)
        elif not enabled and self.window_detector:
            self.window_detector.stop()
            self.window_detector = None
        if hasattr(self, "tray"):
            self.tray.set_auto_detect(enabled)
        logger.info("  [auto-detect] %s", "ON" if enabled else "OFF")

    def _start_detector(self, auto_detect_cfg: dict):
        rules = auto_detect_cfg.get("rules", [])
        if rules:
            self.window_detector = ActiveWindowDetector(
                rules=rules,
                on_match=self._on_auto_detect_match,
                poll_interval_ms=auto_detect_cfg.get("poll_interval_ms", 500),
            )
            self.window_detector.start()

    def _on_auto_detect_match(self, profile_name: str):
        if profile_name != self.current_profile:
            self.switch_profile(profile_name)

    # --- Recording ---

    def toggle_recording(self):
        now = time.time()
        if now - self._last_toggle < DEBOUNCE_SEC:
            return
        self._last_toggle = now

        with self._state_lock:
            if not self.recording:
                self.recording = True
                self.engine_state.phase = EnginePhase.LISTENING
                self.audio.start_recording()
                logger.info("\n[REC] ON  — speak naturally, pauses trigger transcription  (%s to stop)", self.toggle_key.upper())
                self._transcription_thread = threading.Thread(target=self._transcription_loop, daemon=True)
                self._transcription_thread.start()
            else:
                self.recording = False
                self.engine_state.phase = EnginePhase.IDLE
                self.audio.stop_recording()
                logger.info("[STOP] OFF — finishing...")

        self.tray.on_state_changed()

    def _transcription_loop(self):
        """Silence-detection state machine."""
        speech_chunks: list[np.ndarray] = []
        speech_start: float | None = None
        silence_start: float | None = None
        audio_q = self.audio.audio_q

        while self.recording:
            # Check for audio stream errors
            if self.audio.needs_restart:
                if not self.audio.restart_stream():
                    logger.error("Audio stream unrecoverable, stopping recording")
                    self.engine_state.phase = EnginePhase.ERROR
                    self.engine_state.last_error = "Audio stream unrecoverable"
                    self.recording = False
                    break

            try:
                data, rms = audio_q.get(timeout=0.1)
            except queue.Empty:
                if speech_start and time.time() - speech_start >= self.max_speech_sec:
                    self._flush(speech_chunks)
                    speech_chunks, speech_start, silence_start = [], None, None
                    self.engine_state.phase = EnginePhase.LISTENING
                continue

            # Live RMS for audio meter
            self.engine_state.audio_rms = rms

            is_speech = rms > self.energy_threshold

            if is_speech:
                silence_start = None
                if speech_start is None:
                    speech_start = time.time()
                    self.engine_state.phase = EnginePhase.RECORDING
                speech_chunks.append(data)
            else:
                if speech_chunks:
                    speech_chunks.append(data)
                    if silence_start is None:
                        silence_start = time.time()
                    elif time.time() - silence_start >= self.silence_timeout:
                        self._flush(speech_chunks)
                        speech_chunks, speech_start, silence_start = [], None, None
                        self.engine_state.phase = EnginePhase.LISTENING

            if speech_start and time.time() - speech_start >= self.max_speech_sec:
                self._flush(speech_chunks)
                speech_chunks, speech_start, silence_start = [], None, None
                self.engine_state.phase = EnginePhase.LISTENING

        # Final flush
        while True:
            try:
                data, rms = audio_q.get_nowait()
                speech_chunks.append(data)
            except queue.Empty:
                break
        if speech_chunks:
            self._flush(speech_chunks)
        logger.info("  Done.\n")

    def _flush(self, chunks: list[np.ndarray]):
        if not chunks:
            return
        try:
            self._process_speech(chunks)
        except Exception as e:
            logger.error("  [error] %s", e)
            self.engine_state.phase = EnginePhase.ERROR
            self.engine_state.last_error = str(e)

    def _process_speech(self, chunks: list[np.ndarray]):
        audio = AudioCaptureService.resample(chunks)
        duration = len(audio) / WHISPER_RATE

        if len(audio) < WHISPER_RATE // 4:
            return

        record_ms = duration * 1000.0

        # --- Transcribe ---
        self.engine_state.phase = EnginePhase.TRANSCRIBING
        logger.info("  [%.1fs] transcribing...", duration)
        t0 = time.perf_counter()
        try:
            raw_text = self.transcriber.transcribe(audio)
        except Exception as e:
            logger.error("  Transcription failed: %s", e)
            self.engine_state.phase = EnginePhase.ERROR
            self.engine_state.last_error = str(e)
            return
        transcribe_ms = (time.perf_counter() - t0) * 1000.0

        if not raw_text:
            logger.info("  (no speech detected)")
            self.engine_state.phase = EnginePhase.LISTENING
            return

        self.engine_state.last_raw_transcript = raw_text

        # Voice command check
        is_cmd, action = self.commands.check(raw_text)
        if is_cmd:
            if action == "stop":
                logger.info("  [cmd] stop dictation")
                self.toggle_recording()
            else:
                logger.info("  [cmd] %s", raw_text.lower().strip().rstrip(".!?,"))
                self.commands.execute(action)
            return

        # --- LLM cleanup ---
        cleanup_ms = 0.0
        if self.llm_enabled:
            self.engine_state.phase = EnginePhase.CLEANING
            t0 = time.perf_counter()
            cleaned = self.llm.cleanup(raw_text)
            cleanup_ms = (time.perf_counter() - t0) * 1000.0
            logger.info("  raw:     %s", raw_text)
            logger.info("  cleaned: %s", cleaned)
            final = cleaned
        else:
            logger.info("  >> %s", raw_text)
            final = raw_text

        self.engine_state.last_cleaned_text = final

        # --- Type or hold for approval ---
        if self.approval_mode:
            self.engine_state.pending_text = final
            self.engine_state.phase = EnginePhase.PENDING_APPROVAL
            logger.info("  >> pending approval: %s", final)
            self.engine_state.latency = LatencyMetrics(
                record_ms=round(record_ms, 1),
                transcribe_ms=round(transcribe_ms, 1),
                cleanup_ms=round(cleanup_ms, 1),
                type_ms=0.0,
            )
            return

        self.engine_state.phase = EnginePhase.TYPING
        t0 = time.perf_counter()
        try:
            self.output.type_text(final + " ", newline_key=self.newline_key)
        except Exception as e:
            logger.error("  Failed to type text: %s", e)
            self.engine_state.phase = EnginePhase.ERROR
            self.engine_state.last_error = str(e)
            return
        type_ms = (time.perf_counter() - t0) * 1000.0
        logger.info("  >> typed")

        # --- Update latency metrics ---
        self.engine_state.latency = LatencyMetrics(
            record_ms=round(record_ms, 1),
            transcribe_ms=round(transcribe_ms, 1),
            cleanup_ms=round(cleanup_ms, 1),
            type_ms=round(type_ms, 1),
        )

        self.engine_state.phase = EnginePhase.LISTENING

    # --- Approval Mode ---

    def approve_pending(self):
        """Type the pending text and clear it."""
        text = self.engine_state.pending_text
        if not text:
            return
        self.engine_state.phase = EnginePhase.TYPING
        t0 = time.perf_counter()
        try:
            self.output.type_text(text + " ", newline_key=self.newline_key)
        except Exception as e:
            logger.error("  Failed to type approved text: %s", e)
            self.engine_state.phase = EnginePhase.ERROR
            self.engine_state.last_error = str(e)
            return
        type_ms = (time.perf_counter() - t0) * 1000.0
        logger.info("  >> approved and typed")
        self.engine_state.pending_text = ""
        self.engine_state.latency.type_ms = round(type_ms, 1)
        self.engine_state.phase = EnginePhase.LISTENING if self.recording else EnginePhase.IDLE

    def edit_pending(self, new_text: str):
        """Replace pending text with edited version, then type it."""
        self.engine_state.pending_text = new_text
        self.approve_pending()

    def reject_pending(self):
        """Discard the pending text."""
        self.engine_state.pending_text = ""
        logger.info("  >> rejected")
        self.engine_state.phase = EnginePhase.LISTENING if self.recording else EnginePhase.IDLE

    def set_approval_mode(self, enabled: bool):
        """Enable or disable approval mode."""
        self.approval_mode = bool(enabled)
        if hasattr(self, "tray"):
            self.tray.set_approval_mode(self.approval_mode)
        logger.info("  [approval_mode] %s", "ON" if self.approval_mode else "OFF")

    # --- Push-to-Talk ---

    def _register_hotkey(self):
        """Register the hotkey based on current push_to_talk setting."""
        self._hotkey_hooks = []
        if self.toggle_key.startswith("mouse_"):
            # Mouse button hotkey (uses mouse library)
            btn = self.toggle_key.replace("mouse_", "")
            mouse_btn = {"x1": "x", "x2": "x2"}.get(btn, btn)
            if self.push_to_talk:
                h1 = mouse.on_button(lambda: self._ptt_press(), buttons=(mouse_btn,), types=("down",))
                h2 = mouse.on_button(lambda: self._ptt_release(), buttons=(mouse_btn,), types=("up",))
                self._hotkey_hooks = [("mouse", h1), ("mouse", h2)]
            else:
                h = mouse.on_button(lambda: self.toggle_recording(), buttons=(mouse_btn,), types=("down",))
                self._hotkey_hooks = [("mouse", h)]
            logger.info("  [hotkey] %s: %s to %s",
                        "Push-to-talk" if self.push_to_talk else "Toggle",
                        self.toggle_key.upper(),
                        "hold/release" if self.push_to_talk else "start/stop")
        else:
            # Keyboard hotkey
            if self.push_to_talk:
                h1 = keyboard.on_press_key(self.toggle_key, lambda e: self._ptt_press(), suppress=True)
                h2 = keyboard.on_release_key(self.toggle_key, lambda e: self._ptt_release())
                self._hotkey_hooks = [("keyboard", h1), ("keyboard", h2)]
                logger.info("  [hotkey] Push-to-talk: hold %s to record", self.toggle_key.upper())
            else:
                h = keyboard.add_hotkey(self.toggle_key, self.toggle_recording, suppress=True)
                self._hotkey_hooks = [("keyboard", h)]
                logger.info("  [hotkey] Toggle: press %s to start/stop", self.toggle_key.upper())

    def _unregister_hotkey(self):
        """Remove current hotkey registration using saved handles."""
        for item in getattr(self, '_hotkey_hooks', []):
            try:
                if isinstance(item, tuple):
                    kind, hook = item
                    if kind == "mouse":
                        mouse.unhook(hook)
                    else:
                        keyboard.unhook(hook)
                else:
                    keyboard.unhook(item)
            except (KeyError, ValueError):
                pass
        self._hotkey_hooks = []

    def _ptt_press(self):
        """Push-to-talk key down: start recording if not already."""
        now = time.time()
        if now - self._last_toggle < DEBOUNCE_SEC:
            return
        self._last_toggle = now
        if not self.recording:
            with self._state_lock:
                self.recording = True
                self.engine_state.phase = EnginePhase.LISTENING
                self.audio.start_recording()
                logger.info("\n[REC] ON  — push-to-talk active  (release %s to stop)", self.toggle_key.upper())
                self._transcription_thread = threading.Thread(target=self._transcription_loop, daemon=True)
                self._transcription_thread.start()
            self.tray.on_state_changed()

    def _ptt_release(self):
        """Push-to-talk key up: stop recording and flush."""
        if self.recording:
            with self._state_lock:
                self.recording = False
                self.engine_state.phase = EnginePhase.IDLE
                self.audio.stop_recording()
                logger.info("[STOP] OFF — push-to-talk released, finishing...")
            self.tray.on_state_changed()

    def set_push_to_talk(self, enabled: bool):
        """Switch between toggle and push-to-talk hotkey modes."""
        enabled = bool(enabled)
        if enabled == self.push_to_talk:
            return
        # Stop recording if active during mode switch
        if self.recording:
            self.toggle_recording()
        self._unregister_hotkey()
        self.push_to_talk = enabled
        self._register_hotkey()
        if hasattr(self, "tray"):
            self.tray.set_push_to_talk(enabled)
        logger.info("  [push_to_talk] %s", "ON" if enabled else "OFF")

    def set_hotkey(self, new_key: str):
        """Change the activation hotkey at runtime."""
        self._unregister_hotkey()
        # Translate ImGui key names to keyboard library names
        self.toggle_key = _IMGUI_TO_KEYBOARD.get(new_key, new_key)
        self._register_hotkey()
        if hasattr(self, "tray"):
            self.tray.set_hotkey_label(new_key)
        logger.info("  [hotkey] Changed to %s", new_key.upper())

    # --- Mic device ---

    def set_mic_device(self, new_index: int):
        """Switch mic device at runtime. Stops recording if active."""
        if new_index == self.audio.device_index:
            return

        was_recording = self.recording
        if was_recording:
            self.toggle_recording()

        self.audio.switch_device(new_index)
        self.config.cfg["mic_device_index"] = new_index
        self._save_mic_preference(new_index)
        logger.info("  [mic] Switched to device %d", new_index)

        if was_recording:
            self.toggle_recording()

    def _save_mic_preference(self, device_index: int):
        """Persist mic selection to config.json."""
        config_path = os.path.join(self.config.project_dir, "config.json")
        try:
            with open(config_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
            cfg["mic_device_index"] = device_index
            cfg["mic_device_name"] = self.audio.get_device_name()
            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(cfg, f, indent=2, ensure_ascii=False)
                f.write("\n")
        except Exception as e:
            logger.warning("Failed to save mic preference: %s", e)

    # --- DSP config persistence ---

    def _save_dsp_config(self):
        """Persist current DSP settings to config.json."""
        config_path = os.path.join(self.config.project_dir, "config.json")
        try:
            with open(config_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
            cfg["dsp"] = {
                "noise_gate": {k: v for k, v in self.dsp_chain.gate.get_state().items()
                              if k in ("enabled", "open_threshold_dbfs", "close_threshold_dbfs",
                                       "floor_db", "hold_ms", "attack_ms", "release_ms")},
                "compressor": {k: v for k, v in self.dsp_chain.compressor.get_state().items()
                              if k in ("enabled", "threshold_dbfs", "ratio",
                                       "attack_ms", "release_ms", "makeup_gain_db")},
            }
            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(cfg, f, indent=2, ensure_ascii=False)
                f.write("\n")
        except Exception as e:
            logger.warning("Failed to save DSP config: %s", e)

    # --- Lifecycle ---

    def _quit(self):
        """Clean shutdown triggered by tray Quit, Ctrl-C, or console close."""
        if self._shutdown_called:
            return
        self._shutdown_called = True

        if self.recording:
            self.recording = False
            self.audio.stop_recording()
            if self._transcription_thread and self._transcription_thread.is_alive():
                self._transcription_thread.join(timeout=2.0)
        if self.window_detector:
            self.window_detector.stop()
        if self.api_server:
            self.api_server.stop()
        self.tray.stop()
        self._stop_event.set()

    def run(self):
        llm_model = self.config.get("llm_model")
        logger.info("Whisper streaming dictation ready.")
        logger.info("Toggle: %s  |  Model: %s  |  Mic: device %s", self.toggle_key.upper(), self.config.get("whisper_model"), self.config.get("mic_device_index"))
        llm_status = f"ON ({llm_model}, mode: {self.current_mode})" if self.llm_enabled else f"OFF (mode: {self.current_mode})"
        logger.info("LLM: %s", llm_status)
        logger.info("Profile: %s  |  Auto-detect: %s", self.current_profile, "ON" if self.window_detector else "OFF")
        logger.info("Voice commands: %s", ", ".join(self.config.get("voice_commands").keys()))
        logger.info("Silence: threshold=%s  pause=%ss  max=%ss\n", self.energy_threshold, self.silence_timeout, self.max_speech_sec)

        self._register_hotkey()
        self.audio.start_stream()
        self.tray.start()

        if self.api_server:
            self.api_server.start()
            logger.info("API: http://127.0.0.1:%d", self.config.args.port)

        try:
            while not self._stop_event.wait(timeout=0.25):
                pass
        except KeyboardInterrupt:
            self._quit()

        self.audio.stop_stream()
        keyboard.unhook_all()
        logger.info("\nShutdown.")


if __name__ == "__main__":
    # Pre-parse --base-dir for logging setup (full parse happens in ConfigManager)
    import sys
    _base = PROJECT_DIR
    for i, arg in enumerate(sys.argv[1:], 1):
        if arg == "--base-dir" and i < len(sys.argv) - 1:
            _base = os.path.abspath(sys.argv[i + 1])
            break
        if arg.startswith("--base-dir="):
            _base = os.path.abspath(arg.split("=", 1)[1])
            break

    _setup_logging(_base)
    app = DictationApp()
    app.run()
