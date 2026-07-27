import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.hand_frame import Finger, HandFrame  # noqa: E402
from src.gesture_recognizer import GestureRecognizer  # noqa: E402


def _finger(name, extended, tip=(0, 0, 0)):
    return Finger(name=name, extended=extended, tip=tip)


def _frame(t, pos, vel=(0, 0, 0), pinch=0.1, grab=0.1, extended=None):
    extended = extended or {}
    fingers = [_finger(n, extended.get(n, False)) for n in ["thumb", "index", "middle", "ring", "pinky"]]
    return HandFrame(
        timestamp=t, hand_type="right", palm_position=pos, palm_velocity=vel,
        palm_normal=(0, -1, 0), grab_strength=grab, pinch_strength=pinch, fingers=fingers,
    )


# ---- pose gestures ----

def test_point_pose_fires_after_hold():
    r = GestureRecognizer(cooldown_ms=100)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    t = 0.0
    while t < 0.4:  # hold pose 400ms
        r.process(_frame(t, (0, 200, 0), extended={"index": True}))
        t += 0.02
    assert "point" in fired


def test_pinch_pose_fires():
    r = GestureRecognizer(cooldown_ms=100)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    for i in range(20):
        r.process(_frame(i * 0.02, (0, 200, 0), pinch=0.95))
    assert "pinch" in fired


def test_fist_pose_fires():
    r = GestureRecognizer(cooldown_ms=100)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    for i in range(20):
        r.process(_frame(i * 0.02, (0, 200, 0), grab=0.95))
    assert "fist" in fired


def test_open_palm_pose_fires():
    r = GestureRecognizer(cooldown_ms=100)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    ext = {n: True for n in ["thumb", "index", "middle", "ring", "pinky"]}
    for i in range(20):
        r.process(_frame(i * 0.02, (0, 200, 0), extended=ext))
    assert "open_palm" in fired


def test_two_fingers_pose_fires():
    r = GestureRecognizer(cooldown_ms=100)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    ext = {"index": True, "middle": True}
    for i in range(20):
        r.process(_frame(i * 0.02, (0, 200, 0), extended=ext))
    assert "two_fingers" in fired


# ---- motion / swipe gestures ----

def test_swipe_right_detected():
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    # simulate rightward motion: x increases by 200mm over ~0.25s
    for i, x in enumerate(range(0, 220, 10)):
        vel = (900.0, 0.0, 0.0)
        r.process(_frame(i * 0.02, (float(x), 200.0, 0.0), vel=vel))
    assert "swipe_right" in fired


def test_swipe_left_detected():
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    for i, x in enumerate(range(0, -220, -10)):
        vel = (-900.0, 0.0, 0.0)
        r.process(_frame(i * 0.02, (float(x), 200.0, 0.0), vel=vel))
    assert "swipe_left" in fired


def test_swipe_up_detected():
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    for i, y in enumerate(range(100, 320, 10)):
        vel = (0.0, 900.0, 0.0)
        r.process(_frame(i * 0.02, (0.0, float(y), 0.0), vel=vel))
    assert "swipe_up" in fired


def test_swipe_down_detected():
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    for i, y in enumerate(range(320, 100, -10)):
        vel = (0.0, -900.0, 0.0)
        r.process(_frame(i * 0.02, (0.0, float(y), 0.0), vel=vel))
    assert "swipe_down" in fired


def test_no_swipe_on_jitter():
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    # tiny random-ish motion, no clear direction, no velocity
    for i in range(20):
        r.process(_frame(i * 0.02, (0.5 * ((-1) ** i), 200.0, 0.0)))
    assert not [g for g in fired if g.startswith("swipe_")]


# ---- cooldown ----

def test_cooldown_prevents_double_fire():
    r = GestureRecognizer(cooldown_ms=500)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    # Fire pinch, then immediately try again → only one fires.
    for i in range(20):
        r.process(_frame(i * 0.02, (0, 200, 0), pinch=0.95))
    # brief release
    r.process(_frame(0.5, (0, 200, 0), pinch=0.1))
    # try to fire again within cooldown window
    for i in range(20):
        r.process(_frame(0.52 + i * 0.02, (0, 200, 0), pinch=0.95))
    assert fired.count("pinch") == 1
