import argparse
import json
import logging
import os

logger = logging.getLogger(__name__)


DEFAULTS = {
    "whisper_model": "large-v3",
    "mic_device_index": 0,
    "hotkey": "f1",
    "energy_threshold": 0.01,
    "silence_timeout": 1.5,
    "max_speech_seconds": 15,
    "llm_cleanup": True,
    "llm_model": "lmstudio-community/Qwen2.5-7B-Instruct-GGUF",
    "llm_system_prompt_file": "prompts/clean_system.txt",
    "llm_temperature": 0.1,
    "llm_max_tokens": 500,
    "llm_mode": "clean",
    "approval_mode": False,
    "push_to_talk": False,
    "command_prefix": "command",
    "voice_commands": {
        "new line": "enter",
        "newline": "enter",
        "send": "enter",
        "clear": "select_all_delete",
        "stop dictation": "stop",
        "stop dictating": "stop",
    },
}

# Hard-coded infrastructure (never in config)
LLM_URL = "http://localhost:1234/v1/chat/completions"
LLM_TIMEOUT = 10
RECORD_RATE = 48000
WHISPER_RATE = 16000
CHANNELS = 1
COMPUTE_TYPE = "float16"
DEBOUNCE_SEC = 0.5

DEFAULT_SYSTEM_PROMPT = (
    "You are a speech-to-text cleanup assistant. Clean up the transcription:\n"
    "- Remove filler words (um, uh, like, you know, so, basically, actually, right)\n"
    "- Fix grammar and punctuation\n"
    "- Keep the original meaning and tone intact\n"
    "- Output ONLY the cleaned text, nothing else\n"
    "- Do NOT include analysis, reasoning, steps, or commentary\n"
    "- Do NOT add labels, quotes, or markdown formatting\n"
    "- If the input is empty or only filler words, output an empty string\n"
    "/no_think"
)

VALID_LLM_MODES = ("raw", "clean", "prompt", "dev")

DEFAULT_LLM_MODES = {
    "raw": {"llm_cleanup": False},
    "clean": {
        "llm_cleanup": True,
        "llm_system_prompt_file": "prompts/clean_system.txt",
        "llm_temperature": 0.1,
        "llm_max_tokens": 500,
    },
    "prompt": {
        "llm_cleanup": True,
        "llm_system_prompt_file": "prompts/prompt_system.txt",
        "llm_temperature": 0.3,
        "llm_max_tokens": 1000,
    },
    "dev": {
        "llm_cleanup": True,
        "llm_system_prompt_file": "prompts/dev_system.txt",
        "llm_temperature": 0.2,
        "llm_max_tokens": 1000,
    },
}

DEFAULT_PROFILES = {
    "Default": {"llm_mode": "clean"},
}

DEFAULT_AUTO_DETECT = {
    "enabled": False,
    "poll_interval_ms": 500,
    "rules": [],
}

DEFAULT_DSP = {
    "noise_gate": {
        "enabled": True,
        "open_threshold_dbfs": -45.0,
        "close_threshold_dbfs": -50.0,
        "floor_db": -25.0,
        "hold_ms": 100.0,
        "attack_ms": 5.0,
        "release_ms": 150.0,
    },
    "compressor": {
        "enabled": False,
        "threshold_dbfs": -15.0,
        "ratio": 2.0,
        "attack_ms": 5.0,
        "release_ms": 100.0,
        "makeup_gain_db": 0.0,
    },
}


