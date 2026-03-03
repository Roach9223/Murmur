import io
import logging
import os
import sys
import threading

import uvicorn
from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

logger = logging.getLogger(__name__)

VERSION = "1.0.0"


# --- Request models ---

class SetModeRequest(BaseModel):
    mode: str

class SetProfileRequest(BaseModel):
    profile: str

class CommandRequest(BaseModel):
    cmd: str

class ToggleRequest(BaseModel):
    enabled: bool

class EditPendingRequest(BaseModel):
    text: str

class SetHotkeyRequest(BaseModel):
    hotkey: str

class SetMicRequest(BaseModel):
    device_index: int

class CalibrateRequest(BaseModel):
    action: str  # "start" or "finish"


# --- Route factory ---

def create_app(engine) -> FastAPI:
    """Build the FastAPI application. Route handlers close over the engine reference."""
    app = FastAPI(title="Whisper Dictation Engine", version=VERSION)

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # --- Health / Status ---

    @app.get("/health")
    def health():
        return {
            "status": "ok",
            "version": VERSION,
            "uptime_s": round(engine.engine_state.uptime_s, 1),
        }

    @app.get("/status")
    def status():
        with engine._state_lock:
            return engine.engine_state.to_status_dict(engine)

    # --- Controls ---

    @app.post("/control/toggle")
    def control_toggle():
        engine.toggle_recording()
        return {"ok": True, "recording": engine.recording}

    @app.post("/control/start")
    def control_start():
        if not engine.recording:
            engine.toggle_recording()
        return {"ok": True, "recording": engine.recording}

    @app.post("/control/stop")
    def control_stop():
        if engine.recording:
            engine.toggle_recording()
        return {"ok": True, "recording": engine.recording}

    @app.post("/control/set_mode")
    def control_set_mode(req: SetModeRequest):
        valid_modes = engine.config.get_mode_names()
        if req.mode not in valid_modes:
            raise HTTPException(400, f"Unknown mode '{req.mode}'. Valid: {valid_modes}")
        engine._apply_mode(req.mode)
        return {"ok": True, "mode": req.mode}

    @app.post("/control/set_profile")
    def control_set_profile(req: SetProfileRequest):
        valid_profiles = engine.config.get_profile_names()
        if req.profile not in valid_profiles:
            raise HTTPException(400, f"Unknown profile '{req.profile}'. Valid: {valid_profiles}")
        engine.switch_profile(req.profile)
        return {"ok": True, "profile": req.profile}

    @app.post("/control/command")
    def control_command(req: CommandRequest):
        valid_cmds = {"newline", "send", "clear", "stop"}
        if req.cmd not in valid_cmds:
            raise HTTPException(400, f"Unknown command '{req.cmd}'. Valid: {sorted(valid_cmds)}")

        if req.cmd == "stop":
            if engine.recording:
                engine.toggle_recording()
        elif req.cmd == "newline":
            action = engine.commands.commands.get("new line", "enter")
            engine.commands.execute(action)
        elif req.cmd == "send":
            action = engine.commands.commands.get("send", "enter")
            engine.commands.execute(action)
        elif req.cmd == "clear":
            engine.commands.execute("select_all_delete")

        return {"ok": True, "cmd": req.cmd}

    # --- Approval Mode ---

    @app.post("/control/set_approval_mode")
    def control_set_approval_mode(req: ToggleRequest):
        engine.set_approval_mode(req.enabled)
        return {"ok": True, "approval_mode": engine.approval_mode}

    @app.post("/control/approve")
    def control_approve():
        if not engine.engine_state.pending_text:
            raise HTTPException(400, "No pending text to approve")
        engine.approve_pending()
        return {"ok": True}

    @app.post("/control/edit")
    def control_edit(req: EditPendingRequest):
        if not req.text:
            raise HTTPException(400, "Edited text cannot be empty")
        engine.edit_pending(req.text)
        return {"ok": True}

    @app.post("/control/reject")
    def control_reject():
        engine.reject_pending()
        return {"ok": True}

    # --- Push-to-Talk ---

    @app.post("/control/set_push_to_talk")
    def control_set_push_to_talk(req: ToggleRequest):
        engine.set_push_to_talk(req.enabled)
        return {"ok": True, "push_to_talk": engine.push_to_talk}

    # --- Hotkey ---

    @app.post("/control/set_hotkey")
    def control_set_hotkey(req: SetHotkeyRequest):
        if not req.hotkey:
            raise HTTPException(400, "Hotkey cannot be empty")
        engine.set_hotkey(req.hotkey)
        return {"ok": True, "hotkey": engine.toggle_key}

    # --- Mic ---

    @app.post("/control/set_mic")
    def control_set_mic(req: SetMicRequest):
        try:
            engine.set_mic_device(req.device_index)
            return {"ok": True, "mic_device_index": req.device_index}
        except Exception as e:
            raise HTTPException(400, str(e))

    # --- DSP ---

    @app.post("/dsp/calibrate")
    def dsp_calibrate(req: CalibrateRequest):
        chain = getattr(engine, "dsp_chain", None)
        if chain is None or not chain.gate.enabled:
            raise HTTPException(400, "Gate not enabled")
        if req.action == "start":
            chain.gate.start_calibration()
            return {"ok": True, "message": "Calibrating... keep silent for 1.5s"}
        elif req.action == "finish":
            result = chain.gate.finish_calibration()
            if result is None:
                raise HTTPException(400, "Calibration failed (speech detected or no data)")
            try:
                chain.gate.configure(
                    open_threshold_dbfs=result["open_threshold_dbfs"],
                    close_threshold_dbfs=result["close_threshold_dbfs"],
                )
            except ValueError as e:
                raise HTTPException(400, str(e))
            engine._save_dsp_config()
            return {"ok": True, "thresholds": result}
        raise HTTPException(400, "action must be 'start' or 'finish'")

    # --- Config ---

    @app.get("/config")
    def get_config():
        return engine.config.cfg

    @app.post("/config")
    def update_config(body: dict):
        """Partial config update. Merges and applies relevant changes immediately."""
        engine.config.cfg.update(body)

        if "llm_mode" in body:
            engine._apply_mode(body["llm_mode"])
        if "energy_threshold" in body:
            engine.energy_threshold = body["energy_threshold"]
        if "silence_timeout" in body:
            engine.silence_timeout = body["silence_timeout"]
        if "max_speech_seconds" in body:
            engine.max_speech_sec = body["max_speech_seconds"]
        if "approval_mode" in body:
            engine.set_approval_mode(body["approval_mode"])
        if "push_to_talk" in body:
            engine.set_push_to_talk(body["push_to_talk"])

        # DSP config updates
        if "dsp" in body:
            chain = getattr(engine, "dsp_chain", None)
            if chain:
                try:
                    if "noise_gate" in body["dsp"]:
                        chain.gate.configure(**body["dsp"]["noise_gate"])
                    if "compressor" in body["dsp"]:
                        chain.compressor.configure(**body["dsp"]["compressor"])
                except ValueError as e:
                    raise HTTPException(400, str(e))
                engine._save_dsp_config()

        # Spectrum source toggle
        if "spectrum_source" in body:
            engine.audio.set_spectrum_source(body["spectrum_source"] == "pre")

        return {"ok": True}

    # --- Diagnostics ---

    @app.get("/logs/tail")
    def logs_tail(n: int = Query(default=200, ge=1, le=5000)):
        log_path = os.path.join(engine.config.project_dir, "logs", "dictation.log")
        if not os.path.exists(log_path):
            return {"lines": []}

        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            all_lines = f.readlines()

        return {"lines": [line.rstrip("\n") for line in all_lines[-n:]]}

    @app.post("/engine/shutdown")
    def engine_shutdown():
        """Trigger graceful shutdown. Returns immediately; engine stops async."""
        logger.info("[API] Shutdown requested")
        threading.Thread(target=engine._quit, daemon=True).start()
        return {"ok": True, "message": "Shutting down"}

    return app


