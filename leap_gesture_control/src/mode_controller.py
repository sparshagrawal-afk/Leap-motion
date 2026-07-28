"""Mode controller: given the active mode + current gesture, dispatch an action.

Simplified for macOS-only mode: no cursor movement or click handling.
Only responsibilities:
  * continuous two-finger scroll (per frame)
  * dispatch discrete gestures (poses / pose-modified swipes) to their bindings
  * honour the global gestures_enabled master switch
"""
from __future__ import annotations

import logging
from typing import Callable, Optional

from .action_executor import ActionExecutor
from .config_manager import ConfigManager
from .hand_frame import HandFrame

log = logging.getLogger("gesture.mode")


class ModeController:
    def __init__(self, config: ConfigManager, executor: ActionExecutor):
        self.config = config
        self.executor = executor
        self._event_listeners: list[Callable[[str, str], None]] = []
        # Continuous-scroll anchor point (mm) — updated per frame.
        self._scroll_prev_pos: Optional[tuple[float, float]] = None

    # ---------- observability ----------
    def on_event(self, cb: Callable[[str, str], None]) -> None:
        """cb(gesture, action_binding_or_'') fired whenever a gesture is handled."""
        self._event_listeners.append(cb)

    def _emit(self, gesture: str, action: str) -> None:
        for cb in self._event_listeners:
            try:
                cb(gesture, action)
            except Exception:  # noqa: BLE001
                pass

    # ---------- per-frame (continuous gestures) ----------
    def handle_frame(self, frame: HandFrame) -> None:
        if not self.config.data.get("gestures_enabled", True):
            self._scroll_prev_pos = None
            return
        active = self.config.data.get("active_mode", "macos")
        mode_cfg = self.config.data.get("modes", {}).get(active, {})
        if not mode_cfg.get("enabled", True):
            self._scroll_prev_pos = None
            return
        self._handle_scroll(frame, mode_cfg)

    def _handle_scroll(self, frame: HandFrame, mode_cfg: dict) -> bool:
        """Two-finger (index + middle) palm motion → mouse-wheel scroll clicks.

        Returns True if the frame was consumed by scroll (kept for future
        callers that may want to know).
        """
        scroll_cfg = mode_cfg.get("scroll", {})
        if not scroll_cfg.get("enabled", True):
            self._scroll_prev_pos = None
            return False
        extended = set(frame.extended_fingers())
        scroll_active = (
            extended == {"index", "middle"}
            or extended == {"index", "middle", "thumb"}
        )
        if not scroll_active:
            self._scroll_prev_pos = None
            return False

        natural = bool(scroll_cfg.get("natural", True))
        sensitivity = float(scroll_cfg.get("sensitivity", 1.0))
        mm_per_click = max(1.0, 18.0 / sensitivity)

        orientation = self.config.data.get("mount_orientation", "desktop")
        px, py, pz = frame.palm_position
        if orientation == "screen_top":
            cur = (px, py)
        else:
            cur = (px, -pz)

        if self._scroll_prev_pos is None:
            self._scroll_prev_pos = cur
            return True

        dx_mm = cur[0] - self._scroll_prev_pos[0]
        dy_mm = cur[1] - self._scroll_prev_pos[1]
        clicks_x = int(dx_mm / mm_per_click)
        clicks_y = int(dy_mm / mm_per_click)
        if clicks_x == 0 and clicks_y == 0:
            return True

        self._scroll_prev_pos = (
            self._scroll_prev_pos[0] + clicks_x * mm_per_click,
            self._scroll_prev_pos[1] + clicks_y * mm_per_click,
        )
        sign = 1 if natural else -1
        self.executor.scroll(dx=sign * clicks_x, dy=sign * clicks_y)
        self._emit("scroll", f"scroll dx={clicks_x} dy={clicks_y}")
        return True

    # ---------- discrete gestures ----------
    def handle_gesture(self, gesture: str, frame: HandFrame) -> None:
        if not self.config.data.get("gestures_enabled", True):
            return
        active = self.config.data.get("active_mode", "macos")
        mode_cfg = self.config.data.get("modes", {}).get(active, {})
        if not mode_cfg.get("enabled", True):
            return

        bindings = mode_cfg.get("bindings", {}) or {}
        action = bindings.get(gesture, "")
        # Pose-modified fallback (e.g. "swipe_up+open_palm" → "swipe_up").
        if not action and "+" in gesture:
            action = bindings.get(gesture.split("+", 1)[0], "")

        if not action:
            self._emit(gesture, "")
            return

        ok = self.executor.execute(action)
        log.info("mode=%s gesture=%s -> %s [%s]", active, gesture, action,
                 "OK" if ok else "FAIL")
        self._emit(gesture, action)
