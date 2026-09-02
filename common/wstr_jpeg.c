/* wstr_jpeg.c - see wstr_jpeg.h */

#include "wstr_jpeg.h"
#include <string.h>

/* ------------------------------------------------------------------ tables */

static const uint8_t k_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t k_qt_luma[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99
};

static const uint8_t k_qt_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

/* Annex K Huffman specs: bits[i] = how many codes of length i+1. */
static const uint8_t k_dc_luma_bits[16]   = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t k_dc_luma_vals[12]   = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t k_dc_chroma_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t k_dc_chroma_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t k_ac_luma_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t k_ac_luma_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

static const uint8_t k_ac_chroma_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t k_ac_chroma_vals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

/* Fixed-point constants for the integer DCT (Loeffler-Ligtenberg-Moschytz, as
 * in libjpeg's jpeg_fdct_islow), scaled by 2^13.
 *
 * The float AAN transform this replaces was not slow in itself - the 750 has a
 * perfectly good FPU. The cost was at the edges: it has no integer<->float
 * register move, so every sample entering the transform and every coefficient
 * leaving it went through a store/load round trip. At 64 in and 64 out per
 * block and ~3000 blocks a frame, that is a quarter of a million pipeline
 * stalls that the arithmetic never needed. Staying in integers removes all of
 * them. */
#define CONST_BITS 13
#define PASS1_BITS 2

#define FIX_0_298631336  2446
#define FIX_0_390180644  3196
#define FIX_0_541196100  4433
#define FIX_0_765366865  6270
#define FIX_0_899976223  7373
#define FIX_1_175875602  9633
#define FIX_1_501321110  12299
#define FIX_1_847759065  15137
#define FIX_1_961570560  16069
#define FIX_2_053119869  16819
#define FIX_2_562915447  20995
#define FIX_3_072711026  25172

#define DESCALE(x, n)  (((x) + (((int32_t)1) << ((n) - 1))) >> (n))

/* Quantisation rounding bias, in 15.15. 16384 is round-to-nearest; lower
 * values round toward zero, zeroing more small AC coefficients - a deadzone.
 *
 * Measured against a properly area-averaged downscale of the source (so that
 * aliasing counts as error rather than as detail), 11000 gives 7% fewer bytes
 * *and* better fidelity than round-to-nearest, because most of what it
 * discards is sampling noise rather than picture. It also speeds the encoder
 * up, since a zeroed coefficient costs nothing to code.
 *
 * The curve keeps improving down to 7000 on that test image, but only just -
 * and one synthetic image is a thin basis for an aggressive deadzone, which on
 * real content starts eating genuine low-contrast detail. 11000 is the knee. */
#ifndef WSTR_QBIAS
#define WSTR_QBIAS 11000
#endif

/* Average two horizontally adjacent source pixels instead of taking one.
 *
 * Point sampling a 1280-wide frame down to 480 aliases badly, and aliasing is
 * noise - the most expensive thing there is to JPEG-encode, and it is not even
 * signal. Averaging costs almost nothing here because the second pixel is
 * nearly always in the same cache line as the first, and this loop is bound by
 * cache misses rather than arithmetic. A full 2x2 box was also tried: it needs
 * a second source row, doubling the misses, and that is not worth it.
 *
 * Measured at 480x270: 9.7% fewer bytes and 0.9 dB closer to a correct
 * downscale. The read cost is the open question - it is +78% on x86, but x86
 * holds the whole source in cache and is therefore throughput-bound, where the
 * console misses to MEM2 and the second sample rides the line that already
 * missed. If the console's read time jumps rather than creeping, this is the
 * thing to turn off. */
#ifndef WSTR_HAVG
#define WSTR_HAVG 1
#endif

/* ------------------------------------------------------- encoder state */

typedef struct {
    uint16_t code[256];
    uint8_t  size[256];
} huff_tbl;

typedef struct {
    uint8_t  *out;
    size_t    cap;
    size_t    len;
    int       overflow;

    uint32_t  bitbuf;   /* bits pack MSB-first into the top of this word */
    int       bitcnt;

    uint8_t   qt[2][64];      /* natural order                           */
    /* Reciprocal of (qval * 8) in 15.15 fixed point, so quantisation is a
     * multiply and a shift rather than a divide - PowerPC integer division is
     * ~20 cycles and there are 64 of them per block. */
    int32_t   recip[2][64];
    huff_tbl  hdc[2], hac[2];
} jenc;

