import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.config_manager import ConfigManager  # noqa: E402


def _make_app_dir(tmp: Path) -> Path:
    default = {
        "active_mode": "mouse",
        "sensitivity": 1.2,
        "smoothing": 0.35,
        "gesture_cooldown_ms": 450,
        "modes": {
            "mouse": {"enabled": True, "click_gesture": "pinch"},
            "custom": {"enabled": True, "bindings": {"swipe_left": "hotkey:ctrl+z"}},
        },
    }
    (tmp / "config.default.json").write_text(json.dumps(default))
    return tmp


def test_loads_defaults(tmp_path):
    _make_app_dir(tmp_path)
    m = ConfigManager(tmp_path)
    assert m.data["active_mode"] == "mouse"
    assert m.data["modes"]["custom"]["bindings"]["swipe_left"] == "hotkey:ctrl+z"


def test_user_overlay_merges(tmp_path):
    """User file is source of truth once it exists; missing top-level keys
    fall back to defaults (forward-compat when new default keys are added)."""
    _make_app_dir(tmp_path)
    (tmp_path / "config.json").write_text(json.dumps({
        "active_mode": "media",
        "modes": {"custom": {"bindings": {"swipe_right": "key:right"}}},
    }))
    m = ConfigManager(tmp_path)
    assert m.data["active_mode"] == "media"
    assert m.data["modes"]["custom"]["bindings"]["swipe_right"] == "key:right"
    # top-level default keys missing from user file are backfilled
    assert m.data.get("sensitivity") == 1.2


def test_set_and_save(tmp_path):
    _make_app_dir(tmp_path)
    m = ConfigManager(tmp_path)
    m.set_binding("custom", "pinch", "key:enter")
    m.save()
    reloaded = ConfigManager(tmp_path)
    assert reloaded.data["modes"]["custom"]["bindings"]["pinch"] == "key:enter"


def test_remove_binding(tmp_path):
    _make_app_dir(tmp_path)
    m = ConfigManager(tmp_path)
    m.remove_binding("custom", "swipe_left")
    m.save()
    reloaded = ConfigManager(tmp_path)
    assert "swipe_left" not in reloaded.data["modes"]["custom"].get("bindings", {})


def test_reset_restores_defaults(tmp_path):
    _make_app_dir(tmp_path)
    m = ConfigManager(tmp_path)
    m.set("active_mode", "presentation")
    m.save()
    m.reset()
    assert m.data["active_mode"] == "mouse"


def test_corrupt_user_file_falls_back_to_defaults(tmp_path):
    _make_app_dir(tmp_path)
    (tmp_path / "config.json").write_text("{{not json")
    m = ConfigManager(tmp_path)
    assert m.data["active_mode"] == "mouse"


def test_hand_preference_persists(tmp_path):
    _make_app_dir(tmp_path)
    m = ConfigManager(tmp_path)
    m.set("hand_preference", "left")
    m.save()
    reloaded = ConfigManager(tmp_path)
    assert reloaded.data["hand_preference"] == "left"
    # 'both' should also round-trip cleanly.
    reloaded.set("hand_preference", "both")
    reloaded.save()
    again = ConfigManager(tmp_path)
    assert again.data["hand_preference"] == "both"
