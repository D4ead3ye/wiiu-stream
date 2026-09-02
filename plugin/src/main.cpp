/* wiiustream - an Aroma plugin that streams the Wii U's video out over Wi-Fi.
 *
 * Why a plugin and not a homebrew app: a .wuhb replaces whatever was running,
 * so it can only ever stream its own screen. A WUPS plugin is loaded into
 * every title, which means it can hook GX2 and copy the framebuffer of the
 * game that is actually running.
 *
 * The hook point is GX2CopyColorBufferToScanBuffer - the call every title
 * makes to hand a finished frame to the video encoder for the TV or the
 * GamePad. At that moment the colour buffer is complete and about to be
 * scanned out, which is exactly the frame we want.
 *
 * Cost control is the whole design problem here. Reading GPU memory from the
 * CPU means a GX2DrawDone(), which drains the pipeline and would cost the
 * game real frame time if it happened every frame. So:
 *
 *   - captures happen at the requested stream rate, not the game's frame rate
 *     (20 fps out of 60 means we pay this on one frame in three);
 *   - the readback is deferred by a frame. GX2CopySurface is issued on one
 *     capture and the result is only waited on at the next one, by which
 *     point the GPU finished it long ago;
 *   - the downscale, JPEG encode and sendto() all happen on a low-priority
 *     thread pinned to core 2, never on the game's render thread.
 *
 * There is no configuration on the console. The plugin binds a UDP port and
 * waits; the PC app broadcasts a HELLO carrying the resolution, frame rate
 * and quality it wants, and the plugin streams back to whoever asked.
 */

#include <wups.h>

#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/event.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <gx2/enum.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>

#include <sndcore2/core.h>
#include <sndcore2/device.h>

#include <nn/ac.h>
#include <nsysnet/_socket.h>

#include <malloc.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include "wstr_jpeg.h"
#include "wstr_net.h"
#include "wstr_proto.h"
}

WUPS_PLUGIN_NAME("Wii U Stream");
WUPS_PLUGIN_DESCRIPTION("Streams the console's video to a PC over Wi-Fi, for capture-card-free recording and Discord.");
#ifndef WSTR_APP_VERSION
#define WSTR_APP_VERSION "dev"
#endif
WUPS_PLUGIN_VERSION("v" WSTR_APP_VERSION);
WUPS_PLUGIN_AUTHOR("wiiu-stream");
WUPS_PLUGIN_LICENSE("MIT");

/* ---------------------------------------------------------------- state */

#define WORKER_STACK_SIZE (128 * 1024)
#define AUDIO_STACK_SIZE  (32 * 1024)

/* GX2 reports alignment 1 for a LINEAR_SPECIAL surface. That is honest about
 * the tiling and useless as an allocation request, so every allocation here
 * asks for at least this much - a GPU cache line's worth, and comfortably
 * above the 4-byte floor Cafe OS heaps expect. */
#define GX2_SURFACE_MIN_ALIGN 256u

/* Anything larger than this is refused rather than silently downscaled: the
 * encoder is a 1.24 GHz PowerPC and 720p is already beyond what it can do at
 * a useful frame rate. */
#define MAX_OUT_W 1280
#define MAX_OUT_H 720

/* A capture slot is one GPU->CPU readback in flight.
 *
 * Three, not two. With two, a worker that takes slightly longer than the
 * capture interval finds both slots occupied on the very next tick and skips
 * it outright - so the rate does not degrade smoothly, it halves. 57 ms of
 * encode against a 50 ms interval collapsed to 10 fps rather than the ~17 the
 * work actually allows. The third slot costs 3.6 MB and absorbs exactly that
 * overrun. */
enum SlotState {
    SLOT_FREE = 0,   /* nothing in it, available for a new copy      */
    SLOT_GPU,        /* GX2CopySurface issued, GPU may still be busy */
    SLOT_READY,      /* synced and invalidated, worker can read it   */
    SLOT_BUSY        /* worker is encoding it                        */
};

struct Slot {
    GX2Surface    surface;
    volatile int  state;
    bool          allocated;
    OSTime        ts;           /* GPU timestamp of this slot's copy       */
};

static Slot        s_slots[3];
static wstr_net    s_net;
static OSThread    s_worker;
static uint8_t    *s_worker_stack = nullptr;
static OSEvent     s_wake;
static volatile bool s_running   = false;
static bool        s_net_ready   = false;

/* Allocation is the worker's job, never the render thread's.
 *
 * MEMAllocFromDefaultHeapEx takes memory from the *game's* heap, and a title
 * that installed its own allocator means that call runs the game's own code.
 * Doing that from inside the game's render thread, mid-frame, in the middle of
 * its own GX2 call, is asking for a deadlock - and a deadlocked render thread
 * is a hard-frozen console. The render thread now only ever raises a flag. */
static volatile bool s_want_netstart = false;
static volatile bool s_want_status   = false;
static volatile bool s_want_alloc  = false;
static volatile bool s_want_free   = false;

/* Hooking and unhooking audio is worker work, for the same reason sends are.
 *
 * audio_start() calls OSDynLoad_Acquire, which takes the RPL loader lock and
 * may pull in a module; audio_stop() calls OSJoinThread and waits. Both were
 * on the render thread, and audio_start() was retried on *every frame* while
 * AX was not yet initialised - which is precisely the window during a title
 * load, contending for the loader lock with the game's own loading. Anything
 * blocking on the render thread freezes the console rather than erroring. */
static volatile bool s_want_audio_start = false;
static volatile bool s_want_audio_stop  = false;
static uint32_t s_audio_try_ms = 0;
static volatile bool s_slots_ready = false;
static uint32_t s_req_w = 0, s_req_h = 0;
static GX2SurfaceFormat s_req_fmt = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
static uint8_t s_req_tile_mode  = WSTR_TILE_LINEAR_ALIGNED;

/* Encode scratch, allocated on first use and sized to the largest output the
 * PC has asked for so far. Kept out of the capture path entirely. */
static uint8_t *s_y = nullptr, *s_cb = nullptr, *s_cr = nullptr, *s_jpeg = nullptr;
static int      s_buf_w = 0, s_buf_h = 0;
static size_t   s_jpeg_cap = 0;

static uint32_t s_last_capture_ms = 0;
static uint32_t s_next_capture_ms = 0;
static uint32_t s_last_status_ms  = 0;

/* Rolling counters, published in the STATUS packet so the PC can show what
 * the console is really managing rather than what it was asked for. */
static uint32_t s_frames_this_sec = 0;
static uint32_t s_sec_start_ms    = 0;
static uint32_t s_fps_actual      = 0;
static uint32_t s_encode_us       = 0;
static uint32_t s_capture_us      = 0;

/* Where the time actually goes, measured rather than guessed. "1.6 fps and the
 * game is crawling" has several possible causes with completely different
 * fixes - a slow GPU blit, uncached reads of the captured surface, the DCTs,
 * or the sends - and only the console can tell them apart. */
static uint32_t s_wait_us  = 0;   /* blocked on the GPU finishing our copy   */
static uint32_t s_read_us  = 0;   /* downscale + colour convert = the reads  */
static uint32_t s_jpeg_us  = 0;   /* the entropy coding and DCTs             */
static uint32_t s_send_us  = 0;   /* chunked sendto()                        */
static uint32_t s_copy_us  = 0;   /* GX2CopySurface on the render thread     */
static uint32_t s_timing_log_ms = 0;
static uint32_t s_build_log_ms = 0;
static uint32_t s_net_retry_ms = 0;

/* Quality is adapted to fit the link, not left where the slider put it.
 *
 * The sends turned out to be bandwidth-limited rather than call-limited: two
 * runs with very different datagram counts both came out at 7-8 Mbit/s, which
 * is what this console's Wi-Fi will carry. Bytes are therefore the only thing
 * that matters, and the JPEG quality is the dial that controls them.
 *
 * Asking a person to hand-tune that against a number they cannot see is a poor
 * trade when the console already measures its own frame time. This walks the
 * quality down until the work fits the requested interval, and back up when it
 * comfortably does - bounded below so it degrades to "soft but smooth" rather
 * than to mush, and never above what the PC asked for. */
static uint8_t  s_auto_quality  = 0;   /* 0 = not yet initialised */
static uint8_t  s_auto_scale    = 100; /* percent of the requested size   */
static uint8_t  s_quality_asked = 0;
static uint32_t s_quality_log_ms = 0;
/* Below about 55 the DCT artefacts stop being a soft picture and start being
 * a visibly broken one, which is not a trade worth making for frames. The
 * adaptation stops there while it still has resolution in hand, and only goes
 * lower once the frame is already as small as it will go. */
#define QUALITY_FLOOR 45u
#define QUALITY_SOFT_FLOOR 55u

/* Two counters that separate the three things a low stream rate can mean.
 *
 * scan  - how often the title actually calls the hook. Captures can only
 *         happen on a scanout, so this is a hard ceiling: a game presenting at
 *         30 Hz cannot be captured at 40, and asking for 20 gets 15 because
 *         the interval quantises to whole frames.
 * skip  - captures dropped because the encoder still owned both slots. This is
 *         the encode being too slow, which is the only one a lower resolution
 *         fixes. */
static uint32_t s_scan_count   = 0;
static uint32_t s_scan_per_sec = 0;
static uint32_t s_skip_busy    = 0;
static uint32_t s_skip_per_sec = 0;
static uint16_t s_src_w = 0, s_src_h = 0;

/* One-shot latches for the diagnostic log. Each risky step announces itself
 * the first time only - at capture rate an unlatched log would be a flood,
 * and what matters is which step was reached, not how often. Reset whenever a
 * peer connects, so every session starts with a fresh trace. */
static bool s_described    = false;
static bool s_warned_aa    = false;
static bool s_logged_alloc = false;
static bool s_logged_copy  = false;
static bool s_logged_sync  = false;
static bool s_warned_format = false;
static bool s_warned_send   = false;
static bool s_audio_described = false;
static bool s_warned_ax       = false;
static uint32_t s_audio_log_ms = 0;
static bool s_warned_audio_send = false;

