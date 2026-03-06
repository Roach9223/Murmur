import logging
import re

import requests

logger = logging.getLogger(__name__)


class LLMBackend:
    """Base class for LLM backends."""

    def complete(self, system_prompt: str, user_text: str,
                 model: str, temperature: float, max_tokens: int,
                 timeout: int) -> str | None:
        raise NotImplementedError

    def is_available(self) -> bool:
        raise NotImplementedError

    def resolve_model(self, fallback: str) -> str | None:
        raise NotImplementedError

    def close(self):
        pass


class LMStudioBackend(LLMBackend):
    """OpenAI-compatible /v1/chat/completions backend (LM Studio, etc.)."""

    def __init__(self, url: str = "http://localhost:1234/v1/chat/completions",
                 cache_prompt: bool = True):
        self.url = url
        self.cache_prompt = cache_prompt
        self._session = requests.Session()

    def complete(self, system_prompt: str, user_text: str,
                 model: str, temperature: float, max_tokens: int,
                 timeout: int) -> str | None:
        payload = {
            "model": model,
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_text},
            ],
            "temperature": temperature,
            "max_tokens": max_tokens,
            "cache_prompt": self.cache_prompt,
        }
        resp = self._session.post(self.url, json=payload, timeout=timeout)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"].strip()

    def is_available(self) -> bool:
        try:
            resp = self._session.get(
                self.url.replace("/chat/completions", "/models"),
                timeout=2,
            )
            return resp.ok
        except Exception:
            return False

    def resolve_model(self, fallback: str) -> str | None:
        try:
            resp = self._session.get(
                self.url.replace("/chat/completions", "/models"),
                timeout=3,
            )
            resp.raise_for_status()
            models = resp.json().get("data", [])
            if models:
                return models[0]["id"]
        except Exception as e:
            logger.debug("[LLM] Could not auto-detect model: %s", e)
        return None

    def close(self):
        self._session.close()


class LlamaCppBackend(LLMBackend):
    """Native llama.cpp /completion endpoint backend."""

    def __init__(self, base_url: str = "http://localhost:8080",
                 cache_prompt: bool = True,
                 chat_template: str = "chatml"):
        self.base_url = base_url.rstrip("/")
        self.cache_prompt = cache_prompt
        self.chat_template = chat_template
        self._session = requests.Session()

    def _format_prompt(self, system_prompt: str, user_text: str) -> str:
        return (
            f"<|im_start|>system\n{system_prompt}<|im_end|>\n"
            f"<|im_start|>user\n{user_text}<|im_end|>\n"
            f"<|im_start|>assistant\n"
        )

    def complete(self, system_prompt: str, user_text: str,
                 model: str, temperature: float, max_tokens: int,
                 timeout: int) -> str | None:
        payload = {
            "prompt": self._format_prompt(system_prompt, user_text),
            "temperature": temperature,
            "n_predict": max_tokens,
            "cache_prompt": self.cache_prompt,
            "stop": ["<|im_end|>", "<|endoftext|>"],
        }
        resp = self._session.post(
            f"{self.base_url}/completion", json=payload, timeout=timeout,
        )
        resp.raise_for_status()
        return resp.json().get("content", "").strip()

    def is_available(self) -> bool:
        try:
            resp = self._session.get(f"{self.base_url}/health", timeout=2)
            return resp.ok
        except Exception:
            return False

    def resolve_model(self, fallback: str) -> str | None:
        # llama.cpp loads a single model, no resolution needed
        return None

    def close(self):
        self._session.close()


class LLMEnhancer:
    def __init__(self, model: str, system_prompt: str,
                 temperature: float, max_tokens: int, timeout: int,
                 backend: LLMBackend | None = None):
        self._config_model = model
        self._resolved_model = None
        self.system_prompt = system_prompt
        self.temperature = temperature
        self.max_tokens = max_tokens
        self.timeout = timeout
        self.backend = backend or LMStudioBackend()

    @property
    def model(self) -> str:
        """Return the resolved model ID, falling back to config value."""
        return self._resolved_model or self._config_model

    def _resolve_model(self):
        """Query the backend for the loaded model ID and cache it."""
        model_id = self.backend.resolve_model(self._config_model)
        if model_id:
            if model_id != self._resolved_model:
                logger.info("[LLM] Auto-detected model: %s", model_id)
            self._resolved_model = model_id
        elif not self._resolved_model:
            self._resolved_model = None

    def set_backend(self, backend: LLMBackend):
        """Swap to a different backend at runtime."""
        old = self.backend
        self.backend = backend
        self._resolved_model = None
        old.close()

    def configure(self, system_prompt: str, temperature: float, max_tokens: int):
        """Swap LLM parameters at runtime for mode/profile switching."""
        self.system_prompt = system_prompt
        self.temperature = temperature
        self.max_tokens = max_tokens

    def cleanup(self, text: str) -> str:
        """Send text to the LLM backend for cleanup. Falls back to raw text on error."""
        if not text:
            return text
        if not self._resolved_model:
            self._resolve_model()
        try:
            content = self.backend.complete(
                system_prompt=self.system_prompt,
                user_text=text,
                model=self.model,
                temperature=self.temperature,
                max_tokens=self.max_tokens,
                timeout=self.timeout,
            )
            if content:
                content = self._strip_reasoning(content)
            else:
                logger.warning("  [LLM] empty response — using raw text")
                return text

            # Tighter hallucination guard
            if len(content) > len(text) * 1.5 + 30:
                logger.warning("  [LLM output too long (%d vs %d) — using raw text]",
                               len(content), len(text))
                return text

            if not content:
                logger.warning("  [LLM] empty after reasoning strip — using raw text")
                return text
            return content
        except requests.exceptions.HTTPError as e:
            if e.response is not None and e.response.status_code == 400:
                logger.warning("  [LLM 400 error — re-querying model list]")
                self._resolved_model = None
            logger.warning("  [LLM error: %s — using raw text]", e)
            return text
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
        """Quick check if the LLM backend is reachable."""
        return self.backend.is_available()
