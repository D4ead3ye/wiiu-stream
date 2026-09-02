/* fake_console - pretends to be the Wii U, on this PC.
 *
 * It runs the same wstr_net.c and wstr_jpeg.c the plugin does, over the same
 * UDP protocol, against a synthetic 1280x720 framebuffer. So it exercises
 * discovery, the HELLO settings round-trip, chunking, reassembly, the encoder
 * and the whole PC app - everything except the GX2 readback.
 *
 * Two reasons it exists. During development it is the difference between a
 * one-second edit-test loop and a reboot-the-console loop. Afterwards it lets
 * you get Discord pointed at the right window before the Wii U is involved at
 * all, so if the real stream misbehaves you already know the PC half is fine.
 *
 *   fake_console.exe            listens on the console's port and streams
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <winsock2.h>
#include <windows.h>

#include "../common/wstr_jpeg.h"
#include "../common/wstr_net.h"
#include "../common/wstr_proto.h"

#define SRC_W 1280
#define SRC_H 720

/* Matches MAX_OUT_W/H in the plugin - the scratch buffers are sized once
 * for the largest output the PC is allowed to ask for. */
#define MAX_W 1280
#define MAX_H 720

/* GetTickCount() only moves in 15.6 ms steps, which would quantise a
 * requested 20 fps down to 16 and make the pipeline look slower than it is.
 * The performance counter has no such floor. */
static uint32_t now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint32_t)((c.QuadPart * 1000) / f.QuadPart);
}

/* A frame that makes problems obvious: a moving edge shows tearing and
 * reassembly errors, the colour bars show channel swaps, and the ramp below
 * shows banding and chroma error.
 *
 * The background is built once and memcpy'd, with only the moving bar drawn
 * per frame. Generating all 921,600 pixels every time - with a sin() among
 * them - cost more than the JPEG encode did, and capped this tool at 16 fps
 * no matter what rate was asked for, which looks exactly like a bottleneck
 * in the thing it is supposed to be testing.
 */
static void draw_frame(uint8_t *rgba, uint8_t *base, uint32_t t)
{
    int x, y;
    int sweep = (int)((t / 8) % SRC_W);

    memcpy(rgba, base, (size_t)SRC_W * SRC_H * 4);

    for (y = 0; y < SRC_H; y++) {
        uint8_t *row = rgba + (size_t)y * SRC_W * 4;
        for (x = sweep; x < sweep + 24 && x < SRC_W; x++) {
            uint8_t *p = row + x * 4;
            p[0] = 255; p[1] = 32; p[2] = 48;
        }
    }
}