static void reset_log_latches()
{
    s_described = s_warned_aa = false;
    s_logged_alloc = s_logged_copy = s_logged_sync = false;
    s_warned_format = s_warned_send = false;
    s_audio_described = false;
    s_warned_ax = false;
    s_warned_audio_send = false;
    /* Not s_want_free: a release already asked for must still happen. */
    s_want_alloc = false;
}

static inline uint32_t now_ms()
{
    return (uint32_t)OSTicksToMilliseconds(OSGetSystemTime());
}

static inline uint32_t now_us()
{
    return (uint32_t)OSTicksToMicroseconds(OSGetSystemTime());
}

/* Send a line to the PC's log pane.
 *
 * When the console hard-freezes there is no stack trace and no dump - the only
 * evidence is what already reached the wire. So the pattern throughout the
 * capture path is to log immediately *before* each call that could hang, never
 * after: the last line the PC shows names the call that killed it. */
/* The socket is blocking on this console - non-blocking mode is only set under
 * _WIN32, and sendto() is passed flags 0 - so every send is a synchronous round
 * trip into the network stack. On the worker that is merely slow. On the game's
 * render thread, inside the GX2CopyColorBufferToScanBuffer hook, a send that
 * stalls (peer gone, send buffer full, wifi roaming) stops the game presenting
 * frames and the console is hard frozen with no fault to show for it.
 *
 * So the render thread never touches the socket. It posts here and the worker
 * drains it. One producer other than the worker, one consumer, so a plain
 * head/tail ring is enough; a full queue drops the oldest line, because losing
 * a diagnostic beats blocking a frame. */
#define LOGQ_LINES 24
#define LOGQ_LINE  176
static char              s_logq[LOGQ_LINES][LOGQ_LINE];
static volatile uint32_t s_logq_head = 0;
static volatile uint32_t s_logq_tail = 0;

static bool on_worker_thread()
{
    return OSGetCurrentThread() == &s_worker;
}

static void logq_push(const char *line)
{
    uint32_t head = s_logq_head;
    if (head - s_logq_tail >= LOGQ_LINES) {
        s_logq_tail = s_logq_tail + 1; /* drop the oldest, never stall a frame */
    }
    char *dst = s_logq[head % LOGQ_LINES];
    size_t n = strlen(line);
    if (n > LOGQ_LINE - 1) n = LOGQ_LINE - 1;
    memcpy(dst, line, n);
    dst[n] = 0;
    s_logq_head = head + 1;
}

static void logq_drain()
{
    while (s_logq_tail != s_logq_head) {
        const uint32_t tail = s_logq_tail;
        wstr_net_log(&s_net, s_logq[tail % LOGQ_LINES], now_ms());
        s_logq_tail = tail + 1;
    }
}

static void plog(const char *fmt, ...)
{
    if (!s_net_ready || s_net.peer_addr == 0) return;
    if (s_net.stage < WSTR_STAGE_SEND) return;

    char buf[LOGQ_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;

    if (on_worker_thread()) {
        wstr_net_log(&s_net, buf, now_ms());
    } else {
        logq_push(buf);
    }
}

/* The running title's real allocator, not the plugin's.
 *
 * WUPS redirects MEMAllocFromDefaultHeapEx for plugins so a plugin cannot
 * corrupt the game's heap. Sensible - but it means the memory comes from the
 * plugin's own region, which is outside the mapping GX2 translates for the
 * GPU. The capture blit then writes nowhere and the encoder compresses stale
 * memory, with no error anywhere: measured as 960 out of 960 sampled words
 * coming back still holding the poison pattern, on both "heaps", because they
 * were the same heap. The giveaway was the addresses: 0x80d90f00 from the
 * plugin heap and 0x80d74900 from the supposed game heap.
 *
 * In coreinit MEMAllocFromDefaultHeapEx is a *data* export - a pointer holding
 * whatever allocator the title installed - so resolving and dereferencing it
 * gives the real one, and memory the GPU can actually address. */
typedef void *(*MEMAllocFromDefaultHeapExFn)(uint32_t size, int32_t align);
typedef void (*MEMFreeToDefaultHeapFn)(void *ptr);

static MEMAllocFromDefaultHeapExFn s_title_alloc = nullptr;
static MEMFreeToDefaultHeapFn      s_title_free  = nullptr;

static void resolve_title_allocator()
{
    OSDynLoad_Module coreinit = nullptr;
    void **alloc_var = nullptr;
    void **free_var = nullptr;

    s_title_alloc = nullptr;
    s_title_free = nullptr;

    if (OSDynLoad_Acquire("coreinit.rpl", &coreinit) != OS_DYNLOAD_OK) return;

    if (OSDynLoad_FindExport(coreinit, OS_DYNLOAD_EXPORT_DATA,
                             "MEMAllocFromDefaultHeapEx",
                             (void **)&alloc_var) == OS_DYNLOAD_OK &&
        alloc_var != nullptr) {
        s_title_alloc = (MEMAllocFromDefaultHeapExFn)*alloc_var;
    }
    if (OSDynLoad_FindExport(coreinit, OS_DYNLOAD_EXPORT_DATA,
                             "MEMFreeToDefaultHeap",
                             (void **)&free_var) == OS_DYNLOAD_OK &&
        free_var != nullptr) {
        s_title_free = (MEMFreeToDefaultHeapFn)*free_var;
    }

    OSDynLoad_Release(coreinit);

    /* Both or neither - a mismatched pair would free with the wrong allocator. */
    if (!s_title_alloc || !s_title_free) {
        s_title_alloc = nullptr;
        s_title_free = nullptr;
    }
}

/* Did the GPU actually write anything into the capture surface?
 *
 * A blit to memory the GPU cannot reach fails silently - GX2CopySurface
 * returns, the timings look healthy, and the encoder faithfully compresses
 * whatever was already in that memory. Sampling the surface turns "the picture
 * looks like noise" into a number: a poisoned buffer that comes back still
 * poisoned means the copy never landed. */
#define POISON_WORD 0xCDCDCDCDu

static void poison_surface(Slot &s)
{
    uint32_t *p = (uint32_t *)s.surface.image;
    if (!p) return;
    /* Only the first row of each 64 - enough to detect a copy that never
     * happened, cheap enough to do every frame. */
    for (uint32_t y = 0; y < s.surface.height; y += 64) {
        uint32_t *row = p + (size_t)y * s.surface.pitch;
        for (uint32_t x = 0; x < s.surface.width; x += 16) row[x] = POISON_WORD;
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, s.surface.image, s.surface.imageSize);
}

static void report_surface(Slot &s)
{
    const uint32_t *p = (const uint32_t *)s.surface.image;
    uint32_t poisoned = 0, zero = 0, other = 0;
    if (!p) return;

    for (uint32_t y = 0; y < s.surface.height; y += 64) {
        const uint32_t *row = p + (size_t)y * s.surface.pitch;
        for (uint32_t x = 0; x < s.surface.width; x += 16) {
            uint32_t v = row[x];
            if (v == POISON_WORD)  poisoned++;
            else if ((v & 0xffffff00u) == 0) zero++;
            else other++;
        }
    }

    plog("surface check: %u untouched, %u black, %u real (first=%08x)",
         (unsigned)poisoned, (unsigned)zero, (unsigned)other, (unsigned)p[0]);
    if (poisoned > other) {
        plog("the GPU did not write this surface - this memory is not "
             "GPU-visible (allocator: %s)",
             s_title_alloc ? "title" : "plugin fallback");
    }
}

/* ------------------------------------------------------------------ audio */

/* The layout of the final mix callback's argument.
 *
 * wut types that argument as a bare void*, which is why audio was left out of
 * the first version - guessing at an undocumented struct is how you crash
 * someone's console. This is not a guess: it is decaf-emu's definition, which
 * carries explicit CHECK_OFFSET assertions for every field, transcribed with
 * those offsets re-asserted below so a mistake here is a build error rather
 * than a hardware fault. */
struct AXDeviceFinalMixData {
    int32_t **data;         /* 0x00: array of per-channel sample pointers */
    uint16_t  channels;     /* 0x04 */
    uint16_t  samples;      /* 0x06 */
    uint16_t  numDevices;   /* 0x08 */
    uint16_t  channelsOut;  /* 0x0a */
} __attribute__((packed));

static_assert(sizeof(AXDeviceFinalMixData) == 0x0c, "AX mix data size");
static_assert(__builtin_offsetof(AXDeviceFinalMixData, channels) == 0x04, "");
static_assert(__builtin_offsetof(AXDeviceFinalMixData, samples)  == 0x06, "");

/* Half a second of 48 kHz stereo. The callback runs on AX's own thread on a
 * 3 ms cadence and must never block, so it only ever writes into this ring;
 * the worker drains it. Overrun drops the oldest audio rather than stalling
 * the console's audio path. */
#define AUDIO_RING_FRAMES 24576u          /* power of two, for a cheap mask */
#define AUDIO_RING_MASK   (AUDIO_RING_FRAMES - 1u)

static int16_t *s_aring = nullptr;
static volatile uint32_t s_awrite = 0;    /* frame index, free-running */
static volatile uint32_t s_aread  = 0;
static volatile bool s_audio_on = false;
static bool s_audio_hooked = false;
static AXDeviceFinalMixCallback s_prev_mix_cb = nullptr;
static uint32_t s_hooked_device = 0;   /* AX_DEVICE_TYPE_* actually hooked */
static uint32_t s_audio_rate = 48000;
static uint32_t s_audio_dropped = 0;

/* How far to shift AX's 32-bit mix down to reach 16 bits.
 *
 * AX sums voices into 32-bit accumulators, and how much headroom that leaves
 * above 16-bit full scale is not something the headers say. Clamping on the
 * assumption it was already 16-bit turned every loud passage into a square
 * wave - deafening, with the harsh high-frequency edge that clipping makes.
 *
 * Guessing a second time would be no better than the first, so this measures
 * instead: track the largest magnitude ever seen and shift by however much it
 * takes to fit. It only ever increases, so the level settles within a moment
 * of the first loud sound and never clips afterwards. The peak is reported to
 * the PC log too, which is what pins the platform's real scale down. */
static volatile uint32_t s_ax_shift = 0;
static uint32_t s_ax_peak = 0;

/* Snapshot of the callback's arguments, logged from the render thread. The
 * callback itself must not log - plog() sends a UDP packet, and blocking AX's
 * thread on the network would stutter the console's audio. */
static volatile uint16_t s_ax_channels = 0;
static volatile uint16_t s_ax_samples = 0;
static volatile uint16_t s_ax_ndev = 0;
static volatile uint16_t s_ax_chout = 0;

/* The channel pointers themselves, and a few samples from each.
 *
 * ch=6 means this is a surround mix, and whether data[] is six separate
 * channel buffers or one interleaved block behind a single pointer changes
 * completely what data[0] and data[1] mean. The gap between consecutive
 * pointers answers it: samples*4 bytes apart means per-channel blocks laid
 * end to end, anything else means a different layout. Reading an interleaved
 * buffer as if it were one channel is precisely what would sound like
 * high-pitched noise. */
static volatile uint32_t s_ax_p0 = 0, s_ax_p1 = 0, s_ax_p2 = 0;
static volatile int32_t  s_ax_s0[4] = {0,0,0,0};
static volatile int32_t  s_ax_s1[4] = {0,0,0,0};

static void final_mix_cb(void *arg)
{
    /* The title's own callback runs first: it is entitled to modify the final
     * mix, and what we want to capture is what it produced, not what it was
     * handed. Chaining is not optional - dropping it would silently break the
     * audio of any game that uses one. */
    if (s_prev_mix_cb) s_prev_mix_cb(arg);

    if (!s_audio_on || !s_aring || !arg) return;

    AXDeviceFinalMixData *d = (AXDeviceFinalMixData *)arg;
    if (!d->data || d->samples == 0 || d->channels == 0) return;

    const int32_t *l = d->data[0];
    const int32_t *r = (d->channels > 1 && d->data[1]) ? d->data[1] : l;
    if (!l) return;

    if (s_ax_p0 == 0) {
        s_ax_p0 = (uint32_t)(uintptr_t)d->data[0];
        s_ax_p1 = (d->channels > 1) ? (uint32_t)(uintptr_t)d->data[1] : 0;
        s_ax_p2 = (d->channels > 2) ? (uint32_t)(uintptr_t)d->data[2] : 0;
        for (int k = 0; k < 4; k++) {
            s_ax_s0[k] = d->data[0] ? d->data[0][k] : 0;
            s_ax_s1[k] = (d->channels > 1 && d->data[1]) ? d->data[1][k] : 0;
        }
    }

    s_ax_channels = d->channels;
    s_ax_samples  = d->samples;
    s_ax_ndev     = d->numDevices;
    s_ax_chout    = d->channelsOut;

    uint32_t w = s_awrite;
    const uint32_t rd = s_aread;
    uint32_t space = AUDIO_RING_FRAMES - (w - rd);
    uint32_t n = d->samples;

    if (n > space) {
        /* Nobody is draining fast enough. Keep the newest audio - stale
         * samples are worse than a gap, because they push every later sample
         * further out of sync with the picture. */
        s_audio_dropped += n - space;
        n = space;
    }

    /* Find this block's peak first, so the shift is chosen before anything is
     * written rather than partway through it. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = (uint32_t)(l[i] < 0 ? -l[i] : l[i]);
        uint32_t b = (uint32_t)(r[i] < 0 ? -r[i] : r[i]);
        if (a > s_ax_peak) s_ax_peak = a;
        if (b > s_ax_peak) s_ax_peak = b;
    }
    {
        uint32_t shift = s_ax_shift;
        while ((s_ax_peak >> shift) > 32767u && shift < 16u) shift++;
        s_ax_shift = shift;
    }

    const uint32_t shift = s_ax_shift;
    for (uint32_t i = 0; i < n; i++) {
        int32_t sl = l[i] >> shift;
        int32_t sr = r[i] >> shift;
        /* Still clamped: the shift is chosen from the peak so far, and the
         * very first sample of a new loudest passage can land a hair over. */
        if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
        int16_t *slot = s_aring + ((w + i) & AUDIO_RING_MASK) * 2;
        slot[0] = (int16_t)sl;
        slot[1] = (int16_t)sr;
    }

    s_awrite = w + n;
}

