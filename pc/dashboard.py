"""Wii U Stream - the PC end.

Shows the console's video, and gives Discord something to point at.

Discord has no ingest you can push to: there is no RTMP endpoint, and Go Live
is a proprietary WebRTC flow that bots cannot join. So "straight to Discord"
means giving the Discord client a source it already understands. This window
is that source - share it as an application window, or turn on the virtual
camera and pick it as your webcam. Both are covered in the Discord panel.

Rendered with Dear ImGui (imgui-bundle), same stack and same palette as the
AFK bot and bk-wiiu dashboards.

Usage:
    python dashboard.py
"""

import json
import os
import threading
import time
from pathlib import Path

import cv2
import numpy as np
from imgui_bundle import ImVec2, ImVec4, hello_imgui, imgui, immvision

import audio_out
import receiver

try:
    import pyvirtualcam
except Exception:      # optional - the window-share path needs nothing extra
    pyvirtualcam = None

HERE = Path(__file__).resolve().parent
SETTINGS_FILE = HERE / "settings.json"
CAPTURE_DIR = HERE.parent / "captures"

# --- noir palette, shared with the other dashboards in this workspace ---
BLACK_DEEP = ImVec4(0.031, 0.031, 0.039, 1.00)
BLACK_PANEL = ImVec4(0.055, 0.055, 0.067, 1.00)
BLACK_FRAME = ImVec4(0.086, 0.086, 0.102, 1.00)
BLACK_RAISED = ImVec4(0.129, 0.129, 0.153, 1.00)
BORDER = ImVec4(0.137, 0.137, 0.165, 1.00)

RED = ImVec4(0.855, 0.145, 0.220, 1.00)
RED_BRIGHT = ImVec4(1.000, 0.263, 0.333, 1.00)
RED_DIM = ImVec4(0.400, 0.063, 0.098, 1.00)
RED_GHOST = ImVec4(0.855, 0.145, 0.220, 0.26)

TEXT = ImVec4(0.918, 0.918, 0.937, 1.00)
TEXT_DIM = ImVec4(0.510, 0.510, 0.565, 1.00)
TEXT_MUTE = ImVec4(0.325, 0.325, 0.373, 1.00)

OK_GREEN = ImVec4(0.325, 0.788, 0.443, 1.00)
AMBER = ImVec4(0.898, 0.667, 0.271, 1.00)

FONTS = {}

# Presets rather than free numbers: the console's encoder is a 1.24 GHz
# PowerPC, and the useful range between "sharp but 8 fps" and "smooth but
# soft" is narrow enough to name.
RESOLUTIONS = [
    ("320 x 180", 320, 180),
    ("480 x 270", 480, 270),
    ("640 x 360", 640, 360),
    ("854 x 480", 854, 480),
    ("1280 x 720", 1280, 720),
]

SIDEBAR_W = 330.0

# Named starting points, because the three dials interact and the useful
# combinations are not obvious from the sliders alone: resolution drives the
# console's CPU cost, quality drives the bytes, and the link carries about
# 7 Mbit/s. Each of these is a coherent trade rather than a set of numbers.
# Targets are what the console was measured doing, not aspirations - an
# unreachable target just makes the adapter degrade the picture chasing it.
PRESETS = [
    ("Smooth",     320, 180, 30, 65, "~30 fps, softest picture"),
    ("Balanced",   480, 270, 25, 75, "~22 fps; the one to start from"),
    ("Sharp",      640, 360, 20, 80, "~18 fps, most detail"),
    ("Still",     1280, 720,  5, 95, "for screenshots, not for watching"),
]

# How the console waits for the GPU to finish the capture copy.
SYNC_MODES = [
    ("Timestamp", "worker waits for our own submission only"),
    ("None",      "no wait - fastest, may read a torn frame"),
    ("DrawDone",  "drains the pipeline on the render thread (this froze it)"),
]

# The capture stages, in the order the console walks them. Each label says what
# the console is allowed to reach, and the note says what a freeze there means.
STAGES = [
    ("Off",              "receives settings, touches nothing else"),
    ("Send",             "+ status and log packets from the render thread"),
    ("Allocate",         "+ allocates the capture surfaces"),
    ("Copy",             "+ GX2CopySurface, never waits or reads"),
    ("Readback",         "+ GX2DrawDone and cache invalidate"),
    ("Full",             "+ downscale, encode and send"),
]


def load_ui_fonts():
    io = imgui.get_io()
    fonts_dir = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"

    def add(filename, size, key):
        path = fonts_dir / filename
        if not path.exists():
            return None
        cfg = imgui.ImFontConfig()
        cfg.oversample_h = 3
        try:
            FONTS[key] = (io.fonts.add_font_from_file_ttf(str(path), size, cfg), size)
        except Exception:
            return None
        return FONTS[key]

    add("segoeui.ttf", 17.0, "ui")
    add("seguisb.ttf", 17.0, "semi")
    add("seguisb.ttf", 21.0, "title")
    add("CascadiaMono.ttf", 15.0, "mono") or add("consola.ttf", 15.0, "mono")

    if FONTS.get("ui") is not None:
        io.font_default = FONTS["ui"][0]


