"""Tests for the quality-pass improvements:

  1. Stale-history reset when a hand disappears mid-motion (no phantom swipe).
  2. Mouse-mode silently ignores the configured cursor pose (no log spam).
  3. Cursor mapping honours `mount_orientation` (desktop vs screen_top).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.hand_frame import Finger, HandFrame  # noqa: E402
from src.gesture_recognizer import GestureRecognizer  # noqa: E402
from src.config_manager import ConfigManager  # noqa: E402
from src.action_executor import ActionExecutor  # noqa: E402
from src.mode_controller import ModeController  # noqa: E402


def _frame(t, pos, extended=None):
    extended = extended or {}
    fingers = [Finger(name=n, extended=extended.get(n, False), tip=(0, 0, 0))
               for n in ["thumb", "index", "middle", "ring", "pinky"]]
    return HandFrame(
        timestamp=t, hand_type="right", palm_position=pos, palm_velocity=(0, 0, 0),
        palm_normal=(0, -1, 0), grab_strength=0.1, pinch_strength=0.1, fingers=fingers,
    )


# --- 1. Stale-history reset ---

def test_hand_lost_clears_history_and_prevents_phantom_swipe():
    """If the hand disappears (>300ms gap) and re-enters at a different
    location, we must NOT synthesise a swipe from the old-to-new jump."""
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))

    # Hand present on the far left for a bit
    for i in range(6):
        r.process(_frame(i * 0.02, (-200.0, 200.0, 0.0)))
    # ...hand disappears for 800ms...
    # ...then re-enters far right (which would look like a big rightward swipe)
    for i in range(6):
        r.process(_frame(0.8 + i * 0.02, (200.0, 200.0, 0.0)))

    assert not [g for g in fired if g.startswith("swipe_")], (
        f"phantom swipe fired across the hand-lost gap: {fired}"
    )


# --- 2. Mouse-mode cursor pose is silent ---

def test_mouse_mode_ignores_cursor_pose_event(tmp_path):
    (tmp_path / "config.default.json").write_text('''{
        "active_mode": "mouse", "sensitivity": 1.0, "smoothing": 0.3,
        "gesture_cooldown_ms": 200,
        "mount_orientation": "desktop",
        "modes": {
            "mouse": {"enabled": true, "cursor_gesture": "point",
                      "click_gesture": "pinch",
                      "right_click_gesture": "two_fingers_pinch"}
        }
    }''')
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)

    events = []
    mode.on_event(lambda g, a: events.append((g, a)))
    # Firing the cursor pose should NOT emit an event.
    mode.handle_gesture("point", _frame(0.0, (0.0, 200.0, 0.0)))
    assert events == []
    # Firing pinch (the configured click gesture) should still work.
    mode.handle_gesture("pinch", _frame(0.0, (0.0, 200.0, 0.0)))
    assert events == [("pinch", "mouse:left_click")]


# --- 3. Orientation-aware cursor mapping ---

def test_cursor_mapping_desktop_orientation(tmp_path):
    """Desktop mount: palm x → screen x, -palm z → screen y."""
    (tmp_path / "config.default.json").write_text('''{
        "active_mode": "mouse", "sensitivity": 1.0, "smoothing": 0.0,
        "mount_orientation": "desktop",
        "modes": {"mouse": {"enabled": true, "cursor_gesture": "point"}}
    }''')
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    mode._screen_w, mode._screen_h = 1000, 1000  # deterministic mapping

    moves = []
    ex.move_cursor = lambda x, y: moves.append((x, y))  # capture

    # centered (0, 200, 0) → screen center (500, 500)
    mode.handle_frame(_frame(0.0, (0.0, 200.0, 0.0), extended={"index": True}))
    assert moves and abs(moves[-1][0] - 500) < 5 and abs(moves[-1][1] - 500) < 5

    # positive x → right side; positive z (toward sensor) → up on screen (-z)
    mode.handle_frame(_frame(0.1, (250.0, 200.0, -200.0), extended={"index": True}))
    x, y = moves[-1]
    assert x >= 990          # far right
    assert y >= 990          # near bottom (mapped from -pz > 0)


def test_cursor_mapping_screen_top_orientation(tmp_path):
    """Screen-top mount: palm x → screen x, palm y (centered at 200mm) → screen y."""
    (tmp_path / "config.default.json").write_text('''{
        "active_mode": "mouse", "sensitivity": 1.0, "smoothing": 0.0,
        "mount_orientation": "screen_top",
        "modes": {"mouse": {"enabled": true, "cursor_gesture": "point"}}
    }''')
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    mode._screen_w, mode._screen_h = 1000, 1000

    moves = []
    ex.move_cursor = lambda x, y: moves.append((x, y))

    # palm at (0, 200, 0) → center
    mode.handle_frame(_frame(0.0, (0.0, 200.0, 0.0), extended={"index": True}))
    assert moves and abs(moves[-1][0] - 500) < 5 and abs(moves[-1][1] - 500) < 5

    # palm y = 400 (well above center) should push cursor down (y > 500)
    mode.handle_frame(_frame(0.1, (0.0, 400.0, 0.0), extended={"index": True}))
    _, y = moves[-1]
    assert y > 700
