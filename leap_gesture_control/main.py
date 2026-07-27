"""Entry point: launch the Leap Gesture Control desktop app.

Usage:
    python main.py            # try real hardware, fall back to simulator if SDK missing
    python main.py --mock     # force simulator mode (no hardware needed)
    python main.py --headless # start tracker+recognizer without GUI (CLI mode)
"""
from __future__ import annotations

import argparse
import logging
import signal
import sys
import time
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(APP_DIR))

from src.action_executor import ActionExecutor        # noqa: E402
from src.config_manager import ConfigManager          # noqa: E402
from src.gesture_recognizer import GestureRecognizer  # noqa: E402
from src.leap_tracker import LeapTracker, is_available as leap_available  # noqa: E402
from src.mock_tracker import MockTracker              # noqa: E402
from src.mode_controller import ModeController       # noqa: E402


def _setup_logging(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )


def run_headless(force_mock: bool = False) -> None:
    """Runs tracker + recognizer + mode controller without any GUI."""
    cfg = ConfigManager(APP_DIR)
    executor = ActionExecutor(dry_run=False)
    recognizer = GestureRecognizer(
        cooldown_ms=int(cfg.data.get("gesture_cooldown_ms", 450)),
        swipe_min_velocity=float(cfg.data.get("swipe_min_velocity_mm_s", 550.0)),
        pinch_threshold=float(cfg.data.get("pinch_threshold", 0.85)),
    )
    mode = ModeController(cfg, executor)
    mode.on_event(lambda g, a: print(f"[gesture] {g} -> {a or 'no binding'}"))

    use_mock = force_mock or not leap_available()
    tracker = (MockTracker if use_mock else LeapTracker)(
        hand_preference=cfg.data.get("hand_preference", "right")
    )
    if use_mock:
        print("[info] Ultraleap SDK not found — running headless simulator.")

    def on_frame(frame):
        recognizer.process(frame)
        mode.handle_frame(frame)

    def on_gesture(g, f):
        mode.handle_gesture(g, f)

    tracker.on_frame(on_frame)
    tracker.on_status(lambda s: print(f"[tracker] {s}"))
    recognizer.on_gesture(on_gesture)
    tracker.start()
    print("[info] Headless mode active. Ctrl+C to quit.")

    stop = {"v": False}
    def _sig(_a, _b): stop["v"] = True
    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)
    while not stop["v"]:
        time.sleep(0.2)
    tracker.stop()


def run_gui(force_mock: bool = False) -> None:
    # Import here so headless mode doesn't require a display.
    from src.gui import GestureApp
    app = GestureApp(APP_DIR, force_mock=force_mock)
    app.mainloop()


def main() -> None:
    p = argparse.ArgumentParser(description="Leap Motion 2 Gesture Control")
    p.add_argument("--mock", action="store_true", help="Force simulator mode (no hardware needed)")
    p.add_argument("--headless", action="store_true", help="Run without GUI")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()
    _setup_logging(args.verbose)

    if args.headless:
        run_headless(force_mock=args.mock)
    else:
        run_gui(force_mock=args.mock)


if __name__ == "__main__":
    main()
