import ctypes
import ctypes.wintypes
import logging
import re
import threading
import time

logger = logging.getLogger(__name__)


class ActiveWindowDetector:
    """Polls the foreground window title and fires a callback when it matches a profile rule."""

    def __init__(self, rules: list[dict], on_match: callable, poll_interval_ms: int = 500):
        self._rules = [
            (re.compile(r["window_pattern"], re.IGNORECASE), r["profile"])
            for r in rules
        ]
        self._on_match = on_match
        self._poll_interval = poll_interval_ms / 1000.0
        self._last_profile: str | None = None
        self._running = False
        self._thread: threading.Thread | None = None

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._poll_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False

    def _poll_loop(self):
        while self._running:
            try:
                title = self._get_foreground_title()
                matched = self._match_title(title)
                if matched and matched != self._last_profile:
                    self._last_profile = matched
                    self._on_match(matched)
            except Exception as e:
                logger.debug("Window detect poll error: %s", e)
            time.sleep(self._poll_interval)

    def _match_title(self, title: str) -> str | None:
        if not title:
            return None
        for pattern, profile in self._rules:
            if pattern.search(title):
                return profile
        return None

    @staticmethod
    def _get_foreground_title() -> str:
        hwnd = ctypes.windll.user32.GetForegroundWindow()
        length = ctypes.windll.user32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return ""
        buf = ctypes.create_unicode_buffer(length + 1)
        ctypes.windll.user32.GetWindowTextW(hwnd, buf, length + 1)
        return buf.value
