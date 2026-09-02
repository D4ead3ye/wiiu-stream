/* Host-side check for the encoder that has to run on the console.
 *
 * Builds a synthetic RGBA framebuffer, runs it through the exact downscale +
 * encode path the plugin uses, and writes the JPEG out for the Python side to
 * decode and compare against a reference conversion. Catching a bad Huffman
 * table here costs seconds; catching it on hardware costs a reboot. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/wstr_jpeg.h"

#define SRC_W 1280
#define SRC_H 720

int main(int argc, char **argv)
{
    int dst_w = 640, dst_h = 360, quality = 75;
    uint8_t *src, *y, *cb, *cr, *out;
    int y_stride, c_stride, cw, ch, n, x, yy;
    size_t bound;
    FILE *f;
    const char *path = "out.jpg";

    if (argc > 1) dst_w   = atoi(argv[1]);
    if (argc > 2) dst_h   = atoi(argv[2]);
    if (argc > 3) quality = atoi(argv[3]);
    if (argc > 4) path    = argv[4];

    src = malloc((size_t)SRC_W * SRC_H * 4);
    if (!src) return 1;

    /* Something with hard edges, smooth ramps and saturated primaries - the
     * three things that expose ringing, DC drift and channel swaps. */
    for (yy = 0; yy < SRC_H; yy++) {
        for (x = 0; x < SRC_W; x++) {
            uint8_t *p = src + ((size_t)yy * SRC_W + x) * 4;
            int checker = (((x >> 6) + (yy >> 6)) & 1);
            p[0] = (uint8_t)(x * 255 / SRC_W);
            p[1] = (uint8_t)(yy * 255 / SRC_H);
            p[2] = (uint8_t)(checker ? 240 : 16);
            p[3] = 255;
            if (yy < 40)            { p[0] = 255; p[1] = 0;   p[2] = 0;   }
            else if (yy < 80)       { p[0] = 0;   p[1] = 255; p[2] = 0;   }
            else if (yy < 120)      { p[0] = 0;   p[1] = 0;   p[2] = 255; }
            else if (yy < 160)      { p[0] = 255; p[1] = 255; p[2] = 255; }
        }
    }

    cw = (dst_w + 1) / 2;
    ch = (dst_h + 1) / 2;
    y_stride = dst_w;
    c_stride = cw;

    y  = malloc((size_t)y_stride * dst_h);
    cb = malloc((size_t)c_stride * ch);
    cr = malloc((size_t)c_stride * ch);
    bound = wstr_jpeg_bound(dst_w, dst_h);
    out = malloc(bound);
    if (!y || !cb || !cr || !out) return 1;

    wstr_rgba_to_yuv420(src, SRC_W, SRC_H, SRC_W * 4,
                        y, cb, cr, dst_w, dst_h, y_stride, c_stride);

    n = wstr_jpeg_encode(y, cb, cr, dst_w, dst_h, y_stride, c_stride,
                         quality, out, bound);
    if (n < 0) { fprintf(stderr, "encode failed\n"); return 1; }

    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    fwrite(out, 1, (size_t)n, f);
    fclose(f);

    printf("%dx%d q%d -> %d bytes (%.2f bpp)\n",
           dst_w, dst_h, quality, n, (double)n * 8.0 / (dst_w * dst_h));

    /* Also dump the planes, so the Python check can compare the decoded
     * image against what actually went in rather than against a guess. */
    f = fopen("out.yuv", "wb");
    if (f) {
        fwrite(y,  1, (size_t)y_stride * dst_h, f);
        fwrite(cb, 1, (size_t)c_stride * ch, f);
        fwrite(cr, 1, (size_t)c_stride * ch, f);
        fclose(f);
    }
    return 0;
}
