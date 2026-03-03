import logging
import re

import requests

logger = logging.getLogger(__name__)


class LLMEnhancer:
    def __init__(self, url: str, model: str, system_prompt: str,
                 temperature: float, max_tokens: int, timeout: int):
        self.url = url
        self.model = model
        self.system_prompt = system_prompt
        self.temperature = temperature
        self.max_tokens = max_tokens
        self.timeout = timeout

    def configure(self, system_prompt: str, temperature: float, max_tokens: int):
        """Swap LLM parameters at runtime for mode/profile switching."""
        self.system_prompt = system_prompt
        self.temperature = temperature
        self.max_tokens = max_tokens

    def cleanup(self, text: str) -> str:
        """Send text to LM Studio for cleanup. Falls back to raw text on error."""
        if not text:
            return text
        try:
            resp = requests.post(self.url, json={
                "model": self.model,
                "messages": [
                    {"role": "system", "content": self.system_prompt},
                    {"role": "user", "content": text},
                ],
                "temperature": self.temperature,
                "max_tokens": self.max_tokens,
            }, timeout=self.timeout)
            resp.raise_for_status()
            content = resp.json()["choices"][0]["message"]["content"].strip()
            content = self._strip_reasoning(content)

            # Sanity: cleanup should never make text much longer.
            # If the LLM spewed reasoning, fall back to raw text.
            if len(content) > len(text) * 2 + 50:
                logger.warning("  [LLM output too long (%d vs %d) — using raw text]",
                               len(content), len(text))
                return text

            if not content:
                logger.warning("  [LLM] empty after reasoning strip — using raw text")
                return text
            return content
        except Exception as e:
            logger.warning("  [LLM error: %s — using raw text]", e)
            return text

    @staticmethod
    def _strip_reasoning(content: str) -> str:
        """Remove thinking/reasoning artifacts from LLM output."""
        # Strip <think>...</think> blocks (non-greedy)
        content = re.sub(r"<think>[\s\S]*?</think>", "", content).strip()
        # Strip plain-text reasoning blocks before the actual output.
        # Match "Thinking Process:" (or similar) up to a blank line, keep the rest.
        content = re.sub(
            r"(?i)^(?:thinking process|analysis|reasoning|step[s]?\s*\d*)\s*:.*?(?:\n\n)",
            "", content, flags=re.DOTALL
        ).strip()
        # If it still starts with a reasoning header (no blank-line separator),
        # drop lines until we find one that doesn't look like reasoning.
        if re.match(r"(?i)^(?:thinking process|analysis|reasoning|step)", content):
            lines = content.split("\n")
            for i, line in enumerate(lines):
                stripped = line.strip()
                if stripped and not re.match(
                    r"(?i)^(?:\d+[\.\):]|[-*]|thinking|analysis|reasoning|step|role:|task:)",
                    stripped
                ):
                    content = "\n".join(lines[i:]).strip()
                    break
            else:
                content = ""
        return content

    def is_available(self) -> bool:
        """Quick check if LM Studio is reachable."""
        try:
            resp = requests.get(
                self.url.replace("/chat/completions", "/models"),
                timeout=2,
            )
            return resp.ok
        except Exception:
            return False
