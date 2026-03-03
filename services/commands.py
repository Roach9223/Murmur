import time

from services.output import OutputInjector


class CommandRouter:
    def __init__(self, voice_commands_cfg: dict, output: OutputInjector,
                 prefix: str = "command"):
        self.output = output
        self.prefix = prefix.lower().strip()
        self.commands: dict[str, str] = {}
        self.stop_phrases: set[str] = set()
        self.update_commands(voice_commands_cfg)

    def update_commands(self, voice_commands_cfg: dict):
        """Replace the active voice command set at runtime."""
        self.commands = {}
        self.stop_phrases = set()
        for phrase, action_name in voice_commands_cfg.items():
            lower = phrase.lower()
            if action_name == "stop":
                self.stop_phrases.add(lower)
            else:
                self.commands[lower] = action_name

    def check(self, text: str) -> tuple[bool, str | None]:
        """Check if text is a voice command. Returns (is_command, action_name_or_None).

        When a prefix is configured (default: "command"), the spoken text must
        start with the prefix word followed by the command phrase.
        E.g. "command new line" triggers Enter, but "new line" alone is typed as text.
        """
        normalized = text.lower().strip().rstrip(".!?,")

        # Strip required prefix (e.g. "command new line" → "new line")
        if self.prefix:
            expected = self.prefix + " "
            if not normalized.startswith(expected):
                return False, None
            normalized = normalized[len(expected):].strip()

        if normalized in self.stop_phrases:
            return True, "stop"
        if normalized in self.commands:
            return True, self.commands[normalized]
        return False, None

    def execute(self, action_name: str):
        """Execute a command action."""
        if action_name == "enter":
            self.output.press_key("enter")
        elif action_name == "ctrl_enter":
            self.output.press_key("ctrl+enter")
        elif action_name == "select_all_delete":
            self.output.press_key("ctrl+a")
            time.sleep(0.05)
            self.output.press_key("delete")
        elif action_name == "shift_enter":
            self.output.press_key("shift+enter")
        elif action_name == "copy":
            self.output.press_key("ctrl+c")
        elif action_name == "paste":
            self.output.press_key("ctrl+v")
