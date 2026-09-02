/* wstr_jpeg.h - baseline JPEG encoder, 4:2:0, standard Huffman tables.
 *
 * devkitPro ships no JPEG library for the Wii U (portlibs has zlib, libpng,
 * ogg/vorbis and nothing else), and PNG is far too slow and too fat for a
 * 30 fps stream. So this is a small self-contained baseline encoder: AAN
 * float DCT, the Annex K quantisation and Huffman tables, 4:2:0 chroma.
 *
 * Input is planar YCbCr because the downscaler is already touching every
 * pixel to shrink the framebuffer - having it write out planes costs nothing
 * and saves a second full pass for colour conversion.
 */
#ifndef WSTR_JPEG_H
#define WSTR_JPEG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode planar YCbCr 4:2:0 into a JFIF byte stream.
 *
 *   y             luma plane, y_stride bytes per row, w x h
 *   cb, cr        chroma planes, c_stride bytes per row, ((w+1)/2) x ((h+1)/2)
 *   quality       1..100, IJG scale (75 is the usual "good enough")
 *   out, out_max  destination buffer
 *
 * Returns bytes written, or -1 if the buffer was too small. */
int wstr_jpeg_encode(const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                     int w, int h, int y_stride, int c_stride,
                     int quality, uint8_t *out, size_t out_max);

/* Point-sample an RGBA8 framebuffer down to planar YCbCr 4:2:0.
 *
 * Point sampling rather than a box filter is deliberate: MEM2 reads are the
 * expensive part of this whole path, and averaging 2x2 means touching four
 * times as many cache lines for a difference that a 640x360 stream re-encoded
 * by Discord will not show.
 *
 *   src           RGBA8, src_stride bytes per row
 *   dst_w/dst_h   output size; any ratio, it is a straight resample */
void wstr_rgba_to_yuv420(const uint8_t *src, int src_w, int src_h, int src_stride,
                         uint8_t *y, uint8_t *cb, uint8_t *cr,
                         int dst_w, int dst_h, int y_stride, int c_stride);

/* The same, restricted to output rows [row0, row1).
 *
 * The downscale is bound by memory latency on a single core, and it is the
 * one remaining cost that does not trade away picture quality - so it is
 * worth splitting across cores. Rows are independent, which makes this the
 * only part of the pipeline that parallelises cleanly: the JPEG stage cannot,
 * because DC prediction and the bit stream both run start to finish.
 *
 * row0 must be even. Chroma rows are shared between each pair of luma rows,
 * so an odd split would have two threads writing the same chroma row. */
void wstr_rgba_to_yuv420_rows(const uint8_t *src, int src_w, int src_h,
                              int src_stride, uint8_t *y, uint8_t *cb,
                              uint8_t *cr, int dst_w, int dst_h,
                              int y_stride, int c_stride, int row0, int row1);

/* Worst-case output size for a frame, so callers can size a buffer once. */
size_t wstr_jpeg_bound(int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* WSTR_JPEG_H */