/* AX has to be reached through whichever module the *title* loaded.
 *
 * wut's stubs import AX from sndcore2.rpl, but Cafe exposes the same library
 * under two names and plenty of titles use snd_core.rpl instead. Linking
 * against the wrong one does not fail: the loader simply brings up a second,
 * independent instance for us. AXIsInit() on that instance is false, and a
 * final mix callback registered on it is attached to a mixer nothing feeds -
 * which is exactly "audio silently does nothing", with no error anywhere.
 *
 * So both are probed and the one that reports itself initialised wins. This is
 * the same lesson as the allocator: in a plugin, the symbol you linked is not
 * necessarily the one the game is using. */
typedef BOOL (*AXIsInitFn)(void);
typedef uint32_t (*AXGetRateFn)(void);
typedef int32_t (*AXGetMixCbFn)(uint32_t, AXDeviceFinalMixCallback *);
typedef int32_t (*AXSetMixCbFn)(uint32_t, AXDeviceFinalMixCallback);

static AXIsInitFn   ax_is_init   = nullptr;
static AXGetRateFn  ax_get_rate  = nullptr;
static AXGetMixCbFn ax_get_mixcb = nullptr;
static AXSetMixCbFn ax_set_mixcb = nullptr;
static const char  *ax_module    = nullptr;

static bool try_ax_module(const char *name)
{
    OSDynLoad_Module mod = nullptr;
    AXIsInitFn is_init = nullptr;

    if (OSDynLoad_Acquire(name, &mod) != OS_DYNLOAD_OK || !mod) return false;

    OSDynLoad_FindExport(mod, OS_DYNLOAD_EXPORT_FUNC, "AXIsInit",
                         (void **)&is_init);
    /* Initialised is the whole test - an uninitialised instance is the wrong
     * instance, not merely an early one. */
    if (!is_init || !is_init()) {
        OSDynLoad_Release(mod);
        return false;
    }

    ax_is_init = is_init;
    OSDynLoad_FindExport(mod, OS_DYNLOAD_EXPORT_FUNC, "AXGetInputSamplesPerSec",
                         (void **)&ax_get_rate);
    OSDynLoad_FindExport(mod, OS_DYNLOAD_EXPORT_FUNC,
                         "AXGetDeviceFinalMixCallback", (void **)&ax_get_mixcb);
    OSDynLoad_FindExport(mod, OS_DYNLOAD_EXPORT_FUNC,
                         "AXRegisterDeviceFinalMixCallback",
                         (void **)&ax_set_mixcb);

    /* Deliberately not released: the pointers have to stay valid, and the
     * module is the title's own anyway. */
    if (!ax_get_mixcb || !ax_set_mixcb) {
        ax_is_init = nullptr;
        return false;
    }
    ax_module = name;
    return true;
}

static bool resolve_ax()
{
    if (ax_is_init && ax_is_init()) return true;
    return try_ax_module("snd_core.rpl") || try_ax_module("sndcore2.rpl");
}

/* Audio runs on its own thread, not the video worker's.
 *
 * The worker wakes once per capture, so draining audio there delivers it in
 * bursts at the frame rate - at 9 fps that is one burst every 110 ms into a
 * buffer holding 80 ms, which stutters audibly and in time with the video.
 * Worse, a slow encode blocks the drain for as long as it takes.
 *
 * A thread that wakes every few milliseconds decouples the two entirely. It
 * shares the socket with the worker, which is safe here: the two use separate
 * sequence counters and UDP sends are atomic, so the only shared mutable state
 * is the datagram size, which changes at most a handful of times per session. */
static OSThread  s_audio_thread;
static uint8_t  *s_audio_stack = nullptr;
static volatile bool s_audio_running = false;


static void audio_flush();

static int audio_thread_main(int, const char **)
{
    while (s_audio_running) {
        audio_flush();
        /* A 15 ms packet every 15 ms; waking at 5 ms keeps the queue short
         * without spinning. */
        OSSleepTicks(OSMillisecondsToTicks(5));
    }
    return 0;
}

/* Registration is lazy and happens once, from the render thread.
 *
 * Not at plugin start: AX belongs to the title and may not be initialised yet,
 * and registering a callback into an uninitialised audio stack is a crash with
 * none of the diagnostics the video path has. AXIsInit() is the gate. */
