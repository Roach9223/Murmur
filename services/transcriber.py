import logging
import os

import numpy as np

logger = logging.getLogger(__name__)


class TranscriptionEngine:
    def __init__(self, model_size: str, device: str = "cuda", compute_type: str = "float16",
                 model_dir: str | None = None):
        self._model_size = model_size
        self._device = device
        self._compute_type = compute_type
        self._model_dir = model_dir
        self._model = None

    def load_model(self):
        """Load the Whisper model. Call explicitly or it auto-loads on first transcribe()."""
        if self._model is not None:
            return
        from faster_whisper import WhisperModel
        if self._model_dir:
            os.makedirs(self._model_dir, exist_ok=True)
            os.environ["HF_HOME"] = self._model_dir
            logger.info("Model cache: %s", self._model_dir)
        logger.info("Loading Whisper model...")
        self._model = WhisperModel(self._model_size, device=self._device,
                                   compute_type=self._compute_type)
        logger.info("Model loaded.")

    @property
    def is_loaded(self) -> bool:
        return self._model is not None

    def transcribe(self, audio: np.ndarray) -> str:
        """Transcribe 16kHz float32 audio. Returns stripped text or empty string."""
        if self._model is None:
            self.load_model()
        segments, _info = self._model.transcribe(
            audio,
            language="en",
            vad_filter=True,
            repetition_penalty=1.2,
            no_repeat_ngram_size=3,
            condition_on_previous_text=False,
        )
        return "".join(seg.text for seg in segments).strip()

    def transcribe_file(self, file_path: str) -> str:
        """Transcribe an audio file from disk. Supports WAV, MP3, FLAC, M4A."""
        if self._model is None:
            self.load_model()
        segments, _info = self._model.transcribe(
            file_path,
            language="en",
            vad_filter=True,
            repetition_penalty=1.2,
            no_repeat_ngram_size=3,
            condition_on_previous_text=False,
        )
        return "".join(seg.text for seg in segments).strip()

    def transcribe_file_with_progress(self, file_path: str, progress_callback=None) -> str:
        """Transcribe an audio file, reporting progress via callback."""
        if self._model is None:
            self.load_model()
        segments, info = self._model.transcribe(
            file_path,
            language="en",
            vad_filter=True,
            repetition_penalty=1.2,
            no_repeat_ngram_size=3,
            condition_on_previous_text=False,
        )
        total_duration = info.duration if info.duration and info.duration > 0 else 1.0
        parts = []
        for seg in segments:
            parts.append(seg.text)
            if progress_callback and hasattr(seg, 'end'):
                pct = min(seg.end / total_duration * 100.0, 100.0)
                progress_callback(pct)
        return "".join(parts).strip()
