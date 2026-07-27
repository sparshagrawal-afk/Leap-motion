"""Mock tracker: replays a scripted motion so the app is testable without hardware.

Generates HandFrames on a background thread at ~60 FPS. The GUI treats it like
a real tracker (same on_frame / on_status API).
"""
from __future__ import annotations

import math
import threading
import time
from typing import Callable, List

from .hand_frame import Finger, HandFrame


class MockTracker:
    def __init__(self, hand_preference: str = "right"):
        self.hand_preference = hand_preference
        self._listeners: List[Callable[[HandFrame], None]] = []
        self._status_listeners: List[Callable[[str], None]] = []
        self._thread = None
        self._stop_evt = threading.Event()
        self._status = "disconnected"
        # scripted pose: "idle" | "point" | "pinch" | "fist" | "open_palm"
        #                | "swipe_right" | "swipe_left" | "swipe_up" | "swipe_down"
        self.script: str = "idle"

    def on_frame(self, cb):
        self._listeners.append(cb)

    def on_status(self, cb):
        self._status_listeners.append(cb)
        cb(self._status)

    @property
    def status(self) -> str:
        return self._status

    def set_pose(self, pose: str) -> None:
        self.script = pose

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop_evt.clear()
        self._set_status("connected_mock")
        self._thread = threading.Thread(target=self._run, name="MockTracker", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_evt.set()
        self._set_status("disconnected")

    def _set_status(self, s: str) -> None:
        self._status = s
        for cb in self._status_listeners:
            try:
                cb(s)
            except Exception:  # noqa: BLE001
                pass

    def _run(self) -> None:
        t0 = time.time()
        base_x, base_y, base_z = 0.0, 200.0, 0.0
        swipe_start_t = None
        last_pose = None
        while not self._stop_evt.is_set():
            now = time.time()
            t = now - t0
            pose = self.script
            if pose != last_pose:
                swipe_start_t = now
                last_pose = pose

            # positions & velocities per script
            x, y, z = base_x, base_y, base_z
            vx = vy = vz = 0.0
            if pose in ("swipe_right", "swipe_left", "swipe_up", "swipe_down"):
                elapsed = min(now - (swipe_start_t or now), 0.3)
                speed = 900.0  # mm/s
                dist = speed * elapsed
                if pose == "swipe_right":
                    x = base_x + dist;  vx = speed
                elif pose == "swipe_left":
                    x = base_x - dist;  vx = -speed
                elif pose == "swipe_up":
                    y = base_y + dist;  vy = speed
                else:
                    y = base_y - dist;  vy = -speed
                if elapsed >= 0.3:
                    self.script = "idle"

            # finger extension + pinch/grab per pose
            ext = {"thumb": False, "index": False, "middle": False, "ring": False, "pinky": False}
            pinch = 0.1
            grab = 0.1
            if pose == "point":
                ext["index"] = True
            elif pose == "two_fingers":
                ext["index"] = True; ext["middle"] = True
            elif pose == "thumbs_up":
                ext["thumb"] = True
            elif pose == "fist":
                grab = 0.95
            elif pose == "open_palm":
                for k in ext: ext[k] = True
            elif pose == "pinch":
                pinch = 0.95
            elif pose == "two_fingers_pinch":
                pinch = 0.95; ext["middle"] = True

            fingers = [
                Finger(name=n, extended=ext[n],
                       tip=(x + i * 8.0, y + 40.0, z))
                for i, n in enumerate(["thumb", "index", "middle", "ring", "pinky"])
            ]

            frame = HandFrame(
                timestamp=now,
                hand_type=self.hand_preference or "right",
                palm_position=(x + math.sin(t * 4) * 2, y, z),
                palm_velocity=(vx, vy, vz),
                palm_normal=(0.0, -1.0, 0.0),
                grab_strength=grab,
                pinch_strength=pinch,
                fingers=fingers,
                confidence=1.0,
            )
            for cb in self._listeners:
                try:
                    cb(frame)
                except Exception:  # noqa: BLE001
                    pass
            time.sleep(1 / 60)