static void emit_byte(jenc *e, uint8_t b)
{
    if (e->len >= e->cap) { e->overflow = 1; return; }
    e->out[e->len++] = b;
}

static void emit_word(jenc *e, uint16_t w)
{
    emit_byte(e, (uint8_t)(w >> 8));
    emit_byte(e, (uint8_t)(w & 0xff));
}

/* Entropy-coded 0xFF must be escaped as 0xFF 0x00, or a decoder reads it as
 * the start of a marker. */
static void emit_bits(jenc *e, uint16_t code, int nbits)
{
    if (nbits <= 0) return;
    e->bitcnt += nbits;
    e->bitbuf |= ((uint32_t)code & ((1u << nbits) - 1u)) << (32 - e->bitcnt);
    while (e->bitcnt >= 8) {
        uint8_t b = (uint8_t)(e->bitbuf >> 24);
        emit_byte(e, b);
        if (b == 0xff) emit_byte(e, 0x00);
        e->bitbuf <<= 8;
        e->bitcnt  -= 8;
    }
}

static void emit_flush(jenc *e)
{
    /* Pad the final partial byte with 1s, per the spec. emit_bits() drains
     * every whole byte as it goes, so bitcnt is always 0..7 here and exactly
     * one top-up completes the byte. Looping on it instead would push whole
     * extra bytes into the stream, which decoders report as "extraneous
     * bytes before marker 0xd9". */
    if (e->bitcnt > 0) {
        int pad = 8 - e->bitcnt;
        emit_bits(e, (uint16_t)((1u << pad) - 1u), pad);
    }
    e->bitbuf = 0;
    e->bitcnt = 0;
}

static void build_huff(huff_tbl *t, const uint8_t *bits, const uint8_t *vals)
{
    int k = 0, code = 0, len;
    memset(t, 0, sizeof(*t));
    for (len = 1; len <= 16; len++) {
        int n = bits[len - 1], i;
        for (i = 0; i < n; i++) {
            t->code[vals[k]] = (uint16_t)code;
            t->size[vals[k]] = (uint8_t)len;
            code++; k++;
        }
        code <<= 1;
    }
}

static void build_quant(jenc *e, int quality)
{
    int scale, t, i;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;
    scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);

    for (t = 0; t < 2; t++) {
        const uint8_t *base = t ? k_qt_chroma : k_qt_luma;
        for (i = 0; i < 64; i++) {
            int v = (base[i] * scale + 50) / 100;
            if (v < 1)   v = 1;
            if (v > 255) v = 255;   /* baseline JPEG has 8-bit quant only */
            e->qt[t][i] = (uint8_t)v;
        }
        for (i = 0; i < 64; i++) {
            /* The transform leaves coefficients scaled by 8, so the effective
             * divisor is eight times the table entry. */
            int32_t d = (int32_t)e->qt[t][i] * 8;
            e->recip[t][i] = ((1 << 15) + (d / 2)) / d;
        }
    }
}

/* ------------------------------------------------------------- the DCT */

/* Integer DCT, LL&M as in libjpeg's jpeg_fdct_islow.
 *
 * Two passes of the same butterfly, rows then columns. Output is the true DCT
 * scaled by 8, which the quantisation reciprocals above account for. */
