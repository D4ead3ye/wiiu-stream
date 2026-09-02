# wiiu-stream

Streams the Wii U's picture and sound to a PC over Wi-Fi, with no capture card,
and gives Discord something to share.

- **`wiiustream.wps`** — an Aroma plugin. Loads into every title, hooks GX2, and
  sends the framebuffer out as JPEG over UDP.
- **`pc/dashboard.py`** — a Dear ImGui window that shows the stream, plays the
  audio, records it, and can expose it as a webcam.

**[Download the ready-to-use release](../../releases/latest)** — no building, no
Python. Requires [Aroma](https://wiiu.hacks.guide); nothing else.

---

## Discord: what "straight to Discord" can and cannot mean

Discord has **no ingest endpoint** — no RTMP URL, no stream key, and Go Live is a
proprietary WebRTC flow third parties cannot join. Nothing pushes video *into*
Discord from outside its client.

So give its client a source it already understands:

| | What Discord sees | What you need |
|---|---|---|
| **Screen share** | This app's window | Nothing extra |
| **Virtual camera** | a webcam named "OBS Virtual Camera" | OBS Studio, plus `pip install pyvirtualcam` |

Screen share is the default: Go Live → *Application Window* → **Wii U Stream**.
Audio is played by the app, so the screen share carries it too — no virtual
audio cable.

---

## Setup

**Console.** Copy the plugin to your SD card and boot into Aroma:

```
sd:/wiiu/environments/aroma/plugins/wiiustream.wps
```

There is nothing to configure on the console — no IP, no menu. It binds a UDP
port, sits idle until the PC asks, and costs nothing while idle.

**PC.** Either run `WiiUStream.exe` from the release, or from source:

```bash
pip install -r pc/requirements.txt
python pc/dashboard.py
```

It finds the console by broadcast. If your network blocks broadcast (common on
mesh systems and guest networks), type the console's IP into the **Console** box.

---

## Using it

The sidebar changes what the console does within about a second, live.

**Presets** are measured settings rather than guesses — Smooth ~30 fps, Balanced
~22, Sharp ~18 with the most detail. Start at Balanced.

**Auto scaling** lets the console lower quality, then resolution, to hold the
frame rate you asked for. Leave it on; with it off the quality holds and the
frame rate falls instead. The sidebar shows what it actually settled on, since
the slider is a ceiling rather than a setting.

**Source** picks the TV or the GamePad. The GamePad is cheaper for the console
to capture, so it runs faster.

**Audio** captures the console's final mix. The **mixer** choice matters: TV and
GamePad are separate mixes and carry different audio, so a title can be playing
through one and silent on the other. **Audio only** turns capture off entirely —
for watching the picture on the console while the sound comes here.

**Record to mp4**, **Save a frame** (which briefly raises quality, since a
screenshot has no frame rate to hold) and **Save log** all write to `captures/`.

---

## Performance

Everything runs in software on a 1.24 GHz PowerPC 750 — the Wii U exposes no
video encoder to homebrew. Measured on hardware, per frame:

| preset | read | jpeg | send | fps |
|---|---|---|---|---|
| Smooth 320x180 | 8 ms | 3 ms | 15 ms | ~30 |
| Balanced 480x270 | 8 ms | 8 ms | 23 ms | ~22 |
| Sharp 640x360 | 9 ms | 13 ms | 32 ms | ~18 |

Three separate limits, and knowing which one binds is the difference between a
fix and a waste of time:

- **read** — the downscale, bound by memory *latency*. Its cost tracks output
  **height** × source **width**, so narrowing the output does nothing.
- **jpeg** — tracks output pixel count.
- **send** — the Wi-Fi link, about 7 Mbit/s. Only fewer bytes move it.

Quality only shrinks bytes, so it only shortens the send. When the time is going
into the read and the DCTs instead, lowering quality costs picture and buys
nothing — the adaptation checks this before choosing a lever.

Audio is uncompressed 48 kHz stereo, ~1.5 Mbit/s, so it costs real video
bandwidth.

---

## Building

Needs devkitPro with the Wii U workload (`pacman -S wiiu-dev`), and a native gcc
for the PC-side test tool.

```bash
./build.sh                  # fetches and builds the WUPS SDK if missing
bash tools/make-release.sh  # + the PC executable and a release archive
```

`tools/deploy.py` pushes the plugin straight to a console running ftpiiu, which
beats swapping the SD card; set `WIIU_HOST` to its address.

---

## Layout

```
common/       shared by the console and the test rig
  wstr_proto.h    the wire format
  wstr_jpeg.c     baseline JPEG encoder, 4:2:0 (devkitPro ships none)
  wstr_net.c      UDP, discovery, chunking
plugin/src/   the Aroma plugin: GX2 hook, capture slots, encoder thread
pc/           receiver.py (sockets, reassembly) + dashboard.py (UI)
tools/        build scripts, the fake console, the encoder test
docs/         how it works, and what it took to get there
```

---

## Going further

**[docs/internals.md](docs/internals.md)** covers the parts that are not obvious:
how the capture and audio hooks work, the protocol, why the build scripts avoid
`make`, how to test without a console, what to do if the console freezes, and
the measurements behind each optimisation — including two that x86 predicted
backwards.
