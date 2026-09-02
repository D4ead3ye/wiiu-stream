/* Benchmark the two things the console spends its frame budget on.
 *
 * The console reports read and jpeg times per frame, but iterating on them
 * there means a reboot per attempt. The code is identical on both targets, so
 * relative improvements measured here carry over - the PowerPC is roughly an
 * order of magnitude slower in absolute terms, but a change that removes a
 * 64-bit multiply from an inner loop removes it on both.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../common/wstr_jpeg.h"

#define SRC_W 1280
#define SRC_H 720

static double now_ms(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    int dst_w = 640, dst_h = 360, quality = 70, iters = 200;
    uint8_t *src, *y, *cb, *cr, *out;
    int cw, ch, x, yy, i, n = 0;
    size_t bound;
    double t0, t_read, t_jpeg;

    if (argc > 1) dst_w = atoi(argv[1]);
    if (argc > 2) dst_h = atoi(argv[2]);
    if (argc > 3) quality = atoi(argv[3]);

    src = malloc((size_t)SRC_W * SRC_H * 4);
    if (!src) return 1;

    /* Detailed content, because that is what a real game frame looks like to
     * the encoder and it is what makes the entropy coding expensive. */
    for (yy = 0; yy < SRC_H; yy++) {
        for (x = 0; x < SRC_W; x++) {
            uint8_t *p = src + ((size_t)yy * SRC_W + x) * 4;
            int n1 = (x * 7 + yy * 13) & 0xff;
            int n2 = ((x >> 2) * 31 + (yy >> 1) * 17) & 0xff;
            p[0] = (uint8_t)(n1 ^ (yy & 0x3f));
            p[1] = (uint8_t)(n2 + (x & 0x1f));
            p[2] = (uint8_t)((n1 * n2) >> 6);
            p[3] = 255;
        }
    }

    cw = (dst_w + 1) / 2;
    ch = (dst_h + 1) / 2;
    y  = malloc((size_t)dst_w * dst_h);
    cb = malloc((size_t)cw * ch);
    cr = malloc((size_t)cw * ch);
    bound = wstr_jpeg_bound(dst_w, dst_h);
    out = malloc(bound);
    if (!y || !cb || !cr || !out) return 1;

    /* warm */
    wstr_rgba_to_yuv420(src, SRC_W, SRC_H, SRC_W * 4, y, cb, cr,
                        dst_w, dst_h, dst_w, cw);

    t0 = now_ms();
    for (i = 0; i < iters; i++) {
        wstr_rgba_to_yuv420(src, SRC_W, SRC_H, SRC_W * 4, y, cb, cr,
                            dst_w, dst_h, dst_w, cw);
    }
    t_read = (now_ms() - t0) / iters;

    t0 = now_ms();
    for (i = 0; i < iters; i++) {
        n = wstr_jpeg_encode(y, cb, cr, dst_w, dst_h, dst_w, cw,
                             quality, out, bound);
    }
    t_jpeg = (now_ms() - t0) / iters;

    printf("%4dx%-4d q%-3d  read %6.3f ms   jpeg %6.3f ms   total %6.3f ms"
           "   (%d bytes)\n",
           dst_w, dst_h, quality, t_read, t_jpeg, t_read + t_jpeg, n);
    return 0;
}
