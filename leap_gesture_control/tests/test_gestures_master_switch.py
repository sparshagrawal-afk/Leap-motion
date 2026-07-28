"""Tests for the global gestures_enabled master switch.

When gestures_enabled is False, the ModeController must not:
  * move the cursor
  * emit scroll events
  * dispatch any binding action

The tracker itself keeps running so status readouts still work — but that's
outside the mode controller.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.hand_frame import Finger, HandFrame  # noqa: E402
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


def _bootstrap(tmp_path):
    (tmp_path / "config.default.json").write_text('''{
        "active_mode": "macos", "gestures_enabled": true,
        "sensitivity": 1.0, "smoothing": 0.0,
        "mount_orientation": "desktop", "gesture_cooldown_ms": 200,
        "modes": {
            "macos": {
                "enabled": true,
                "cursor_gesture": "point",
                "click_gesture": "pinch",
                "right_click_gesture": "two_fingers_pinch",
                "scroll": {"enabled": true, "natural": true, "sensitivity": 1.0},
                "bindings": {"fist": "key:f4"}
            }
        }
    }''')
    return ConfigManager(tmp_path)


def test_master_switch_suppresses_cursor_movement(tmp_path):
    cfg = _bootstrap(tmp_path)
    cfg.set("gestures_enabled", False)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    moves = []
    ex.move_cursor = lambda x, y: moves.append((x, y))
    # point pose is normally the cursor pose — it should be inert now
    mode.handle_frame(_frame(0.0, (0.0, 200.0, 0.0), extended={"index": True}))
    assert moves == []


def test_master_switch_suppresses_scroll(tmp_path):
    cfg = _bootstrap(tmp_path)
    cfg.set("gestures_enabled", False)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    scrolls = []
    ex.scroll = lambda dx=0, dy=0: scrolls.append((dx, dy))
    ext = {"index": True, "middle": True}
    for i in range(15):
        mode.handle_frame(_frame(i * 0.02, (0.0, 200.0, -3.0 * i), extended=ext))
    assert scrolls == []


def test_master_switch_suppresses_gesture_binding(tmp_path):
    cfg = _bootstrap(tmp_path)
    cfg.set("gestures_enabled", False)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    mode.handle_gesture("fist", _frame(0.0, (0, 200, 0)))
    # No action should have been dispatched.
    assert ex.last_action is None


def test_re_enabling_restores_dispatch(tmp_path):
    """Toggling back on immediately restores gesture dispatch."""
    cfg = _bootstrap(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)

    cfg.set("gestures_enabled", False)
    mode.handle_gesture("fist", _frame(0.0, (0, 200, 0)))
    assert ex.last_action is None

    cfg.set("gestures_enabled", True)
    mode.handle_gesture("fist", _frame(1.0, (0, 200, 0)))
    assert ex.last_action == "key:f4"


def test_persistence(tmp_path):
    _bootstrap(tmp_path)
    m = ConfigManager(tmp_path)
    m.set("gestures_enabled", False)
    m.save()
    again = ConfigManager(tmp_path)
    assert again.data["gestures_enabled"] is False
