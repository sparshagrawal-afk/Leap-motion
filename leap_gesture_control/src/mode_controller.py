"""Mode controller: given the active mode + current gesture, dispatch an action.

Also handles Mouse-mode cursor mapping (point → move cursor, pinch → click).
"""
from __future__ import annotations

import logging
import time
from typing import Callable, Optional

from .action_executor import ActionExecutor
from .config_manager import ConfigManager
from .hand_frame import HandFrame

log = logging.getLogger("gesture.mode")


class ModeController:
    def __init__(self, config: ConfigManager, executor: ActionExecutor):
        self.config = config
        self.executor = executor
        self._screen_w = 1920
        self._screen_h = 1080
        self._smooth_xy: Optional[tuple[float, float]] = None
        self._last_click_ts = 0.0
        self._event_listeners: list[Callable[[str, str], None]] = []
        self._detect_screen_size()

    def on_event(self, cb: Callable[[str, str], None]) -> None:
        """cb(gesture, action_binding_or_'') fired whenever a gesture is handled."""
        self._event_listeners.append(cb)

    def _emit(self, gesture: str, action: str) -> None:
        for cb in self._event_listeners:
            try:
                cb(gesture, action)
            except Exception:  # noqa: BLE001
                pass

    def _detect_screen_size(self) -> None:
        try:
            import pyautogui  # type: ignore
            w, h = pyautogui.size()
            self._screen_w, self._screen_h = int(w), int(h)
        except Exception:  # noqa: BLE001
            pass

    # ---------- Called on every frame (for cursor / scroll modes) ----------
    def handle_frame(self, frame: HandFrame) -> None:
        active = self.config.data.get("active_mode", "mouse")
        if active != "mouse":
            return
        mouse_cfg = self.config.data["modes"].get("mouse", {})
        if not mouse_cfg.get("enabled", True):
            return
        # Only move cursor when the "cursor" pose is active (default: point).
        cursor_pose = mouse_cfg.get("cursor_gesture", "point")
        extended = set(frame.extended_fingers())
        is_cursor_pose = False
        if cursor_pose == "point":
            is_cursor_pose = extended == {"index"} or extended == {"index", "thumb"}
        elif cursor_pose == "open_palm":
            is_cursor_pose = len(extended) >= 4
        if not is_cursor_pose:
            self._smooth_xy = None
            return

        # Map palm position (Leap coords) to screen x/y based on how the
        # sensor is physically mounted:
        #  * "desktop"     — sensor flat on the desk, camera facing up. The
        #                    user's hand moves in the horizontal x/z plane, so
        #                    palm x → screen x and (-palm z) → screen y.
        #  * "screen_top"  — sensor clipped to top of monitor facing the user.
        #                    Hand moves in the x/y plane, so palm x → screen x
        #                    and palm y → screen y.
        px, py, pz = frame.palm_position
        sensitivity = float(self.config.data.get("sensitivity", 1.0))
        smoothing = float(self.config.data.get("smoothing", 0.3))
        orientation = self.config.data.get("mount_orientation", "desktop")
        if orientation == "screen_top":
            # palm y is measured from the sensor; center it around ~200 mm.
            axis_x, axis_y = px, (py - 200.0)
        else:  # desktop
            axis_x, axis_y = px, -pz
        norm_x = max(-1.0, min(1.0, axis_x / (250.0 / sensitivity)))
        norm_y = max(-1.0, min(1.0, axis_y / (200.0 / sensitivity)))
        sx = (norm_x * 0.5 + 0.5) * self._screen_w
        sy = (norm_y * 0.5 + 0.5) * self._screen_h
        if self._smooth_xy is None:
            self._smooth_xy = (sx, sy)
        else:
            a = 1.0 - smoothing
            self._smooth_xy = (
                self._smooth_xy[0] * smoothing + sx * a,
                self._smooth_xy[1] * smoothing + sy * a,
            )
        self.executor.move_cursor(int(self._smooth_xy[0]), int(self._smooth_xy[1]))

    # ---------- Called when a discrete gesture fires ----------
    def handle_gesture(self, gesture: str, frame: HandFrame) -> None:
        active = self.config.data.get("active_mode", "mouse")
        modes = self.config.data.get("modes", {})
        mode_cfg = modes.get(active, {})
        if not mode_cfg.get("enabled", True):
            return  # silent: mode disabled

        # Mouse mode has its own action mapping (click gestures).
        # The cursor pose (e.g. `point`) is the *neutral* state used to move
        # the pointer; it should never fire as an action event and should not
        # spam the activity log with "no binding".
        if active == "mouse":
            if gesture == mode_cfg.get("cursor_gesture", "point"):
                return
            action = self._mouse_mode_action(gesture, mode_cfg)
        else:
            action = (mode_cfg.get("bindings") or {}).get(gesture, "")

        if not action:
            self._emit(gesture, "")
            return

        # Prevent double-clicks on jittery pinch:
        if action.startswith("mouse:") and (time.time() - self._last_click_ts) < 0.4:
            self._emit(gesture, action + " (skipped)")
            return
        if action.startswith("mouse:"):
            self._last_click_ts = time.time()

        ok = self.executor.execute(action)
        log.info("mode=%s gesture=%s -> %s [%s]", active, gesture, action, "OK" if ok else "FAIL")
        self._emit(gesture, action)

    def _mouse_mode_action(self, gesture: str, cfg: dict) -> str:
        if gesture == cfg.get("click_gesture", "pinch"):
            return "mouse:left_click"
        if gesture == cfg.get("right_click_gesture", "two_fingers_pinch"):
            return "mouse:right_click"
        return ""
