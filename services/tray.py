import threading

from PIL import Image, ImageDraw
from pystray import Icon, Menu, MenuItem


class TrayService:
    """System tray icon for dictation state and control."""

    _COLOR_IDLE = "#6B7280"
    _COLOR_RECORDING = "#22C55E"
    _ICON_SIZE = 64

    def __init__(
        self,
        toggle_callback,
        is_recording_callback,
        quit_callback,
        hotkey_label: str = "F1",
        mode_names: list[str] | None = None,
        profile_names: list[str] | None = None,
        current_mode: str = "clean",
        current_profile: str = "Default",
        on_mode_changed=None,
        on_profile_changed=None,
        auto_detect_enabled: bool = False,
        on_auto_detect_toggled=None,
        approval_mode: bool = False,
        push_to_talk: bool = False,
        on_approval_mode_toggled=None,
        on_push_to_talk_toggled=None,
    ):
        self._toggle = toggle_callback
        self._is_recording = is_recording_callback
        self._quit = quit_callback
        self._hotkey_label = hotkey_label.upper()

        self._mode_names = mode_names or ["raw", "clean", "prompt", "dev"]
        self._profile_names = profile_names or ["Default"]
        self._current_mode = current_mode
        self._current_profile = current_profile
        self._on_mode_changed = on_mode_changed
        self._on_profile_changed = on_profile_changed
        self._auto_detect_enabled = auto_detect_enabled
        self._on_auto_detect_toggled = on_auto_detect_toggled
        self._approval_mode = approval_mode
        self._push_to_talk = push_to_talk
        self._on_approval_mode_toggled = on_approval_mode_toggled
        self._on_push_to_talk_toggled = on_push_to_talk_toggled

        self._icon_idle = self._create_icon(self._COLOR_IDLE)
        self._icon_recording = self._create_icon(self._COLOR_RECORDING)

        self._tray_icon: Icon | None = None
        self._thread: threading.Thread | None = None

    # --- Public API ---

    def start(self):
        """Start the tray icon in a daemon thread."""
        self._tray_icon = Icon(
            name="whisper-dictation",
            icon=self._icon_idle,
            title=self._make_tooltip(),
            menu=self._build_menu(),
        )
        self._thread = threading.Thread(
            target=self._tray_icon.run,
            daemon=True,
        )
        self._thread.start()

    def stop(self):
        """Stop the tray icon."""
        if self._tray_icon:
            self._tray_icon.stop()

    def on_state_changed(self):
        """Update icon and tooltip to reflect current recording state."""
        if not self._tray_icon:
            return
        recording = self._is_recording()
        self._tray_icon.icon = (
            self._icon_recording if recording else self._icon_idle
        )
        self._tray_icon.title = self._make_tooltip()
        self._tray_icon.update_menu()

    def set_mode(self, mode_name: str):
        """Update displayed mode."""
        self._current_mode = mode_name
        if self._tray_icon:
            self._tray_icon.update_menu()

    def set_profile(self, profile_name: str):
        """Update displayed profile."""
        self._current_profile = profile_name
        if self._tray_icon:
            self._tray_icon.update_menu()

    def set_auto_detect(self, enabled: bool):
        """Update auto-detect toggle state."""
        self._auto_detect_enabled = enabled
        if self._tray_icon:
            self._tray_icon.update_menu()

    def set_approval_mode(self, enabled: bool):
        """Update approval mode toggle state."""
        self._approval_mode = enabled
        if self._tray_icon:
            self._tray_icon.update_menu()

    def set_push_to_talk(self, enabled: bool):
        """Update push-to-talk toggle state."""
        self._push_to_talk = enabled
        if self._tray_icon:
            self._tray_icon.update_menu()

    def set_hotkey_label(self, key: str):
        """Update the displayed hotkey label."""
        self._hotkey_label = key.upper()
        if self._tray_icon:
            self._tray_icon.update_menu()

    # --- Menu callbacks ---

    def _on_toggle_clicked(self, icon, item):
        self._toggle()

    def _on_quit_clicked(self, icon, item):
        self._quit()

    def _on_auto_detect_clicked(self, icon, item):
        if self._on_auto_detect_toggled:
            self._on_auto_detect_toggled(not self._auto_detect_enabled)

    def _on_approval_mode_clicked(self, icon, item):
        if self._on_approval_mode_toggled:
            self._on_approval_mode_toggled(not self._approval_mode)

    def _on_push_to_talk_clicked(self, icon, item):
        if self._on_push_to_talk_toggled:
            self._on_push_to_talk_toggled(not self._push_to_talk)

    # --- Menu builder ---

    def _build_menu(self) -> Menu:
        mode_items = [
            MenuItem(
                name.capitalize(),
                self._make_mode_callback(name),
                checked=self._make_mode_check(name),
                radio=True,
            )
            for name in self._mode_names
        ]
        profile_items = [
            MenuItem(
                name,
                self._make_profile_callback(name),
                checked=self._make_profile_check(name),
                radio=True,
            )
            for name in self._profile_names
        ]
        return Menu(
            MenuItem(
                self._dictation_label,
                self._on_toggle_clicked,
                checked=lambda item: self._is_recording(),
                default=True,
            ),
            Menu.SEPARATOR,
            MenuItem("Mode", Menu(*mode_items)),
            MenuItem("Profile", Menu(*profile_items)),
            MenuItem(
                "Auto-detect window",
                self._on_auto_detect_clicked,
                checked=lambda item: self._auto_detect_enabled,
            ),
            Menu.SEPARATOR,
            MenuItem(
                "Approval mode",
                self._on_approval_mode_clicked,
                checked=lambda item: self._approval_mode,
            ),
            MenuItem(
                "Push-to-talk",
                self._on_push_to_talk_clicked,
                checked=lambda item: self._push_to_talk,
            ),
            Menu.SEPARATOR,
            MenuItem("Quit", self._on_quit_clicked),
        )

    def _make_mode_callback(self, mode_name: str):
        def on_click(icon, item):
            if self._on_mode_changed:
                self._on_mode_changed(mode_name)
        return on_click

    def _make_mode_check(self, mode_name: str):
        def check(item):
            return self._current_mode == mode_name
        return check

    def _make_profile_callback(self, profile_name: str):
        def on_click(icon, item):
            if self._on_profile_changed:
                self._on_profile_changed(profile_name)
        return on_click

    def _make_profile_check(self, profile_name: str):
        def check(item):
            return self._current_profile == profile_name
        return check

    # --- Helpers ---

    def _dictation_label(self, item) -> str:
        state = "ON" if self._is_recording() else "OFF"
        return f"Dictation {state}  ({self._hotkey_label})"

    def _make_tooltip(self) -> str:
        if self._is_recording():
            return f"Whisper Dictation - Recording... [{self._current_mode}]"
        return f"Whisper Dictation - {self._current_profile} [{self._current_mode}]"

    @classmethod
    def _create_icon(cls, color: str) -> Image.Image:
        size = cls._ICON_SIZE
        image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        dc = ImageDraw.Draw(image)
        margin = 4
        dc.ellipse(
            [margin, margin, size - margin, size - margin],
            fill=color,
        )
        return image
