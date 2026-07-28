"""Tests for the new 'macOS' mode: pose-modified swipes, continuous scroll,
and Mission Control–style bindings."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.hand_frame import Finger, HandFrame  # noqa: E402
from src.gesture_recognizer import GestureRecognizer  # noqa: E402
from src.config_manager import ConfigManager  # noqa: E402
from src.action_executor import ActionExecutor  # noqa: E402
from src.mode_controller import ModeController  # noqa: E402


def _frame(t, pos, extended=None, vel=(0, 0, 0)):
    extended = extended or {}
    fingers = [Finger(name=n, extended=extended.get(n, False), tip=(0, 0, 0))
               for n in ["thumb", "index", "middle", "ring", "pinky"]]
    return HandFrame(
        timestamp=t, hand_type="right", palm_position=pos, palm_velocity=vel,
        palm_normal=(0, -1, 0), grab_strength=0.1, pinch_strength=0.1, fingers=fingers,
    )


# ---- pose-modified swipes ----

def test_open_palm_swipe_up_emits_modified_variant():
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    ext = {n: True for n in ["thumb", "index", "middle", "ring", "pinky"]}
    for i, y in enumerate(range(100, 320, 10)):
        r.process(_frame(i * 0.02, (0.0, float(y), 0.0), extended=ext, vel=(0, 900, 0)))
    assert "swipe_up" in fired
    assert "swipe_up+open_palm" in fired


def test_two_fingers_swipe_left_emits_modified_variant():
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    ext = {"index": True, "middle": True}
    for i, x in enumerate(range(0, -220, -10)):
        r.process(_frame(i * 0.02, (float(x), 200.0, 0.0), extended=ext, vel=(-900, 0, 0)))
    assert "swipe_left" in fired
    assert "swipe_left+two_fingers" in fired


def test_pinch_pose_does_not_taint_swipe_modifier():
    """Pinch is a click; a swipe made while pinched shouldn't be reported as
    'swipe_+pinch' because that would double-fire clicks and swipes."""
    r = GestureRecognizer(cooldown_ms=100, swipe_min_velocity=400)
    fired = []
    r.on_gesture(lambda g, f: fired.append(g))
    for i, x in enumerate(range(0, 220, 10)):
        # pinch pose active during the whole motion
        fingers = [Finger(name=n, extended=False, tip=(0, 0, 0))
                   for n in ["thumb", "index", "middle", "ring", "pinky"]]
        f = HandFrame(timestamp=i * 0.02, hand_type="right",
                      palm_position=(float(x), 200.0, 0.0),
                      palm_velocity=(900, 0, 0), palm_normal=(0, -1, 0),
                      grab_strength=0.1, pinch_strength=0.95, fingers=fingers)
        r.process(f)
    assert "swipe_right" in fired
    # No pinch-modified swipe variant should ever be emitted.
    assert not any(g.startswith("swipe_right+pinch") for g in fired)


# ---- macOS mode: cursor + click + scroll + bindings ----

def _write_default_config(tmp_path: Path) -> None:
    (tmp_path / "config.default.json").write_text('''{
        "active_mode": "macos", "sensitivity": 1.0, "smoothing": 0.0,
        "mount_orientation": "desktop", "gesture_cooldown_ms": 200,
        "modes": {
            "macos": {
                "enabled": true,
                "cursor_gesture": "point",
                "click_gesture": "pinch",
                "right_click_gesture": "two_fingers_pinch",
                "scroll": {"enabled": true, "natural": true, "sensitivity": 1.0},
                "bindings": {
                    "swipe_up+open_palm": "hotkey:ctrl+up",
                    "swipe_down+open_palm": "hotkey:ctrl+down",
                    "swipe_left+open_palm": "hotkey:ctrl+left",
                    "swipe_right+open_palm": "hotkey:ctrl+right",
                    "fist": "key:f4"
                }
            }
        }
    }''')


def test_macos_mode_point_moves_cursor(tmp_path):
    _write_default_config(tmp_path)
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    mode._screen_w, mode._screen_h = 1000, 1000
    moves = []
    ex.move_cursor = lambda x, y: moves.append((x, y))
    mode.handle_frame(_frame(0.0, (0.0, 200.0, 0.0), extended={"index": True}))
    assert moves and abs(moves[-1][0] - 500) < 5 and abs(moves[-1][1] - 500) < 5


def test_macos_mode_pinch_left_clicks(tmp_path):
    _write_default_config(tmp_path)
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    events = []
    mode.on_event(lambda g, a: events.append((g, a)))
    mode.handle_gesture("pinch", _frame(0.0, (0, 200, 0)))
    assert ("pinch", "mouse:left_click") in events


def test_macos_open_palm_swipe_up_triggers_mission_control(tmp_path):
    _write_default_config(tmp_path)
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    events = []
    mode.on_event(lambda g, a: events.append((g, a)))
    # macOS mode should prefer the pose-modified binding over the base one.
    mode.handle_gesture("swipe_up+open_palm", _frame(0.0, (0, 200, 0)))
    assert ("swipe_up+open_palm", "hotkey:ctrl+up") in events
    assert ex.last_action == "hotkey:ctrl+up"


def test_macos_fist_triggers_launchpad(tmp_path):
    _write_default_config(tmp_path)
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    mode.handle_gesture("fist", _frame(0.0, (0, 200, 0)))
    assert ex.last_action == "key:f4"


def test_macos_two_finger_motion_emits_scroll(tmp_path):
    """Sliding a two-finger pose down over ~40 mm produces at least one
    downward scroll click (natural direction)."""
    _write_default_config(tmp_path)
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    scrolls = []
    ex.scroll = lambda dx=0, dy=0: scrolls.append((dx, dy))
    ext = {"index": True, "middle": True}
    # move hand forward (desktop mount → screen Y up) then back down
    for i in range(20):
        # In desktop orientation vertical scroll axis is -pz.
        # Moving pz from 0 → -60 makes vertical axis go 0 → +60 (screen up).
        mode.handle_frame(_frame(i * 0.02, (0.0, 200.0, -3.0 * i), extended=ext))
    # There should be at least one non-zero dy scroll click.
    assert any(dy > 0 for dx, dy in scrolls), f"no upward scroll clicks: {scrolls}"


def test_macos_scroll_disabled_falls_back_to_cursor(tmp_path):
    """When scroll is disabled, two-finger pose shouldn't scroll — and it also
    shouldn't move the cursor (two_fingers isn't the cursor pose)."""
    _write_default_config(tmp_path)
    cfg = ConfigManager(tmp_path)
    cfg.data["modes"]["macos"]["scroll"]["enabled"] = False
    cfg.save()
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    scrolls = []
    moves = []
    ex.scroll = lambda dx=0, dy=0: scrolls.append((dx, dy))
    ex.move_cursor = lambda x, y: moves.append((x, y))
    ext = {"index": True, "middle": True}
    for i in range(10):
        mode.handle_frame(_frame(i * 0.02, (0.0, 200.0, -3.0 * i), extended=ext))
    assert scrolls == []
    assert moves == []  # two_fingers pose is neither scroll nor cursor


def test_media_mode_still_uses_base_swipe(tmp_path):
    """Regression: media mode binds bare 'swipe_left' etc. — a pose-modified
    swipe should still fire the base action if only the base is bound."""
    (tmp_path / "config.default.json").write_text('''{
        "active_mode": "media", "gesture_cooldown_ms": 200,
        "modes": {
            "media": {"enabled": true, "bindings": {"swipe_left": "media_previous"}}
        }
    }''')
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    mode = ModeController(cfg, ex)
    events = []
    mode.on_event(lambda g, a: events.append((g, a)))
    mode.handle_gesture("swipe_left+two_fingers", _frame(0.0, (0, 200, 0)))
    # Should fall back to base 'swipe_left' binding.
    assert ex.last_action == "media_previous"
    assert events and events[0][1] == "media_previous"