static void fdct8x8(int32_t *data)
{
    int32_t tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    int32_t tmp10, tmp11, tmp12, tmp13;
    int32_t z1, z2, z3, z4, z5;
    int32_t *p = data;
    int ctr;

    for (ctr = 0; ctr < 8; ctr++, p += 8) {
        tmp0 = p[0] + p[7]; tmp7 = p[0] - p[7];
        tmp1 = p[1] + p[6]; tmp6 = p[1] - p[6];
        tmp2 = p[2] + p[5]; tmp5 = p[2] - p[5];
        tmp3 = p[3] + p[4]; tmp4 = p[3] - p[4];

        tmp10 = tmp0 + tmp3; tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2; tmp12 = tmp1 - tmp2;

        p[0] = (tmp10 + tmp11) << PASS1_BITS;
        p[4] = (tmp10 - tmp11) << PASS1_BITS;

        z1 = (tmp12 + tmp13) * FIX_0_541196100;
        p[2] = DESCALE(z1 + tmp13 * FIX_0_765366865,   CONST_BITS - PASS1_BITS);
        p[6] = DESCALE(z1 + tmp12 * -FIX_1_847759065,  CONST_BITS - PASS1_BITS);

        z1 = tmp4 + tmp7;
        z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6;
        z4 = tmp5 + tmp7;
        z5 = (z3 + z4) * FIX_1_175875602;

        tmp4 *= FIX_0_298631336;
        tmp5 *= FIX_2_053119869;
        tmp6 *= FIX_3_072711026;
        tmp7 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223;
        z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560;
        z4 *= -FIX_0_390180644;

        z3 += z5;
        z4 += z5;

        p[7] = DESCALE(tmp4 + z1 + z3, CONST_BITS - PASS1_BITS);
        p[5] = DESCALE(tmp5 + z2 + z4, CONST_BITS - PASS1_BITS);
        p[3] = DESCALE(tmp6 + z2 + z3, CONST_BITS - PASS1_BITS);
        p[1] = DESCALE(tmp7 + z1 + z4, CONST_BITS - PASS1_BITS);
    }

    p = data;
    for (ctr = 0; ctr < 8; ctr++, p++) {
        tmp0 = p[0]  + p[56]; tmp7 = p[0]  - p[56];
        tmp1 = p[8]  + p[48]; tmp6 = p[8]  - p[48];
        tmp2 = p[16] + p[40]; tmp5 = p[16] - p[40];
        tmp3 = p[24] + p[32]; tmp4 = p[24] - p[32];

        tmp10 = tmp0 + tmp3; tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2; tmp12 = tmp1 - tmp2;

        p[0]  = DESCALE(tmp10 + tmp11, PASS1_BITS);
        p[32] = DESCALE(tmp10 - tmp11, PASS1_BITS);

        z1 = (tmp12 + tmp13) * FIX_0_541196100;
        p[16] = DESCALE(z1 + tmp13 * FIX_0_765366865,  CONST_BITS + PASS1_BITS);
        p[48] = DESCALE(z1 + tmp12 * -FIX_1_847759065, CONST_BITS + PASS1_BITS);

        z1 = tmp4 + tmp7;
        z2 = tmp5 + tmp6;
        z3 = tmp4 + tmp6;
        z4 = tmp5 + tmp7;
        z5 = (z3 + z4) * FIX_1_175875602;

        tmp4 *= FIX_0_298631336;
        tmp5 *= FIX_2_053119869;
        tmp6 *= FIX_3_072711026;
        tmp7 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223;
        z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560;
        z4 *= -FIX_0_390180644;

        z3 += z5;
        z4 += z5;

        p[56] = DESCALE(tmp4 + z1 + z3, CONST_BITS + PASS1_BITS);
        p[40] = DESCALE(tmp5 + z2 + z4, CONST_BITS + PASS1_BITS);
        p[24] = DESCALE(tmp6 + z2 + z3, CONST_BITS + PASS1_BITS);
        p[8]  = DESCALE(tmp7 + z1 + z4, CONST_BITS + PASS1_BITS);
    }
}

/* Significant bits in the magnitude of v - the JPEG "category". */
static int magnitude(int v)
{
    int n = 0;
    unsigned a = (unsigned)(v < 0 ? -v : v);
    while (a) { n++; a >>= 1; }
    return n;
}