class push_font:
    """`with push_font("semi"):` - falls through silently if the face is
    missing, so a machine without Segoe still renders."""

    def __init__(self, key):
        self.entry = FONTS.get(key)

    def __enter__(self):
        if self.entry is not None:
            imgui.push_font(self.entry[0], self.entry[1])
        return self

    def __exit__(self, *exc):
        if self.entry is not None:
            imgui.pop_font()
        return False


def apply_noir_style():
    s = imgui.get_style()

    s.window_rounding = 10.0
    s.child_rounding = 9.0
    s.frame_rounding = 7.0
    s.popup_rounding = 9.0
    s.scrollbar_rounding = 9.0
    s.grab_rounding = 7.0
    s.tab_rounding = 7.0
    for attr in ("selectable_rounding", "menu_item_rounding", "image_rounding"):
        if hasattr(s, attr):
            setattr(s, attr, 6.0)

    s.window_border_size = 1.0
    s.child_border_size = 1.0
    s.frame_border_size = 0.0
    s.popup_border_size = 1.0
    s.tab_bar_border_size = 0.0

    s.window_padding = ImVec2(16, 14)
    s.frame_padding = ImVec2(14, 8)
    s.item_spacing = ImVec2(10, 10)
    s.item_inner_spacing = ImVec2(8, 6)
    s.cell_padding = ImVec2(10, 7)
    s.scrollbar_size = 11.0
    s.grab_min_size = 11.0

    def set_col(idx, col):
        if idx is None:
            return
        try:
            s.set_color_(int(idx.value if hasattr(idx, "value") else idx), col)
        except Exception:
            pass

    C = imgui.Col_
    set_col(C.text, TEXT)
    set_col(C.text_disabled, TEXT_MUTE)
    set_col(C.window_bg, BLACK_DEEP)
    set_col(C.child_bg, BLACK_PANEL)
    set_col(C.popup_bg, BLACK_PANEL)
    set_col(C.border, BORDER)
    set_col(C.border_shadow, ImVec4(0, 0, 0, 0))

    set_col(C.frame_bg, BLACK_FRAME)
    set_col(C.frame_bg_hovered, BLACK_RAISED)
    set_col(C.frame_bg_active, BLACK_RAISED)

    set_col(C.title_bg, BLACK_DEEP)
    set_col(C.title_bg_active, BLACK_DEEP)
    set_col(C.title_bg_collapsed, BLACK_DEEP)
    set_col(C.menu_bar_bg, BLACK_PANEL)

    set_col(C.scrollbar_bg, ImVec4(0, 0, 0, 0))
    set_col(C.scrollbar_grab, ImVec4(0.20, 0.20, 0.24, 1.0))
    set_col(C.scrollbar_grab_hovered, ImVec4(0.30, 0.30, 0.35, 1.0))
    set_col(C.scrollbar_grab_active, RED)

    set_col(C.check_mark, RED_BRIGHT)
    set_col(C.slider_grab, RED)
    set_col(C.slider_grab_active, RED_BRIGHT)

    set_col(C.button, BLACK_FRAME)
    set_col(C.button_hovered, BLACK_RAISED)
    set_col(C.button_active, RED_DIM)

    set_col(C.header, ImVec4(RED.x, RED.y, RED.z, 0.22))
    set_col(C.header_hovered, ImVec4(RED.x, RED.y, RED.z, 0.32))
    set_col(C.header_active, ImVec4(RED.x, RED.y, RED.z, 0.42))

    set_col(C.separator, BORDER)
    set_col(C.separator_hovered, RED_DIM)
    set_col(C.separator_active, RED)

    set_col(C.resize_grip, ImVec4(0, 0, 0, 0))
    set_col(C.resize_grip_hovered, RED_GHOST)
    set_col(C.resize_grip_active, RED)
    set_col(C.nav_cursor, RED_BRIGHT)


# --------------------------------------------------------------- recording

class Recorder:
    """Writes the stream to an mp4 on its own clock.

    Frames arrive whenever Wi-Fi and the console's encoder allow, which is not
    a constant rate. Writing each arrival straight into a fixed-fps container
    would give a file whose duration is wrong. So this samples the newest
    frame at exactly the target rate instead - repeating a frame when nothing
    new has come in, which is what a screen recorder does anyway.
    """

    def __init__(self, rx, fps, path):
        self.rx = rx
        self.fps = max(1, int(fps))
        self.path = path
        self.frames = 0
        self.started = time.monotonic()
        self._writer = None
        self._running = True
        self._thread = threading.Thread(target=self._run, name="wstr-rec",
                                        daemon=True)
        self._thread.start()

    def _run(self):
        interval = 1.0 / self.fps
        next_at = time.monotonic()
        while self._running:
            now = time.monotonic()
            if now < next_at:
                time.sleep(min(interval, next_at - now))
                continue
            next_at += interval
            # A long stall (window dragged, machine asleep) would otherwise
            # make this spin trying to catch up.
            if next_at < now:
                next_at = now + interval

            frame, _ = self.rx.latest()
            if frame is None:
                continue
            if self._writer is None:
                h, w = frame.shape[:2]
                CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
                fourcc = cv2.VideoWriter_fourcc(*"mp4v")
                self._writer = cv2.VideoWriter(str(self.path), fourcc,
                                               float(self.fps), (w, h))
                self._size = (w, h)
            if frame.shape[1::-1] != self._size:
                # Resolution changed mid-recording; a VideoWriter cannot
                # follow that, so pad/crop is the only alternative to
                # stopping. Stopping is more honest.
                break
            self._writer.write(frame)
            self.frames += 1

        if self._writer is not None:
            self._writer.release()
            self._writer = None
        self._running = False

    @property
    def running(self):
        return self._running

    def stop(self):
        self._running = False
        self._thread.join(timeout=2.0)


