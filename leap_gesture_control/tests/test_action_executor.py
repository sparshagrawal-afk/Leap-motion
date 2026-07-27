import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.action_executor import ActionExecutor  # noqa: E402


def test_dry_run_records_last_action():
    ex = ActionExecutor(dry_run=True)
    assert ex.execute("media_play_pause") is True
    assert ex.last_action == "media_play_pause"
    assert ex.execute("hotkey:ctrl+c") is True
    assert ex.last_action == "hotkey:ctrl+c"


def test_dry_run_accepts_key_binding():
    ex = ActionExecutor(dry_run=True)
    assert ex.execute("key:right") is True


def test_dry_run_accepts_mouse_binding():
    ex = ActionExecutor(dry_run=True)
    assert ex.execute("mouse:left_click") is True


def test_empty_binding_returns_false():
    ex = ActionExecutor(dry_run=True)
    assert ex.execute("") is False


def test_dry_run_move_cursor_and_scroll_no_op():
    ex = ActionExecutor(dry_run=True)
    ex.move_cursor(100, 200)   # should not raise
    ex.scroll(3)               # should not raise