/* Encode one 8x8 block. Returns the new DC predictor for this component. */
static int encode_block(jenc *e, int32_t *blk, int tbl, int dc_pred)
{
    int zq[64];
    int i, diff, nbits, run, end;
    const int32_t *recip = e->recip[tbl];
    const huff_tbl *hdc = &e->hdc[tbl];
    const huff_tbl *hac = &e->hac[tbl];

    fdct8x8(blk);

    /* Quantise straight into zigzag order, so the AC scan below is linear.
     * Multiply by the reciprocal and shift; rounding to nearest is the +2^14
     * before the shift, and the sign is handled explicitly so both directions
     * round away from zero the way the float version did. */
    for (i = 0; i < 64; i++) {
        int nat = k_zigzag[i];
        int32_t v = blk[nat];
        int32_t r = recip[nat];
        zq[i] = (v >= 0) ?  (int)(( v * r + WSTR_QBIAS) >> 15)
                         : -(int)((-v * r + WSTR_QBIAS) >> 15);
    }

    diff  = zq[0] - dc_pred;
    nbits = magnitude(diff);
    emit_bits(e, hdc->code[nbits], hdc->size[nbits]);
    if (nbits) {
        /* Negative values are coded as the low bits of (v - 1). */
        emit_bits(e, (uint16_t)(diff < 0 ? diff - 1 : diff), nbits);
    }

    end = 0;
    for (i = 63; i > 0; i--) { if (zq[i]) { end = i; break; } }

    run = 0;
    for (i = 1; i <= end; i++) {
        int sym;
        if (zq[i] == 0) { run++; continue; }
        while (run >= 16) {                 /* ZRL for each full run of 16 */
            emit_bits(e, hac->code[0xf0], hac->size[0xf0]);
            run -= 16;
        }
        nbits = magnitude(zq[i]);
        sym = (run << 4) | nbits;
        emit_bits(e, hac->code[sym], hac->size[sym]);
        emit_bits(e, (uint16_t)(zq[i] < 0 ? zq[i] - 1 : zq[i]), nbits);
        run = 0;
    }
    if (end < 63) emit_bits(e, hac->code[0x00], hac->size[0x00]);   /* EOB */

    return zq[0];
}

/* ----------------------------------------------------------- the headers */

static void write_headers(jenc *e, int w, int h)
{
    int t, i;

    emit_word(e, 0xffd8);                       /* SOI */

    emit_word(e, 0xffe0);                       /* APP0 / JFIF */
    emit_word(e, 16);
    emit_byte(e, 0x4a); emit_byte(e, 0x46);     /* "JFIF\0" */
    emit_byte(e, 0x49); emit_byte(e, 0x46);
    emit_byte(e, 0x00);
    emit_word(e, 0x0101);                       /* version 1.1 */
    emit_byte(e, 0);                            /* density units: none */
    emit_word(e, 1); emit_word(e, 1);           /* 1:1 pixel aspect */
    emit_byte(e, 0); emit_byte(e, 0);           /* no thumbnail */

    for (t = 0; t < 2; t++) {                   /* DQT, one per table */
        emit_word(e, 0xffdb);
        emit_word(e, 67);
        emit_byte(e, (uint8_t)t);               /* 8-bit precision, id = t */
        for (i = 0; i < 64; i++) emit_byte(e, e->qt[t][k_zigzag[i]]);
    }

    emit_word(e, 0xffc0);                       /* SOF0, baseline */
    emit_word(e, 17);
    emit_byte(e, 8);
    emit_word(e, (uint16_t)h);
    emit_word(e, (uint16_t)w);
    emit_byte(e, 3);
    emit_byte(e, 1); emit_byte(e, 0x22); emit_byte(e, 0);  /* Y,  2x2, qt 0 */
    emit_byte(e, 2); emit_byte(e, 0x11); emit_byte(e, 1);  /* Cb, 1x1, qt 1 */
    emit_byte(e, 3); emit_byte(e, 0x11); emit_byte(e, 1);  /* Cr, 1x1, qt 1 */

    {                                           /* DHT x4 */
        const uint8_t *bits[4] = { k_dc_luma_bits, k_ac_luma_bits,
                                   k_dc_chroma_bits, k_ac_chroma_bits };
        const uint8_t *vals[4] = { k_dc_luma_vals, k_ac_luma_vals,
                                   k_dc_chroma_vals, k_ac_chroma_vals };
        const int      nval[4] = { 12, 162, 12, 162 };
        const uint8_t ident[4] = { 0x00, 0x10, 0x01, 0x11 };
        int k;
        for (k = 0; k < 4; k++) {
            int n = nval[k];
            emit_word(e, 0xffc4);
            emit_word(e, (uint16_t)(2 + 1 + 16 + n));
            emit_byte(e, ident[k]);
            for (i = 0; i < 16; i++) emit_byte(e, bits[k][i]);
            for (i = 0; i < n;  i++) emit_byte(e, vals[k][i]);
        }
    }

    emit_word(e, 0xffda);                       /* SOS */
    emit_word(e, 12);
    emit_byte(e, 3);
    emit_byte(e, 1); emit_byte(e, 0x00);        /* Y  uses DC0/AC0 */
    emit_byte(e, 2); emit_byte(e, 0x11);        /* Cb uses DC1/AC1 */
    emit_byte(e, 3); emit_byte(e, 0x11);        /* Cr uses DC1/AC1 */
    emit_byte(e, 0); emit_byte(e, 63); emit_byte(e, 0);
}