class APIServer:
    """Runs uvicorn in a daemon thread."""

    def __init__(self, engine, host: str = "127.0.0.1", port: int = 8899):
        self._engine = engine
        self._host = host
        self._port = port
        self._thread: threading.Thread | None = None
        self._server: uvicorn.Server | None = None

    def start(self):
        """Start uvicorn in a daemon thread."""
        # PyInstaller --windowed sets sys.stderr/stdout to None, which crashes
        # uvicorn's DefaultFormatter (isatty() call). Patch both streams and
        # disable uvicorn's own log config entirely (we have our own logging).
        if sys.stderr is None:
            sys.stderr = io.StringIO()
        if sys.stdout is None:
            sys.stdout = io.StringIO()

        fastapi_app = create_app(self._engine)

        config = uvicorn.Config(
            app=fastapi_app,
            host=self._host,
            port=self._port,
            log_level="warning",
            log_config=None,
            access_log=False,
        )
        self._server = uvicorn.Server(config)

        self._thread = threading.Thread(
            target=self._run_server,
            daemon=True,
            name="api-server",
        )
        self._thread.start()
        logger.info("API server started on http://%s:%d", self._host, self._port)

    def _run_server(self):
        try:
            self._server.run()
        except Exception as e:
            logger.error("API server failed: %s", e)

    def stop(self):
        """Signal uvicorn to shut down gracefully."""
        if self._server:
            self._server.should_exit = True
            logger.info("API server stopping...")
