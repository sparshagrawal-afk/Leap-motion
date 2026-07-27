"""Action executor: performs system actions (mouse, keyboard, media) for a gesture.

The executor is decoupled from the recognizer. Bindings are strings of the form:
    "media_play_pause"      -> media control
    "media_next" / "media_previous"
    "volume_up" / "volume_down" / "volume_mute"
    "key:<name>"            -> single key press, e.g. "key:right"
    "hotkey:ctrl+shift+t"   -> combo
    "mouse:left_click" / "mouse:right_click" / "mouse:double_click"
    "text:<string>"         -> types the given text
"""
from __future__ import annotations

import logging
import platform
from typing import Callable, Dict, Optional

log = logging.getLogger("gesture.actions")


class ActionExecutor:
    def __init__(self, dry_run: bool = False):
        self.dry_run = dry_run
        self._pyautogui = None
        self._keyboard = None
        self._mouse = None
        if not dry_run:
            self._lazy_import_backends()
        self._handlers: Dict[str, Callable[[str], None]] = {
            "media_play_pause": lambda _: self._media("playpause"),
            "media_next":       lambda _: self._media("nexttrack"),
            "media_previous":   lambda _: self._media("prevtrack"),
            "volume_up":        lambda _: self._media("volumeup"),
            "volume_down":      lambda _: self._media("volumedown"),
            "volume_mute":      lambda _: self._media("volumemute"),
        }
        self._last_action: Optional[str] = None

    # ---------- backends ----------
    def _lazy_import_backends(self) -> None:
        try:
            import pyautogui  # type: ignore
            pyautogui.FAILSAFE = False
            self._pyautogui = pyautogui
        except Exception as e:  # noqa: BLE001
            log.warning("pyautogui unavailable: %s", e)
        try:
            from pynput.keyboard import Controller as KbCtl  # type: ignore
            from pynput.mouse import Controller as MsCtl, Button  # type: ignore
            self._keyboard = KbCtl()
            self._mouse = MsCtl()
            self._mouse_button = Button
        except Exception as e:  # noqa: BLE001
            log.warning("pynput unavailable: %s", e)

    # ---------- public API ----------
    @property
    def last_action(self) -> Optional[str]:
        return self._last_action

    def execute(self, binding: str) -> bool:
        """Execute a binding string. Returns True on success."""
        if not binding:
            return False
        self._last_action = binding
        log.info("execute action: %s", binding)
        if self.dry_run:
            return True
        try:
            if binding in self._handlers:
                self._handlers[binding](binding)
                return True
            if binding.startswith("key:"):
                return self._press_key(binding[4:].strip())
            if binding.startswith("hotkey:"):
                return self._hotkey(binding[7:].strip())
            if binding.startswith("mouse:"):
                return self._mouse_action(binding[6:].strip())
            if binding.startswith("text:"):
                return self._type_text(binding[5:])
        except Exception as e:  # noqa: BLE001
            log.error("action failed: %s -> %s", binding, e)
        return False

    def move_cursor(self, x: int, y: int) -> None:
        if self.dry_run or self._pyautogui is None:
            return
        try:
            self._pyautogui.moveTo(int(x), int(y), duration=0)
        except Exception as e:  # noqa: BLE001
            log.debug("move_cursor failed: %s", e)

    def scroll(self, dy: int) -> None:
        if self.dry_run or self._pyautogui is None:
            return
        try:
            self._pyautogui.scroll(int(dy))
        except Exception as e:  # noqa: BLE001
            log.debug("scroll failed: %s", e)

    # ---------- internals ----------
    def _press_key(self, key: str) -> bool:
        if self._pyautogui is None:
            return False
        self._pyautogui.press(key)
        return True

    def _hotkey(self, combo: str) -> bool:
        if self._pyautogui is None:
            return False
        parts = [p.strip() for p in combo.replace("-", "+").split("+") if p.strip()]
        if not parts:
            return False
        self._pyautogui.hotkey(*parts)
        return True

    def _type_text(self, text: str) -> bool:
        if self._pyautogui is None:
            return False
        self._pyautogui.typewrite(text, interval=0.01)
        return True

    def _mouse_action(self, name: str) -> bool:
        if self._pyautogui is None:
            return False
        if name == "left_click":
            self._pyautogui.click()
        elif name == "right_click":
            self._pyautogui.rightClick()
        elif name == "double_click":
            self._pyautogui.doubleClick()
        else:
            return False
        return True

    def _media(self, code: str) -> None:
        """Media keys work cross-platform via pyautogui on Windows/macOS.
        On Linux, we fall back to `playerctl` via hotkeys if available.
        """
        system = platform.system()
        mapping = {
            "playpause":  "playpause",
            "nexttrack":  "nexttrack",
            "prevtrack":  "prevtrack",
            "volumeup":   "volumeup",
            "volumedown": "volumedown",
            "volumemute": "volumemute",
        }
        key = mapping.get(code)
        if not key or self._pyautogui is None:
            return
        if system in ("Windows", "Darwin"):
            self._pyautogui.press(key)
        else:
            # Linux: pyautogui supports these key names on X11 in most setups.
            try:
                self._pyautogui.press(key)
            except Exception as e:  # noqa: BLE001
                log.debug("linux media key failed: %s", e)
