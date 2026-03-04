"""WavRecorder — non-blocking WAV file writer for mic audio.

Runs a dedicated writer thread that drains a queue of float32 audio blocks
and writes int16 PCM to a WAV file.  The push() method is designed to be
called from the WASAPI audio callback without ever blocking.
"""

import logging
import os
import queue
import struct
import threading
import time
import wave

import numpy as np

logger = logging.getLogger(__name__)

_SENTINEL = None  # poison pill to stop writer thread


class WavRecorder:
    def __init__(self, sample_rate: int = 48000, channels: int = 1,
                 queue_maxsize: int = 200):
        self._sample_rate = sample_rate
        self._channels = channels
        self._queue: queue.Queue = queue.Queue(maxsize=queue_maxsize)
        self._writer_thread: threading.Thread | None = None
        self._wav_file: wave.Wave_write | None = None
        self._is_recording = False
        self._samples_written = 0
        self._dropped_frames = 0
        self._current_path: str | None = None

    # --- Public API ---

    def start(self, path: str) -> None:
        """Open a WAV file and start the writer thread."""
        if self._is_recording:
            logger.warning("[rec] Already recording, ignoring start()")
            return

        os.makedirs(os.path.dirname(path), exist_ok=True)

        self._wav_file = wave.open(path, "wb")
        self._wav_file.setnchannels(self._channels)
        self._wav_file.setsampwidth(2)  # int16
        self._wav_file.setframerate(self._sample_rate)

        self._current_path = path
        self._samples_written = 0
        self._dropped_frames = 0

        # Drain any stale data
        while not self._queue.empty():
            try:
                self._queue.get_nowait()
            except queue.Empty:
                break

        self._is_recording = True
        self._writer_thread = threading.Thread(
            target=self._writer_loop, daemon=True, name="wav-writer")
        self._writer_thread.start()
        logger.info("[rec] Started recording to %s", path)

    def stop(self) -> dict:
        """Stop recording, finalize WAV, return summary."""
        if not self._is_recording:
            return {"path": None, "seconds": 0, "dropped_frames": 0}

        self._is_recording = False
        # Send sentinel to unblock writer
        try:
            self._queue.put(_SENTINEL, timeout=2.0)
        except queue.Full:
            pass

        if self._writer_thread is not None:
            self._writer_thread.join(timeout=3.0)
            self._writer_thread = None

        if self._wav_file is not None:
            try:
                self._wav_file.close()
            except Exception as e:
                logger.error("[rec] Error closing WAV: %s", e)
            self._wav_file = None

        seconds = self._samples_written / self._sample_rate if self._sample_rate else 0
        path = self._current_path
        dropped = self._dropped_frames
        logger.info("[rec] Stopped recording: %.1fs, %d dropped frames, %s",
                    seconds, dropped, path)
        return {"path": path, "seconds": round(seconds, 2),
                "dropped_frames": dropped}

    def push(self, block: np.ndarray) -> None:
        """Non-blocking enqueue from audio callback. Drops on full."""
        if not self._is_recording:
            return
        try:
            self._queue.put_nowait(block.copy())
        except queue.Full:
            self._dropped_frames += 1

    # --- Properties ---

    @property
    def is_recording(self) -> bool:
        return self._is_recording

    @property
    def seconds_written(self) -> float:
        return self._samples_written / self._sample_rate if self._sample_rate else 0.0

    @property
    def dropped_frames(self) -> int:
        return self._dropped_frames

    @property
    def current_path(self) -> str | None:
        return self._current_path

    # --- Writer thread ---

    def _writer_loop(self):
        """Drain queue and write int16 PCM to the open WAV file."""
        while True:
            try:
                block = self._queue.get(timeout=0.5)
            except queue.Empty:
                if not self._is_recording:
                    break
                continue

            if block is _SENTINEL:
                break

            try:
                # float32 [-1,1] -> int16
                clipped = np.clip(block, -1.0, 1.0)
                pcm = (clipped * 32767).astype(np.int16)
                raw = pcm.tobytes()
                self._wav_file.writeframes(raw)
                self._samples_written += len(pcm)
            except Exception as e:
                logger.error("[rec] Write error: %s", e)
                break

        # Drain remaining
        while not self._queue.empty():
            try:
                block = self._queue.get_nowait()
                if block is _SENTINEL:
                    continue
                clipped = np.clip(block, -1.0, 1.0)
                pcm = (clipped * 32767).astype(np.int16)
                self._wav_file.writeframes(pcm.tobytes())
                self._samples_written += len(pcm)
            except (queue.Empty, Exception):
                break
