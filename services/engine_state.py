import enum
import os
import time
from dataclasses import dataclass, field

from services.audio import AudioCaptureService


class EnginePhase(str, enum.Enum):
    """Granular engine state exposed via /status API."""
    IDLE = "idle"
    LISTENING = "listening"
    RECORDING = "recording"
    TRANSCRIBING = "transcribing"
    CLEANING = "cleaning"
    TYPING = "typing"
    PENDING_APPROVAL = "pending_approval"
    ERROR = "error"


@dataclass
class LatencyMetrics:
    """Timing breakdown for the last processed speech chunk."""
    record_ms: float = 0.0
    transcribe_ms: float = 0.0
    cleanup_ms: float = 0.0
    type_ms: float = 0.0


@dataclass
class EngineState:
    """Shared state container. Written by transcription pipeline, read by API server."""
    phase: EnginePhase = EnginePhase.IDLE
    start_time: float = field(default_factory=time.time)
    last_raw_transcript: str = ""
    last_cleaned_text: str = ""
    audio_rms: float = 0.0
    vad_speech_prob: float = 0.0
    latency: LatencyMetrics = field(default_factory=LatencyMetrics)
    last_error: str = ""
    pending_text: str = ""

    @property
    def uptime_s(self) -> float:
        return time.time() - self.start_time

    def to_status_dict(self, app) -> dict:
        """Build the full /status response payload."""
        return {
            "state": self.phase.value,
            "mic_device": app.config.get("mic_device_index"),
            "model_whisper": app.config.get("whisper_model"),
            "cleanup_enabled": app.llm_enabled,
            "cleanup_model": app.config.get("llm_model") if app.llm_enabled else None,
            "cleanup_backend": app.config.cfg.get("llm_backend", {}).get("type", "lmstudio"),
            "cleanup_backend_url": app.config.cfg.get("llm_backend", {}).get(
                app.config.cfg.get("llm_backend", {}).get("type", "lmstudio"), {}
            ).get("url", ""),
            "cleanup_backend_urls": {
                "lmstudio": app.config.cfg.get("llm_backend", {}).get("lmstudio", {}).get("url", ""),
                "llamacpp": app.config.cfg.get("llm_backend", {}).get("llamacpp", {}).get("url", ""),
            },
            "current_mode": app.current_mode,
            "current_profile": app.current_profile,
            "last_raw_transcript": self.last_raw_transcript,
            "last_cleaned_text": self.last_cleaned_text,
            "latency_ms": {
                "record": round(self.latency.record_ms, 1),
                "transcribe": round(self.latency.transcribe_ms, 1),
                "cleanup": round(self.latency.cleanup_ms, 1),
                "type": round(self.latency.type_ms, 1),
            },
            "audio_rms": round(app.audio.live_rms, 6),
            "fft_bins": app.audio.get_cached_fft_bins() if hasattr(app, 'audio') and app.audio._stream else [],
            "is_speech": (self.vad_speech_prob >= app.vad.threshold
                         if app.vad_enabled and app.vad
                         else self.audio_rms > app.energy_threshold),
            "vad_enabled": getattr(app, 'vad_enabled', False),
            "vad_speech_prob": round(self.vad_speech_prob, 3) if getattr(app, 'vad_enabled', False) else None,
            "audio_queue_drops": app.audio.queue_drops,
            "errors": self.last_error or None,
            "approval_mode": app.approval_mode,
            "push_to_talk": app.push_to_talk,
            "pending_text": self.pending_text,
            "recording": app.recording,
            "model_loading": getattr(app, '_model_loading', False),
            "hotkey": app.toggle_key,
            "mode_names": app.config.get_mode_names(),
            "profile_names": app.config.get_profile_names(),
            "input_devices": AudioCaptureService.enumerate_input_devices(),
            "mic_device_index": app.audio.device_index,
            "mic_device_name": app.audio.get_device_name(),
            "dsp": app.audio.get_dsp_state(),
            "spectrum_pre_dsp": app.audio._spectrum_pre_dsp,
            "wav_recording": {
                "active": app.wav_recorder.is_recording if app.wav_recorder else False,
                "path": app.wav_recorder.current_path if app.wav_recorder else "",
                "seconds": round(app.wav_recorder.seconds_written, 1) if app.wav_recorder else 0,
                "dropped_frames": app.wav_recorder.dropped_frames if app.wav_recorder else 0,
                "source": app.record_source,
            },
            "ffmpeg_available": getattr(app, 'ffmpeg_available', False),
            "recordings_dir": os.path.join(
                app.config.project_dir,
                app.config.cfg.get("recording", {}).get("save_dir", "Recordings"),
            ),
            "last_recording_path": getattr(app, 'last_recording_path', None),
            "file_transcription": getattr(app, '_file_transcription', {
                "active": False, "status": "idle",
                "input_path": "", "output_path": "", "error": "",
                "progress": 0.0,
            }),
            "transcripts_dir": os.path.join(
                app.config.project_dir,
                app.config.cfg.get("transcription", {}).get("save_dir", "Transcriptions"),
            ),
        }
