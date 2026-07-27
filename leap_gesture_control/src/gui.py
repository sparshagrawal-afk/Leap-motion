"""Minimal-clean GUI built with customtkinter.

Layout:
  - Left sidebar: Mode selector (Mouse / Media / Presentation / Custom) + Start/Stop.
  - Main:
      * Status card (tracker status, active hand, FPS, last gesture, last action)
      * Bindings editor for the active mode
      * Sensitivity / smoothing / cooldown sliders
  - Bottom: log stream.
"""
from __future__ import annotations

import logging
import queue
import time
from pathlib import Path
from typing import Callable, Dict, Optional

import customtkinter as ctk

from .action_executor import ActionExecutor
from .config_manager import ConfigManager
from .gesture_recognizer import GestureRecognizer
from .hand_frame import HandFrame
from .leap_tracker import LeapTracker, is_available as leap_available
from .mock_tracker import MockTracker
from .mode_controller import ModeController

log = logging.getLogger("gesture.gui")

APP_TITLE = "Leap Gesture Control"
MODES = ["mouse", "media", "presentation", "custom"]
GESTURES = [
    "point", "open_palm", "fist", "thumbs_up", "two_fingers",
    "pinch", "two_fingers_pinch",
    "swipe_left", "swipe_right", "swipe_up", "swipe_down",
]
ACTION_PRESETS = [
    "media_play_pause", "media_next", "media_previous",
    "volume_up", "volume_down", "volume_mute",
    "key:right", "key:left", "key:up", "key:down",
    "key:space", "key:enter", "key:escape", "key:tab",
    "hotkey:ctrl+c", "hotkey:ctrl+v", "hotkey:ctrl+z", "hotkey:ctrl+y",
    "hotkey:ctrl+tab", "hotkey:ctrl+shift+tab",
    "mouse:left_click", "mouse:right_click", "mouse:double_click",
]