static void audio_start()
{
    if (s_audio_hooked) return;
    if (!resolve_ax()) {
        /* Retried on every scanout - a title that has not started its audio
         * yet will start it later, and this costs two failed lookups until
         * it does. */
        if (!s_warned_ax) {
            plog("audio: no initialised AX in snd_core.rpl or sndcore2.rpl yet");
            s_warned_ax = true;
        }
        return;
    }

    if (!s_aring) {
        s_aring = (int16_t *)malloc((size_t)AUDIO_RING_FRAMES * 2 * sizeof(int16_t));
        if (!s_aring) {
            plog("audio: ring allocation failed");
            return;
        }
    }
    s_awrite = 0;
    s_aread = 0;
    s_audio_dropped = 0;
    s_ax_p0 = 0;

    s_audio_rate = ax_get_rate ? ax_get_rate() : 48000;
    if (s_audio_rate < 8000 || s_audio_rate > 96000) s_audio_rate = 48000;

    s_prev_mix_cb = nullptr;
    s_hooked_device = (s_net.audio_source == WSTR_AUDIO_SRC_DRC)
                          ? AX_DEVICE_TYPE_DRC : AX_DEVICE_TYPE_TV;
    ax_get_mixcb(s_hooked_device, &s_prev_mix_cb);
    ax_set_mixcb(s_hooked_device, final_mix_cb);

    /* Started before the callback is registered, so the first samples the
     * callback produces already have something draining them. */
    if (!s_audio_stack) {
        s_audio_stack = (uint8_t *)memalign(16, AUDIO_STACK_SIZE);
    }
    if (s_audio_stack && !s_audio_running) {
        s_audio_running = true;
        if (OSCreateThread(&s_audio_thread, audio_thread_main, 0, nullptr,
                           s_audio_stack + AUDIO_STACK_SIZE, AUDIO_STACK_SIZE,
                           18, (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
            OSSetThreadName(&s_audio_thread, "wiiu-stream audio");
            OSResumeThread(&s_audio_thread);
        } else {
            s_audio_running = false;
            plog("audio: could not start the sender thread");
        }
    }

    s_audio_hooked = true;
    plog("audio: hooked via %s on %s at %u Hz (previous callback %s)",
         ax_module ? ax_module : "?",
         s_hooked_device == AX_DEVICE_TYPE_DRC ? "gamepad" : "tv",
         (unsigned)s_audio_rate, s_prev_mix_cb ? "chained" : "none");
}

static void audio_stop()
{
    if (!s_audio_hooked) return;
    /* Hand the title's own callback back before we go, or its audio stops
     * with us. */
    if (ax_set_mixcb) ax_set_mixcb(s_hooked_device, s_prev_mix_cb);
    s_prev_mix_cb = nullptr;
    s_audio_hooked = false;
    s_audio_on = false;

    /* Stopped after the callback is unhooked, so nothing is still being
     * produced while the drain is going away. */
    if (s_audio_running) {
        s_audio_running = false;
        OSJoinThread(&s_audio_thread, nullptr);
    }
    if (s_audio_stack) { free(s_audio_stack); s_audio_stack = nullptr; }
    if (s_aring) { free(s_aring); s_aring = nullptr; }
    s_awrite = 0;
    s_aread = 0;
}

/* Drain whatever the callback has queued and put it on the wire. Runs on the
 * worker, never in the callback - a sendto() on AX's thread would stall the
 * console's audio. */
static void audio_flush()
{
    if (!s_aring || !s_audio_on) return;

    /* Size the block to the datagram, not the other way round.
     *
     * The datagram size adapts to whatever the console's network stack will
     * accept, and it has been observed dropping to 552 bytes. A fixed 2880
     * byte audio block then arrives as six chunks - and audio is not
     * reassembled at the far end, so every packet was thrown away. Fitting the
     * block to the current datagram keeps it a single packet at any rung. */
    uint32_t chunk_frames = s_net.chunk_size / 4u;   /* 4 bytes per frame */
    if (chunk_frames > WSTR_AUDIO_FRAMES) chunk_frames = WSTR_AUDIO_FRAMES;
    if (chunk_frames < 64) chunk_frames = 64;

    for (;;) {
        uint32_t w = s_awrite;
        uint32_t r = s_aread;
        uint32_t avail = w - r;
        if (avail < chunk_frames) break;

        /* One packet's worth, unwrapped into a contiguous staging buffer -
         * the ring's split at the wrap point is not the network's problem.
         * File-scope rather than on the stack: at 15 ms blocks this is 2.8 KB,
         * and only the audio thread ever touches it. */
        static int16_t staging[WSTR_AUDIO_FRAMES * 2];
        for (uint32_t i = 0; i < chunk_frames; i++) {
            const int16_t *slot = s_aring + ((r + i) & AUDIO_RING_MASK) * 2;
            staging[i * 2 + 0] = slot[0];
            staging[i * 2 + 1] = slot[1];
        }

        if (wstr_net_send_audio(&s_net, (const uint8_t *)staging,
                                (size_t)chunk_frames * 4, now_ms()) != 0) {
            /* Said once. A refused audio send is invisible otherwise - it
             * looks exactly like "audio is not working", while still costing
             * the link every attempt. */
            if (!s_warned_audio_send) {
                plog("audio: send failed (errno %u) at %u byte blocks",
                     (unsigned)s_net.last_send_errno,
                     (unsigned)(chunk_frames * 4));
                s_warned_audio_send = true;
            }
            break;      /* the link is refusing; try again next wake */
        }
        s_aread = r + chunk_frames;
    }
}

/* --------------------------------------------------------- scratch buffers */

/* The encode scratch is plain malloc, not the game's heap: the GPU never sees
 * these planes, so there is no reason to take memory from the running title -
 * or to call its allocator - just to hold them. */
static void free_scratch()
{
    if (s_y)    { free(s_y);    s_y = nullptr; }
    if (s_cb)   { free(s_cb);   s_cb = nullptr; }
    if (s_cr)   { free(s_cr);   s_cr = nullptr; }
    if (s_jpeg) { free(s_jpeg); s_jpeg = nullptr; }
    s_buf_w = s_buf_h = 0;
    s_jpeg_cap = 0;
}

static bool ensure_scratch(int w, int h)
{
    if (s_y && w <= s_buf_w && h <= s_buf_h) return true;

    free_scratch();

    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    size_t bound = wstr_jpeg_bound(w, h);

    s_y    = (uint8_t *)malloc((size_t)w * h);
    s_cb   = (uint8_t *)malloc((size_t)cw * ch);
    s_cr   = (uint8_t *)malloc((size_t)cw * ch);
    s_jpeg = (uint8_t *)malloc(bound);

    if (!s_y || !s_cb || !s_cr || !s_jpeg) {
        /* A game that has already eaten its heap is a real possibility, and
         * failing to stream is much better than failing to run. */
        free_scratch();
        return false;
    }

    s_buf_w = w;
    s_buf_h = h;
    s_jpeg_cap = bound;
    return true;
}

/* ------------------------------------------------------------ capture slots */

static void free_slot(Slot &s)
{
    if (s.allocated && s.surface.image) {
        if (s_title_free) s_title_free(s.surface.image);
        else              MEMFreeToDefaultHeap(s.surface.image);
    }
    memset(&s.surface, 0, sizeof(s.surface));
    s.allocated = false;
    s.state = SLOT_FREE;
}

/* A linear-special surface is the one tiling mode the CPU can read directly,
 * so GX2CopySurface untiles into it for us.
 *
 * The format is copied from the source rather than forced to RGBA8. Asking
 * GX2CopySurface to convert format *and* tiling in one blit is the more
 * demanding of the two operations and not something the hardware promises for
 * every pair - and a blit the GPU cannot retire is a GPU hang, which on this
 * console means the whole thing locks up with nothing to read afterwards.
 * Tiling alone is the conversion that matters here; the CPU side then checks
 * the format it actually got and refuses anything that is not 8-bit RGBA. */
static bool alloc_slot(Slot &s, uint32_t w, uint32_t h, GX2SurfaceFormat fmt,
                       uint8_t tile)
{
    const GX2TileMode tile_mode = (tile == WSTR_TILE_LINEAR_SPECIAL)
                                      ? GX2_TILE_MODE_LINEAR_SPECIAL
                                      : GX2_TILE_MODE_LINEAR_ALIGNED;

    if (s.allocated && s.surface.width == w && s.surface.height == h &&
        s.surface.format == fmt && s.surface.tileMode == tile_mode) {
        return true;
    }

    free_slot(s);

    memset(&s.surface, 0, sizeof(s.surface));
    s.surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    s.surface.width     = w;
    s.surface.height    = h;
    s.surface.depth     = 1;
    s.surface.mipLevels = 1;
    s.surface.format    = fmt;
    s.surface.aa        = GX2_AA_MODE1X;
    /* Declared as a colour buffer as well as a texture: this surface is
     * something the GPU *writes*, and USE_TEXTURE alone describes memory it
     * only ever reads. */
    s.surface.use       = (GX2SurfaceUse)(GX2_SURFACE_USE_TEXTURE |
                                          GX2_SURFACE_USE_COLOR_BUFFER);
    s.surface.tileMode  = tile_mode;
    s.surface.swizzle   = 0;

    plog("calc surface %ux%u fmt=0x%x tile=%u...", (unsigned)w, (unsigned)h,
         (unsigned)fmt, (unsigned)tile_mode);
    GX2CalcSurfaceSizeAndAlignment(&s.surface);
    plog("calc ok: size=%u align=%u pitch=%u",
         (unsigned)s.surface.imageSize, (unsigned)s.surface.alignment,
         (unsigned)s.surface.pitch);

    /* If GX2 returned something absurd for this format/tiling combination,
     * asking the heap for it is how a bad number becomes a hung console. A
     * 720p RGBA8 surface is under 4 MB; anything past 64 MB is nonsense. */
    if (s.surface.imageSize == 0 || s.surface.imageSize > (64u << 20) ||
        s.surface.alignment > (1u << 20)) {
        plog("REFUSING absurd surface: size=%u align=%u",
             (unsigned)s.surface.imageSize, (unsigned)s.surface.alignment);
        memset(&s.surface, 0, sizeof(s.surface));
        return false;
    }

    /* GX2 reports alignment 1 for LINEAR_SPECIAL, which is true of the tiling
     * but useless as an allocation request. Cafe OS expanded heaps want at
     * least 4, and anything the GPU reads wants a cache line or better - the
     * one observed hang was MEMAllocFromDefaultHeapEx(3686400, 1) never
     * returning while holding the heap lock, which then took the whole
     * console down with it. Ask for a GPU-sane alignment instead, and round
     * the size to match so the allocator is never asked for a partial block. */
    uint32_t align = s.surface.alignment;
    if (align < GX2_SURFACE_MIN_ALIGN) align = GX2_SURFACE_MIN_ALIGN;
    uint32_t size = (s.surface.imageSize + align - 1) & ~(align - 1);
    if (align != s.surface.alignment) {
        plog("align %u is unusable - allocating with %u instead",
             (unsigned)s.surface.alignment, (unsigned)align);
    }

    /* The title's own allocator, resolved out of coreinit - see
     * resolve_title_allocator(). The WUPS-redirected one is kept only as a
     * fallback so a failure to resolve degrades to "no picture" rather than
     * "no plugin". */
    if (!s_title_alloc) resolve_title_allocator();

    if (s_title_alloc) {
        plog("title MEMAllocFromDefaultHeapEx(%u, %u)...",
             (unsigned)size, (unsigned)align);
        s.surface.image = s_title_alloc(size, (int32_t)align);
    } else {
        plog("plugin-redirected MEMAllocFromDefaultHeapEx(%u, %u) - the title's "
             "allocator could not be resolved, the GPU may not see this...",
             (unsigned)size, (unsigned)align);
        s.surface.image = MEMAllocFromDefaultHeapEx(size, align);
    }

    if (!s.surface.image) {
        plog("allocation returned NULL");
        memset(&s.surface, 0, sizeof(s.surface));
        return false;
    }
    plog("alloc ok at %p", s.surface.image);

    s.allocated  = true;
    s.state = SLOT_FREE;
    return true;
}

static void free_all_slots()
{
    for (auto &s : s_slots) free_slot(s);
}

/* True when the worker owns none of the capture slots, and therefore is not
 * reading a surface or writing into the encode scratch. The only safe moment
 * for the render thread to free either. */
static bool worker_idle()
{
    for (auto &s : s_slots) {
        if (s.state != SLOT_FREE) return false;
    }
    return true;
}

/* A second core for the downscale.
 *
 * The downscale is 27-30% of every frame and is bound by memory latency, not
 * arithmetic - one core spends most of it waiting on MEM2. It is also the only
 * stage that parallelises cleanly, because output rows are independent; the
 * JPEG stage cannot, since DC prediction and the bit stream both run from
 * start to finish.
 *
 * So the encoder thread hands the top half to a helper on core 0 and does the
 * bottom half itself. Core 1 is left alone: that is where titles usually run
 * their main thread, and this whole design is built around not costing the
 * game frame time.
 */
#define HELPER_STACK_SIZE (32 * 1024)

static OSThread  s_helper;
static uint8_t  *s_helper_stack = nullptr;
static OSEvent   s_helper_go, s_helper_done;
static volatile bool s_helper_running = false;
static bool      s_helper_ready = false;

/* The slice the helper is to do; written before it is woken, read after. */
static struct {
    const uint8_t *src;
    int src_w, src_h, src_stride;
    uint8_t *y, *cb, *cr;
    int dst_w, dst_h, y_stride, c_stride;
    int row0, row1;
} s_slice;

static int helper_main(int, const char **)
{
    while (s_helper_running) {
        OSWaitEvent(&s_helper_go);
        if (!s_helper_running) break;
        wstr_rgba_to_yuv420_rows(s_slice.src, s_slice.src_w, s_slice.src_h,
                                 s_slice.src_stride, s_slice.y, s_slice.cb,
                                 s_slice.cr, s_slice.dst_w, s_slice.dst_h,
                                 s_slice.y_stride, s_slice.c_stride,
                                 s_slice.row0, s_slice.row1);
        OSSignalEvent(&s_helper_done);
    }

    /* Release a worker that is waiting on a slice this thread will now never
     * produce. Without this, a stop that lands between the worker signalling
     * "go" and this thread waking leaves the worker blocked forever - and the
     * join in the shutdown path then never returns, which is a frozen console
     * on exit rather than an error. */
    OSSignalEvent(&s_helper_done);
    return 0;
}

static void helper_start()
{
    if (s_helper_ready) return;
    s_helper_stack = (uint8_t *)memalign(16, HELPER_STACK_SIZE);
    if (!s_helper_stack) return;

    OSInitEvent(&s_helper_go,   FALSE, OS_EVENT_MODE_AUTO);
    OSInitEvent(&s_helper_done, FALSE, OS_EVENT_MODE_AUTO);
    s_helper_running = true;

    if (!OSCreateThread(&s_helper, helper_main, 0, nullptr,
                        s_helper_stack + HELPER_STACK_SIZE, HELPER_STACK_SIZE,
                        20, (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
        s_helper_running = false;
        free(s_helper_stack);
        s_helper_stack = nullptr;
        return;
    }
    OSSetThreadName(&s_helper, "wiiu-stream downscale");
    OSResumeThread(&s_helper);
    s_helper_ready = true;
}

/* ---------------------------------------------------------- worker thread */

/* s_net is touched by both threads: the render thread applies incoming HELLO
 * settings in wstr_net_poll(), the worker reads them and bumps the sequence
 * counter when it sends. That race is deliberate and benign - every field is
 * an aligned 32-bit-or-smaller scalar, so a reader sees either the old value
 * or the new one, never half of each, and the worst outcome is one frame
 * encoded at the previous quality. A mutex here would put the render thread
 * at risk of blocking on the encoder, which is the one thing this design
 * spends all its effort avoiding.
 */

static int encode_and_send(Slot &slot)
{
    const int src_w = (int)slot.surface.width;
    const int src_h = (int)slot.surface.height;

    /* The surface carries whatever format the game rendered in - see
     * alloc_slot(). Only the 8-bit RGBA layouts are four bytes per pixel with
     * red first, which is what the downscaler assumes; anything else would be
     * read as noise. Say so once rather than streaming garbage. */
    if (slot.surface.format != GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8 &&
        slot.surface.format != GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8) {
        if (!s_warned_format) {
            plog("unsupported surface format 0x%x - cannot encode",
                 (unsigned)slot.surface.format);
            s_warned_format = true;
        }
        return -1;
    }

    int out_w = s_net.out_width  ? s_net.out_width  : 640;
    int out_h = s_net.out_height ? s_net.out_height : 360;

    if (s_net.auto_quality && s_auto_scale != 100 && s_auto_scale >= 50) {
        out_w = out_w * s_auto_scale / 100;
        out_h = out_h * s_auto_scale / 100;
    }
    if (out_w > MAX_OUT_W) out_w = MAX_OUT_W;
    if (out_h > MAX_OUT_H) out_h = MAX_OUT_H;
    if (out_w > src_w)     out_w = src_w;
    if (out_h > src_h)     out_h = src_h;
    /* 4:2:0 wants even dimensions; the encoder would cope, but an odd width
     * costs a whole extra column of chroma for nothing. */
    out_w &= ~1;
    out_h &= ~1;
    if (out_w < 16 || out_h < 16) return -1;

    if (!ensure_scratch(out_w, out_h)) return -1;

    const int y_stride = out_w;
    const int c_stride = (out_w + 1) / 2;

    /* Timed in three separate pieces. Reading the captured surface, the DCTs,
     * and the sends have nothing in common and no shared fix, so one combined
     * "encode took N ms" number would not be actionable. */
    uint32_t t0 = now_us();
    {
        /* Even, so the two halves never share a chroma row. Small frames are
         * not worth the handshake. */
        const int split = (out_h / 2) & ~1;
        if (s_helper_ready && out_h >= 144 && split > 0) {
            s_slice.src = (const uint8_t *)slot.surface.image;
            s_slice.src_w = src_w;
            s_slice.src_h = src_h;
            s_slice.src_stride = (int)slot.surface.pitch * 4;
            s_slice.y = s_y; s_slice.cb = s_cb; s_slice.cr = s_cr;
            s_slice.dst_w = out_w; s_slice.dst_h = out_h;
            s_slice.y_stride = y_stride; s_slice.c_stride = c_stride;
            s_slice.row0 = 0; s_slice.row1 = split;

            /* A slice that timed out below may signal after we gave up on
             * it, and an auto-reset event latches that signal - which would
             * then satisfy the *next* frame's wait instantly and hand back a
             * half-stale image. Clear it before dispatching. */
            OSResetEvent(&s_helper_done);
            OSSignalEvent(&s_helper_go);
            wstr_rgba_to_yuv420_rows((const uint8_t *)slot.surface.image,
                                     src_w, src_h, (int)slot.surface.pitch * 4,
                                     s_y, s_cb, s_cr, out_w, out_h,
                                     y_stride, c_stride, split, out_h);
            /* Bounded. The helper signals on every exit path, so this should
             * never expire - but a missed wake here would hang the shutdown
             * join, and no frame is worth that. Doing the slice again single
             * threaded is a far better failure than a locked console. */
            if (!OSWaitEventWithTimeout(&s_helper_done,
                                        OSMillisecondsToTicks(500))) {
                wstr_rgba_to_yuv420_rows((const uint8_t *)slot.surface.image,
                                         src_w, src_h,
                                         (int)slot.surface.pitch * 4,
                                         s_y, s_cb, s_cr, out_w, out_h,
                                         y_stride, c_stride, 0, split);
            }
        } else {
            wstr_rgba_to_yuv420((const uint8_t *)slot.surface.image,
                                src_w, src_h, (int)slot.surface.pitch * 4,
                                s_y, s_cb, s_cr,
                                out_w, out_h, y_stride, c_stride);
        }
    }
    s_read_us = now_us() - t0;

    uint32_t t1 = now_us();
    /* A quality change from the PC resets the search - the slider is the
     * ceiling and the starting point, not a value to be quietly ignored. */
    if (s_net.quality != s_quality_asked) {
        s_quality_asked = s_net.quality;
        s_auto_quality = s_net.quality;
        s_auto_scale = 100;
    }

    int n = wstr_jpeg_encode(s_y, s_cb, s_cr, out_w, out_h, y_stride, c_stride,
                             s_auto_quality, s_jpeg, s_jpeg_cap);
    s_jpeg_us = now_us() - t1;

    s_encode_us = s_read_us + s_jpeg_us;
    if (n <= 0) return -1;

    uint8_t flags = (s_net.source == 1) ? WSTR_FLAG_DRC : 0;
    uint32_t t2 = now_us();
    int r = wstr_net_send_frame(&s_net, s_jpeg, (size_t)n,
                                (uint16_t)out_w, (uint16_t)out_h, flags, now_ms());
    s_send_us = now_us() - t2;

    /* One frame's whole cost against one frame's budget. */
    {
        const uint8_t fps = s_net.fps ? s_net.fps : 20;
        const uint32_t budget_us = 1000000u / fps;
        const uint32_t spent = s_read_us + s_jpeg_us + s_send_us;
        const uint32_t now = now_ms();

        if (!s_net.auto_quality) {
            /* Held at the requested value. Someone who has turned the
             * adaptation off wants the quality they asked for, even if the
             * frame rate suffers for it - that is the whole point of the
             * switch. */
            if (s_auto_quality != s_quality_asked) {
                s_auto_quality = s_quality_asked;
                plog("quality held at %u (auto scaling off)",
                     (unsigned)s_auto_quality);
            }
        } else if (now - s_quality_log_ms >= 400) {
            /* Step in proportion to how far off we are.
             *
             * A fixed step of 5 once a second took eight seconds to walk 70
             * down to 30 - correct, and far too slow to be useful: by the time
             * it settled the person watching had already concluded the stream
             * was simply slow. Overshooting badly deserves a big correction
             * immediately; being slightly off deserves a small one. */
            uint8_t q = s_auto_quality;
            int drop = 0;

            /* Degrade only when genuinely behind, not merely short.
             *
             * The old threshold was 5% over budget, which meant a 55 ms frame
             * against a 50 ms target - 18 fps instead of 20 - triggered the
             * full ladder: quality all the way to 40, then resolution to 75%.
             * That is a badly pixelated picture bought for two frames a second
             * nobody asked for. A quarter over budget is the point where the
             * frame rate is actually suffering. */
            if (spent > budget_us * 2)              drop = 20;
            else if (spent > (budget_us * 3) / 2)   drop = 10;
            else if (spent > (budget_us * 5) / 4)   drop = 3;

            /* Pick the lever that can actually move this frame.
             *
             * Quality only shrinks bytes, so it only shortens the send. When
             * the time is going into the downscale and the DCTs instead,
             * lowering quality costs picture and buys nothing - which is
             * exactly what was observed: 70 down to 40 with the frame time
             * unchanged at 55 ms. Resolution is the only lever that touches
             * all three. */
            const bool bytes_dominate = (s_send_us * 5) > (spent * 2);
            if (drop && !bytes_dominate && s_auto_scale > 50) {
                s_auto_scale -= 25;
                s_auto_quality = s_quality_asked;
                plog("scale -> %u%% (frame %ums vs %ums; send is only %ums, so "
                     "quality cannot help)", (unsigned)s_auto_scale,
                     (unsigned)(spent / 1000), (unsigned)(budget_us / 1000),
                     (unsigned)(s_send_us / 1000));
                drop = 0;
                q = s_auto_quality;
            }

            if (drop) {
                q = (q > QUALITY_FLOOR + drop) ? (uint8_t)(q - drop)
                                               : (uint8_t)QUALITY_FLOOR;
            } else if (spent < (budget_us * 3) / 5 && q + 3 <= s_quality_asked) {
                /* Climb back gently - a burst of easy frames should not undo
                 * the whole adaptation and start it oscillating. */
                q += 3;
            }

            /* Trade resolution before blockiness.
             *
             * Below about 40 the DCT artefacts are more objectionable than the
             * softness of a smaller frame, so quality stops there and scale
             * takes over, with quality restored to what was asked for at the
             * new size. Scale is also the better lever on this hardware: the
             * downscale reads whole cache lines across each source row, so its
             * cost tracks the output *height*, and halving that halves the
             * read - which the quality dial cannot touch at all.
             *
             * QUALITY_FLOOR remains as the last resort once the frame is
             * already as small as it will go. */
            const uint8_t soft_floor = (s_auto_scale > 50) ? QUALITY_SOFT_FLOOR
                                                           : QUALITY_FLOOR;
            if (drop && q < soft_floor && s_auto_scale > 50) {
                s_auto_scale -= 25;
                s_auto_quality = s_quality_asked;
                plog("scale -> %u%% (frame %ums vs %ums budget); quality back to %u",
                     (unsigned)s_auto_scale, (unsigned)(spent / 1000),
                     (unsigned)(budget_us / 1000), (unsigned)s_auto_quality);
            } else if (q != s_auto_quality) {
                if (q < soft_floor) q = soft_floor;
                plog("quality %u -> %u (frame %ums vs %ums budget, scale %u%%)",
                     (unsigned)s_auto_quality, (unsigned)q,
                     (unsigned)(spent / 1000), (unsigned)(budget_us / 1000),
                     (unsigned)s_auto_scale);
                s_auto_quality = q;
            } else if (q >= s_quality_asked && s_auto_scale < 100 &&
                       spent < (budget_us * 6) / 10) {
                s_auto_scale += 25;
                plog("headroom to spare - raising scale to %u%%",
                     (unsigned)s_auto_scale);
            }
            s_quality_log_ms = now;
        }
    }
    if (s_net.last_send_errno && !s_warned_send) {
        plog("sendto() failed (errno %u) - datagrams now %u bytes",
             (unsigned)s_net.last_send_errno, (unsigned)s_net.chunk_size);
        s_warned_send = true;
    }
    return r;
}

/* Defined with the lifecycle code below; the worker owns every call to it,
 * because it blocks while the network configuration comes up. */
static bool net_start();

static int worker_main(int, const char **)
{
    while (s_running) {
        OSWaitEvent(&s_wake);
        if (!s_running) break;

        /* Everything that touches the socket happens here, on this thread,
         * because sends block on this console and the render thread cannot
         * afford to. */
        if (s_want_netstart) {
            s_want_netstart = false;
            if (!s_net_ready) {
                s_net_ready = net_start();
            }
        }
        if (s_want_audio_stop) {
            s_want_audio_stop = false;
            audio_stop();
        }
        if (s_want_audio_start) {
            s_want_audio_start = false;
            const uint32_t want_dev = (s_net.audio_source == WSTR_AUDIO_SRC_DRC)
                                          ? AX_DEVICE_TYPE_DRC : AX_DEVICE_TYPE_TV;
            if (s_audio_hooked && s_hooked_device != want_dev) {
                plog("audio: switching to the %s mixer",
                     want_dev == AX_DEVICE_TYPE_DRC ? "gamepad" : "tv");
                audio_stop();
                s_audio_described = false;
                s_ax_p0 = 0;
            }
            audio_start();
        }

        logq_drain();
        if (s_want_status) {
            s_want_status = false;
            if (s_net_ready && s_net.stage >= WSTR_STAGE_SEND) {
                wstr_status_t st;
                memset(&st, 0, sizeof(st));
                st.src_width  = s_src_w;
                st.src_height = s_src_h;
                st.out_width  = (uint16_t)s_buf_w;
                st.out_height = (uint16_t)s_buf_h;
                st.fps_actual = (uint8_t)(s_fps_actual > 255 ? 255 : s_fps_actual);
                st.quality    = s_auto_quality ? s_auto_quality : s_net.quality;
                st.audio_on   = 0;
                st.source     = s_net.source;
                st.encode_us  = s_encode_us;
                st.capture_us = s_capture_us;
                wstr_net_send_status(&s_net, &st, now_ms());
            }
        }

        /* Release first: a disconnect while buffers are held should give the
         * memory back before anything else is considered. */
        if (s_want_free) {
            s_slots_ready = false;
            free_all_slots();
            free_scratch();
            s_want_free = false;
        }

        if (s_want_alloc && !s_slots_ready) {
            bool ok = true;
            for (auto &slot : s_slots) {
                if (!alloc_slot(slot, s_req_w, s_req_h, s_req_fmt,
                                s_req_tile_mode)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                plog("capture surfaces ready");
            } else {
                plog("capture surface allocation FAILED");
                free_all_slots();
            }
            /* Published last, so the render thread never sees a half-built
             * set of slots. */
            s_slots_ready = ok;
            s_want_alloc = false;
        }

        /* Collect copies the render thread issued and left for us.
         *
         * This is the whole point of the timestamp: GX2WaitTimeStamp blocks
         * until our own submission has retired, and blocking here costs the
         * game nothing, whereas the equivalent wait on the render thread took
         * the console down. */
        const uint8_t sync = s_net.sync_mode;
        if (sync != WSTR_SYNC_DRAWDONE) {
            for (auto &slot : s_slots) {
                if (slot.state != SLOT_GPU) continue;
                slot.state = SLOT_BUSY;

                if (sync == WSTR_SYNC_TIMESTAMP) {
                    if (!s_logged_sync) { plog("GX2WaitTimeStamp..."); }
                    uint32_t tw = now_us();
                    GX2WaitTimeStamp(slot.ts);
                    s_wait_us = now_us() - tw;
                }
                if (!s_logged_sync) { plog("GX2Invalidate..."); }
                GX2Invalidate(GX2_INVALIDATE_MODE_CPU, slot.surface.image,
                              slot.surface.imageSize);
                if (!s_logged_sync) {
                    plog("readback ok");
                    report_surface(slot);
                    s_logged_sync = true;
                }

                if (s_net.stage >= WSTR_STAGE_FULL &&
                    encode_and_send(slot) == 0) {
                    s_frames_this_sec++;
                }
                slot.state = SLOT_FREE;
            }
        }

        /* Slots the render thread already synced for us (the DrawDone path). */
        for (auto &slot : s_slots) {
            if (slot.state != SLOT_READY) continue;
            slot.state = SLOT_BUSY;

            if (encode_and_send(slot) == 0) {
                s_frames_this_sec++;
            }

            slot.state = SLOT_FREE;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- the hook */

static void on_scanout(const GX2ColorBuffer *cb, GX2ScanTarget target)
{
    if (!s_running || !cb || !cb->surface.image) return;

    if (!s_net_ready) {
        /* net_start() calls ACConnectWithConfigId, which blocks while the
         * network configuration is brought up. That cannot happen on the render
         * thread. Ask the worker and come back next frame. */
        uint32_t now = now_ms();
        if (now - s_net_retry_ms >= 2000 && !s_want_netstart) {
            s_net_retry_ms = now;
            s_want_netstart = true;
            OSSignalEvent(&s_wake);
        }
        return;
    }

    /* One target at a time - streaming the TV and the GamePad simultaneously
     * would double the cost for something nobody asked for. */
    const GX2ScanTarget wanted = (s_net.source == 1) ? GX2_SCAN_TARGET_DRC
                                                     : GX2_SCAN_TARGET_TV;
    if (target != wanted) return;

    const uint32_t t = now_ms();

    if (!wstr_net_poll(&s_net, t)) {
        /* Nobody listening. Release the memory rather than sitting on ~11 MB
         * of the game's heap for a stream that is not running.
         *
         * The worker does the freeing, for the same reason it does the
         * allocating, and only once it owns none of the slots - freeing a
         * surface the encoder is still reading would be a use-after-free in
         * someone else's game. */
        if (s_slots_ready && !s_want_free && worker_idle()) {
            s_want_free = true;
            OSSignalEvent(&s_wake);
        }
        reset_log_latches();
        return;
    }

    s_src_w = (uint16_t)cb->surface.width;
    s_src_h = (uint16_t)cb->surface.height;

    if (s_net.stage >= WSTR_STAGE_SEND && t - s_last_status_ms >= 1000) {
        /* Built from globals the worker reads anyway, so it only needs asking.
         * Same reason as the log queue: no sendto on the render thread. */
        s_want_status = true;
        s_last_status_ms = t;
        OSSignalEvent(&s_wake);
    }

    s_scan_count++;

    if (t - s_sec_start_ms >= 1000) {
        s_fps_actual = s_frames_this_sec;
        s_scan_per_sec = s_scan_count;
        s_skip_per_sec = s_skip_busy;
        s_frames_this_sec = 0;
        s_scan_count = 0;
        s_skip_busy = 0;
        s_sec_start_ms = t;
    }

    /* One timing line a second while actually streaming. This is the number
     * that says whether the cost is the GPU blit, the reads, the DCTs or the
     * network - four problems with four different answers. */
    /* Repeat the build stamp now and then.
     *
     * It is otherwise sent once per connection, so restarting the PC app
     * inside the console's connection timeout leaves the new session with no
     * idea which binary it is talking to - and every number in a saved log is
     * meaningless without that. Four lines a minute is a cheap guarantee. */
    if (t - s_build_log_ms >= 15000) {
        plog("build " WSTR_APP_VERSION " (" __DATE__ " " __TIME__ ")");
        s_build_log_ms = t;
    }

    if (s_net.stage >= WSTR_STAGE_COPY && t - s_timing_log_ms >= 1000) {
        /* copy is on the game's own render thread, so it is the number that
         * decides whether the game stutters; the rest is the worker's. */
        plog("%ufps scan%u skip%u | copy %u.%u | wait %u.%u read %u.%u "
             "jpeg %u.%u send %u.%u ms",
             (unsigned)s_fps_actual,
             (unsigned)s_scan_per_sec, (unsigned)s_skip_per_sec,
             (unsigned)(s_copy_us / 1000), (unsigned)((s_copy_us / 100) % 10),
             (unsigned)(s_wait_us / 1000), (unsigned)((s_wait_us / 100) % 10),
             (unsigned)(s_read_us / 1000), (unsigned)((s_read_us / 100) % 10),
             (unsigned)(s_jpeg_us / 1000), (unsigned)((s_jpeg_us / 100) % 10),
             (unsigned)(s_send_us / 1000), (unsigned)((s_send_us / 100) % 10));
        s_timing_log_ms = t;
    }

    /* Describe the buffer once per connection. Which format and tile mode the
     * running title actually uses is the single most useful fact for working
     * out why a capture misbehaves, and it cannot be guessed from the PC. */
    if (!s_described) {
        plog("scanout %ux%u fmt=0x%x tile=0x%x aa=%u pitch=%u use=0x%x",
             (unsigned)cb->surface.width, (unsigned)cb->surface.height,
             (unsigned)cb->surface.format, (unsigned)cb->surface.tileMode,
             (unsigned)cb->surface.aa, (unsigned)cb->surface.pitch,
             (unsigned)cb->surface.use);
        plog("build " WSTR_APP_VERSION " (" __DATE__ " " __TIME__ ")");
        plog("stage=%u - capture path is gated here", (unsigned)s_net.stage);
        s_described = true;
    }

    /* MSAA colour buffers would need GX2ResolveAAColorBuffer first. Almost
     * nothing on this console renders the scan buffer with AA, so rather than
     * carry a second path, skip and let the PC show a stalled stream. */
    if (cb->surface.aa != GX2_AA_MODE1X) {
        if (!s_warned_aa) {
            plog("surface is MSAA (aa=%u) - not captured",
                 (unsigned)cb->surface.aa);
            s_warned_aa = true;
        }
        return;
    }

    if (s_net.want_audio) {
        /* Ask the worker to hook, or to re-hook on the other mixer. Rate
         * limited: until the title initialises AX this cannot succeed, and
         * retrying the module lookup every frame is pure loader-lock traffic
         * during exactly the load it would interfere with. */
        const uint32_t want_dev = (s_net.audio_source == WSTR_AUDIO_SRC_DRC)
                                      ? AX_DEVICE_TYPE_DRC : AX_DEVICE_TYPE_TV;
        if ((!s_audio_hooked || s_hooked_device != want_dev) &&
            !s_want_audio_start && t - s_audio_try_ms >= 1000) {
            s_audio_try_ms = t;
            s_want_audio_start = true;
            OSSignalEvent(&s_wake);
        }
        s_audio_on = s_audio_hooked;
        if (s_audio_hooked && !s_audio_described && s_ax_samples) {
            plog("audio: %u Hz, ch=%u samples=%u devices=%u chOut=%u",
                 (unsigned)s_audio_rate, (unsigned)s_ax_channels,
                 (unsigned)s_ax_samples, (unsigned)s_ax_ndev,
                 (unsigned)s_ax_chout);
            s_audio_described = true;
            plog("audio: ptrs %08x %08x %08x (gap %d, expect %u for planar)",
                 (unsigned)s_ax_p0, (unsigned)s_ax_p1, (unsigned)s_ax_p2,
                 (int)(s_ax_p1 - s_ax_p0), (unsigned)(s_ax_samples * 4));
            plog("audio: ch0 %ld %ld %ld %ld | ch1 %ld %ld %ld %ld",
                 (long)s_ax_s0[0], (long)s_ax_s0[1], (long)s_ax_s0[2],
                 (long)s_ax_s0[3], (long)s_ax_s1[0], (long)s_ax_s1[1],
                 (long)s_ax_s1[2], (long)s_ax_s1[3]);
        }
        if (s_audio_hooked && t - s_audio_log_ms >= 2000) {
            plog("audio: peak %u -> shift %u (%u dropped)",
                 (unsigned)s_ax_peak, (unsigned)s_ax_shift,
                 (unsigned)s_audio_dropped);
            s_audio_log_ms = t;
        }
    } else {
        s_audio_on = false;
    }

    /* Audio-only: give the surfaces back and do nothing else.
     *
     * The point is to make the audio as steady as it can be while the picture
     * is being watched on the console itself, so this releases ~11 MB of the
     * title's heap, stops the GPU copies, and leaves the whole link and the
     * encoder thread to the audio. */
    if (s_net.video_off) {
        if (s_slots_ready && !s_want_free && worker_idle()) {
            plog("audio only - releasing capture surfaces");
            s_want_free = true;
            OSSignalEvent(&s_wake);
        }
        return;
    }

    const uint8_t stage = s_net.stage;
    if (stage < WSTR_STAGE_ALLOC) return;

    /* Ask the worker for buffers and come back later. The render thread does
     * not allocate - see the note on s_want_alloc. Until the worker has
     * published them there is nothing here to copy into. */
    if (s_slots_ready && s_slots[0].allocated &&
        s_req_tile_mode != s_net.tile_mode && worker_idle()) {
        /* Changing tiling means new surfaces - the whole point is to compare
         * them, so it has to take effect without a reboot. */
        plog("tiling changed - releasing surfaces");
        s_want_free = true;
        OSSignalEvent(&s_wake);
        return;
    }

    if (!s_slots_ready) {
        if (!s_want_alloc) {
            s_req_w   = cb->surface.width;
            s_req_h   = cb->surface.height;
            s_req_fmt = cb->surface.format;
            s_req_tile_mode  = s_net.tile_mode;
            if (!s_logged_alloc) {
                plog("requesting %ux%u capture surfaces (allocator: %s)",
                     (unsigned)s_req_w, (unsigned)s_req_h,
                     s_title_alloc ? "title" : "plugin fallback");
                s_logged_alloc = true;
            }
            s_want_alloc = true;
            OSSignalEvent(&s_wake);
        }
        return;
    }

    /* Captures can only happen on a scanout, so the interval quantises to
     * whole game frames. Waiting for a *full* interval since the last capture
     * then rounds every wait up: at 30 Hz presentation, a 50 ms target waits
     * 33 ms, finds it too early, and takes the next one at 66 ms - 15 fps
     * instead of 20, and no setting fixes it.
     *
     * Tracking the next due time and advancing it by the interval keeps the
     * average right instead, alternating 33 and 66 ms. The clamp stops a stall
     * (a loading screen, a menu that stops presenting) from banking credit and
     * then firing a burst of back-to-back captures. */
    const uint8_t fps = s_net.fps ? s_net.fps : 20;
    const uint32_t interval = 1000u / fps;
    if ((int32_t)(t - s_next_capture_ms) < 0) return;
    s_next_capture_ms += interval;
    if ((int32_t)(t - s_next_capture_ms) > (int32_t)interval) {
        s_next_capture_ms = t + interval;
    }

    /* Nothing free means the encoder still holds both slots from earlier
     * captures - the interval elapsed and we cannot use it. Counted, because
     * this is the difference between "the game is not presenting often enough"
     * and "the encode cannot keep up", which need opposite fixes. */
    bool any_free = false;
    for (auto &slot : s_slots) {
        if (slot.state == SLOT_FREE) { any_free = true; break; }
    }
    if (!any_free) s_skip_busy++;

    uint32_t t0 = now_us();

    /* Hand the previous tick's copy to the encoder now, a full interval after
     * it was submitted. Waking the worker here rather than at issue time is
     * what keeps its GPU wait and its 3.7 MB read off the game's frame. */
    if (stage >= WSTR_STAGE_READBACK && s_net.sync_mode != WSTR_SYNC_DRAWDONE) {
        for (auto &slot : s_slots) {
            if (slot.state == SLOT_GPU) {
                OSSignalEvent(&s_wake);
                break;
            }
        }
    }

    /* Collect the copy issued at the previous capture. By now the GPU has had
     * a whole stream interval to finish it, so this DrawDone is waiting on
     * work that is already done rather than stalling on our own copy. */
    /* The legacy drain, kept only so the two can be compared on hardware.
     * This is the path that froze the console: GX2DrawDone() waits for the
     * whole pipeline to retire, and calling it here means doing that from
     * inside the game's own GX2 call while it is still building the command
     * buffer this very frame belongs to. Every other sync mode hands the
     * waiting to the worker thread instead. */
    if (stage >= WSTR_STAGE_READBACK && s_net.sync_mode == WSTR_SYNC_DRAWDONE) {
        for (auto &slot : s_slots) {
            if (slot.state != SLOT_GPU) continue;

            if (!s_logged_sync) { plog("GX2DrawDone() on the render thread..."); }
            GX2DrawDone();
            if (!s_logged_sync) { plog("GX2Invalidate..."); }
            GX2Invalidate(GX2_INVALIDATE_MODE_CPU, slot.surface.image,
                          slot.surface.imageSize);
            if (!s_logged_sync) { plog("readback ok"); s_logged_sync = true; }

            if (stage >= WSTR_STAGE_FULL) {
                slot.state = SLOT_READY;
                OSSignalEvent(&s_wake);
            } else {
                /* Synced but deliberately not handed to the encoder. */
                slot.state = SLOT_FREE;
            }
        }
    }

    if (stage < WSTR_STAGE_COPY) return;

    /* Issue the next one into whatever slot the worker is not holding. */
    for (auto &slot : s_slots) {
        if (slot.state != SLOT_FREE) continue;
        /* Belt and braces after the title-switch bug above: a slot with no
         * image is not something to hand the GPU. */
        if (!slot.allocated || !slot.surface.image) return;

        if (!s_logged_sync) {
            /* Only until the first check has reported - after that this is
             * pure overhead on every frame. */
            poison_surface(slot);
        }
        if (!s_logged_copy) { plog("GX2CopySurface..."); }
        uint32_t tc = now_us();
        GX2CopySurface(&cb->surface, 0, 0, &slot.surface, 0, 0);
        s_copy_us = now_us() - tc;
        if (!s_logged_copy) { plog("copy issued ok"); s_logged_copy = true; }

        if (stage >= WSTR_STAGE_READBACK) {
            /* GX2 buffers commands and only submits them on a flush, so
             * GX2GetLastSubmittedTimeStamp() on its own would hand back the
             * *previous* submission - the worker would then wait for something
             * already retired and read the surface before the GPU had written
             * it. Flushing first makes the timestamp actually cover our copy.
             * GX2Flush only submits, it does not wait, so unlike GX2DrawDone
             * it does not stall this thread.
             *
             * Written before the state, so the worker never sees a slot marked
             * ready with a stale timestamp. */
            GX2Flush();
            slot.ts = GX2GetLastSubmittedTimeStamp();
            slot.state = SLOT_GPU;
            /* Deliberately not signalling the worker here. The copy was only
             * just submitted; waking the encoder now means it blocks on the
             * GPU while the game is mid-frame and then reads 3.7 MB straight
             * through the memory bus the game is using. The next capture tick
             * picks it up instead, by which point the GPU has had a whole
             * interval to retire it - which is the deferral this design is
             * built around. */
        } else {
            /* At the COPY stage nothing will ever read this surface, so hand
             * the slot straight back and let copies keep flowing - a blit that
             * only hangs the GPU on the second or third go would otherwise be
             * missed. */
            slot.state = SLOT_FREE;
        }
        s_last_capture_ms = t;
        break;
    }

    s_capture_us = now_us() - t0;
}

DECL_FUNCTION(void, GX2CopyColorBufferToScanBuffer,
              const GX2ColorBuffer *buffer, GX2ScanTarget scan_target)
{
    /* The game's own scanout goes first, so our copy is queued behind the
     * frame the user is actually waiting to see. */
    real_GX2CopyColorBufferToScanBuffer(buffer, scan_target);
    on_scanout(buffer, scan_target);
}

WUPS_MUST_REPLACE(GX2CopyColorBufferToScanBuffer,
                  WUPS_LOADER_LIBRARY_GX2, GX2CopyColorBufferToScanBuffer);

/* ------------------------------------------------------------- lifecycle */

static bool net_start()
{
    /* Titles that never touch the network leave the stack down, so bring it
     * up ourselves. Both calls are safe to repeat on a title that already
     * did it. */
    ACConfigId cfg;
    ACInitialize();
    if (NNResult_IsSuccess(ACGetStartupId(&cfg))) {
        ACConnectWithConfigId(cfg);
    }
    socket_lib_init();

    return wstr_net_open(&s_net, WSTR_PORT_CONSOLE) == 0;
}

/* Everything here is process-global and outlives a title switch, so it all has
 * to be put back by hand when the next title starts.
 *
 * Missing one of these is not a small bug: s_slots_ready survived a launch
 * once, so the new title skipped allocation, copied into a zeroed slot and
 * handed the encoder a surface with no image behind it. The format check
 * caught that one; GX2CopySurface into a null image would not have been so
 * polite. */
static void reset_session_state()
{
    memset(s_slots, 0, sizeof(s_slots));
    s_slots_ready = false;
    s_want_alloc  = false;
    s_want_free   = false;
    s_req_w = s_req_h = 0;

    s_last_capture_ms = 0;
    s_next_capture_ms = 0;
    s_last_status_ms  = 0;
    s_timing_log_ms   = 0;
    s_build_log_ms    = 0;
    s_net_retry_ms    = 0;
    s_audio_try_ms    = 0;
    s_want_audio_start = false;
    s_want_audio_stop = false;
    s_sec_start_ms    = now_ms();
    s_frames_this_sec = 0;
    s_fps_actual      = 0;
    s_wait_us = s_read_us = s_jpeg_us = s_send_us = s_copy_us = 0;
    s_encode_us = s_capture_us = 0;
    s_src_w = s_src_h = 0;

    s_auto_quality = 0;
    s_quality_asked = 0;
    s_auto_scale = 100;

    reset_log_latches();
}

ON_APPLICATION_START()
{
    /* Deliberately not resolving the title's allocator here: at application
     * start the title has not run its own initialisation yet, so the pointer
     * coreinit holds is still the previous one. It is resolved on first use
     * instead, by which time the title owns it. */
    s_title_alloc = nullptr;
    s_title_free = nullptr;

    reset_session_state();

    /* The socket is opened here but a failure is not fatal any more.
     *
     * Launching a title tears the previous instance down and brings this one
     * up immediately afterwards, and if the bind lands while the network stack
     * is still releasing the old socket it fails - after which the plugin used
     * to return and stay dead for the entire title. That is precisely what
     * "loading the game stopped the stream and it never came back" looks like:
     * no video, no audio, and no log lines to say why, because logging needs
     * the same socket. on_scanout retries it. */
    s_net_ready = net_start();

    OSInitEvent(&s_wake, FALSE, OS_EVENT_MODE_AUTO);

    s_worker_stack = (uint8_t *)memalign(16, WORKER_STACK_SIZE);
    if (!s_worker_stack) {
        wstr_net_close(&s_net);
        s_net_ready = false;
        return;
    }

    s_running = true;

    /* Core 2 and a priority below the default 16, so the encode never
     * competes with the game's own threads for the core it renders on. */
    if (!OSCreateThread(&s_worker, worker_main, 0, nullptr,
                        s_worker_stack + WORKER_STACK_SIZE, WORKER_STACK_SIZE,
                        20, (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        s_running = false;
        free(s_worker_stack);
        s_worker_stack = nullptr;
        wstr_net_close(&s_net);
        s_net_ready = false;
        return;
    }

    OSSetThreadName(&s_worker, "wiiu-stream encoder");
    OSResumeThread(&s_worker);

    helper_start();
}

ON_APPLICATION_ENDS()
{
    /* Order matters here, and getting it wrong freezes the console on exit
     * rather than erroring - which is what pressing HOME while streaming used
     * to do. Three rules:
     *
     *   - unhook the title's audio first, so it stops calling into code that
     *     is being dismantled;
     *   - wake everything before joining anything, including the helper's
     *     "done" event, in case the worker is waiting on a slice that will
     *     never arrive;
     *   - close the socket before any join, because a thread blocked in a
     *     send would otherwise hang the join forever.
     *
     * Then join in reverse dependency order: the worker waits on the helper,
     * so the worker has to go first.
     */
    if (s_audio_hooked) {
        if (ax_set_mixcb) ax_set_mixcb(s_hooked_device, s_prev_mix_cb);
        s_prev_mix_cb = nullptr;
        s_audio_hooked = false;
        s_audio_on = false;
    }

    s_running = false;
    s_audio_running = false;
    if (s_helper_ready) {
        s_helper_running = false;
        OSSignalEvent(&s_helper_go);
        OSSignalEvent(&s_helper_done);
    }
    OSSignalEvent(&s_wake);

    if (s_net_ready) {
        wstr_net_close(&s_net);
        s_net_ready = false;
    }

    if (s_worker_stack) {
        OSJoinThread(&s_worker, nullptr);
        free(s_worker_stack);
        s_worker_stack = nullptr;
    }
    if (s_helper_ready) {
        OSJoinThread(&s_helper, nullptr);
        free(s_helper_stack);
        s_helper_stack = nullptr;
        s_helper_ready = false;
    }
    if (s_audio_stack) {
        OSJoinThread(&s_audio_thread, nullptr);
        free(s_audio_stack);
        s_audio_stack = nullptr;
    }
    if (s_aring) { free(s_aring); s_aring = nullptr; }
    s_awrite = 0;
    s_aread = 0;

    /* Safe now: nothing is running that could still be reading them. */
    free_all_slots();
    free_scratch();
}
