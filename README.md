# wiiu-stream

Streams the Wii U's video to a PC over Wi-Fi, with no capture card, and gives
Discord something to point at.

Two halves:

- **`wiiustream.wps`** — an Aroma plugin. It loads into every title, hooks GX2,
  and sends the framebuffer out as a JPEG stream over UDP.
- **`pc/dashboard.py`** — a Dear ImGui window that shows the stream, records it,
  and can expose it as a webcam.

---

## Read this first: what "straight to Discord" can and cannot mean

Discord has **no ingest endpoint**. There is no RTMP URL, no stream key, and Go
Live is a proprietary WebRTC flow that bots and third-party clients cannot join.
Nothing can push video *into* Discord from outside its own client.

What works instead is giving the Discord client a source it already understands.
There are two, and the app supports both:

| | What Discord sees | What you need |
|---|---|---|
| **Screen share** | This app's window | Nothing extra |
| **Virtual camera** | A webcam called "OBS Virtual Camera" | OBS Studio installed, plus `pip install pyvirtualcam` |

Screen share is the default and needs no setup: Go Live → *Application Window* →
pick **Wii U Stream**. Note that Discord's free tier caps screen share at 720p30,
which is well above what this link delivers anyway.

Audio comes down the same link and is played by this app, which means Discord's
screen share picks it up along with the picture — no virtual audio cable. See
[Audio](#audio).

---

## Setup

### Console

Requires [Aroma](https://wiiu.hacks.guide/). Copy the plugin to your SD card:

```
sd:/wiiu/environments/aroma/plugins/wiiustream.wps
```

Reboot into Aroma. That is the entire console-side setup — **there is nothing to
configure**, no IP to type in, no menu to open. The plugin binds a UDP port and
sits idle until the PC app says hello, and it costs nothing while idle.

### PC

```bash
pip install -r pc/requirements.txt
python pc/dashboard.py
```

The app starts listening immediately and finds the console by broadcast. Launch
any game and it should appear within a second or two.

If your network blocks broadcast traffic (some mesh systems and most guest
networks do), type the console's IP into the **Console** box in the sidebar.

---

## Using it

The sidebar controls the console live — moving a slider changes what the Wii U
encodes within about a second, with no restart:

- **Resolution** — 320×180 up to 1280×720. 640×360 is the default and the sweet
  spot; see [Performance](#performance).
- **Frame rate** — 5 to 30 fps. This is the *capture* rate, not the game's; the
  game keeps running at its own speed.
- **Quality** — JPEG quality. Mostly trades bitrate, not CPU.
- **Source** - the TV output or the GamePad screen. One at a time.
- **Auto scaling** - lets the console lower quality, and then resolution, to
  hold the frame rate. Off means it keeps the quality asked for and the frame
  rate falls instead.

The stats bar underneath the video shows what you are actually getting versus
what you asked for, including the console's own encode time per frame. If `fps`
sits well below the slider, the console is the bottleneck — drop the resolution.

**Save log** writes the console's trace to `captures/`, with the stage, heap,
sync mode and stream settings recorded at the top — a log that does not say what
produced it is only half the evidence. **Copy** puts the same text on the
clipboard, and **Clear** starts a fresh trace before a new attempt.

**Record to mp4** and **Save a frame** write into `captures/`. Recording samples
the stream on its own clock, so the file's duration is correct even when frames
arrive unevenly.

---

## Performance

The Wii U has no video encoder available to homebrew, so everything below runs
in software on a 1.24 GHz PowerPC 750. Measured on hardware, per frame:

| preset | read | jpeg | send | fps |
|---|---|---|---|---|
| Smooth 320x180 | 8 ms | 3 ms | 15 ms | ~30 |
| Balanced 480x270 | 8 ms | 8 ms | 23 ms | ~22 |
| Sharp 640x360 | 9 ms | 13 ms | 32 ms | ~18 |

Three separate limits, and knowing which one is binding is the difference
between a fix and a waste of time:

- **read** is the downscale, and it is bound by memory *latency* rather than
  bandwidth - the CPU spends most of it waiting on MEM2. Its cost tracks output
  **height** x source **width**, so narrowing the output does nothing, and the
  GamePad source (854 wide) is a third cheaper than the TV (1280).
- **jpeg** tracks output pixel count.
- **send** is the Wi-Fi link, which carries about 7 Mbit/s. Nothing but sending
  fewer bytes moves it.

Quality only shrinks bytes, so it only shortens the send. When the time is
going into the read and the DCTs instead, lowering quality costs picture and
buys nothing - the adaptation checks this before choosing which lever to pull.

### What was done to get here

Every one of these came from a measurement, and several contradicted a
reasonable guess:

- **Capture tiling.** `GX2_TILE_MODE_LINEAR_SPECIAL` is value 16, outside the
  hardware's 0-15 tile modes - a driver construct, detiled on the *CPU*. That
  was 3.6 MB of software work per capture, on the game's render thread.
  `LINEAR_ALIGNED` is a real hardware mode: `copy 0.0 ms`.
- **Integer DCT.** The float AAN transform was not slow in itself; the 750 has
  no integer<->float register move, so every sample in and coefficient out was
  a store/load round trip - a quarter of a million stalls a frame. Going
  integer was worth 15% on x86 and 30% on the console.
- **Two-core downscale.** Output rows are independent, so half goes to a helper
  on core 0. Nearly halved the read. The JPEG stage stays single-threaded: DC
  prediction and the bit stream both run start to finish.
- **Horizontal 2-pixel average.** Point sampling aliases, and aliasing is noise
  - expensive to encode and not even signal. 10% fewer bytes and 0.9 dB closer
  to a correct downscale, for no measurable read cost on the console.
- **Deadzone quantisation.** Rounding toward zero rather than to-nearest: 8%
  fewer bytes, *better* fidelity, and a faster encoder, since a zeroed
  coefficient costs nothing to code.

x86 mispredicted two of these outright. It holds the whole framebuffer in cache
and has cheap float conversion, so it is a poor model for a latency-bound
in-order PowerPC - the bench in `tools/` is for correctness and relative sanity,
not for deciding what is fast.

## Audio

**Stream audio** captures the console's final mix and plays it on this PC. That
placement is the point: Discord's screen share captures the audio of the window
it is sharing, so the sound reaches the call with no virtual audio cable and
nothing to install. Playback goes through Windows' own `waveOut` via ctypes for
the same reason - a `pip install` for the audio half would undo it.

**Mixer** picks the TV or the GamePad. These are separate device mixes on this
console and are genuinely different audio, so a title can be playing through one
and silent on the other.

**Audio only** switches video capture off entirely: no surfaces, no GPU copies,
no encoding. For watching the picture on the console itself while the sound
comes here - it hands the whole link and the encoder thread to keeping the audio
steady, and gives ~11 MB of the title's heap back.

### How it is captured

`AXRegisterDeviceFinalMixCallback` hands out the final mix, but devkitPro types
the callback's argument as a bare `void *` with no struct behind it. The layout
used here is decaf-emu's `AXDeviceFinalMixData`, which carries explicit
`CHECK_OFFSET` assertions per field, re-asserted with `static_assert` in the
plugin so a mistake is a build error rather than a hardware fault.

Three things the plugin has to get right, each of which broke it once:

- The callback runs on AX's own thread every 3 ms and must never block, so it
  only writes a ring buffer; a separate thread does the sending. Draining it
  from the video worker made the audio stutter *in time with the frame rate*.
- It **chains** to whatever callback the title already had, calling it first. A
  game may modify its own final mix, and dropping its callback would silently
  kill its audio.
- AX is reached through whichever module the title loaded. wut imports from
  `sndcore2.rpl`, many titles use `snd_core.rpl`, and linking the wrong name
  does not fail - it quietly hands you a second, unused instance whose
  `AXIsInit()` is false and whose mixer nothing feeds.

Samples travel big-endian like every other field in this protocol. Getting that
wrong is not subtle and does not look like an endianness bug: a quiet 0x009D
read the wrong way round becomes 0x9D00, so the output is full-scale noise with
the real audio faintly underneath.

### Cost

48 kHz stereo, uncompressed, ~1.5 Mbit/s against a link that carries about 7 -
so audio costs real video bandwidth. Blocks are sized to fit whatever datagram
the console's stack currently accepts; a fixed size larger than that arrives in
pieces, and audio is time-critical enough that reassembling it is a last resort.

---

## If the console freezes

A hard freeze leaves no stack trace, no dump and no log — only a console that
stopped. So the capture path is gated: the PC picks how far the console is
allowed to go, the setting takes effect on the next HELLO, and the log pane
under the video shows the console's own trace of what it reached.

Each step logs *immediately before* it runs, never after, so the last line you
see names the call that hung.

**A fresh install starts at stage Off.** Walk it up one step at a time with the
**Capture stage** dropdown and note where the console stops:

| Stage | What it adds | A freeze here means |
|---|---|---|
| 0 Off | receives settings only | the socket poll on the render thread |
| 1 Send | status + log packets | `sendto` is blocking the render thread |
| 2 Allocate | allocates capture surfaces | the allocator — see the note below |
| 3 Copy | `GX2CopySurface` | the blit is hanging the GPU |
| 4 Readback | GPU wait + invalidate | the wait — try another **gpu sync** mode |
| 5 Full | downscale, encode, send | the encoder thread or the frame sends |

No reboot is needed *between* stages — only after a freeze. The console keeps
running at every stage below the one that breaks, so you can sit at stage 3 and
watch the log to confirm it is healthy before stepping to 4.

### What the game heap actually did

Observed on hardware, from the console's own trace:

```
calc ok: size=3686400 align=1 pitch=1280
MEMAllocFromDefaultHeapEx(3686400, 1) from the game heap...
```

— and nothing after it. Two things came out of that. `MEMAllocFromDefaultHeapEx`
never returned, and because it holds the heap lock while not returning, the
game's next allocation blocked too and the console went down with it. The call
was already on the encoder thread by then, so this is not a render-thread
reentrancy problem: it is the allocator.

And `GX2CalcSurfaceSizeAndAlignment` reports **alignment 1** for a
`LINEAR_SPECIAL` surface. That is true of the tiling and useless as an
allocation request — Cafe OS heaps expect at least 4, and GPU-visible memory
wants a cache line. Every allocation now asks for at least 256 bytes of
alignment, with the size rounded up to match, so the allocator is never handed
a degenerate request.

### What the readback stage did

With allocation fixed, the next freeze was at stage 4 — `GX2DrawDone()`.

`GX2DrawDone()` waits for the *entire* GPU pipeline to retire, and the old code
called it from inside the game's own `GX2CopyColorBufferToScanBuffer`, mid-frame,
while the game was still building the command buffer that frame belongs to.
Forcing a full drain at that point is a good way to deadlock the driver.

The fix is to wait for our own submission instead of for everything.
`GX2GetLastSubmittedTimeStamp()` is recorded right after the copy is issued, and
`GX2WaitTimeStamp()` waits for exactly that one — on the encoder thread, where
blocking costs the game nothing. The **gpu sync** dropdown picks between:

| Mode | What it does |
|---|---|
| Timestamp | waits for our own copy, on the worker thread — the default |
| None | no wait at all; fastest, may occasionally read a torn frame |
| DrawDone | the old full drain on the render thread — kept for comparison |

### Surface memory

Capture surfaces have to live in memory the GPU can write to, and getting there
took two wrong turns worth recording.

WUPS redirects `MEMAllocFromDefaultHeapEx` for plugins, so what looked like a
choice between "the game's heap" and "the plugin's heap" was the same heap
twice — the addresses gave it away, 150 KB apart. That memory is outside the
mapping GX2 translates for the GPU, so the capture blit wrote nowhere at all,
silently, and the encoder faithfully compressed whatever was already there.

In coreinit, `MEMAllocFromDefaultHeapEx` is a *data* export — a pointer holding
whatever allocator the title installed — so the plugin resolves it through
`OSDynLoad` and gets the real one. The same lesson applies to AX, which wut
imports from `sndcore2.rpl` while many titles use `snd_core.rpl`: linking the
wrong name does not fail, it quietly gives you a second, unused instance.

**In a plugin, the symbol you linked is not necessarily the one the game is
using.** The surface check exists to catch exactly this: it poisons the buffer
before the copy and reports how much came back untouched.

Allocation and freeing both happen on the encoder thread, never on the game's
render thread — a deadlock there freezes the whole console, and there was never
a reason to allocate mid-frame in the first place. The encode scratch is plain
`malloc`, since the GPU never sees it.

The trace also reports the running title's actual surface format, tile mode and
AA setting on the first frame. That is the one fact that cannot be guessed from
the PC, and it decides whether the capture path is even applicable to that game.

## Building

Needs devkitPro with the Wii U workload (`pacman -S wiiu-dev`), and a native
gcc for the PC-side test tool.

```bash
./build.sh
```

That fetches and builds the WUPS SDK if it is missing, builds the plugin, and
builds the fake console.

### Why the build scripts do not use make

devkitPro's `wups_rules` and `wut_rules` go through devkitPro's msys2 `make`,
and **msys2 strips `TMP`, `TEMP` and `TMPDIR` from every child process**. With
all three unset, `powerpc-eabi-gcc` falls back to `GetTempPath()`, which returns
`C:\WINDOWS`, and every compile and link dies with:

```
Cannot create temporary file in C:\WINDOWS\: Permission denied
```

This is the same trap documented in `bk-wiiu/WIIU-BUILD.md`, and the same answer
applies: drive the compiler directly with a writable `TMPDIR`. Neither the
plugin nor libwups has generated sources, so a shell script loses nothing.

---

## Testing without the console

`build/fake_console.exe` impersonates the Wii U on the PC. It runs the *same*
`wstr_net.c` and `wstr_jpeg.c` the plugin does, over the same protocol, against
a synthetic 720p framebuffer:

```bash
./build/fake_console.exe
python pc/dashboard.py
```

Useful twice over: it turns a reboot-the-console edit loop into a one-second
one, and it lets you get Discord pointed at the right window before the Wii U is
involved — so if the real stream misbehaves later, you already know the PC half
is fine.

`tools/test_jpeg.c` is a standalone check of the encoder: it runs a test pattern
through the real downscale and encode path and dumps both the JPEG and the input
planes, so a decoder's output can be compared against what actually went in.

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
```

## Protocol

UDP, big-endian, 24-byte header + payload, capped at a 1400-byte MTU.

The PC broadcasts `HELLO` about once a second carrying the resolution, frame
rate, quality and source it wants. The console streams to whoever sent the most
recent one and stops when they stop arriving — so the HELLO is the pairing, the
settings channel and the keepalive all at once, and closing the PC app leaves
the console idle rather than encoding into the void.

Every video frame is a complete JPEG. There is no inter-frame prediction, so a
lost chunk costs exactly one frame and the next is whole again — the right trade
on a LAN, where bandwidth is the resource with headroom and latency is not.
