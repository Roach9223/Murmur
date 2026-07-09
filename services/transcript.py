"""TranscriptLogger — append a running conversation transcript to a file.

Used by "conversation mode" (system-audio loopback): each finalized transcript
line is appended with a timestamp to logs/conversations/conversation-<ts>.md.
Writes happen from the transcription thread (not the realtime audio callback), so
plain buffered file I/O is fine. This is a data artifact, not application logging.
"""

import datetime
import logging
import os
import threading

logger = logging.getLogger(__name__)


class TranscriptLogger:
    def __init__(self, base_dir: str):
        self._dir = os.path.join(base_dir, "logs", "conversations")
        self._file = None
        self._path: str | None = None
        self._lock = threading.Lock()

    @property
    def active(self) -> bool:
        return self._file is not None

    @property
    def current_path(self) -> str | None:
        return self._path

    def start_session(self) -> str | None:
        """Open a new timestamped transcript file. Idempotent-ish: closes any
        prior session first. Returns the path, or None on failure."""
        with self._lock:
            self._close_locked()
            try:
                os.makedirs(self._dir, exist_ok=True)
                stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
                self._path = os.path.join(self._dir, f"conversation-{stamp}.md")
                self._file = open(self._path, "a", encoding="utf-8")
                header = datetime.datetime.now().strftime("# Conversation — %Y-%m-%d %H:%M:%S\n\n")
                self._file.write(header)
                self._file.flush()
                logger.info("[transcript] Logging conversation to %s", self._path)
                return self._path
            except Exception as e:
                logger.warning("[transcript] Failed to start session: %s", e)
                self._file = None
                self._path = None
                return None

    def append(self, text: str):
        """Append one timestamped transcript line. No-op if no session is active."""
        text = (text or "").strip()
        if not text:
            return
        with self._lock:
            if self._file is None:
                return
            try:
                ts = datetime.datetime.now().strftime("%H:%M:%S")
                self._file.write(f"{ts} — {text}\n")
                self._file.flush()
            except Exception as e:
                logger.warning("[transcript] Write error: %s", e)

    def stop_session(self):
        """Close the current transcript file. Safe to call when inactive."""
        with self._lock:
            self._close_locked()

    def _close_locked(self):
        if self._file is not None:
            try:
                self._file.close()
            except Exception:
                pass
            logger.info("[transcript] Closed conversation log %s", self._path)
            self._file = None
