"""Hand-frame data structures shared across tracker, recognizer, and GUI.

Kept independent from the LeapC SDK so recognizer/tests don't depend on hardware.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Tuple


Vec3 = Tuple[float, float, float]  # millimeters


@dataclass
class Finger:
    name: str            # thumb | index | middle | ring | pinky
    extended: bool
    tip: Vec3


@dataclass
class HandFrame:
    """A single frame of hand data (SDK-agnostic)."""
    timestamp: float                # seconds
    hand_type: str                  # "left" | "right"
    palm_position: Vec3             # mm
    palm_velocity: Vec3             # mm/s
    palm_normal: Vec3               # unit vector
    grab_strength: float            # 0..1  (1 = fist)
    pinch_strength: float           # 0..1  (1 = tight pinch)
    fingers: List[Finger] = field(default_factory=list)
    confidence: float = 1.0

    def extended_fingers(self) -> List[str]:
        return [f.name for f in self.fingers if f.extended]

    def finger(self, name: str) -> Optional[Finger]:
        for f in self.fingers:
            if f.name == name:
                return f
        return None