/* Copy an 8x8 block out of a plane, clamping at the right and bottom edges,
 * so sizes that are not multiples of 16 replicate their last row/column
 * instead of needing a padded copy of the whole plane. */
static void fetch_block(int32_t *dst, const uint8_t *plane, int stride,
                        int px, int py, int pw, int ph)
{
    int r, c;

    /* Fast path for a block that lies entirely inside the plane, which is
     * every block except the last column and row of MCUs. The general path
     * below pays two compares per pixel purely to handle those edges - 64
     * branches per block, on every block, to serve the few percent that need
     * it. At 640x360 that is 3600 blocks a frame. */
    if (px + 8 <= pw && py + 8 <= ph) {
        const uint8_t *row = plane + (size_t)py * stride + px;
        for (r = 0; r < 8; r++, row += stride) {
            dst[r * 8 + 0] = (int32_t)row[0] - 128;
            dst[r * 8 + 1] = (int32_t)row[1] - 128;
            dst[r * 8 + 2] = (int32_t)row[2] - 128;
            dst[r * 8 + 3] = (int32_t)row[3] - 128;
            dst[r * 8 + 4] = (int32_t)row[4] - 128;
            dst[r * 8 + 5] = (int32_t)row[5] - 128;
            dst[r * 8 + 6] = (int32_t)row[6] - 128;
            dst[r * 8 + 7] = (int32_t)row[7] - 128;
        }
        return;
    }

    for (r = 0; r < 8; r++) {
        const uint8_t *row;
        int sy = py + r;
        if (sy >= ph) sy = ph - 1;
        row = plane + (size_t)sy * stride;
        for (c = 0; c < 8; c++) {
            int sx = px + c;
            if (sx >= pw) sx = pw - 1;
            dst[r * 8 + c] = (int32_t)row[sx] - 128;
        }
    }
}

size_t wstr_jpeg_bound(int w, int h)
{
    /* Headers are ~600 bytes; baseline 4:2:0 entropy data stays well under
     * 2 bytes per pixel even at quality 100. */
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    return (size_t)w * (size_t)h * 2u + 4096u;
}

