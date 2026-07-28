"""Behaviour that survived the simplification pass:
   the recognizer clears its motion history when the hand disappears so
   re-entering the tracking volume can't synthesise a phantom swipe."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.hand_frame import Finger, HandFrame  # noqa: E402
from src.gesture_recognizer import GestureRecognizer  # noqa: E402


def _frame(t, pos, extended=None):
    extended = extended or {}
    fingers = [Finger(name=n, extended=extended.get(n, False), tip=(0, 0, 0))
               for n in ["thumb", "index", "middle", "ring", "pinky"]]
    return HandFrame(
        timestamp=t, hand_type="right", palm_position=pos, palm_velocity=(0, 0, 0),
        palm_normal=(0, -1, 0), grab_strength=0.1, pinch_strength=0.1, fingers=fingers,
    )


def test_hand_lost_clears_history_and_prevents_phantom_swipe():
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    # Hand present on the far left for a bit.
    for i in range(6):
        r.process(_frame(i * 0.02, (-200.0, 200.0, 0.0)))
    # ...disappears for 800 ms...
    # ...then re-enters far right (which would look like a big rightward swipe).
    for i in range(6):
        r.process(_frame(0.8 + i * 0.02, (200.0, 200.0, 0.0)))
    assert not [g for g in fired if g.startswith("swipe_")], (
        f"phantom swipe fired across the hand-lost gap: {fired}"
    )