# ---------------------------------------------------------- virtual camera

class VirtualCam:
    """Optional. Feeds frames to an OBS-style virtual camera device so Discord
    can select it as a webcam."""

    def __init__(self, rx, width, height, fps):
        self.rx = rx
        self.error = None
        self.frames = 0
        self._cam = None
        self._size = (width, height)
        self._fps = max(1, int(fps))
        self._running = True
        self._thread = threading.Thread(target=self._run, name="wstr-vcam",
                                        daemon=True)
        self._thread.start()

    def _run(self):
        try:
            self._cam = pyvirtualcam.Camera(width=self._size[0],
                                            height=self._size[1],
                                            fps=self._fps, fmt=pyvirtualcam.PixelFormat.BGR)
        except Exception as exc:
            self.error = str(exc)
            self._running = False
            return

        blank = np.zeros((self._size[1], self._size[0], 3), dtype=np.uint8)
        with self._cam as cam:
            while self._running:
                frame, _ = self.rx.latest()
                if frame is None:
                    out = blank
                elif frame.shape[1::-1] != self._size:
                    out = cv2.resize(frame, self._size,
                                     interpolation=cv2.INTER_LINEAR)
                else:
                    out = frame
                cam.send(out)
                cam.sleep_until_next_frame()
                self.frames += 1

    @property
    def device(self):
        return self._cam.device if self._cam else None

    @property
    def running(self):
        return self._running

    def stop(self):
        self._running = False
        self._thread.join(timeout=2.0)


# ------------------------------------------------------------------- app

