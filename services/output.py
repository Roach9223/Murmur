import keyboard


class OutputInjector:
    def type_text(self, text: str, newline_key: str = "enter"):
        """Type text via keyboard.write() — SendInput with KEYEVENTF_UNICODE.

        If newline_key is not 'enter', newlines in the text are replaced with
        the specified key combo (e.g. 'shift+enter' for chat interfaces where
        Enter submits the message).
        """
        if newline_key != "enter" and "\n" in text:
            parts = text.split("\n")
            for i, part in enumerate(parts):
                if part:
                    keyboard.write(part)
                if i < len(parts) - 1:
                    keyboard.press_and_release(newline_key)
        else:
            keyboard.write(text)

    def press_key(self, combo: str):
        """Press a key combo (e.g. 'enter', 'ctrl+a', 'delete')."""
        keyboard.press_and_release(combo)