int wstr_jpeg_encode(const uint8_t *y, const uint8_t *cb, const uint8_t *cr,
                     int w, int h, int y_stride, int c_stride,
                     int quality, uint8_t *out, size_t out_max)
{
    jenc e;
    int mcux, mcuy, mx, my;
    int cw, ch;
    int dc_y = 0, dc_cb = 0, dc_cr = 0;
    int32_t blk[64];

    if (!y || !cb || !cr || !out || w <= 0 || h <= 0) return -1;

    memset(&e, 0, sizeof(e));
    e.out = out;
    e.cap = out_max;

    build_quant(&e, quality);
    build_huff(&e.hdc[0], k_dc_luma_bits,   k_dc_luma_vals);
    build_huff(&e.hac[0], k_ac_luma_bits,   k_ac_luma_vals);
    build_huff(&e.hdc[1], k_dc_chroma_bits, k_dc_chroma_vals);
    build_huff(&e.hac[1], k_ac_chroma_bits, k_ac_chroma_vals);

    write_headers(&e, w, h);

    cw = (w + 1) / 2;
    ch = (h + 1) / 2;
    mcux = (w + 15) / 16;
    mcuy = (h + 15) / 16;

    for (my = 0; my < mcuy; my++) {
        for (mx = 0; mx < mcux; mx++) {
            /* 4 luma blocks then one each of Cb and Cr - the 2x2 sampling
             * order the SOF above declares. */
            fetch_block(blk, y, y_stride, mx*16,     my*16,     w, h);
            dc_y  = encode_block(&e, blk, 0, dc_y);
            fetch_block(blk, y, y_stride, mx*16 + 8, my*16,     w, h);
            dc_y  = encode_block(&e, blk, 0, dc_y);
            fetch_block(blk, y, y_stride, mx*16,     my*16 + 8, w, h);
            dc_y  = encode_block(&e, blk, 0, dc_y);
            fetch_block(blk, y, y_stride, mx*16 + 8, my*16 + 8, w, h);
            dc_y  = encode_block(&e, blk, 0, dc_y);

            fetch_block(blk, cb, c_stride, mx*8, my*8, cw, ch);
            dc_cb = encode_block(&e, blk, 1, dc_cb);
            fetch_block(blk, cr, c_stride, mx*8, my*8, cw, ch);
            dc_cr = encode_block(&e, blk, 1, dc_cr);

            if (e.overflow) return -1;
        }
    }

    emit_flush(&e);
    emit_word(&e, 0xffd9);                      /* EOI */

    return e.overflow ? -1 : (int)e.len;
}

/* ------------------------------------------------------- downscale + YCbCr */

void wstr_rgba_to_yuv420(const uint8_t *src, int src_w, int src_h, int src_stride,
                         uint8_t *y, uint8_t *cb, uint8_t *cr,
                         int dst_w, int dst_h, int y_stride, int c_stride)
{
    wstr_rgba_to_yuv420_rows(src, src_w, src_h, src_stride, y, cb, cr,
                             dst_w, dst_h, y_stride, c_stride, 0, dst_h);
}