class Dashboard:
    def __init__(self):
        self.settings = receiver.Settings()
        self.manual_ip = ""
        self.res_index = 2
        self._load_settings()

        self.rx = receiver.Receiver(self.settings, self.manual_ip)
        self.recorder = None
        self.vcam = None
        self.audio = None
        self._grab_thread = None
        self.toast = ""
        self.toast_until = 0.0
        self._last_shown_id = -1
        self._last_log_id = -1

    # -- persistence --------------------------------------------------------

    def _load_settings(self):
        try:
            data = json.loads(SETTINGS_FILE.read_text())
        except Exception:
            return
        self.res_index = int(data.get("res_index", self.res_index))
        self.res_index = max(0, min(len(RESOLUTIONS) - 1, self.res_index))
        self.settings.width = RESOLUTIONS[self.res_index][1]
        self.settings.height = RESOLUTIONS[self.res_index][2]
        self.settings.fps = int(data.get("fps", self.settings.fps))
        self.settings.quality = int(data.get("quality", self.settings.quality))
        self.settings.source = int(data.get("source", self.settings.source))
        self.settings.stage = max(0, min(len(STAGES) - 1,
                                         int(data.get("stage", self.settings.stage))))
        self.settings.sync_mode = max(0, min(len(SYNC_MODES) - 1,
                                             int(data.get("sync_mode", 0))))
        self.settings.tile_mode = 1 if int(data.get("tile_mode", 0)) else 0
        self.settings.want_audio = 1 if int(data.get("want_audio", 0)) else 0
        self.settings.auto_quality = 1 if int(data.get("auto_quality", 1)) else 0
        self.settings.video_off = 1 if int(data.get("video_off", 0)) else 0
        self.settings.audio_source = 1 if int(data.get("audio_source", 0)) else 0
        self.manual_ip = str(data.get("manual_ip", ""))

    def _save_settings(self):
        # Never while a grab is in flight. It turns auto scaling off and quality
        # up for a second or two, and saving in that window would persist those
        # as the user's settings - which is a very confusing way to end up
        # permanently capped at 12 fps.
        if self._grab_thread is not None and self._grab_thread.is_alive():
            return
        try:
            SETTINGS_FILE.write_text(json.dumps({
                "res_index": self.res_index,
                "fps": self.settings.fps,
                "quality": self.settings.quality,
                "source": self.settings.source,
                "stage": self.settings.stage,
                "sync_mode": self.settings.sync_mode,
                "tile_mode": self.settings.tile_mode,
                "want_audio": self.settings.want_audio,
                "auto_quality": self.settings.auto_quality,
                "video_off": self.settings.video_off,
                "audio_source": self.settings.audio_source,
                "manual_ip": self.manual_ip,
            }, indent=2))
        except Exception:
            pass

    def _say(self, msg, seconds=3.0):
        self.toast = msg
        self.toast_until = time.monotonic() + seconds

    # -- pieces of the UI ---------------------------------------------------

    def _status_pill(self):
        rx = self.rx
        if not rx.running:
            colour, label = TEXT_MUTE, "stopped"
        elif rx.connected():
            colour, label = OK_GREEN, f"streaming from {rx.stats.console_ip}"
        elif rx.stats.console_ip:
            colour, label = AMBER, "console stopped sending"
        else:
            colour, label = AMBER, "looking for the console..."

        draw = imgui.get_window_draw_list()
        pos = imgui.get_cursor_screen_pos()
        r = 5.0
        cy = pos.y + imgui.get_text_line_height() * 0.5
        draw.add_circle_filled(ImVec2(pos.x + r, cy), r, imgui.get_color_u32(colour))
        imgui.dummy(ImVec2(r * 2 + 8, imgui.get_text_line_height()))
        imgui.same_line()
        imgui.text_colored(colour, label)

    def _header(self):
        with push_font("title"):
            imgui.text("Wii U Stream")
        imgui.same_line()
        imgui.set_cursor_pos_x(imgui.get_cursor_pos_x() + 14)
        imgui.set_cursor_pos_y(imgui.get_cursor_pos_y() + 4)
        self._status_pill()

        imgui.same_line()
        avail = imgui.get_window_width()
        imgui.set_cursor_pos_x(avail - SIDEBAR_W - 60)
        if self.rx.running:
            if imgui.button("Stop", ImVec2(120, 0)):
                self._stop_all()
        else:
            imgui.push_style_color(imgui.Col_.button, RED_DIM)
            imgui.push_style_color(imgui.Col_.button_hovered, RED)
            if imgui.button("Start", ImVec2(120, 0)):
                self.rx.manual_ip = self.manual_ip
                self.rx.error = None
                self.rx.start()
            imgui.pop_style_color(2)

    def _preview(self, size):
        imgui.begin_child("##preview", size, imgui.ChildFlags_.borders.value)

        frame, fid = self.rx.latest()
        inner = imgui.get_content_region_avail()

        if self.settings.video_off:
            msg = "audio only - video capture is off"
            frame = None
        elif frame is None:
            msg = ("waiting for the console..." if self.rx.running
                   else "press Start")
            imgui.set_cursor_pos(ImVec2(inner.x * 0.5 - 90, inner.y * 0.5 - 10))
            imgui.text_colored(TEXT_MUTE, msg)
        else:
            h, w = frame.shape[:2]
            # Letterbox into whatever the window gives us, so a 16:9 stream
            # never stretches when the sidebar changes the aspect available.
            scale = min(inner.x / w, inner.y / h)
            dw, dh = max(16, int(w * scale)), max(16, int(h * scale))
            imgui.set_cursor_pos(ImVec2((inner.x - dw) * 0.5 + 8,
                                        (inner.y - dh) * 0.5 + 8))
            immvision.image_display("##video", frame, (dw, dh),
                                    refresh_image=(fid != self._last_shown_id))
            self._last_shown_id = fid

        imgui.end_child()

    def _log_pane(self, size):
        """The console's own trace.

        Each risky step in the capture path logs immediately before it runs, so
        when the console locks up the last line here names the call that did
        it - which is the only evidence a hard freeze leaves behind.
        """
        lines, log_id = self.rx.log_lines()

        # The toolbar sits outside the scrolling region, not inside it. Put it
        # in the same child as the lines and it scrolls away with them - by the
        # time a session has produced a trace worth keeping, the Save button
        # has left the top of the pane.
        #
        # A freeze ends the session but not this window, so the trace is still
        # here to be kept. Saving writes the settings alongside it - a log that
        # does not say which stage, heap and sync mode produced it is only half
        # the evidence.
        if imgui.small_button("Save log"):
            path = self._save_log(lines)
            self._say(f"saved {path.name}" if path else "could not write the log")
        imgui.same_line()
        if imgui.small_button("Copy"):
            imgui.set_clipboard_text(self._log_text(lines))
            self._say(f"copied {len(lines)} lines")
        imgui.same_line()
        if imgui.small_button("Clear"):
            self.rx.clear_log()
        imgui.same_line()
        imgui.text_colored(TEXT_MUTE, f"{len(lines)} lines")

        # Whatever height the toolbar just used comes off the scrolling area,
        # so the pane as a whole still fits the space it was given.
        used = imgui.get_frame_height_with_spacing()
        imgui.begin_child("##log", ImVec2(size.x, max(48.0, size.y - used)),
                          imgui.ChildFlags_.borders.value)

        if not lines:
            imgui.text_colored(TEXT_MUTE, "console log - nothing yet")
        else:
            with push_font("mono"):
                for stamp, text in lines:
                    colour = TEXT
                    low = text.lower()
                    if "fail" in low or "unsupported" in low:
                        colour = RED_BRIGHT
                    elif text.endswith("..."):
                        colour = AMBER
                    elif " ok" in low or low.endswith("ok"):
                        colour = OK_GREEN
                    imgui.text_colored(TEXT_MUTE, stamp)
                    imgui.same_line()
                    imgui.text_colored(colour, text)
            # Follow the tail, so the last line before a freeze stays visible.
            if log_id != self._last_log_id:
                imgui.set_scroll_here_y(1.0)
                self._last_log_id = log_id

        imgui.end_child()

    def _log_header(self):
        """The settings that produced this trace, written at the top of a
        saved log so it can be read months later, or by someone else."""
        st = self.rx.stats
        return "\n".join([
            "# wiiu-stream log",
            f"# saved      {time.strftime('%Y-%m-%d %H:%M:%S')}",
            f"# plugin     {st.console_build or 'UNKNOWN - console sent no build stamp'}",
            f"# auto       quality {'on' if self.settings.auto_quality else 'off'}"
            f", console using q{st.console_quality or '?'}",
            f"# stage      {self.settings.stage} "
            f"({STAGES[self.settings.stage][0]})",
            f"# gpu sync   {SYNC_MODES[self.settings.sync_mode][0]}",
            f"# tiling     {'linear special' if self.settings.tile_mode else 'linear aligned'}",
            f"# stream     {self.settings.width}x{self.settings.height} "
            f"@ {self.settings.fps}fps q{self.settings.quality} "
            f"{'gamepad' if self.settings.source else 'tv'}",
            f"# console    {st.console_ip or 'not found'} "
            f"src {st.src_w}x{st.src_h}",
            f"# received   {st.frames} frames, {st.dropped} lost, "
            f"{st.fps:.1f} fps, {st.mbps:.2f} Mbit/s",
            "",
        ])

    def _log_text(self, lines):
        body = "\n".join(f"{stamp}  {text}" for stamp, text in lines)
        return self._log_header() + body + "\n"

    def _save_log(self, lines):
        try:
            CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
            path = CAPTURE_DIR / time.strftime("wiiu-log-%Y%m%d-%H%M%S.txt")
            path.write_text(self._log_text(lines), encoding="utf-8")
            return path
        except OSError:
            return None

    def _stat(self, label, value, colour=TEXT):
        imgui.text_colored(TEXT_MUTE, label)
        imgui.same_line()
        with push_font("mono"):
            imgui.text_colored(colour, value)

    def _stats_bar(self, width):
        st = self.rx.stats
        # An explicit width, not 0. A child of width 0 fills the rest of the
        # window, which would stretch the enclosing group to full width and
        # push the sidebar past the right edge - where it renders happily and
        # invisibly.
        imgui.begin_child("##stats", ImVec2(width, 44),
                          imgui.ChildFlags_.borders.value)

        fps_colour = OK_GREEN if st.fps >= self.settings.fps * 0.75 else AMBER
        self._stat("fps", f"{st.fps:5.1f}", fps_colour)
        imgui.same_line(0, 24)
        self._stat("bitrate", f"{st.mbps:5.2f} Mbit/s")
        imgui.same_line(0, 24)
        self._stat("frame", f"{st.frame_bytes / 1024:5.1f} KB")
        imgui.same_line(0, 24)
        self._stat("lost", f"{st.dropped}", AMBER if st.dropped else TEXT_DIM)
        if self.settings.want_audio:
            imgui.same_line(0, 24)
            secs = self.rx.audio_bytes / (48000.0 * 4.0)
            under = self.audio.underruns if self.audio else 0
            self._stat("audio", f"{secs:5.1f}s",
                       OK_GREEN if secs > 0 else AMBER)
            if under:
                imgui.same_line(0, 8)
                imgui.text_colored(TEXT_MUTE, f"({under} gaps)")
        imgui.same_line(0, 24)
        if st.src_w:
            self._stat("console", f"{st.src_w}x{st.src_h}"
                                  f" enc {st.console_encode_ms:.0f}ms")

        imgui.end_child()

    def _controls(self):
        imgui.begin_child("##controls", ImVec2(SIDEBAR_W, 0),
                          imgui.ChildFlags_.borders.value)

        changed = False

        with push_font("semi"):
            imgui.text_colored(TEXT_DIM, "CAPTURE STAGE")
        imgui.separator()
        imgui.set_next_item_width(-1)
        if imgui.begin_combo("##stage", STAGES[self.settings.stage][0]):
            for i, (name, note) in enumerate(STAGES):
                if imgui.selectable(f"{i}  {name}", i == self.settings.stage)[0]:
                    self.settings.stage = i
                    self.rx.add_local_log(f"stage -> {i} ({name})")
                    changed = True
                if imgui.is_item_hovered():
                    imgui.set_tooltip(note)
            imgui.end_combo()
        imgui.push_text_wrap_pos(0)
        imgui.text_colored(TEXT_MUTE, STAGES[self.settings.stage][1])
        if self.settings.stage < len(STAGES) - 1:
            imgui.text_colored(AMBER, "not streaming - this is a bisect step")
        imgui.pop_text_wrap_pos()

        imgui.spacing()
        imgui.text_colored(TEXT_MUTE, "capture tiling")
        for i, name in enumerate(("Linear aligned", "Linear special")):
            if imgui.radio_button(name, self.settings.tile_mode == i):
                self.settings.tile_mode = i
                self.rx.add_local_log(f"tiling -> {name.lower()}")
                changed = True
            if i == 0:
                imgui.same_line(0, 12)

        imgui.spacing()
        imgui.text_colored(TEXT_MUTE, "gpu sync")
        imgui.set_next_item_width(-1)
        if imgui.begin_combo("##sync", SYNC_MODES[self.settings.sync_mode][0]):
            for i, (name, note) in enumerate(SYNC_MODES):
                if imgui.selectable(name, i == self.settings.sync_mode)[0]:
                    self.settings.sync_mode = i
                    self.rx.add_local_log(f"gpu sync -> {name.lower()}")
                    changed = True
                if imgui.is_item_hovered():
                    imgui.set_tooltip(note)
            imgui.end_combo()
        imgui.push_text_wrap_pos(0)
        imgui.text_colored(TEXT_MUTE, SYNC_MODES[self.settings.sync_mode][1])
        imgui.pop_text_wrap_pos()

        imgui.spacing()
        with push_font("semi"):
            imgui.text_colored(TEXT_DIM, "STREAM")
        imgui.separator()

        avail_w = imgui.get_content_region_avail().x
        btn_w = (avail_w - imgui.get_style().item_spacing.x * 3) / 4.0
        for i, (name, w, h, fps, q, note) in enumerate(PRESETS):
            active = (self.settings.width == w and self.settings.height == h
                      and self.settings.fps == fps
                      and self.settings.quality == q)
            if active:
                imgui.push_style_color(imgui.Col_.button, RED_DIM)
            if imgui.button(name, ImVec2(btn_w, 0)):
                self.settings.width, self.settings.height = w, h
                self.settings.fps, self.settings.quality = fps, q
                self.res_index = next((k for k, r in enumerate(RESOLUTIONS)
                                       if r[1] == w and r[2] == h),
                                      self.res_index)
                self.rx.add_local_log(f"preset -> {name} ({w}x{h} {fps}fps q{q})")
                changed = True
            if active:
                imgui.pop_style_color()
            if imgui.is_item_hovered():
                imgui.set_tooltip(note)
            if i < len(PRESETS) - 1:
                imgui.same_line()


        imgui.set_next_item_width(-1)
        preview = RESOLUTIONS[self.res_index][0]
        if imgui.begin_combo("##res", preview):
            for i, (label, w, h) in enumerate(RESOLUTIONS):
                if imgui.selectable(label, i == self.res_index)[0]:
                    self.res_index = i
                    self.settings.width, self.settings.height = w, h
                    changed = True
            imgui.end_combo()
        if self.res_index >= 3:
            imgui.text_colored(AMBER,
                               "above 640x360 the console's\nencoder is the bottleneck")

        imgui.set_next_item_width(-1)
        ch, v = imgui.slider_int("##fps", self.settings.fps, 5, 30,
                                 "target %d fps")
        if ch:
            self.settings.fps = v
            changed = True

        imgui.set_next_item_width(-1)
        ch, v = imgui.slider_int("##q", self.settings.quality, 20, 95,
                                 "max quality %d")
        if ch:
            self.settings.quality = v
            changed = True

        # The console lowers quality by itself until a frame fits the interval,
        # so the slider is a ceiling, not a setting. Without showing what it
        # actually settled on, moving the slider looks like it does nothing -
        # the console just walks straight back down to where it was.
        if imgui.checkbox("Auto scaling",
                          bool(self.settings.auto_quality))[0]:
            self.settings.auto_quality = 0 if self.settings.auto_quality else 1
            self.rx.add_local_log(
                "auto scaling " + ("on" if self.settings.auto_quality else "off"))
            changed = True
        if not self.settings.auto_quality:
            st = self.rx.stats
            target = max(1, self.settings.fps)
            if self.rx.connected() and st.fps < target * 0.75:
                imgui.push_text_wrap_pos(0)
                imgui.text_colored(AMBER,
                                   f"quality is held at {self.settings.quality}, "
                                   f"so the frame rate takes the hit: "
                                   f"{st.fps:.0f} of {target} fps. Tick this to "
                                   f"let the console trade quality for smoothness.")
                imgui.pop_text_wrap_pos()
            else:
                imgui.text_colored(TEXT_MUTE, "quality is held; fps falls instead")

        cq = self.rx.stats.console_quality
        if cq and self.settings.auto_quality and cq < self.settings.quality:
            imgui.text_colored(AMBER,
                               f"console lowered it to {cq} to hold "
                               f"{self.settings.fps} fps")
        elif cq:
            imgui.text_colored(TEXT_MUTE, f"console is using {cq}")

        imgui.spacing()
        with push_font("semi"):
            imgui.text_colored(TEXT_DIM, "AUDIO")
        imgui.separator()

        if imgui.checkbox("Stream audio", bool(self.settings.want_audio))[0]:
            self.settings.want_audio = 0 if self.settings.want_audio else 1
            self._apply_audio()
            changed = True

        if self.settings.want_audio:
            if self.audio and self.audio.error:
                imgui.text_colored(AMBER, self.audio.error)
            else:
                imgui.text_colored(TEXT_MUTE,
                                   "played here, so Discord's screen share")
                imgui.text_colored(TEXT_MUTE, "picks it up with the video")

            # The TV and GamePad are fed by separate mixes on this console, so
            # this is a real choice, not a routing preference: a title can be
            # playing through one and silent on the other.
            imgui.text_colored(TEXT_MUTE, "mixer")
            for i, name in enumerate(("TV", "GamePad")):
                if imgui.radio_button(f"{name}##amix",
                                      self.settings.audio_source == i):
                    self.settings.audio_source = i
                    self.rx.add_local_log(f"audio mixer -> {name.lower()}")
                    changed = True
                if i == 0:
                    imgui.same_line(0, 20)

            if imgui.checkbox("Audio only (no video)",
                              bool(self.settings.video_off))[0]:
                self.settings.video_off = 0 if self.settings.video_off else 1
                self.rx.add_local_log(
                    "audio only " + ("on" if self.settings.video_off else "off"))
                changed = True
            if self.settings.video_off:
                imgui.push_text_wrap_pos(0)
                imgui.text_colored(TEXT_MUTE,
                                   "capture is off entirely - watch the picture "
                                   "on the console, and the whole link and "
                                   "encoder go to keeping the sound steady")
                imgui.pop_text_wrap_pos()

        imgui.spacing()
        with push_font("semi"):
            imgui.text_colored(TEXT_DIM, "SOURCE")
        imgui.separator()
        for i, name in enumerate(("TV", "GamePad")):
            if imgui.radio_button(name, self.settings.source == i):
                self.settings.source = i
                changed = True
            if i == 0:
                imgui.same_line(0, 20)

        imgui.spacing()
        with push_font("semi"):
            imgui.text_colored(TEXT_DIM, "CONSOLE")
        imgui.separator()
        imgui.text_colored(TEXT_MUTE, "found by broadcast - only fill this")
        imgui.text_colored(TEXT_MUTE, "in if your network blocks it")
        imgui.set_next_item_width(-1)
        ch, v = imgui.input_text_with_hint("##ip", "192.168.1.110", self.manual_ip)
        if ch:
            self.manual_ip = v
            self.rx.manual_ip = v
            changed = True

        imgui.spacing()
        with push_font("semi"):
            imgui.text_colored(TEXT_DIM, "OUTPUT")
        imgui.separator()
        self._output_section()

        imgui.spacing()
        with push_font("semi"):
            imgui.text_colored(TEXT_DIM, "GETTING THIS INTO DISCORD")
        imgui.separator()
        imgui.push_text_wrap_pos(0)
        imgui.text_colored(TEXT_MUTE,
            "Discord has no address to stream to - no RTMP, and Go Live is "
            "closed to third parties. Give its client a source instead:")
        imgui.spacing()
        imgui.text_colored(TEXT, "Screen share")
        imgui.text_colored(TEXT_MUTE,
            "Go Live, choose Application Window, pick this window. Nothing "
            "else to install.")
        imgui.spacing()
        imgui.text_colored(TEXT, "As a webcam")
        if pyvirtualcam is None:
            imgui.text_colored(TEXT_MUTE,
                "Needs OBS Studio installed (for its virtual camera driver) "
                "and: pip install pyvirtualcam")
        else:
            imgui.text_colored(TEXT_MUTE,
                "Turn on the virtual camera above, then pick it as your "
                "camera in Discord's voice settings.")
        imgui.pop_text_wrap_pos()

        imgui.end_child()

        if changed:
            self._save_settings()

    def _output_section(self):
        # Recording
        if self.recorder and self.recorder.running:
            secs = time.monotonic() - self.recorder.started
            imgui.push_style_color(imgui.Col_.button, RED_DIM)
            if imgui.button(f"Stop recording  {int(secs // 60):02d}:"
                            f"{int(secs % 60):02d}", ImVec2(-1, 0)):
                path = self.recorder.path
                self.recorder.stop()
                self.recorder = None
                self._say(f"saved {path.name}")
            imgui.pop_style_color()
        else:
            if self.recorder and not self.recorder.running:
                self.recorder = None
            if imgui.button("Record to mp4", ImVec2(-1, 0)):
                if self.rx.latest()[0] is None:
                    self._say("nothing to record yet")
                else:
                    name = time.strftime("wiiu-%Y%m%d-%H%M%S.mp4")
                    self.recorder = Recorder(self.rx, self.settings.fps,
                                             CAPTURE_DIR / name)
                    self._say(f"recording to captures/{name}")

        if self._grab_thread is not None and self._grab_thread.is_alive():
            imgui.begin_disabled()
            imgui.button("Grabbing at full quality...", ImVec2(-1, 0))
            imgui.end_disabled()
        elif imgui.button("Save a frame", ImVec2(-1, 0)):
            if self.rx.latest()[0] is None:
                self._say("nothing to save yet")
            else:
                self._grab_thread = threading.Thread(
                    target=self._grab_full_quality, name="wstr-grab",
                    daemon=True)
                self._grab_thread.start()

        # Virtual camera
        if pyvirtualcam is None:
            imgui.begin_disabled()
            imgui.button("Virtual camera (not installed)", ImVec2(-1, 0))
            imgui.end_disabled()
        elif self.vcam and self.vcam.running:
            if imgui.button("Stop virtual camera", ImVec2(-1, 0)):
                self.vcam.stop()
                self.vcam = None
            else:
                imgui.text_colored(OK_GREEN, f"-> {self.vcam.device}")
        else:
            if self.vcam and self.vcam.error:
                imgui.text_colored(AMBER, self.vcam.error[:60])
            if imgui.button("Start virtual camera", ImVec2(-1, 0)):
                self.vcam = VirtualCam(self.rx, self.settings.width,
                                       self.settings.height, self.settings.fps)

    # -- frame --------------------------------------------------------------

    def gui(self):
        self._header()

        if self.rx.error:
            imgui.text_colored(RED_BRIGHT, self.rx.error)

        imgui.spacing()

        avail = imgui.get_content_region_avail()
        left_w = avail.x - SIDEBAR_W - imgui.get_style().item_spacing.x

        log_h = 150.0
        imgui.begin_group()
        self._preview(ImVec2(left_w, avail.y - 54 - log_h - 10))
        self._stats_bar(left_w)
        self._log_pane(ImVec2(left_w, log_h))
        imgui.end_group()

        imgui.same_line()
        self._controls()

        if self.toast and time.monotonic() < self.toast_until:
            imgui.set_cursor_pos(ImVec2(20, imgui.get_window_height() - 34))
            imgui.text_colored(OK_GREEN, self.toast)

    def _grab_full_quality(self):
        """Ask for the best frame the console can make, then save that.

        The stream deliberately runs at whatever quality holds the frame rate -
        often around 30 - so saving the newest stream frame gives a picture far
        worse than the console can actually produce. A screenshot has no frame
        rate to hold, so this lifts the ceiling and turns off the adaptation for
        as long as it takes a few frames to come through, then puts both back.
        """
        prev_q = self.settings.quality
        prev_auto = self.settings.auto_quality
        want_w = self.settings.width
        confirmed = False
        try:
            self.settings.quality = 95
            self.settings.auto_quality = 0
            # Push it immediately. Waiting for the next heartbeat would burn a
            # second of the budget below before the console even heard about
            # it - which is why the first version saved a blurry frame: it
            # counted three frames that were all still the old settings.
            self.rx.poke()

            # Wait for the console to *confirm*, not for a guessed delay. It
            # reports the quality it is really using once a second, and the
            # frame size arrives in every packet header - so full size at high
            # quality is proof the new settings are in the picture rather than
            # merely in flight.
            deadline = time.monotonic() + 6.0
            while time.monotonic() < deadline:
                frame, _ = self.rx.latest()
                if (self.rx.stats.console_quality >= 90 and frame is not None
                        and frame.shape[1] >= want_w):
                    confirmed = True
                    break
                time.sleep(0.05)

            # Then two more frames, so the one saved was encoded under the
            # confirmed settings rather than being the frame that confirmed it.
            prev_id = self.rx.latest()[1]
            seen = 0
            while seen < 2 and time.monotonic() < deadline:
                _, fid = self.rx.latest()
                if fid != prev_id:
                    prev_id = fid
                    seen += 1
                time.sleep(0.02)

            frame, _ = self.rx.latest()
            if frame is None:
                self._say("nothing to save")
                return
            CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
            name = time.strftime("wiiu-%Y%m%d-%H%M%S.png")
            cv2.imwrite(str(CAPTURE_DIR / name), frame)
            self._say(f"saved captures/{name} at quality 95" if confirmed else
                      f"saved captures/{name} - console did not reach full "
                      f"quality in time")
        finally:
            self.settings.quality = prev_q
            self.settings.auto_quality = prev_auto
            self.rx.poke()

    def _apply_audio(self):
        """Open or close the local sink to match the setting. The console only
        sends audio while want_audio is set, so the two are kept in step."""
        if self.settings.want_audio and self.audio is None:
            self.audio = audio_out.AudioOut(rate=48000, channels=2)
            self.audio.start()
            if self.audio.error:
                self.rx.add_local_log(f"audio: {self.audio.error}")
            else:
                self.rx.audio_sink = self.audio.feed
                self.rx.add_local_log("audio: playing locally at 48 kHz stereo")
        elif not self.settings.want_audio and self.audio is not None:
            self.rx.audio_sink = None
            self.audio.stop()
            self.audio = None
            self.rx.add_local_log("audio: stopped")

    def _stop_all(self):
        if self.audio:
            self.rx.audio_sink = None
            self.audio.stop()
            self.audio = None
        if self.recorder:
            self.recorder.stop()
            self.recorder = None
        if self.vcam:
            self.vcam.stop()
            self.vcam = None
        self.rx.stop()

    def on_exit(self):
        self._stop_all()
        self._save_settings()

    def run(self):
        params = hello_imgui.RunnerParams()
        params.app_window_params.window_title = "Wii U Stream"
        params.app_window_params.window_geometry.size = (1180, 700)
        params.imgui_window_params.default_imgui_window_type = (
            hello_imgui.DefaultImGuiWindowType.provide_full_screen_window
        )

        # No fps idling here, unlike the other dashboards in this workspace:
        # this window *is* the video output when it is being screen-shared,
        # so dropping to 9 fps when the mouse stops moving would drop the
        # stream Discord sees with it.
        params.fps_idling.enable_idling = False

        params.callbacks.load_additional_fonts = load_ui_fonts
        params.callbacks.setup_imgui_style = apply_noir_style
        params.callbacks.show_gui = self.gui
        params.callbacks.before_exit = self.on_exit
        # Start listening straight away. Discovery is a broadcast the console
        # answers or ignores, so there is nothing to decide first - making the
        # user press Start just to see their own console is a click for
        # nothing. Stop is there for when they want the console to stop
        # encoding.
        params.callbacks.post_init = self._post_init
        hello_imgui.run(params)

    def _post_init(self):
        # Frames come out of cv2.imdecode, so they are BGR. immvision requires
        # being told once, before the first image_display, and panics if not.
        immvision.use_bgr_color_order()
        self.rx.manual_ip = self.manual_ip
        self._apply_audio()
        self.rx.start()


if __name__ == "__main__":
    Dashboard().run()
