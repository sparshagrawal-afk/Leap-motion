import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.mock_tracker import MockTracker  # noqa: E402
from src.gesture_recognizer import GestureRecognizer  # noqa: E402
from src.action_executor import ActionExecutor  # noqa: E402
from src.config_manager import ConfigManager  # noqa: E402
from src.mode_controller import ModeController  # noqa: E402


def test_mock_tracker_end_to_end_swipe_right_triggers_media_next(tmp_path):
    # bootstrap a minimal config
    (tmp_path / "config.default.json").write_text('''{
        "active_mode": "media",
        "sensitivity": 1.0, "smoothing": 0.3, "gesture_cooldown_ms": 200,
        "swipe_min_velocity_mm_s": 400, "pinch_threshold": 0.85,
        "modes": {
            "mouse": {"enabled": true, "cursor_gesture": "point", "click_gesture": "pinch"},
            "media": {"enabled": true, "bindings": {"swipe_right": "media_next"}}
        }
    }''')
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    rec = GestureRecognizer(cooldown_ms=200, swipe_min_velocity=400)
    mode = ModeController(cfg, ex)

    events: list[tuple[str, str]] = []
    mode.on_event(lambda g, a: events.append((g, a)))
    rec.on_gesture(mode.handle_gesture)

    tracker = MockTracker()
    tracker.on_frame(rec.process)
    tracker.start()
    time.sleep(0.15)
    tracker.set_pose("swipe_right")
    # let it produce frames for ~0.6s so recognizer window can trigger
    time.sleep(0.7)
    tracker.stop()

    swipe_events = [e for e in events if e[0] == "swipe_right"]
    assert swipe_events, f"no swipe_right event; got {events}"
    assert swipe_events[0][1] == "media_next"
    assert ex.last_action == "media_next"


def test_mock_tracker_pinch_triggers_click_in_mouse_mode(tmp_path):
    (tmp_path / "config.default.json").write_text('''{
        "active_mode": "mouse",
        "sensitivity": 1.0, "smoothing": 0.3, "gesture_cooldown_ms": 200,
        "swipe_min_velocity_mm_s": 400, "pinch_threshold": 0.85,
        "modes": {
            "mouse": {"enabled": true, "cursor_gesture": "point",
                      "click_gesture": "pinch", "right_click_gesture": "two_fingers_pinch"}
        }
    }''')
    cfg = ConfigManager(tmp_path)
    ex = ActionExecutor(dry_run=True)
    rec = GestureRecognizer(cooldown_ms=200)
    mode = ModeController(cfg, ex)
    events = []
    mode.on_event(lambda g, a: events.append((g, a)))
    rec.on_gesture(mode.handle_gesture)

    tracker = MockTracker()
    tracker.on_frame(rec.process)
    tracker.start()
    tracker.set_pose("pinch")
    time.sleep(0.5)
    tracker.stop()

    pinch_events = [e for e in events if e[0] == "pinch"]
    assert pinch_events, f"no pinch event; got {events}"
    assert pinch_events[0][1] == "mouse:left_click"