void wstr_rgba_to_yuv420_rows(const uint8_t *src, int src_w, int src_h,
                              int src_stride, uint8_t *y, uint8_t *cb,
                              uint8_t *cr, int dst_w, int dst_h,
                              int y_stride, int c_stride, int row0, int row1)
{
    /* 16.16 fixed-point stepping through the source, so any ratio works and
     * the inner loop contains no division.
     *
     * The position is accumulated rather than computed as dx * xstep. That
     * multiply needs 64 bits to stay exact, and a 64-bit multiply per pixel on
     * a 32-bit PowerPC is several instructions plus a register pair - for a
     * value that only ever advances by a constant. An add does the same job.
     *
     * This loop is memory-bound on the console far more than arithmetic-bound:
     * the same code costs 0.5 ms on x86 and 19 ms on the Wii U, a much worse
     * ratio than the encoder's, which is what a stream of cache misses through
     * MEM2 looks like. Hence the prefetch - the access pattern is a predictable
     * forward walk, which is exactly what dcbt is for. */
    uint32_t xstep, ystep, yacc;
    int dy, dx;
#if WSTR_HAVG
    const uint8_t *hb;
    uint8_t hv[4];
#endif

    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
    if (row0 < 0) row0 = 0;
    if (row1 > dst_h) row1 = dst_h;
    if (row0 >= row1) return;

    xstep = ((uint32_t)src_w << 16) / (uint32_t)dst_w;
    ystep = ((uint32_t)src_h << 16) / (uint32_t)dst_h;

    /* Seek the accumulator to the first row of this slice. One 64-bit multiply
     * per call, rather than per pixel. */
    yacc = (uint32_t)(((uint64_t)row0 * ystep));

    for (dy = row0; dy < row1; dy++, yacc += ystep) {
        const uint8_t *srow;
        uint8_t *yrow, *cbrow, *crrow;
        uint32_t sy = yacc >> 16;
        uint32_t xacc = 0;

        if ((int)sy >= src_h) sy = (uint32_t)(src_h - 1);
        srow  = src + (size_t)sy * src_stride;
        yrow  = y  + (size_t)dy * y_stride;

        if ((dy & 1) == 0) {
            /* Chroma row: every other pixel also writes Cb/Cr. Handled two
             * pixels at a time so the "is this an even column" test that used
             * to run per pixel disappears into the loop structure. */
            cbrow = cb + (size_t)(dy >> 1) * c_stride;
            crrow = cr + (size_t)(dy >> 1) * c_stride;

            for (dx = 0; dx + 1 < dst_w; dx += 2) {
                const uint8_t *p;
                int r, g, b, u, v;

                {
                    const uint8_t *sp = srow + ((xacc >> 16) << 2);
                    /* Prefetch the source, 256 bytes ahead - not p, which
                     * under averaging points at a 4-byte staging value.
                     * Consecutive samples are only ~11 bytes apart at these
                     * ratios, so one cache line of lead is barely six samples
                     * of warning; this loop moves ~1.4 MB a frame at an
                     * effective 73 MB/s and is latency-bound, not
                     * bandwidth-bound. It is waiting, and the fix for waiting
                     * is to ask earlier. */
                    __builtin_prefetch(sp + 256);
#if WSTR_HAVG
                    {
                        uint32_t sxb = (xacc >> 16) + 1;
                        if ((int)sxb >= src_w) sxb = (uint32_t)(src_w - 1);
                        hb = srow + (sxb << 2);
                        hv[0] = (uint8_t)((sp[0] + hb[0]) >> 1);
                        hv[1] = (uint8_t)((sp[1] + hb[1]) >> 1);
                        hv[2] = (uint8_t)((sp[2] + hb[2]) >> 1);
                        p = hv;
                    }
#else
                    p = sp;                   /* RGBA8: R first in memory */
#endif
                }
                r = p[0]; g = p[1]; b = p[2];
                xacc += xstep;

                /* JFIF full-range BT.601, 16.16 fixed point. */
                yrow[dx] = (uint8_t)((19595 * r + 38470 * g + 7471 * b) >> 16);

                /* Chroma is sampled, not averaged - same reasoning as the
                 * luma point sampling. */
                u = ((-11059 * r - 21709 * g + 32768 * b) >> 16) + 128;
                v = (( 32768 * r - 27439 * g -  5329 * b) >> 16) + 128;
                if (u < 0) u = 0; else if (u > 255) u = 255;
                if (v < 0) v = 0; else if (v > 255) v = 255;
                cbrow[dx >> 1] = (uint8_t)u;
                crrow[dx >> 1] = (uint8_t)v;

                p = srow + ((xacc >> 16) << 2);
                r = p[0]; g = p[1]; b = p[2];
                xacc += xstep;
                yrow[dx + 1] = (uint8_t)((19595 * r + 38470 * g + 7471 * b) >> 16);
            }
            if (dx < dst_w) {                     /* odd width */
                const uint8_t *p = srow + ((xacc >> 16) << 2);
                int r = p[0], g = p[1], b = p[2];
                int u = ((-11059 * r - 21709 * g + 32768 * b) >> 16) + 128;
                int v = (( 32768 * r - 27439 * g -  5329 * b) >> 16) + 128;
                if (u < 0) u = 0; else if (u > 255) u = 255;
                if (v < 0) v = 0; else if (v > 255) v = 255;
                yrow[dx] = (uint8_t)((19595 * r + 38470 * g + 7471 * b) >> 16);
                cbrow[dx >> 1] = (uint8_t)u;
                crrow[dx >> 1] = (uint8_t)v;
            }
        } else {
            /* Luma-only row - half the work and none of the clamping. */
            for (dx = 0; dx < dst_w; dx++, xacc += xstep) {
                const uint8_t *sp = srow + ((xacc >> 16) << 2);
                int R, G, B;
                __builtin_prefetch(sp + 256);
#if WSTR_HAVG
                {
                    uint32_t sxb = (xacc >> 16) + 1;
                    const uint8_t *q;
                    if ((int)sxb >= src_w) sxb = (uint32_t)(src_w - 1);
                    q = srow + (sxb << 2);
                    R = (sp[0] + q[0]) >> 1;
                    G = (sp[1] + q[1]) >> 1;
                    B = (sp[2] + q[2]) >> 1;
                }
#else
                R = sp[0]; G = sp[1]; B = sp[2];
#endif
                yrow[dx] = (uint8_t)((19595 * R + 38470 * G + 7471 * B) >> 16);
            }
        }
    }
}
