"""UDP side of the Wii U stream, kept away from the UI thread.

The console never learns this PC's address from configuration. It binds a port
and waits; this module broadcasts a HELLO carrying the settings we want, and
the console streams back to whoever asked. The HELLO repeats about once a
second, which doubles as a keepalive - stop sending and the console stops
encoding, so closing the app costs the game nothing.

One frame is one JPEG, split across MTU-sized chunks. There is no inter-frame
prediction, so a lost chunk costs exactly one frame and the next one is whole
again. That is the right trade for a LAN stream: an I-frame-only codec is
wasteful on bandwidth and free on latency and complexity, and bandwidth is the
resource this link has spare.
"""

import socket
import struct
import threading
import time
from collections import deque

import cv2
import numpy as np

# --- wire format, mirrors common/wstr_proto.h -------------------------------

MAGIC = 0x57535452  # 'WSTR'
VERSION = 1

PORT_CONSOLE = 41414
PORT_PC = 41415

PKT_VIDEO = 1
PKT_AUDIO = 2
PKT_HELLO = 3
PKT_STATUS = 4
PKT_BYE = 5
PKT_LOG = 6

FLAG_KEY = 0x01
FLAG_DRC = 0x02

# How far into the capture path the console may go. Mirrors WSTR_STAGE_* in
# common/wstr_proto.h; see there for why this exists.
STAGE_OFF = 0
STAGE_SEND = 1
STAGE_ALLOC = 2
STAGE_COPY = 3
STAGE_READBACK = 4
STAGE_FULL = 5

# Capture surfaces always come from the game's heap - it is MEM2, and only
# memory the GPU can address can receive the capture blit. The plugin's own
# heap allocates fine and then silently receives nothing. The byte stays on the
# wire so the packet layout does not shift.
ALLOC_GAME_HEAP = 0

# How the console waits for the GPU. Mirrors WSTR_SYNC_* in wstr_proto.h.
SYNC_TIMESTAMP = 0
SYNC_NONE = 1
SYNC_DRAWDONE = 2

# Capture surface tiling. LINEAR_SPECIAL is a driver construct the GPU has no
# hardware mode for; LINEAR_ALIGNED is a real one. Mirrors WSTR_TILE_*.
TILE_LINEAR_ALIGNED = 0
TILE_LINEAR_SPECIAL = 1

# Which mixer the console captures. The TV and GamePad are fed from separate
# device mixes and are not the same audio. Mirrors WSTR_AUDIO_SRC_*.
AUDIO_SRC_TV = 0
AUDIO_SRC_DRC = 1

HDR = struct.Struct(">IBBBBIHHHHHH")   # 24 bytes
assert HDR.size == 24

HELLO_BODY = struct.Struct(">HBBHHBBBBBBBBB")   # 17 bytes
STATUS_BODY = struct.Struct(">HHHHBBBBII")

HELLO_INTERVAL = 1.0
# A frame whose chunks stop arriving is never completed. Holding more than a
# handful of partials just delays admitting the loss.
MAX_PARTIAL_FRAMES = 8
STALE_SECONDS = 3.0


def _pack_header(ptype, flags=0, seq=0, idx=0, cnt=1, plen=0, w=0, h=0):
    return HDR.pack(MAGIC, VERSION, ptype, flags, 0, seq, idx, cnt, plen, w, h,
                    int(time.monotonic() * 1000) & 0xFFFF)


class Settings:
    """What we ask the console for. Read by the sender thread every HELLO, so
    changing a field takes effect within a second without any handshake."""

    def __init__(self):
        self.width = 640
        self.height = 360
        self.fps = 20
        self.quality = 70
        self.source = 0        # 0 = TV, 1 = GamePad
        self.want_audio = 0    # reserved; the console does not send audio yet
        # Starts safe rather than streaming. The capture path is what hangs
        # the console when it goes wrong, and a freeze costs a power cycle -
        # so a fresh install waits to be told how far it may go. The choice
        # persists, so this only ever costs one click, once.
        self.stage = STAGE_OFF
        self.alloc_mode = ALLOC_GAME_HEAP
        # GX2DrawDone on the render thread is what froze the console at the
        # readback stage; wait for our own submission instead.
        self.sync_mode = SYNC_TIMESTAMP
        self.tile_mode = TILE_LINEAR_ALIGNED
        # Let the console lower quality to hold the frame rate. Off means
        # it keeps the quality asked for and the fps falls instead.
        self.auto_quality = 1
        # Audio only: the console skips capture entirely. For watching the
        # picture on the console itself while the sound comes here.
        self.video_off = 0
        self.audio_source = AUDIO_SRC_TV

    def pack(self):
        return HELLO_BODY.pack(PORT_PC, self.quality, self.fps,
                               self.width, self.height,
                               self.want_audio, self.source, self.stage,
                               self.alloc_mode, self.sync_mode,
                               self.tile_mode, self.auto_quality,
                               self.video_off, self.audio_source)


