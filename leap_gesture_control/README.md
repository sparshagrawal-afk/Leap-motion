# Leap Gesture Control

A minimal-clean desktop app that turns your **Ultraleap Leap Motion 2** into a hands-free trackpad-gesture layer for macOS. Wave your hand — trigger Mission Control, App Exposé, switch Spaces, launch apps, and scroll.

Built with Python + `customtkinter` + the official **Ultraleap Gemini Python bindings** (`leapc-python-bindings`).

---

## Features

- 🖱️ **Two-finger scroll** — extend index + middle finger, move your hand; the OS scrolls in that direction. Natural (macOS default) or traditional direction, sensitivity-tunable.
- 🚦 **Mission Control** — open-palm swipe **up** (Ctrl+↑)
- 📋 **App Exposé** — open-palm swipe **down** (Ctrl+↓)
- 🔄 **Switch Spaces** — open-palm swipe **left / right** (Ctrl+←/→)
- 🚀 **Launchpad** — close your fist (F4)
- 🎚️ **Master switch** — pause all OS actions in one click while the tracker keeps running.
- 🕹️ **Simulator mode** — the app works without hardware for demos: `python main.py --mock`.
- 📝 Config is a plain JSON file; every change in the UI saves automatically.

---

## Prerequisites

1. **Ultraleap Leap Motion 2** (or any Ultraleap hand-tracking camera).
2. **Ultraleap Gemini Hand Tracking Software** — <https://leap2.ultraleap.com/downloads/leap-motion-controller-2/>
3. **Python 3.10+**
4. **Ultraleap Python bindings** (`leapc-python-bindings`):
   ```bash
   git clone https://github.com/ultraleap/leapc-python-bindings.git
   cd leapc-python-bindings
   pip install -r requirements.txt
   pip install -e leapc-python-api
   ```

If any of these are missing, the app falls back to **simulator mode**.

## Run

```bash
cd leap_gesture_control
pip install -r requirements.txt
python main.py              # real hardware if available
python main.py --mock       # simulator (no hardware)
python main.py --headless   # background service, no GUI
```

**macOS**: grant **Accessibility** permission to your Terminal/IDE so `pyautogui`/`pynput` can send keystrokes and scroll wheel events. *System Settings → Privacy & Security → Accessibility*.

---

## The GUI

- **Sidebar**: **Start Tracking** button, **Gestures active** master switch (toggles OFF to silence every OS side-effect while the tracker keeps running), **Reset to defaults**.
- **Status card**: tracker state · tracked hand · FPS · last gesture · last action.
- **Tunables**: scroll sensitivity, gesture cooldown, sensor mount (`desktop` / `screen_top`), tracked hand (`left` / `right` / `both`).
- **Bindings tab**: scroll on/off + natural-scroll toggle, and a mapping row for every recognised gesture with a **Test** button next to each.
- **Activity Log tab**: rolling log of every gesture fired and its resulting action.

---

## Gestures & default bindings

| Gesture | How to make it | Default action |
| --- | --- | --- |
| `two_fingers` motion | Extend index + middle, move hand | **Scroll** (natural direction) |
| `swipe_up+open_palm` | All 5 fingers extended, swipe up | Ctrl+↑ · Mission Control |
| `swipe_down+open_palm` | All 5, swipe down | Ctrl+↓ · App Exposé |
| `swipe_left+open_palm` | All 5, swipe left | Ctrl+← · previous Space |
| `swipe_right+open_palm` | All 5, swipe right | Ctrl+→ · next Space |
| `fist` | All fingers closed | F4 · Launchpad |

Every binding is editable in the Bindings tab. Binding syntax:

| Prefix | Example | Effect |
| --- | --- | --- |
| _(built-in)_ | `media_play_pause` / `media_next` / `volume_up` … | Media keys |
| `key:` | `key:right`, `key:f4` | Single key |
| `hotkey:` | `hotkey:ctrl+shift+t` | Key combo |
| `text:` | `text:hello world` | Type text |

---

## Sensor mounting orientation

Choose from the **Sensor mount** control in the tunables card:

- **`desktop`** — sensor flat on the desk facing up. Hand moves in the horizontal x/z plane; scroll direction is `-z` (forward = up).
- **`screen_top`** — sensor clipped to top of monitor facing the user. Hand moves in the x/y plane; scroll direction is `y` (up = up).

Change any time — no restart.

---

## Tests

```bash
pip install pytest
pytest -q
```

38 tests covering the recognizer (pose + swipe + cooldown + hand-lost reset + pose-modified swipe emission), config load/save/reset, executor binding parsing, macOS mode dispatch, master switch, and a full mock-tracker → recognizer → mode → executor loop.

---

## Project layout

```
leap_gesture_control/
├── main.py                 # entry point (GUI / --mock / --headless)
├── config.default.json     # factory defaults
├── requirements.txt
├── README.md
├── src/
│   ├── hand_frame.py       # SDK-agnostic data types
│   ├── leap_tracker.py     # Ultraleap Gemini adapter
│   ├── mock_tracker.py     # scripted frames, no hardware
│   ├── gesture_recognizer.py
│   ├── action_executor.py  # media / key / hotkey / scroll
│   ├── mode_controller.py  # scroll + gesture dispatch + master switch
│   ├── config_manager.py
│   └── gui.py              # customtkinter UI
└── tests/                  # 38 pytest tests
```

---

## Troubleshooting

- **App shows "SDK missing"** → install the Gemini tracking software AND `leapc-python-bindings` in the same Python venv you're launching from.
- **Nothing tracks** → click **Start Tracking**, and check the status card FPS is non-zero. Verify Ultraleap Visualizer still sees your hand.
- **Only tracks one hand** → change **Tracked hand** in the tunables card to `left`, `right`, or `both`.
- **Scroll fires too easily / not easily** → move the **Scroll sensitivity** slider (lower = coarser, higher = finer).
- **Gestures fire twice** → increase **Gesture cooldown (ms)**.
- **You want to pause without stopping the tracker** → flip the sidebar **Gestures active** switch to off.

## License

MIT.