class ConfigManager:
    def __init__(self, project_dir: str):
        self.project_dir = project_dir
        self.cfg = dict(DEFAULTS)
        self._parse_cli_args()       # parse first to get --base-dir
        self._load_config_file()     # uses correct project_dir
        self._inject_defaults()
        self._apply_cli_overrides()  # CLI flags override config.json

    def _load_config_file(self):
        config_path = os.path.join(self.project_dir, "config.json")
        if os.path.exists(config_path):
            try:
                with open(config_path, "r", encoding="utf-8") as f:
                    user_cfg = json.load(f)
                self.cfg.update(user_cfg)
                logger.info("Loaded config.json")
            except Exception as e:
                logger.warning("Failed to load config.json: %s — using defaults", e)
        else:
            logger.info("No config.json found — using defaults")

    def _inject_defaults(self):
        """Ensure llm_modes, profiles, auto_detect, and dsp sections exist."""
        if "llm_modes" not in self.cfg:
            self.cfg["llm_modes"] = dict(DEFAULT_LLM_MODES)
        if "profiles" not in self.cfg:
            self.cfg["profiles"] = dict(DEFAULT_PROFILES)
        if "auto_detect" not in self.cfg:
            self.cfg["auto_detect"] = dict(DEFAULT_AUTO_DETECT)
        if "llm_mode" not in self.cfg:
            self.cfg["llm_mode"] = "clean"
        # DSP: inject full default or backfill missing sub-keys
        if "dsp" not in self.cfg:
            self.cfg["dsp"] = json.loads(json.dumps(DEFAULT_DSP))
        else:
            for section in ("noise_gate", "compressor"):
                if section not in self.cfg["dsp"]:
                    self.cfg["dsp"][section] = dict(DEFAULT_DSP[section])
                else:
                    for k, v in DEFAULT_DSP[section].items():
                        if k not in self.cfg["dsp"][section]:
                            self.cfg["dsp"][section][k] = v

    def _parse_cli_args(self):
        parser = argparse.ArgumentParser(description="Whisper streaming dictation")
        parser.add_argument("--no-cleanup", action="store_true",
                            help="Disable LLM cleanup (use raw Whisper output)")
        parser.add_argument("--server", action="store_true",
                            help="Enable HTTP API server for external control")
        parser.add_argument("--port", type=int, default=8899,
                            help="HTTP API server port (default: 8899)")
        parser.add_argument("--base-dir", type=str, default=None,
                            help="Base directory for config, prompts, models, logs (default: script dir)")
        self.args = parser.parse_args()

        # Override project_dir if --base-dir was provided
        if self.args.base_dir:
            self.project_dir = os.path.abspath(self.args.base_dir)

    def _apply_cli_overrides(self):
        """Apply CLI flags that override config.json values."""
        if self.args.no_cleanup:
            self.cfg["llm_cleanup"] = False
            self.cfg["llm_mode"] = "raw"

    def get(self, key: str):
        return self.cfg[key]

    def get_mode_names(self) -> list[str]:
        return list(self.cfg["llm_modes"].keys())

    def get_profile_names(self) -> list[str]:
        return list(self.cfg["profiles"].keys())

    def get_auto_detect_config(self) -> dict:
        return self.cfg["auto_detect"]

    def resolve_mode(self, mode_name: str) -> dict:
        """Merge global defaults with mode-specific config."""
        if mode_name not in self.cfg["llm_modes"]:
            mode_name = "clean"
        result = {
            "llm_cleanup": self.cfg.get("llm_cleanup", True),
            "llm_system_prompt_file": self.cfg.get("llm_system_prompt_file", "prompts/clean_system.txt"),
            "llm_temperature": self.cfg.get("llm_temperature", 0.1),
            "llm_max_tokens": self.cfg.get("llm_max_tokens", 500),
        }
        result.update(self.cfg["llm_modes"][mode_name])
        return result

    def resolve_profile(self, profile_name: str) -> dict:
        """Merge global defaults + mode config + profile overrides."""
        profiles = self.cfg["profiles"]
        if profile_name not in profiles:
            profile_name = next(iter(profiles))
        profile = profiles[profile_name]
        mode_name = profile.get("llm_mode", self.cfg.get("llm_mode", "clean"))
        result = dict(self.cfg)
        result.update(self.resolve_mode(mode_name))
        for k, v in profile.items():
            if k != "llm_mode":
                result[k] = v
        result["llm_mode"] = mode_name
        return result

    def load_system_prompt(self, prompt_file: str) -> str:
        """Load a system prompt file. Falls back to DEFAULT_SYSTEM_PROMPT."""
        path = os.path.join(self.project_dir, prompt_file)
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                return f.read().strip()
        return DEFAULT_SYSTEM_PROMPT
