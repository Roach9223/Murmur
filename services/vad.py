"""Silero VAD wrapper for real-time speech boundary detection."""

import logging

import numpy as np
import torch
from scipy.signal import resample_poly

from services.config import RECORD_RATE, WHISPER_RATE

logger = logging.getLogger(__name__)

RESAMPLE_DOWN = RECORD_RATE // WHISPER_RATE  # 48000 / 16000 = 3

# Silero VAD accepts these exact window sizes at 16kHz
VALID_WINDOW_SIZES = (512, 1024, 1536)  # 32ms, 64ms, 96ms


class VoiceActivityDetector:
    """Silero VAD service. Resamples 48kHz chunks to 16kHz and runs speech detection."""

    def __init__(self, threshold: float = 0.5,
                 min_silence_ms: int = 300,
                 speech_pad_ms: int = 30,
                 window_size: int = 512):
        if window_size not in VALID_WINDOW_SIZES:
            raise ValueError(f"window_size must be one of {VALID_WINDOW_SIZES}")

        self.threshold = threshold
        self.min_silence_ms = min_silence_ms
        self.speech_pad_ms = speech_pad_ms
        self.window_size = window_size
        self._model = None
        self._loaded = False

        # Accumulation buffer for sub-window-size chunks after resampling
        self._resample_buf = np.empty(0, dtype=np.float32)

    def load_model(self) -> bool:
        """Load Silero VAD model via torch.hub. Returns True on success."""
        try:
            self._model, _ = torch.hub.load(
                repo_or_dir="snakers4/silero-vad",
                model="silero_vad",
                force_reload=False,
                trust_repo=True,
            )
            self._loaded = True
            logger.info("[vad] Silero VAD model loaded (window=%d samples)", self.window_size)
            return True
        except Exception as e:
            logger.warning("[vad] Failed to load Silero VAD: %s", e)
            self._loaded = False
            return False

    @property
    def is_loaded(self) -> bool:
        return self._loaded

    def reset(self):
        """Reset model state and accumulation buffer. Call between recording sessions."""
        if self._model is not None:
            self._model.reset_states()
        self._resample_buf = np.empty(0, dtype=np.float32)

    def process_chunk(self, chunk_48k: np.ndarray) -> tuple[np.ndarray, list[tuple[float, bool]]]:
        """Resample a 48kHz chunk to 16kHz, run VAD, return (chunk_16k, vad_results).

        Single resample serves both accumulation and VAD inference.
        Returns empty list for vad_results if not enough samples accumulated yet.
        """
        mono = chunk_48k.squeeze()
        chunk_16k = resample_poly(mono, up=1, down=RESAMPLE_DOWN).astype(np.float32)

        # Accumulate for VAD window
        self._resample_buf = np.concatenate([self._resample_buf, chunk_16k])

        results = []
        while len(self._resample_buf) >= self.window_size:
            window = self._resample_buf[:self.window_size]
            self._resample_buf = self._resample_buf[self.window_size:]

            tensor = torch.from_numpy(window)
            prob = float(self._model(tensor, 16000).item())
            results.append((prob, prob >= self.threshold))

        return chunk_16k, results

    def configure(self, **kwargs):
        """Update VAD parameters at runtime."""
        if "threshold" in kwargs:
            val = float(kwargs["threshold"])
            if not 0.0 <= val <= 1.0:
                raise ValueError(f"threshold must be 0.0-1.0, got {val}")
            self.threshold = val
        if "min_silence_ms" in kwargs:
            val = int(kwargs["min_silence_ms"])
            if val < 0:
                raise ValueError(f"min_silence_ms must be >= 0, got {val}")
            self.min_silence_ms = val
        if "speech_pad_ms" in kwargs:
            val = int(kwargs["speech_pad_ms"])
            if val < 0:
                raise ValueError(f"speech_pad_ms must be >= 0, got {val}")
            self.speech_pad_ms = val
