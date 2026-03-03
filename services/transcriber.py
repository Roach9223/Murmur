import logging
import os

import numpy as np
from faster_whisper import WhisperModel

logger = logging.getLogger(__name__)


class TranscriptionEngine:
    def __init__(self, model_size: str, device: str = "cuda", compute_type: str = "float16",
                 model_dir: str | None = None):
        if model_dir:
            os.makedirs(model_dir, exist_ok=True)
            os.environ["HF_HOME"] = model_dir
            logger.info("Model cache: %s", model_dir)
        logger.info("Loading Whisper model...")
        self.model = WhisperModel(model_size, device=device, compute_type=compute_type)
        logger.info("Model loaded.")

    def transcribe(self, audio: np.ndarray) -> str:
        """Transcribe 16kHz float32 audio. Returns stripped text or empty string."""
        segments, _info = self.model.transcribe(
            audio,
            language="en",
            vad_filter=True,
            repetition_penalty=1.2,
            no_repeat_ngram_size=3,
            condition_on_previous_text=False,
        )
        return "".join(seg.text for seg in segments).strip()
