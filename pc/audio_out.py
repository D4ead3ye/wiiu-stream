"""Play the console's audio on this PC, with no third-party dependency.

Why waveOut and not sounddevice/pyaudio: the whole point of playing it here is
that Discord's screen share captures the audio of the window it is sharing, so
the console's sound reaches the call without a virtual audio cable or any other
install. Requiring `pip install` for the audio half would undo that, so this
drives Windows' own multimedia API through ctypes instead.

The design is the usual double-buffered waveOut loop: a pool of headers is
queued to the device, and each one is refilled and requeued as the device
finishes with it. Audio arrives over a lossy link, so a header with nothing to
put in it is filled with silence rather than being held back - a late buffer
would push every later one further behind, and the gap is less noticeable than
the drift.
"""

import ctypes
import ctypes.wintypes as wt
import threading

winmm = ctypes.WinDLL("winmm")

WAVE_MAPPER = 0xFFFFFFFF
WAVE_FORMAT_PCM = 1
WHDR_DONE = 0x00000001
MMSYSERR_NOERROR = 0


class WAVEFORMATEX(ctypes.Structure):
    _fields_ = [
        ("wFormatTag", wt.WORD),
        ("nChannels", wt.WORD),
        ("nSamplesPerSec", wt.DWORD),
        ("nAvgBytesPerSec", wt.DWORD),
        ("nBlockAlign", wt.WORD),
        ("wBitsPerSample", wt.WORD),
        ("cbSize", wt.WORD),
    ]


class WAVEHDR(ctypes.Structure):
    pass


WAVEHDR._fields_ = [
    ("lpData", ctypes.c_char_p),
    ("dwBufferLength", wt.DWORD),
    ("dwBytesRecorded", wt.DWORD),
    ("dwUser", ctypes.POINTER(wt.DWORD)),
    ("dwFlags", wt.DWORD),
    ("dwLoops", wt.DWORD),
    ("lpNext", ctypes.POINTER(WAVEHDR)),
    ("reserved", ctypes.POINTER(wt.DWORD)),
]


class AudioOut:
    """Queue-and-play sink for interleaved 16-bit stereo."""

    # Four buffers of 20 ms. Fewer and any scheduling hiccup underruns; more
    # and the audio lags the picture, which matters because they are being
    # watched together.
    BUFFER_MS = 20
    BUFFERS = 4

    def __init__(self, rate=48000, channels=2):
        self.rate = rate
        self.channels = channels
        self.error = None
        self.underruns = 0
        self.played = 0

        self._hwo = wt.HANDLE()
        self._lock = threading.Lock()
        self._queue = bytearray()
        self._running = False
        self._thread = None
        self._frame_bytes = 2 * channels
        self._chunk = int(rate * self.BUFFER_MS / 1000) * self._frame_bytes

    def start(self):
        if self._running:
            return
        fmt = WAVEFORMATEX(
            wFormatTag=WAVE_FORMAT_PCM,
            nChannels=self.channels,
            nSamplesPerSec=self.rate,
            nAvgBytesPerSec=self.rate * self._frame_bytes,
            nBlockAlign=self._frame_bytes,
            wBitsPerSample=16,
            cbSize=0,
        )
        rc = winmm.waveOutOpen(ctypes.byref(self._hwo), WAVE_MAPPER,
                               ctypes.byref(fmt), 0, 0, 0)
        if rc != MMSYSERR_NOERROR:
            self.error = f"waveOutOpen failed ({rc})"
            return

        self._running = True
        self._thread = threading.Thread(target=self._run, name="wstr-audio",
                                        daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None
        if self._hwo:
            winmm.waveOutReset(self._hwo)
            winmm.waveOutClose(self._hwo)
            self._hwo = wt.HANDLE()

    @property
    def running(self):
        return self._running

    def feed(self, pcm: bytes):
        """Hand over interleaved S16 stereo. Never blocks."""
        with self._lock:
            self._queue.extend(pcm)
            # Cap the backlog. If the producer outruns the device the extra
            # audio is pure latency - dropping the oldest keeps the stream in
            # step with the video instead of drifting further behind it.
            limit = self._chunk * (self.BUFFERS + 4)
            if len(self._queue) > limit:
                del self._queue[:len(self._queue) - limit]

    def _take(self, nbytes):
        with self._lock:
            if len(self._queue) >= nbytes:
                out = bytes(self._queue[:nbytes])
                del self._queue[:nbytes]
                return out
            out = bytes(self._queue)
            self._queue.clear()
        # Short *or* empty both count. Only counting partial fills reported a
        # completely silent stream as perfectly healthy, which is exactly the
        # case worth noticing.
        self.underruns += 1
        return out + b"\x00" * (nbytes - len(out))

    def _run(self):
        buffers = [ctypes.create_string_buffer(self._chunk)
                   for _ in range(self.BUFFERS)]
        headers = []
        for buf in buffers:
            h = WAVEHDR()
            h.lpData = ctypes.cast(buf, ctypes.c_char_p)
            h.dwBufferLength = self._chunk
            h.dwFlags = 0
            winmm.waveOutPrepareHeader(self._hwo, ctypes.byref(h),
                                       ctypes.sizeof(h))
            headers.append(h)

        # Prime every buffer before starting, so the device never sees a gap
        # while the first packets are still in flight.
        for buf, h in zip(buffers, headers):
            ctypes.memmove(buf, self._take(self._chunk), self._chunk)
            winmm.waveOutWrite(self._hwo, ctypes.byref(h), ctypes.sizeof(h))

        while self._running:
            idle = True
            for buf, h in zip(buffers, headers):
                if h.dwFlags & WHDR_DONE:
                    ctypes.memmove(buf, self._take(self._chunk), self._chunk)
                    h.dwFlags &= ~WHDR_DONE
                    winmm.waveOutWrite(self._hwo, ctypes.byref(h),
                                       ctypes.sizeof(h))
                    self.played += self._chunk
                    idle = False
            if idle:
                # A whole buffer is 20 ms; polling much faster than that just
                # burns a core for nothing.
                threading.Event().wait(0.004)

        winmm.waveOutReset(self._hwo)
        for h in headers:
            winmm.waveOutUnprepareHeader(self._hwo, ctypes.byref(h),
                                         ctypes.sizeof(h))