class GestureApp(ctk.CTk):
    def __init__(self, app_dir: Path, force_mock: bool = False):
        super().__init__()
        ctk.set_appearance_mode("light")
        ctk.set_default_color_theme("blue")

        self.title(APP_TITLE)
        self.geometry("1120x720")
        self.minsize(980, 640)

        self.app_dir = app_dir
        self.config_mgr = ConfigManager(app_dir)
        self.executor = ActionExecutor(dry_run=False)
        self.recognizer = GestureRecognizer(
            cooldown_ms=int(self.config_mgr.data.get("gesture_cooldown_ms", 450)),
            swipe_min_velocity=float(self.config_mgr.data.get("swipe_min_velocity_mm_s", 550.0)),
            pinch_threshold=float(self.config_mgr.data.get("pinch_threshold", 0.85)),
        )
        self.mode = ModeController(self.config_mgr, self.executor)
        self.recognizer.on_gesture(self._on_gesture)
        self.mode.on_event(self._on_mode_event)

        self.use_mock = force_mock or not leap_available()
        self.tracker = (MockTracker if self.use_mock else LeapTracker)(
            hand_preference=self.config_mgr.data.get("hand_preference", "right")
        )
        self.tracker.on_frame(self._on_frame)
        self.tracker.on_status(self._on_status)

        self._running = False
        self._log_q: "queue.Queue[str]" = queue.Queue()
        self._fps_frames = 0
        self._fps_t0 = time.time()
        self._current_fps = 0.0
        self._last_gesture = "—"
        self._last_action = "—"

        self._build_layout()
        self.after(150, self._poll_log_queue)
        self.after(400, self._refresh_status)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---------- layout ----------
    def _build_layout(self) -> None:
        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        # Sidebar ------------------------------------------------------------
        sidebar = ctk.CTkFrame(self, width=220, corner_radius=0)
        sidebar.grid(row=0, column=0, sticky="nsw")
        sidebar.grid_rowconfigure(7, weight=1)

        ctk.CTkLabel(sidebar, text=APP_TITLE, font=ctk.CTkFont(size=18, weight="bold")).grid(
            row=0, column=0, padx=20, pady=(24, 6), sticky="w"
        )
        subtitle = "Leap Motion 2 · Ultraleap" + ("  ·  (simulator)" if self.use_mock else "")
        ctk.CTkLabel(sidebar, text=subtitle, text_color=("#666", "#aaa"),
                     font=ctk.CTkFont(size=11)).grid(row=1, column=0, padx=20, pady=(0, 20), sticky="w")

        ctk.CTkLabel(sidebar, text="MODE", font=ctk.CTkFont(size=11, weight="bold"),
                     text_color=("#888", "#888")).grid(row=2, column=0, padx=20, pady=(4, 6), sticky="w")

        self.mode_var = ctk.StringVar(value=self.config_mgr.data.get("active_mode", "mouse"))
        for i, m in enumerate(MODES):
            btn = ctk.CTkRadioButton(
                sidebar, text=m.capitalize(), variable=self.mode_var, value=m,
                command=self._on_mode_change, font=ctk.CTkFont(size=14),
            )
            btn.grid(row=3 + i, column=0, padx=24, pady=6, sticky="w")

        self.toggle_btn = ctk.CTkButton(
            sidebar, text="Start Tracking", height=44,
            font=ctk.CTkFont(size=14, weight="bold"),
            command=self._toggle_tracking,
        )
        self.toggle_btn.grid(row=8, column=0, padx=20, pady=(0, 12), sticky="ew")

        self.reset_btn = ctk.CTkButton(
            sidebar, text="Reset to defaults", height=32, fg_color="transparent",
            border_width=1, text_color=("#333", "#ddd"),
            command=self._reset_config,
        )
        self.reset_btn.grid(row=9, column=0, padx=20, pady=(0, 20), sticky="ew")

        # Main area ----------------------------------------------------------
        main = ctk.CTkFrame(self, corner_radius=0, fg_color=("#f6f7f9", "#1a1a1a"))
        main.grid(row=0, column=1, sticky="nsew")
        main.grid_columnconfigure(0, weight=1)
        main.grid_rowconfigure(2, weight=1)

        # Status card
        status = ctk.CTkFrame(main, corner_radius=12)
        status.grid(row=0, column=0, padx=20, pady=(20, 10), sticky="ew")
        for c in range(4):
            status.grid_columnconfigure(c, weight=1)

        self.status_label = self._stat_cell(status, 0, "Tracker", "Disconnected")
        self.hand_label = self._stat_cell(status, 1, "Hand", self.config_mgr.data.get("hand_preference", "right"))
        self.fps_label = self._stat_cell(status, 2, "FPS", "0")
        self.gesture_label = self._stat_cell(status, 3, "Last gesture", "—")
        # second row of status
        self.action_label = self._stat_cell(status, 0, "Last action", "—", row=1, columnspan=4)

        # Tunables card
        tune = ctk.CTkFrame(main, corner_radius=12)
        tune.grid(row=1, column=0, padx=20, pady=10, sticky="ew")
        tune.grid_columnconfigure(1, weight=1)

        ctk.CTkLabel(tune, text="Cursor sensitivity", font=ctk.CTkFont(size=13)).grid(
            row=0, column=0, padx=(20, 12), pady=12, sticky="w")
        self.sens_slider = ctk.CTkSlider(tune, from_=0.4, to=2.5, number_of_steps=42,
                                         command=self._on_sensitivity_change)
        self.sens_slider.set(float(self.config_mgr.data.get("sensitivity", 1.2)))
        self.sens_slider.grid(row=0, column=1, padx=(0, 12), pady=12, sticky="ew")
        self.sens_val = ctk.CTkLabel(tune, text=f"{self.sens_slider.get():.2f}", width=42)
        self.sens_val.grid(row=0, column=2, padx=(0, 20), pady=12)

        ctk.CTkLabel(tune, text="Cursor smoothing", font=ctk.CTkFont(size=13)).grid(
            row=1, column=0, padx=(20, 12), pady=12, sticky="w")
        self.smooth_slider = ctk.CTkSlider(tune, from_=0.0, to=0.9, number_of_steps=45,
                                           command=self._on_smoothing_change)
        self.smooth_slider.set(float(self.config_mgr.data.get("smoothing", 0.35)))
        self.smooth_slider.grid(row=1, column=1, padx=(0, 12), pady=12, sticky="ew")
        self.smooth_val = ctk.CTkLabel(tune, text=f"{self.smooth_slider.get():.2f}", width=42)
        self.smooth_val.grid(row=1, column=2, padx=(0, 20), pady=12)

        ctk.CTkLabel(tune, text="Gesture cooldown (ms)", font=ctk.CTkFont(size=13)).grid(
            row=2, column=0, padx=(20, 12), pady=12, sticky="w")
        self.cool_slider = ctk.CTkSlider(tune, from_=150, to=1200, number_of_steps=42,
                                         command=self._on_cooldown_change)
        self.cool_slider.set(float(self.config_mgr.data.get("gesture_cooldown_ms", 450)))
        self.cool_slider.grid(row=2, column=1, padx=(0, 12), pady=12, sticky="ew")
        self.cool_val = ctk.CTkLabel(tune, text=f"{int(self.cool_slider.get())}", width=42)
        self.cool_val.grid(row=2, column=2, padx=(0, 20), pady=12)

        # Bindings + Log tabs
        self.tabs = ctk.CTkTabview(main, corner_radius=12)
        self.tabs.grid(row=2, column=0, padx=20, pady=(10, 20), sticky="nsew")
        self.tabs.add("Bindings")
        self.tabs.add("Activity Log")
        self._build_bindings_tab(self.tabs.tab("Bindings"))
        self._build_log_tab(self.tabs.tab("Activity Log"))

        if self.use_mock:
            self._append_log("[info] Ultraleap SDK not detected — running in simulator mode. "
                             "See README.md to install leapc-python-bindings for real hardware.")

    def _stat_cell(self, parent, col, title, value, row=0, columnspan=1):
        cell = ctk.CTkFrame(parent, fg_color="transparent")
        cell.grid(row=row, column=col, columnspan=columnspan, padx=16, pady=14, sticky="ew")
        ctk.CTkLabel(cell, text=title.upper(), font=ctk.CTkFont(size=10, weight="bold"),
                     text_color=("#999", "#777")).pack(anchor="w")
        lbl = ctk.CTkLabel(cell, text=value, font=ctk.CTkFont(size=17, weight="bold"))
        lbl.pack(anchor="w", pady=(2, 0))
        return lbl

    def _build_bindings_tab(self, parent) -> None:
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(1, weight=1)

        head = ctk.CTkFrame(parent, fg_color="transparent")
        head.grid(row=0, column=0, sticky="ew", padx=8, pady=(8, 6))
        ctk.CTkLabel(head, text="Gesture bindings for active mode",
                     font=ctk.CTkFont(size=14, weight="bold")).pack(side="left")
        ctk.CTkLabel(head, text="  (changes save automatically)",
                     text_color=("#888", "#888")).pack(side="left")

        self.bindings_frame = ctk.CTkScrollableFrame(parent, fg_color=("#ffffff", "#232323"))
        self.bindings_frame.grid(row=1, column=0, sticky="nsew", padx=8, pady=(0, 8))
        self.bindings_frame.grid_columnconfigure(1, weight=1)

        self._render_bindings()

    def _render_bindings(self) -> None:
        # clear
        for w in self.bindings_frame.winfo_children():
            w.destroy()
        active = self.mode_var.get()
        cfg = self.config_mgr.data["modes"].get(active, {})
        if active == "mouse":
            # special layout: cursor / click gestures
            rows = [
                ("Cursor pose", "cursor_gesture", ["point", "open_palm"]),
                ("Left click", "click_gesture", ["pinch", "fist", "thumbs_up"]),
                ("Right click", "right_click_gesture", ["two_fingers_pinch", "two_fingers", "thumbs_up"]),
            ]
            for i, (label, key, choices) in enumerate(rows):
                ctk.CTkLabel(self.bindings_frame, text=label, anchor="w",
                             font=ctk.CTkFont(size=13)).grid(row=i, column=0, padx=12, pady=8, sticky="w")
                var = ctk.StringVar(value=cfg.get(key, choices[0]))
                combo = ctk.CTkOptionMenu(
                    self.bindings_frame, variable=var, values=choices,
                    command=lambda v, k=key: self._set_mouse_field(k, v),
                    width=200,
                )
                combo.grid(row=i, column=1, padx=12, pady=8, sticky="w")
            return

        bindings: Dict[str, str] = cfg.get("bindings", {}) or {}
        for i, g in enumerate(GESTURES):
            ctk.CTkLabel(self.bindings_frame, text=g, anchor="w",
                         font=ctk.CTkFont(size=13)).grid(row=i, column=0, padx=12, pady=6, sticky="w")
            current = bindings.get(g, "")
            values = [""] + ACTION_PRESETS
            if current and current not in values:
                values.append(current)
            var = ctk.StringVar(value=current)
            combo = ctk.CTkComboBox(
                self.bindings_frame, values=values, variable=var, width=340,
                command=lambda v, gg=g: self._set_binding(gg, v),
            )
            combo.grid(row=i, column=1, padx=12, pady=6, sticky="w")
            # test button
            ctk.CTkButton(
                self.bindings_frame, text="Test", width=64,
                command=lambda gg=g: self._test_binding(gg),
            ).grid(row=i, column=2, padx=(0, 12), pady=6)

        if self.use_mock:
            ctk.CTkLabel(self.bindings_frame,
                         text="Simulator: use ‘Test’ buttons or the mock controls in Activity Log tab.",
                         text_color=("#777", "#aaa")).grid(row=len(GESTURES), column=0,
                                                            columnspan=3, padx=12, pady=10, sticky="w")

    def _build_log_tab(self, parent) -> None:
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(1, weight=1)

        if self.use_mock:
            controls = ctk.CTkFrame(parent, fg_color="transparent")
            controls.grid(row=0, column=0, sticky="ew", padx=8, pady=(8, 6))
            ctk.CTkLabel(controls, text="Simulator:",
                         font=ctk.CTkFont(size=12, weight="bold")).pack(side="left", padx=(4, 8))
            for pose in ["point", "pinch", "fist", "open_palm", "two_fingers", "thumbs_up",
                         "swipe_left", "swipe_right", "swipe_up", "swipe_down"]:
                ctk.CTkButton(controls, text=pose, width=110, height=28,
                              command=lambda p=pose: self._sim_pose(p)).pack(side="left", padx=3)

        self.log_box = ctk.CTkTextbox(parent, corner_radius=8, font=ctk.CTkFont(family="Menlo", size=12))
        self.log_box.grid(row=1, column=0, sticky="nsew", padx=8, pady=(0, 8))
        self.log_box.configure(state="disabled")

    # ---------- interaction ----------
    def _toggle_tracking(self) -> None:
        if not self._running:
            self.tracker.start()
            self._running = True
            self.toggle_btn.configure(text="Stop Tracking", fg_color="#c0392b", hover_color="#a93226")
            self._append_log("[info] Tracker started.")
        else:
            self.tracker.stop()
            self._running = False
            self.toggle_btn.configure(text="Start Tracking", fg_color=["#3B8ED0", "#1F6AA5"],
                                      hover_color=["#36719F", "#144870"])
            self._append_log("[info] Tracker stopped.")

    def _on_mode_change(self) -> None:
        self.config_mgr.set("active_mode", self.mode_var.get())
        self.config_mgr.save()
        self._render_bindings()
        self._append_log(f"[info] Mode → {self.mode_var.get()}")

    def _set_binding(self, gesture: str, value: str) -> None:
        active = self.mode_var.get()
        if not value:
            self.config_mgr.remove_binding(active, gesture)
        else:
            self.config_mgr.set_binding(active, gesture, value)
        self.config_mgr.save()

    def _set_mouse_field(self, key: str, value: str) -> None:
        self.config_mgr.data["modes"]["mouse"][key] = value
        self.config_mgr.save()

    def _test_binding(self, gesture: str) -> None:
        active = self.mode_var.get()
        cfg = self.config_mgr.data["modes"].get(active, {})
        if active == "mouse":
            if gesture == cfg.get("click_gesture"):
                self.executor.execute("mouse:left_click")
            elif gesture == cfg.get("right_click_gesture"):
                self.executor.execute("mouse:right_click")
            else:
                self._append_log(f"[test] no action for '{gesture}' in mouse mode")
            return
        action = (cfg.get("bindings") or {}).get(gesture)
        if not action:
            self._append_log(f"[test] no binding for '{gesture}'")
            return
        ok = self.executor.execute(action)
        self._append_log(f"[test] {gesture} → {action}  [{'OK' if ok else 'FAIL'}]")

    def _sim_pose(self, pose: str) -> None:
        if not self._running:
            self._append_log("[warn] start the tracker first")
            return
        if isinstance(self.tracker, MockTracker):
            self.tracker.set_pose(pose)

    def _on_sensitivity_change(self, v: float) -> None:
        self.config_mgr.set("sensitivity", round(float(v), 2))
        self.sens_val.configure(text=f"{float(v):.2f}")
        self.config_mgr.save()

    def _on_smoothing_change(self, v: float) -> None:
        self.config_mgr.set("smoothing", round(float(v), 2))
        self.smooth_val.configure(text=f"{float(v):.2f}")
        self.config_mgr.save()

    def _on_cooldown_change(self, v: float) -> None:
        ms = int(v)
        self.config_mgr.set("gesture_cooldown_ms", ms)
        self.cool_val.configure(text=str(ms))
        self.recognizer.cooldown_ms = ms
        self.config_mgr.save()

    def _reset_config(self) -> None:
        self.config_mgr.reset()
        self.mode_var.set(self.config_mgr.data.get("active_mode", "mouse"))
        self.sens_slider.set(float(self.config_mgr.data.get("sensitivity", 1.2)))
        self.smooth_slider.set(float(self.config_mgr.data.get("smoothing", 0.35)))
        self.cool_slider.set(float(self.config_mgr.data.get("gesture_cooldown_ms", 450)))
        self._on_sensitivity_change(self.sens_slider.get())
        self._on_smoothing_change(self.smooth_slider.get())
        self._on_cooldown_change(self.cool_slider.get())
        self._render_bindings()
        self._append_log("[info] Configuration reset to defaults.")

    # ---------- tracker callbacks (background thread) ----------
    def _on_frame(self, frame: HandFrame) -> None:
        self._fps_frames += 1
        # forward to recognizer + mode controller
        self.recognizer.process(frame)
        self.mode.handle_frame(frame)

    def _on_status(self, status: str) -> None:
        text = {
            "connected": "Connected",
            "connecting": "Connecting…",
            "connected_mock": "Simulator",
            "disconnected": "Disconnected",
            "sdk_missing": "SDK missing",
            "error": "Error",
        }.get(status, status)
        # marshal to UI thread
        self.after(0, lambda: self.status_label.configure(text=text))

    def _on_gesture(self, gesture: str, frame: HandFrame) -> None:
        self._last_gesture = gesture
        self.mode.handle_gesture(gesture, frame)

    def _on_mode_event(self, gesture: str, action: str) -> None:
        self._last_action = action or "—"
        self._append_log(f"[gesture] {gesture}  →  {action or 'no binding'}")

    # ---------- UI polling ----------
    def _refresh_status(self) -> None:
        # FPS
        now = time.time()
        elapsed = now - self._fps_t0
        if elapsed >= 1.0:
            self._current_fps = self._fps_frames / elapsed
            self._fps_frames = 0
            self._fps_t0 = now
        self.fps_label.configure(text=f"{self._current_fps:.0f}")
        self.gesture_label.configure(text=self._last_gesture)
        self.action_label.configure(text=self._last_action)
        self.after(300, self._refresh_status)

    def _append_log(self, msg: str) -> None:
        self._log_q.put(f"{time.strftime('%H:%M:%S')}  {msg}")

    def _poll_log_queue(self) -> None:
        drained = 0
        while not self._log_q.empty() and drained < 40:
            msg = self._log_q.get_nowait()
            self.log_box.configure(state="normal")
            self.log_box.insert("end", msg + "\n")
            self.log_box.see("end")
            self.log_box.configure(state="disabled")
            drained += 1
        self.after(200, self._poll_log_queue)

    def _on_close(self) -> None:
        try:
            self.tracker.stop()
        except Exception:  # noqa: BLE001
            pass
        self.destroy()
