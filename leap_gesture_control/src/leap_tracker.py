"""Leap Motion 2 tracker: connects to the Ultraleap Gemini SDK via leapc-python-bindings.

If the SDK / bindings are not installed, `is_available()` returns False and
the app falls back to `mock_tracker.MockTracker`.
"""
from __future__ import annotations

import logging
import threading
import time
from typing import Callable, Optional

from .hand_frame import Finger, HandFrame

log = logging.getLogger("gesture.leap")

_FINGER_NAMES = ["thumb", "index", "middle", "ring", "pinky"]


def is_available() -> bool:
    try:
        import leap  # type: ignore  # noqa: F401
        return True
    except Exception:  # noqa: BLE001
        return False


class LeapTracker:
    """Wraps leapc-python-bindings and emits SDK-agnostic HandFrame objects."""

    def __init__(self, hand_preference: str = "right"):
        self.hand_preference = hand_preference
        self._listeners: list[Callable[[HandFrame], None]] = []
        self._status_listeners: list[Callable[[str], None]] = []
        self._connection = None
        self._listener = None
        self._thread: Optional[threading.Thread] = None
        self._stop_evt = threading.Event()
        self._status = "disconnected"

    # ---------- public ----------
    def on_frame(self, cb: Callable[[HandFrame], None]) -> None:
        self._listeners.append(cb)

    def on_status(self, cb: Callable[[str], None]) -> None:
        self._status_listeners.append(cb)

    @property
    def status(self) -> str:
        return self._status

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop_evt.clear()
        self._thread = threading.Thread(target=self._run, name="LeapTracker", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_evt.set()
        try:
            if self._connection is not None:
                self._connection.disconnect()
        except Exception:  # noqa: BLE001
            pass
        self._set_status("disconnected")

    # ---------- internals ----------
    def _set_status(self, s: str) -> None:
        self._status = s
        for cb in self._status_listeners:
            try:
                cb(s)
            except Exception:  # noqa: BLE001
                pass

    def _run(self) -> None:
        try:
            import leap  # type: ignore
        except Exception as e:  # noqa: BLE001
            log.error("Ultraleap Python bindings not installed: %s", e)
            self._set_status("sdk_missing")
            return

        outer = self

        class _Listener(leap.Listener):  # type: ignore[misc]
            def on_connection_event(self, event):
                outer._set_status("connected")

            def on_device_event(self, event):
                try:
                    with event.device.open():
                        pass
                except Exception:  # noqa: BLE001
                    pass

            def on_tracking_event(self, event):
                outer._handle_tracking(event)

        try:
            self._listener = _Listener()
            self._connection = leap.Connection()
            self._connection.add_listener(self._listener)
            self._set_status("connecting")
            with self._connection.open():
                # High-fidelity desktop mode by default.
                try:
                    self._connection.set_tracking_mode(leap.TrackingMode.Desktop)
                except Exception:  # noqa: BLE001
                    pass
                while not self._stop_evt.is_set():
                    time.sleep(0.05)
        except Exception as e:  # noqa: BLE001
            log.error("Leap connection failed: %s", e)
            self._set_status("error")
        finally:
            self._set_status("disconnected")

    def _handle_tracking(self, event) -> None:
        try:
            import leap  # type: ignore
        except Exception:  # noqa: BLE001
            return

        for hand in event.hands:
            # Handedness
            try:
                is_left = hand.type == leap.HandType.Left
            except Exception:  # noqa: BLE001
                is_left = getattr(hand, "type", 0) == 0
            hand_type = "left" if is_left else "right"
            if self.hand_preference in ("left", "right") and hand_type != self.hand_preference:
                continue

            palm_pos = _vec(hand.palm.position)
            palm_vel = _vec(getattr(hand.palm, "velocity", (0.0, 0.0, 0.0)))
            palm_norm = _vec(getattr(hand.palm, "normal", (0.0, 1.0, 0.0)))
            grab = float(getattr(hand, "grab_strength", 0.0))
            pinch = float(getattr(hand, "pinch_strength", 0.0))
            confidence = float(getattr(hand, "confidence", 1.0))

            fingers: list[Finger] = []
            digits = getattr(hand, "digits", None) or []
            for i, digit in enumerate(digits[:5]):
                name = _FINGER_NAMES[i] if i < 5 else f"finger_{i}"
                extended = bool(getattr(digit, "is_extended", False))
                tip = _finger_tip(digit)
                fingers.append(Finger(name=name, extended=extended, tip=tip))

            frame = HandFrame(
                timestamp=time.time(),
                hand_type=hand_type,
                palm_position=palm_pos,
                palm_velocity=palm_vel,
                palm_normal=palm_norm,
                grab_strength=grab,
                pinch_strength=pinch,
                fingers=fingers,
                confidence=confidence,
            )
            for cb in self._listeners:
                try:
                    cb(frame)
                except Exception as e:  # noqa: BLE001
                    log.debug("listener error: %s", e)


def _vec(v) -> tuple[float, float, float]:
    if v is None:
        return (0.0, 0.0, 0.0)
    if isinstance(v, tuple):
        return (float(v[0]), float(v[1]), float(v[2]))
    return (float(v.x), float(v.y), float(v.z))


def _finger_tip(digit) -> tuple[float, float, float]:
    # LeapC digit exposes 4 bones; the distal (index 3) tip is the fingertip.
    try:
        distal = digit.distal
        return _vec(distal.next_joint)
    except Exception:  # noqa: BLE001
        try:
            return _vec(digit.bones[3].next_joint)
        except Exception:  # noqa: BLE001
            return (0.0, 0.0, 0.0)