class Stats:
    def __init__(self):
        self.frames = 0            # completed frames since start
        self.dropped = 0           # frames abandoned with chunks missing
        self.bytes = 0
        self.fps = 0.0
        self.mbps = 0.0
        self.last_frame_at = 0.0
        self.frame_bytes = 0       # size of the most recent frame
        self.console_ip = None
        self.console_fps = 0       # what the console says it is encoding
        self.console_quality = 0   # what it settled on, which is not the ask
        self.console_build = ""    # from the plugin's own stamp
        self.console_encode_ms = 0.0
        self.console_capture_ms = 0.0
        self.src_w = 0
        self.src_h = 0


class Receiver:
    """Owns the sockets and the reassembly buffer.

    Everything the UI reads goes through `lock`; frames are handed over as
    whole decoded arrays so the UI never touches a partially written buffer.
    """

    def __init__(self, settings=None, manual_ip=""):
        self.settings = settings or Settings()
        self.stats = Stats()
        self.manual_ip = manual_ip

        self.lock = threading.Lock()
        self.frame = None          # latest decoded BGR frame
        self.frame_id = 0          # bumps on every new frame, for the UI
        self.error = None
        # Diagnostic lines from the console. Bounded, because the interesting
        # ones are always the most recent - and when the console locks up, the
        # last line is the whole answer.
        # 2000, not 200. One timing line a second evicted the startup trace -
        # the build stamp, the surface details, the adaptation decisions -
        # within about three minutes, so a log saved after a real session
        # contained only the least interesting lines in it.
        self.log = deque(maxlen=2000)
        self.log_id = 0
        # Set by the UI to a callable taking raw S16 stereo bytes.
        self.audio_sink = None
        self.audio_bytes = 0

        self._sock = None
        self._running = False
        self._threads = []
        self._partial = {}         # seq -> {chunks, count, size, first_seen}
        self._next_expected = None
        self._window_start = 0.0
        self._window_frames = 0
        self._window_bytes = 0
        self._peer = None
        self._apartial = {}

    # -- lifecycle ----------------------------------------------------------

    def start(self):
        if self._running:
            return
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            # Deliberately no SO_REUSEADDR. On Windows it lets a second
            # instance bind the same UDP port and quietly take the packets,
            # so both copies show "waiting for the console" while the console
            # is streaming perfectly - which cost real debugging time. Without
            # it the second instance fails to bind and says so.
            # A 720p frame at quality 90 is ~40 chunks arriving back to back;
            # the default receive buffer drops them on a busy machine.
            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
            self._sock.bind(("", PORT_PC))
            self._sock.settimeout(0.25)
        except OSError as exc:
            self.error = f"could not bind UDP {PORT_PC}: {exc}"
            self._sock = None
            return

        self._running = True
        self._peer = None
        self._window_start = time.monotonic()
        self._threads = [
            threading.Thread(target=self._recv_loop, name="wstr-recv", daemon=True),
            threading.Thread(target=self._hello_loop, name="wstr-hello", daemon=True),
        ]
        for t in self._threads:
            t.start()

    def stop(self):
        if not self._running:
            return
        self._running = False
        self._say_bye()
        for t in self._threads:
            t.join(timeout=1.0)
        self._threads = []
        if self._sock:
            self._sock.close()
            self._sock = None
        with self.lock:
            self.frame = None
        self.stats.console_ip = None
        self._peer = None

    @property
    def running(self):
        return self._running

    def connected(self):
        return (self.stats.console_ip is not None
                and time.monotonic() - self.stats.last_frame_at < STALE_SECONDS)

    # -- discovery ----------------------------------------------------------

    def _targets(self):
        """Where to send HELLO.

        Broadcast is what makes this zero-config, but Windows machines with a
        VPN or a Hyper-V switch have several interfaces and the global
        255.255.255.255 does not always leave the one facing the console. So
        also send to the /24 broadcast of every local address we can see, and
        to an explicit IP if the user typed one.
        """
        out = ["255.255.255.255"]
        if self.manual_ip.strip():
            out.append(self.manual_ip.strip())
        # Once the console has answered, talk to it directly - broadcast keeps
        # going too, so unplugging and replugging still recovers.
        if self.stats.console_ip:
            out.append(self.stats.console_ip)
        try:
            for info in socket.getaddrinfo(socket.gethostname(), None,
                                           socket.AF_INET):
                ip = info[4][0]
                if ip.startswith("127."):
                    continue
                out.append(ip.rsplit(".", 1)[0] + ".255")
        except OSError:
            pass
        return list(dict.fromkeys(out))

    def _hello_loop(self):
        while self._running:
            body = self.settings.pack()
            pkt = _pack_header(PKT_HELLO, plen=len(body)) + body
            for ip in self._targets():
                try:
                    self._sock.sendto(pkt, (ip, PORT_CONSOLE))
                except OSError:
                    pass
            time.sleep(HELLO_INTERVAL)

    def poke(self):
        """Send a HELLO right now instead of waiting for the next heartbeat.

        Settings normally reach the console within a second, which is fine for
        a slider and useless for anything that then wants to wait on the
        result."""
        if not self._sock:
            return
        body = self.settings.pack()
        pkt = _pack_header(PKT_HELLO, plen=len(body)) + body
        for ip in self._targets():
            try:
                self._sock.sendto(pkt, (ip, PORT_CONSOLE))
            except OSError:
                pass

    def _say_bye(self):
        if not self._sock:
            return
        pkt = _pack_header(PKT_BYE)
        for ip in self._targets():
            try:
                self._sock.sendto(pkt, (ip, PORT_CONSOLE))
            except OSError:
                pass

    # -- receive ------------------------------------------------------------

    def _recv_loop(self):
        while self._running:
            try:
                # Sized for a full WSTR_MTU datagram plus header.
                data, addr = self._sock.recvfrom(16384)
            except socket.timeout:
                self._expire_partials()
                continue
            except OSError:
                break

            if len(data) < HDR.size:
                continue
            magic, ver, ptype, flags, _rsv, seq, idx, cnt, plen, w, h, _ts = \
                HDR.unpack_from(data)
            if magic != MAGIC or ver != VERSION:
                continue

            payload = data[HDR.size:HDR.size + plen]
            if len(payload) != plen:
                continue

            # An explicitly named console is exclusive: it is the only thing
            # worth listening to, and on a LAN with a second console - or a
            # fake_console running for comparison - first-responder wins is
            # exactly the wrong rule.
            if self.manual_ip.strip() and addr[0] != self.manual_ip.strip():
                continue

            # Otherwise lock onto the first console that answers, and ignore
            # the rest.
            # HELLO goes out as a broadcast, so a Wii U and a fake_console on
            # the same LAN will both reply - and since they number their frames
            # independently, mixing them shreds the reassembly and interleaves
            # two unrelated logs into one useless trace.
            if self._peer is None:
                self._peer = addr[0]
                self.stats.console_ip = addr[0]
            elif addr[0] != self._peer:
                continue

            if ptype == PKT_VIDEO:
                self._on_video(seq, idx, cnt, payload, w, h, flags)
            elif ptype == PKT_STATUS:
                self._on_status(payload)
            elif ptype == PKT_AUDIO:
                self._on_audio(seq, idx, cnt, payload)
            elif ptype == PKT_LOG:
                self._on_log(payload)

    def _on_log(self, payload):
        text = payload.decode("utf-8", "replace").strip()
        if not text:
            return
        # Remember the console's build stamp separately: it is the one line
        # that must survive into any saved log, because every number in the
        # file is meaningless without knowing which binary produced it.
        if text.startswith("build "):
            self.stats.console_build = text[6:]
        with self.lock:
            self.log.append((time.strftime("%H:%M:%S"), text))
            self.log_id += 1

    def add_local_log(self, text):
        """Note something the PC did, in the same stream as the console's own
        lines, so the ordering between the two is visible."""
        with self.lock:
            self.log.append((time.strftime("%H:%M:%S"), text))
            self.log_id += 1

    def clear_log(self):
        with self.lock:
            self.log.clear()
            self.log_id += 1

    def log_lines(self):
        with self.lock:
            return list(self.log), self.log_id

    def _on_audio(self, seq, idx, cnt, payload):
        """Audio needs no reassembly: each packet is a whole block of frames.

        It is also never buffered here waiting for a missing piece - a gap in
        audio is better filled immediately with silence than filled correctly
        two packets later, because late audio drifts against the picture and
        stays drifted."""
        if cnt > 1:
            # Belt and braces. The console now sizes audio blocks to fit one
            # datagram, but it did not always, and silently dropping anything
            # that arrives in pieces is how audio failed completely for several
            # sessions while the console reported it was sending perfectly.
            entry = self._apartial.get(seq)
            if entry is None:
                entry = {"chunks": [None] * cnt, "have": 0}
                self._apartial[seq] = entry
                # Audio is continuous, so anything still incomplete is already
                # too late to be worth holding.
                for old in [k for k in self._apartial if _seq_older(k, seq)]:
                    del self._apartial[old]
            if idx >= cnt or entry["chunks"][idx] is not None:
                return
            entry["chunks"][idx] = payload
            entry["have"] += 1
            if entry["have"] != cnt:
                return
            payload = b"".join(entry["chunks"])
            del self._apartial[seq]

        self.audio_bytes += len(payload)
        sink = self.audio_sink
        if sink is not None:
            # The console is big-endian and sends the samples in its own order,
            # like every other field in this protocol. Windows wants
            # little-endian, and skipping this does not sound like an
            # endianness bug - a quiet 0x009D becomes 0x9D00, so the output is
            # full-scale noise with the real audio faintly underneath.
            sink(np.frombuffer(payload, dtype=">i2").astype("<i2").tobytes())

    def _on_status(self, payload):
        if len(payload) < STATUS_BODY.size:
            return
        (sw, sh, ow, oh, fps, q, _audio, _src,
         enc_us, cap_us) = STATUS_BODY.unpack_from(payload)
        self.stats.console_quality = q
        self.stats.src_w = sw
        self.stats.src_h = sh
        self.stats.console_fps = fps
        self.stats.console_encode_ms = enc_us / 1000.0
        self.stats.console_capture_ms = cap_us / 1000.0

    def _on_video(self, seq, idx, cnt, payload, w, h, flags):
        entry = self._partial.get(seq)
        if entry is None:
            entry = {"chunks": [None] * cnt, "have": 0, "count": cnt,
                     "w": w, "h": h, "first": time.monotonic()}
            self._partial[seq] = entry
        if idx >= entry["count"] or entry["chunks"][idx] is not None:
            return

        entry["chunks"][idx] = payload
        entry["have"] += 1

        if entry["have"] != entry["count"]:
            return

        del self._partial[seq]
        self._complete(seq, b"".join(entry["chunks"]), flags)
        self._expire_partials(newest=seq)

    def _expire_partials(self, newest=None):
        """Give up on frames that will never finish.

        Anything older than the newest completed frame is not coming back:
        UDP can reorder a little, but a chunk from two frames ago is lost.
        """
        if not self._partial:
            return
        now = time.monotonic()
        doomed = []
        for seq, entry in self._partial.items():
            stale = now - entry["first"] > 1.0
            superseded = newest is not None and _seq_older(seq, newest)
            if stale or superseded:
                doomed.append(seq)
        # Even without a newer completed frame, do not let partials pile up.
        if len(self._partial) - len(doomed) > MAX_PARTIAL_FRAMES:
            extra = sorted(set(self._partial) - set(doomed))
            doomed.extend(extra[:len(self._partial) - MAX_PARTIAL_FRAMES])
        for seq in doomed:
            self._partial.pop(seq, None)
            self.stats.dropped += 1

    def _complete(self, seq, data, flags):
        img = cv2.imdecode(np.frombuffer(data, dtype=np.uint8),
                           cv2.IMREAD_COLOR)
        if img is None:
            self.stats.dropped += 1
            return

        now = time.monotonic()
        with self.lock:
            self.frame = img
            self.frame_id += 1
        self.stats.frames += 1
        self.stats.bytes += len(data)
        self.stats.frame_bytes = len(data)
        self.stats.last_frame_at = now

        self._window_frames += 1
        self._window_bytes += len(data)
        elapsed = now - self._window_start
        if elapsed >= 1.0:
            self.stats.fps = self._window_frames / elapsed
            self.stats.mbps = self._window_bytes * 8 / elapsed / 1e6
            self._window_frames = 0
            self._window_bytes = 0
            self._window_start = now

    def latest(self):
        """Return (frame, frame_id). The frame is the receiver's own array -
        callers must not write to it."""
        with self.lock:
            return self.frame, self.frame_id


def _seq_older(a, b):
    """Sequence comparison that survives the 32-bit wrap."""
    return ((b - a) & 0xFFFFFFFF) < 0x80000000 and a != b
