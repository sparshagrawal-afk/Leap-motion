"""Gesture recognizer: turns a stream of HandFrames into discrete gesture events.

Recognized gestures:
  - pose: point, open_palm, fist, thumbs_up, two_fingers, pinch, two_fingers_pinch
  - motion: swipe_left, swipe_right, swipe_up, swipe_down

Discrete gestures debounce via a cooldown window so a single physical motion
does not fire multiple actions.
"""
from __future__ import annotations

from collections import deque
from typing import Callable, Deque, List, Optional

from .hand_frame import HandFrame

# Pose gestures are fired once when a stable pose is held for >= HOLD_MS.
POSE_HOLD_MS = 180
# Motion gestures use velocity + displacement over a short window.
MOTION_WINDOW_S = 0.35
MIN_SWIPE_DISPLACEMENT_MM = 90.0


def _classify_pose(frame: HandFrame, pinch_threshold: float) -> Optional[str]:
    extended = set(frame.extended_fingers())
    pinch = frame.pinch_strength
    grab = frame.grab_strength

    # Pinch variants first (they can overlap with pose)
    if pinch >= pinch_threshold:
        if extended >= {"middle"} and "index" not in extended:
            return "two_fingers_pinch"
        return "pinch"

    if grab >= 0.85 and not extended:
        return "fist"
    if extended == {"index"}:
        return "point"
    if extended == {"index", "middle"}:
        return "two_fingers"
    if extended == {"thumb"}:
        return "thumbs_up"
    if extended >= {"thumb", "index", "middle", "ring", "pinky"}:
        return "open_palm"
    return None


class GestureRecognizer:
    """Stateful recognizer. Feed frames in, receive gesture events out."""

    ALL_GESTURES = [
        "point", "open_palm", "fist", "thumbs_up", "two_fingers",
        "pinch", "two_fingers_pinch",
        "swipe_left", "swipe_right", "swipe_up", "swipe_down",
        "fist_move",
    ]

    def __init__(
        self,
        cooldown_ms: int = 450,
        swipe_min_velocity: float = 550.0,
        pinch_threshold: float = 0.85,
    ):
        self.cooldown_ms = cooldown_ms
        self.swipe_min_velocity = swipe_min_velocity
        self.pinch_threshold = pinch_threshold
        self._history: Deque[HandFrame] = deque(maxlen=48)
        self._last_gesture_time: dict[str, float] = {}
        self._current_pose: Optional[str] = None
        self._pose_started_at: float = 0.0
        self._listeners: List[Callable[[str, HandFrame], None]] = []

    # ---------- Public API ----------
    def on_gesture(self, cb: Callable[[str, HandFrame], None]) -> None:
        self._listeners.append(cb)

    def reset(self) -> None:
        self._history.clear()
        self._last_gesture_time.clear()
        self._current_pose = None

    def process(self, frame: HandFrame) -> List[str]:
        """Feed one frame; return list of gestures fired this frame."""
        # If there's a large time gap since the last frame, the hand was lost.
        # Clear motion history so we don't produce a phantom swipe when it
        # re-appears at a very different position.
        if self._history:
            gap = frame.timestamp - self._history[-1].timestamp
            if gap > 0.3:
                self._history.clear()
                self._current_pose = None
        self._history.append(frame)
        fired: List[str] = []

        # -- pose detection --
        pose = _classify_pose(frame, self.pinch_threshold)
        now = frame.timestamp
        if pose != self._current_pose:
            self._current_pose = pose
            self._pose_started_at = now
        elif pose is not None:
            held_ms = (now - self._pose_started_at) * 1000.0
            if held_ms >= POSE_HOLD_MS and self._can_fire(pose, now):
                self._mark_fired(pose, now)
                fired.append(pose)

        # -- motion (swipe) detection --
        swipe = self._detect_swipe(now)
        if swipe and self._can_fire(swipe, now):
            self._mark_fired(swipe, now)
            fired.append(swipe)
            # Also emit a pose-modified variant (e.g. "swipe_up+open_palm") so
            # modes can distinguish e.g. Mission Control (open-palm swipe up)
            # from a plain swipe-up. The base event still fires for backward
            # compatibility with modes that don't care about the modifier.
            swipe_pose = self._dominant_pose_during_swipe()
            if swipe_pose and swipe_pose not in ("pinch", "two_fingers_pinch"):
                fired.append(f"{swipe}+{swipe_pose}")

        for g in fired:
            for cb in self._listeners:
                try:
                    cb(g, frame)
                except Exception:  # noqa: BLE001
                    pass
        return fired

    # ---------- internals ----------
    def _can_fire(self, gesture: str, now: float) -> bool:
        last = self._last_gesture_time.get(gesture, 0.0)
        return (now - last) * 1000.0 >= self.cooldown_ms

    def _mark_fired(self, gesture: str, now: float) -> None:
        self._last_gesture_time[gesture] = now
        # Also cool down all swipes together (including pose-modified variants)
        # so one physical motion never fires twice.
        if gesture.startswith("swipe_"):
            base = gesture.split("+", 1)[0]
            for g in ("swipe_left", "swipe_right", "swipe_up", "swipe_down"):
                self._last_gesture_time[g] = now
            # Also mark the specific modified variant so its cooldown applies.
            self._last_gesture_time[base] = now

    def _detect_swipe(self, now: float) -> Optional[str]:
        if len(self._history) < 4:
            return None
        # collect frames inside the motion window
        window: List[HandFrame] = []
        for f in reversed(self._history):
            if now - f.timestamp <= MOTION_WINDOW_S:
                window.append(f)
            else:
                break
        if len(window) < 4:
            return None
        window.reverse()
        start = window[0].palm_position
        end = window[-1].palm_position
        dt = max(1e-3, window[-1].timestamp - window[0].timestamp)
        dx = end[0] - start[0]
        dy = end[1] - start[1]
        # Use displacement / dt as robust average velocity (less noisy than
        # the SDK's instantaneous palm_velocity on a single frame).
        vx = dx / dt
        vy = dy / dt

        abs_dx, abs_dy = abs(dx), abs(dy)
        abs_vx, abs_vy = abs(vx), abs(vy)

        # Require a real displacement, not just a jitter spike.
        if max(abs_dx, abs_dy) < MIN_SWIPE_DISPLACEMENT_MM:
            return None
        # Dominant axis must exceed threshold velocity.
        if abs_dx > abs_dy * 1.4 and abs_vx >= self.swipe_min_velocity:
            return "swipe_right" if dx > 0 else "swipe_left"
        if abs_dy > abs_dx * 1.4 and abs_vy >= self.swipe_min_velocity:
            return "swipe_up" if dy > 0 else "swipe_down"
        return None

    def _dominant_pose_during_swipe(self) -> Optional[str]:
        """Return the most common pose observed in the last motion window.

        Used to attach a pose modifier to a swipe event so e.g. an open-palm
        swipe can be told apart from a two-finger swipe.
        """
        if not self._history:
            return None
        now = self._history[-1].timestamp
        counts: dict[str, int] = {}
        for f in reversed(self._history):
            if now - f.timestamp > MOTION_WINDOW_S:
                break
            pose = _classify_pose(f, self.pinch_threshold)
            if pose:
                counts[pose] = counts.get(pose, 0) + 1
        if not counts:
            return None
        return max(counts, key=counts.get)