static void build_background(uint8_t *base)
{
    int x, y;
    const int bar_h = SRC_H / 8;
    static const uint8_t bars[8][3] = {
        {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
        {255,0,255},   {255,0,0},   {0,0,255},   {24,24,24}
    };

    for (y = 0; y < SRC_H; y++) {
        uint8_t *row = base + (size_t)y * SRC_W * 4;
        int band = y / bar_h;
        if (band > 7) band = 7;
        for (x = 0; x < SRC_W; x++) {
            uint8_t *p = row + x * 4;
            if (y < SRC_H / 2) {
                p[0] = bars[band][0];
                p[1] = bars[band][1];
                p[2] = bars[band][2];
            } else {
                p[0] = (uint8_t)(x * 255 / SRC_W);
                p[1] = (uint8_t)((y - SRC_H / 2) * 255 / (SRC_H / 2));
                p[2] = (uint8_t)(128 + 127 * sin(x * 0.01));
            }
            p[3] = 255;
        }
    }
}

int main(void)
{
    WSADATA wsa;
    wstr_net net;
    uint8_t *rgba, *base, *y, *cb, *cr, *jpeg;
    size_t jpeg_cap;
    uint32_t last_frame = 0, last_status = 0, last_sec = 0;
    uint32_t frames_this_sec = 0, fps_actual = 0, encode_us = 0;
    int cur_w = 0, cur_h = 0;
    int announced = 0;
    unsigned last_stage = 0xff;
    uint32_t audio_start_ms = 0, audio_frames_sent = 0;
    int audio_running = 0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    /* Windows rounds every Sleep() up to the current timer resolution, which
     * defaults to 15.6 ms. Without this the pacing loop below cannot hit any
     * rate that is not a divisor of 64 Hz - a requested 20 fps comes out as
     * 16, which reads as a bottleneck in the pipeline this tool exists to
     * rule out. */
    timeBeginPeriod(1);

    if (wstr_net_open(&net, WSTR_PORT_CONSOLE) != 0) {
        fprintf(stderr, "could not bind UDP %d - is a real console app or "
                        "another copy of this already running?\n",
                WSTR_PORT_CONSOLE);
        return 1;
    }

    rgba = malloc((size_t)SRC_W * SRC_H * 4);
    base = malloc((size_t)SRC_W * SRC_H * 4);
    jpeg_cap = wstr_jpeg_bound(MAX_W, MAX_H);
    jpeg = malloc(jpeg_cap);
    y  = malloc((size_t)MAX_W * MAX_H);
    cb = malloc((size_t)MAX_W * MAX_H / 4 + MAX_W);
    cr = malloc((size_t)MAX_W * MAX_H / 4 + MAX_W);
    if (!rgba || !base || !jpeg || !y || !cb || !cr) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    build_background(base);

    printf("fake console listening on UDP %d\n", WSTR_PORT_CONSOLE);
    printf("start the PC app (pc/dashboard.py) and press Start.\n\n");

    for (;;) {
        uint32_t t = now_ms();

        if (!wstr_net_poll(&net, t)) {
            if (announced) {
                printf("peer gone\n");
                announced = 0;
            }
            Sleep(20);
            continue;
        }

        if (!announced) {
            printf("streaming to %u.%u.%u.%u:%u\n",
                   (net.peer_addr >> 24) & 0xff, (net.peer_addr >> 16) & 0xff,
                   (net.peer_addr >> 8) & 0xff, net.peer_addr & 0xff,
                   net.peer_port);
            /* Mirror the plugin's own trace so the log pane can be checked
             * here rather than on hardware. */
            wstr_net_log(&net, "fake console connected", t);
            wstr_net_log(&net, "scanout 1280x720 fmt=0x1a tile=0x4 aa=0 (synthetic)", t);
            announced = 1;
            last_stage = 0xff;
        }

        if (net.stage != last_stage) {
            char msg[96];
            snprintf(msg, sizeof(msg), "stage=%u - capture path is gated here",
                     (unsigned)net.stage);
            wstr_net_log(&net, msg, t);
            last_stage = net.stage;
        }

        /* Audio only: the plugin skips capture entirely. */
        if (net.video_off) {
            if (!net.want_audio) { Sleep(5); continue; }
        } else
        /* Below the full stage the plugin allocates and copies but never
         * encodes, so there is nothing to send. */
        if (net.stage < WSTR_STAGE_FULL) {
            Sleep(5);
            continue;
        }

        /* Audio, when asked for: a quiet tone at the real rate and block
         * size, so the PC's ring, its waveOut loop and the packet cadence all
         * get exercised without a console in the room. */
        if (net.want_audio) {
            uint64_t due;
            if (!audio_running) {
                /* Start the clock when audio is switched on, not at process
                 * start: now_ms() counts from boot, so an unset origin makes
                 * the first "due" figure enormous and floods the link with
                 * hours of tone in a second. */
                audio_start_ms = t;
                audio_frames_sent = 0;
                audio_running = 1;
            }
            /* 64-bit: ms * 48000 overflows 32 bits after ~89 seconds. */
            due = (uint64_t)(t - audio_start_ms) * WSTR_AUDIO_RATE / 1000u;
            while (audio_frames_sent + WSTR_AUDIO_FRAMES <= due) {
                static int16_t block[WSTR_AUDIO_FRAMES * 2];
                for (int i = 0; i < WSTR_AUDIO_FRAMES; i++) {
                    double ph = (audio_frames_sent + i) * 2.0 * 3.14159265
                                * 440.0 / WSTR_AUDIO_RATE;
                    int16_t v = (int16_t)(sin(ph) * 6000.0);
                    /* Byte-swapped, because this stands in for a big-endian
                     * console and the protocol carries audio in the console's
                     * own order. Emitting host-order samples here would make
                     * the test rig disagree with the hardware it imitates -
                     * and would have hidden the byte order bug rather than
                     * catching it. */
                    uint16_t be = (uint16_t)((((uint16_t)v & 0x00ff) << 8) |
                                             (((uint16_t)v & 0xff00) >> 8));
                    block[i * 2 + 0] = (int16_t)be;
                    block[i * 2 + 1] = (int16_t)be;
                }
                if (wstr_net_send_audio(&net, (const uint8_t *)block,
                                        sizeof(block), t) != 0) break;
                audio_frames_sent += WSTR_AUDIO_FRAMES;
            }
        } else {
            audio_running = 0;
        }

        if (t - last_sec >= 1000) {
            fps_actual = frames_this_sec;
            frames_this_sec = 0;
            last_sec = t;
        }

        if (t - last_status >= 1000) {
            wstr_status_t st;
            memset(&st, 0, sizeof(st));
            st.src_width  = SRC_W;
            st.src_height = SRC_H;
            st.out_width  = (uint16_t)cur_w;
            st.out_height = (uint16_t)cur_h;
            st.fps_actual = (uint8_t)(fps_actual > 255 ? 255 : fps_actual);
            st.quality    = net.quality;
            st.source     = net.source;
            st.encode_us  = encode_us;
            wstr_net_send_status(&net, &st, t);
            last_status = t;
        }

        {
            /* Sleep for the time actually remaining, not a fixed Sleep(1).
             * Windows rounds a sleep up to the current timer resolution -
             * 15.6 ms by default - so polling with Sleep(1) quantises every
             * rate to a multiple of 64 Hz, and a requested 20 fps arrives as
             * 16. That looks exactly like a bottleneck in the pipeline this
             * tool exists to exonerate. Yield for the last millisecond so the
             * wake-up lands on time without spinning for the whole interval. */
            uint32_t interval = 1000u / (net.fps ? net.fps : 20);
            uint32_t elapsed  = t - last_frame;
            if (elapsed < interval) {
                uint32_t remaining = interval - elapsed;
                Sleep(remaining > 2 ? remaining - 2 : 0);
                continue;
            }
            last_frame = t;
        }

        if (net.video_off) { Sleep(2); continue; }

        cur_w = net.out_width  ? net.out_width  : 640;
        cur_h = net.out_height ? net.out_height : 360;
        if (cur_w > MAX_W) cur_w = MAX_W;
        if (cur_h > MAX_H) cur_h = MAX_H;
        cur_w &= ~1;
        cur_h &= ~1;

        draw_frame(rgba, base, t);

        {
            LARGE_INTEGER f, a, b;
            int n, c_stride = (cur_w + 1) / 2;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);

            wstr_rgba_to_yuv420(rgba, SRC_W, SRC_H, SRC_W * 4,
                                y, cb, cr, cur_w, cur_h, cur_w, c_stride);
            n = wstr_jpeg_encode(y, cb, cr, cur_w, cur_h, cur_w, c_stride,
                                 net.quality, jpeg, jpeg_cap);

            QueryPerformanceCounter(&b);
            encode_us = (uint32_t)((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart);

            if (n > 0 && wstr_net_send_frame(&net, jpeg, (size_t)n,
                                             (uint16_t)cur_w, (uint16_t)cur_h,
                                             net.source == 1 ? WSTR_FLAG_DRC : 0,
                                             t) == 0) {
                frames_this_sec++;
            }
        }
    }
}
