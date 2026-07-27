"""Configuration manager: loads, saves, and validates user config."""
from __future__ import annotations

import json
import os
from copy import deepcopy
from pathlib import Path
from typing import Any, Dict

DEFAULT_CONFIG_FILENAME = "config.default.json"
USER_CONFIG_FILENAME = "config.json"


class ConfigManager:
    def __init__(self, app_dir: Path):
        self.app_dir = Path(app_dir)
        self.default_path = self.app_dir / DEFAULT_CONFIG_FILENAME
        self.user_path = self.app_dir / USER_CONFIG_FILENAME
        self._config: Dict[str, Any] = {}
        self.load()

    def load(self) -> Dict[str, Any]:
        with open(self.default_path, "r", encoding="utf-8") as f:
            base = json.load(f)
        if self.user_path.exists():
            try:
                with open(self.user_path, "r", encoding="utf-8") as f:
                    user = json.load(f)
                # User file is source of truth once it exists (so deletions stick).
                # Only fall back to defaults for top-level keys the user file
                # doesn't have yet (helps when we ship new config keys).
                for k, v in base.items():
                    user.setdefault(k, v)
                self._config = user
                return self._config
            except (json.JSONDecodeError, OSError):
                # corrupt user file → keep defaults
                pass
        self._config = base
        return self._config

    def save(self) -> None:
        with open(self.user_path, "w", encoding="utf-8") as f:
            json.dump(self._config, f, indent=2)

    def reset(self) -> Dict[str, Any]:
        if self.user_path.exists():
            os.remove(self.user_path)
        return self.load()

    @property
    def data(self) -> Dict[str, Any]:
        return self._config

    def get(self, key: str, default: Any = None) -> Any:
        return self._config.get(key, default)

    def set(self, key: str, value: Any) -> None:
        self._config[key] = value

    def set_binding(self, mode: str, gesture: str, action: str) -> None:
        mode_cfg = self._config["modes"].setdefault(mode, {"enabled": True, "bindings": {}})
        mode_cfg.setdefault("bindings", {})[gesture] = action

    def remove_binding(self, mode: str, gesture: str) -> None:
        try:
            del self._config["modes"][mode]["bindings"][gesture]
        except KeyError:
            pass

    @staticmethod
    def _deep_merge(base: Dict[str, Any], overlay: Dict[str, Any]) -> Dict[str, Any]:
        out = deepcopy(base)
        for k, v in overlay.items():
            if k in out and isinstance(out[k], dict) and isinstance(v, dict):
                out[k] = ConfigManager._deep_merge(out[k], v)
            else:
                out[k] = deepcopy(v)
        return out
