# Leap Gesture Control

A minimal-clean desktop application that turns your **Ultraleap Leap Motion 2** hand tracking into system-wide actions: mouse control, media control, presentation slides, and fully customisable keyboard shortcuts.

Built with Python + `customtkinter` + the official **Ultraleap Gemini Python bindings** (`leapc-python-bindings`).

---

## Features

- 🖱️ **Mouse mode** — pointing pose moves the cursor, pinch = left-click, two-finger pinch = right-click. Cursor smoothing + adjustable sensitivity.
- 🎵 **Media mode** — swipe left/right for previous/next track, swipe up/down for volume, pinch to play/pause.
- 📽️ **Presentation mode** — swipe right/left to advance slides, pinch to toggle full-screen.
- 🎛️ **Custom mode** — bind any recognised gesture to any keystroke, hotkey combo, media command, or mouse action from the GUI.
- 🧠 Robust gesture recognizer (pose hold + velocity/displacement swipes + per-gesture cooldown) so a single motion never fires twice.
- 🕹️ **Built-in simulator** — the app works without hardware for testing/demoing. Just run `python main.py --mock` and drive gestures from the "Activity Log" tab.
- 📝 Config is a plain JSON file; every change in the UI saves automatically.

---

## Prerequisites (real-hardware mode)

1. **Ultraleap Leap Motion 2** (or any Ultraleap hand-tracking camera).
2. **Ultraleap Gemini Hand Tracking Software** installed on your machine — <https://developer.leapmotion.com/tracking-software-download>
3. **Python 3.10+**
4. **Ultraleap Python bindings** (`leapc-python-bindings`):
   ```bash
   git clone https://github.com/ultraleap/leapc-python-bindings.git
   cd leapc-python-bindings
   pip install -r requirements.txt
   pip install -e leapc-python-api
   ```
   If your SDK is not in the default location, set `LEAPSDK_INSTALL_LOCATION` before installing.

If any of these are missing, the app automatically falls back to **simulator mode**.

## Install app dependencies

```bash
cd leap_gesture_control
pip install -r requirements.txt
```

## Run

```bash
python main.py                # GUI, real hardware if available
python main.py --mock         # GUI in simulator mode
python main.py --headless     # no GUI (runs recognizer + actions in the background)
python main.py -v             # verbose logs
```

Windows users: no extra step. macOS users: grant **Accessibility** permission to the terminal/Python interpreter (System Settings → Privacy & Security → Accessibility) so `pyautogui`/`pynput` can move the mouse and send keys. Linux (X11) works out of the box; on Wayland, run in an XWayland session.

---

## Gestures

| Gesture             | How to make it                                             |
| ------------------- | ---------------------------------------------------------- |
| `point`             | Only the index finger extended                             |
| `two_fingers`       | Index + middle finger extended                             |
| `thumbs_up`         | Only the thumb extended                                    |
| `open_palm`         | All five fingers extended                                  |
| `fist`              | All fingers closed (grab strength ≥ 0.85)                  |
| `pinch`             | Thumb + index tips touching (pinch strength ≥ 0.85)        |
| `two_fingers_pinch` | Pinch with middle finger extended                          |
| `swipe_left/right`  | Horizontal palm motion (≥ 90 mm & ≥ 550 mm/s)              |
| `swipe_up/down`     | Vertical palm motion                                       |

## Default action bindings

**Media mode**
- `swipe_left` → previous track
- `swipe_right` → next track
- `swipe_up` → volume up
- `swipe_down` → volume down
- `pinch` → play / pause

**Presentation mode**
- `swipe_right` → Right arrow (next slide)
- `swipe_left` → Left arrow (previous slide)
- `swipe_up` → Escape (exit fullscreen)
- `pinch` → Space (start / pause presentation)

**Custom mode** (fully user-editable)
- `swipe_left` → Ctrl+Z
- `swipe_right` → Ctrl+Y
- `pinch` → Enter
- `thumbs_up` → Ctrl+C
- `two_fingers` → Ctrl+V

All bindings can be changed in the **Bindings** tab. Use the **Test** button next to any binding to fire the action without gesturing.

## Binding syntax

You can type any custom string in the binding combobox:

| Prefix       | Example                | Effect                          |
| ------------ | ---------------------- | ------------------------------- |
| _(built-in)_ | `media_play_pause`     | Media key play/pause            |
| _(built-in)_ | `media_next`           | Media key next                  |
| _(built-in)_ | `media_previous`       | Media key previous              |
| _(built-in)_ | `volume_up` / `_down`  | Media key volume                |
| `key:`       | `key:right`            | Press a single key              |
| `hotkey:`    | `hotkey:ctrl+shift+t`  | Press a key combo               |
| `mouse:`     | `mouse:left_click`     | Left / right / double click     |
| `text:`      | `text:hello world`     | Type the given text             |

---

## Development / tests

The recognizer and config manager have zero hardware dependencies. Run:

```bash
pip install pytest
pytest -q
```

Included suites:
- `tests/test_gesture_recognizer.py` — pose + swipe detection, cooldown behaviour.
- `tests/test_config_manager.py`     — defaults, deep-merged user overrides, save/reset.
- `tests/test_action_executor.py`    — binding parsing (dry-run).
- `tests/test_end_to_end_mock.py`    — full MockTracker → recognizer → mode → executor loop.

---

## Project layout

```
leap_gesture_control/
├── main.py                       # entry point (GUI / headless / --mock)
├── config.default.json           # factory defaults
├── requirements.txt
├── src/
│   ├── hand_frame.py             # SDK-agnostic hand data types
│   ├── leap_tracker.py           # Ultraleap Gemini bindings adapter
│   ├── mock_tracker.py           # scripted hand-frame producer (no hw)
│   ├── gesture_recognizer.py     # poses + swipes + cooldown
│   ├── action_executor.py        # media / key / hotkey / mouse
│   ├── mode_controller.py        # gesture → binding dispatch + cursor mapping
│   ├── config_manager.py
│   └── gui.py                    # customtkinter UI
└── tests/
```

---

## Troubleshooting

- **`Ultraleap SDK not detected`** in the app header → install the Gemini Hand Tracking Software **and** `leapc-python-bindings`.  Restart the app.
- **Cursor doesn’t move** in Mouse mode → make sure your active pose is `point` (only index extended). Increase `sensitivity` and lower `smoothing` if it feels sluggish.
- **Media keys ignored on Linux** → install `playerctl` and remap the media entries to `hotkey:` shortcuts your desktop understands.
- **Gestures fire twice** → increase the `gesture_cooldown_ms` slider (default 450 ms).

---

## License

MIT (do whatever you want, no warranty).
