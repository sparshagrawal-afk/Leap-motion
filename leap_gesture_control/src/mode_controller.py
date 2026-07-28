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
    # Modes that use point-to-move-cursor + pinch-to-click.
    _MOUSE_LIKE_MODES = ("mouse", "macos")

    def __init__(self, config: ConfigManager, executor: ActionExecutor):
        self.config = config
        self.executor = executor
        self._screen_w = 1920
        self._screen_h = 1080
        self._smooth_xy: Optional[tuple[float, float]] = None
        self._last_click_ts = 0.0
        self._event_listeners: list[Callable[[str, str], None]] = []
        # Continuous-scroll state (macOS mode)
        self._scroll_prev_pos: Optional[tuple[float, float]] = None
        self._scroll_last_emit_ts: float = 0.0
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
        # Global master switch: when off, we suppress every OS side-effect
        # (cursor move, scroll, clicks, hotkeys) while the tracker keeps
        # running so the user can still see hand data in the GUI.
        if not self.config.data.get("gestures_enabled", True):
            self._smooth_xy = None
            self._scroll_prev_pos = None
            return
        active = self.config.data.get("active_mode", "mouse")
        if active not in self._MOUSE_LIKE_MODES:
            return
        mode_cfg = self.config.data["modes"].get(active, {})
        if not mode_cfg.get("enabled", True):
            return

        # macOS mode gets continuous two-finger scroll on top of cursor.
        if active == "macos" and self._handle_scroll(frame, mode_cfg):
            # Scroll gesture consumed this frame; suppress cursor move so the
            # pointer doesn't drift while the user is scrolling.
            self._smooth_xy = None
            return

        # Only move cursor when the "cursor" pose is active (default: point).
        cursor_pose = mode_cfg.get("cursor_gesture", "point")
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

    # ---------- Continuous scroll (macOS mode) ----------
    def _handle_scroll(self, frame: HandFrame, mode_cfg: dict) -> bool:
        """If the scroll pose is active, emit scroll deltas. Returns True if
        the frame was consumed by scrolling (so cursor movement is skipped).
        """
        scroll_cfg = mode_cfg.get("scroll", {})
        if not scroll_cfg.get("enabled", True):
            self._scroll_prev_pos = None
            return False
        # Scroll pose = index + middle extended, no others.
        extended = set(frame.extended_fingers())
        scroll_active = extended == {"index", "middle"} or extended == {"index", "middle", "thumb"}
        if not scroll_active:
            self._scroll_prev_pos = None
            return False

        # Read tunables per call so live UI changes take effect.
        natural = bool(scroll_cfg.get("natural", True))
        sensitivity = float(scroll_cfg.get("sensitivity", 1.0))
        # How many mm of palm motion = one scroll "click" (mouse wheel unit).
        mm_per_click = max(1.0, 18.0 / sensitivity)

        # Pick the 2D axes for scrolling based on sensor orientation.
        orientation = self.config.data.get("mount_orientation", "desktop")
        px, py, pz = frame.palm_position
        if orientation == "screen_top":
            cur = (px, py)          # horizontal, vertical
        else:  # desktop
            cur = (px, -pz)         # horizontal, forward/back → vertical

        if self._scroll_prev_pos is None:
            self._scroll_prev_pos = cur
            return True

        dx_mm = cur[0] - self._scroll_prev_pos[0]
        dy_mm = cur[1] - self._scroll_prev_pos[1]

        # Only emit whole clicks; keep the fractional remainder for next frame.
        clicks_x = int(dx_mm / mm_per_click)
        clicks_y = int(dy_mm / mm_per_click)
        if clicks_x == 0 and clicks_y == 0:
            return True  # still active, just below the emission threshold

        # Advance the anchor by the emitted whole-click distance so residual
        # motion accumulates across frames instead of being dropped.
        self._scroll_prev_pos = (
            self._scroll_prev_pos[0] + clicks_x * mm_per_click,
            self._scroll_prev_pos[1] + clicks_y * mm_per_click,
        )

        # Natural scroll: hand up → content up (page moves down). Traditional
        # is inverted.
        sign = 1 if natural else -1
        self.executor.scroll(dx=sign * clicks_x, dy=sign * clicks_y)
        self._scroll_last_emit_ts = frame.timestamp
        self._emit("scroll", f"scroll dx={clicks_x} dy={clicks_y}")
        return True

    # ---------- Called when a discrete gesture fires ----------
    def handle_gesture(self, gesture: str, frame: HandFrame) -> None:
        # Global master switch — see handle_frame().
        if not self.config.data.get("gestures_enabled", True):
            return
        active = self.config.data.get("active_mode", "mouse")
        modes = self.config.data.get("modes", {})
        mode_cfg = modes.get(active, {})
        if not mode_cfg.get("enabled", True):
            return  # silent: mode disabled

        # Mouse-like modes (mouse, macos) share the cursor-pose + click logic.
        if active in self._MOUSE_LIKE_MODES:
            if gesture == mode_cfg.get("cursor_gesture", "point"):
                return
            action = self._mouse_mode_action(gesture, mode_cfg)
            # macOS mode also supports pose-modified swipe bindings on top of
            # the click gestures. Try the specific modified binding first,
            # then fall back to the base gesture.
            if not action and active == "macos":
                bindings = mode_cfg.get("bindings", {}) or {}
                action = bindings.get(gesture, "")
                if not action and "+" in gesture:
                    action = bindings.get(gesture.split("+", 1)[0], "")
        else:
            bindings = mode_cfg.get("bindings", {}) or {}
            action = bindings.get(gesture, "")
            # Fall back to the base swipe if a pose-modified one isn't bound.
            if not action and "+" in gesture:
                action = bindings.get(gesture.split("+", 1)[0], "")

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
