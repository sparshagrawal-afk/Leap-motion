# Leap Gesture Control — PRD

## Original problem statement
> Gesture control software using Leap Motion 2

## User choices (from clarification)
- **Type**: Desktop app (Python + Ultraleap LeapC SDK) that reads hand tracking and triggers system actions
- **Actions supported**: All (mouse control, media control, presentation control, custom keyboard shortcuts)
- **Hardware**: User owns Leap Motion 2 + has Ultraleap Gemini SDK installed locally
- **UI style**: Minimal clean

## Architecture
- Pure Python desktop application (runs on Windows / macOS / Linux) — no cloud stack.
- GUI: `customtkinter` (minimal, clean, modern).
- Hardware layer: `leapc-python-bindings` (Ultraleap Gemini SDK). Auto-falls-back to an in-process simulator if bindings are missing.
- Action layer: `pyautogui` + `pynput` for mouse / keyboard / media keys.
- Config: single JSON file (`config.json`) alongside `config.default.json`.

## Module layout
```
leap_gesture_control/
├── main.py                       # entry point (GUI / --headless / --mock)
├── config.default.json           # factory defaults
├── requirements.txt
├── README.md
├── src/
│   ├── hand_frame.py             # SDK-agnostic hand data types
│   ├── leap_tracker.py           # Ultraleap Gemini bindings adapter
│   ├── mock_tracker.py           # scripted hand-frame producer (no hw)
│   ├── gesture_recognizer.py     # 7 poses + 4 swipe motions + cooldown
│   ├── action_executor.py        # media / key / hotkey / mouse / text
│   ├── mode_controller.py        # gesture → binding + cursor mapping
│   ├── config_manager.py
│   └── gui.py                    # customtkinter UI
└── tests/                        # 24 unit + e2e tests (pytest)
```

## Implemented (2026-01-27)
- ✅ Hardware adapter for Ultraleap Gemini Python bindings (`LeapTracker`) with automatic fallback to `MockTracker`.
- ✅ Gesture recognizer with 7 poses (`point`, `open_palm`, `fist`, `thumbs_up`, `two_fingers`, `pinch`, `two_fingers_pinch`) + 4 swipes (`swipe_left/right/up/down`) + `fist_move`.
- ✅ Cooldown/debounce so a single motion never fires twice; velocity + displacement thresholds for swipes.
- ✅ Four modes with defaults:
  - **Mouse**: `point` → cursor, `pinch` → left-click, `two_fingers_pinch` → right-click.
  - **Media**: swipes + pinch → play/pause, prev/next, vol±.
  - **Presentation**: swipes → arrow keys, pinch → space.
  - **Custom**: user-editable, defaults include undo/redo/copy/paste/tab switching.
- ✅ Cursor movement with sensitivity + smoothing sliders and screen-size auto-detection.
- ✅ Configurable action strings: `media_*`, `volume_*`, `key:*`, `hotkey:*`, `mouse:*`, `text:*`.
- ✅ Minimal-clean customtkinter GUI: sidebar (mode + start/stop), status card (tracker/hand/FPS/last gesture/last action), tunables card (sensitivity/smoothing/cooldown sliders + sensor-mount toggle), Bindings tab (per-mode editor with Test button), Activity Log tab, simulator controls (only when running mock).
- ✅ Autosave on every UI change; reset-to-defaults button.
- ✅ Headless mode (`main.py --headless`) for background use with no window.
- ✅ Simulator mode (`main.py --mock`) — full app without hardware.
- ✅ Test suite: **28/28 passing** — recognizer poses/swipes/cooldown, config save/load/reset/deletion, executor binding parsing, and a full MockTracker → recognizer → mode → executor e2e loop.

## Quality pass (2026-01-27)
- ✅ Swipe detection now uses average velocity from displacement/time (more robust than SDK's instantaneous per-frame velocity).
- ✅ Stale-history reset: if the tracked hand disappears for >300ms, motion history is cleared so no phantom swipe fires when the hand re-enters at a different location.
- ✅ Mouse mode silently ignores fires of the configured cursor pose — no more "no binding" log spam when the user simply points.
- ✅ New `mount_orientation` config (`desktop` | `screen_top`) with UI toggle; picks the correct axis-to-screen mapping for either desk-mounted or monitor-clipped sensors.
- ✅ Added 4 new tests covering the four improvements above.
- ✅ Unused imports cleaned up; codebase lints clean under ruff.

## macOS mode (2026-01-27)
- ✅ New **macOS mode** — trackpad-parity gestures alongside the existing four modes.
- ✅ **Pose-modified swipes** — recognizer now emits both the base swipe (`swipe_up`) and a pose-tagged variant (`swipe_up+open_palm`, `swipe_left+two_fingers`, …) so modes can distinguish e.g. a plain swipe from a Mission Control gesture. Base events still fire, so existing modes are unaffected.
- ✅ **Continuous two-finger scroll** — index + middle extended + palm motion produces per-frame scroll clicks in the executor. Configurable natural/traditional direction, per-mode enable toggle, sensitivity-aware click threshold. Falls through cleanly to cursor movement when scroll is disabled.
- ✅ ActionExecutor: `scroll(dx, dy)` supports both vertical (mouse wheel) and horizontal (pyautogui hscroll on macOS, Shift+scroll fallback elsewhere).
- ✅ Default macOS bindings:
  - `pinch` → left click, `two_fingers_pinch` → right click
  - `swipe_up+open_palm` → Ctrl+↑ (Mission Control)
  - `swipe_down+open_palm` → Ctrl+↓ (App Exposé)
  - `swipe_left/right+open_palm` → Ctrl+←/→ (switch Spaces)
  - `fist` → F4 (Launchpad)
- ✅ GUI: mode radio adds "macOS", bindings tab exposes the shared cursor/click rows plus scroll-enable and natural-scroll switches; the gesture list is expanded to include all pose-modified swipe variants.
- ✅ Fallback: modes that bind bare `swipe_left` still work when a pose-modified `swipe_left+two_fingers` fires — the controller looks up the modifier first, then the base.
- ✅ Test suite: **38/38 passing** (28 previous + 10 new covering pose-modified swipes, macOS cursor/click, Mission Control binding, Launchpad, scroll continuous emission, scroll-disabled fallback, base-swipe regression).

## Test verification
- `pytest tests/ -q` → 24 passed.
- `python main.py --headless --mock` boots cleanly (OS action backends warn only because this cloud sandbox has no display — expected; will work on user machine).
- Lint: no errors (`ruff` via mcp_lint_python).

## Notes on environment
- This is a **desktop** app, not a web app. It cannot be tested with the automated browser testing agent — it has no HTTP surface and requires a display + physical Leap Motion 2. Testing was performed with an offline unit + integration suite (24 tests) using a mock tracker that replays scripted hand frames through the real recognizer/executor pipeline.
- Users run `pip install -r requirements.txt` + `pip install -e leapc-python-api` (from the leapc-python-bindings repo) once, then `python main.py`.

## Backlog / next candidates (P1)
- Overlay HUD (transparent on-screen indicator showing current pose + last gesture) using a topmost tkinter window.
- Per-application profiles (auto-switch mode based on foreground window title on Windows/macOS).
- Recording a gesture from the device and using DTW to match custom user-defined gestures.
- Auto-start on login (systemtray icon).
- Web dashboard for configuration if the user later wants remote setup.

## Backlog / P2
- Pinch-drag ("grab & throw") gesture combining pose + motion.
- Two-handed gestures (zoom, rotate) mapped to Ctrl+scroll / Ctrl+Shift+scroll.
- Import/export config profiles.
