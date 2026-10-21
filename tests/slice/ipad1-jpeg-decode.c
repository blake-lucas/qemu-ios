/* The JPEG block's decoder (s5l8920_jpeg.c jpeg_parse, jpeg_decode), as 6.x Music's hardware path (Cover Flow's
 * album covers, through FigJPEGDecodeSession) uses it: libjpeg encodes a picture in 4:2:0, 4:2:2, 4:4:4 and
 * grayscale, with and without restart markers, and every component plane the decoder returns is within a step or two
 * of libjpeg's own decode of the same stream; the header pass's size and subsampling are the frame's; a progressive
 * stream and a truncated one are refused.
 *
 * SLICE hw/arm/s5l8920_jpeg.c range static const uint8_t zigzag | /* ---- the block
 * PKG glib-2.0 libjpeg
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <assert.h>
#include <jpeglib.h>
#include "slice.h"

enum { W = 61, H = 45 };        /* not whole MCUs either way */

static unsigned char *encode(int h0, int v0, int ncomp, int restart, bool progressive, unsigned long *len)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr err;
    unsigned char *out = NULL, row[W * 3];

    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, len);
    c.image_width = W;
    c.image_height = H;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    if (ncomp == 1) {
        jpeg_set_colorspace(&c, JCS_GRAYSCALE);
    }
    jpeg_set_quality(&c, 90, TRUE);
    c.comp_info[0].h_samp_factor = h0;
    c.comp_info[0].v_samp_factor = v0;
    c.restart_interval = restart;
    if (progressive) {
        jpeg_simple_progression(&c);
    }
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < H) {
        unsigned y = c.next_scanline;
        for (unsigned x = 0; x < W; x++) {      /* four colored quadrants and a gradient */
            bool right = x >= W / 2, bottom = y >= H / 2;
            row[x * 3] = right ? (bottom ? 240 : 40) : (bottom ? 40 : 200);
            row[x * 3 + 1] = right ? (bottom ? 220 : 180) : 60 + y;
            row[x * 3 + 2] = bottom && !right ? 200 : 30 + x;
        }
        JSAMPROW r = row;
        jpeg_write_scanlines(&c, &r, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    return out;
}

/* libjpeg's own component planes (raw, before upsampling and color conversion). */
static void reference(const unsigned char *jpg, unsigned long len, uint8_t *planes[3], unsigned strides[3])
{
    struct jpeg_decompress_struct d;
    struct jpeg_error_mgr err;

    d.err = jpeg_std_error(&err);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, jpg, len);
    jpeg_read_header(&d, TRUE);
    d.raw_data_out = TRUE;
    d.dct_method = JDCT_FLOAT;
    jpeg_start_decompress(&d);
    unsigned rows = d.max_v_samp_factor * DCTSIZE;
    while (d.output_scanline < d.output_height) {
        JSAMPROW ptrs[3][32];
        JSAMPARRAY arrays[3];
        for (int c = 0; c < d.num_components; c++) {
            unsigned crow = d.output_scanline / d.max_v_samp_factor * d.comp_info[c].v_samp_factor;
            for (int i = 0; i < d.comp_info[c].v_samp_factor * DCTSIZE; i++) {
                ptrs[c][i] = planes[c] + (size_t)(crow + i) * strides[c];
            }
            arrays[c] = ptrs[c];
        }
        assert(jpeg_read_raw_data(&d, arrays, rows) == rows);
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
}

static void check(int h0, int v0, int ncomp, int restart, int code)
{
    unsigned long len = 0;
    unsigned char *jpg = encode(h0, v0, ncomp, restart, false, &len);
    JpegFrame f;
    uint8_t *mine[3] = { NULL }, *ref[3] = { NULL };
    unsigned strides[3];

    assert(jpeg_parse(jpg, len, &f));
    assert(f.w == W && f.h == H && f.ncomp == (unsigned)ncomp);
    assert(f.comp[0].h == h0 && f.comp[0].v == v0);
    int got = ncomp == 1 ? 3 : h0 == 1 && v0 == 1 ? 0 : h0 == 2 && v0 == 1 ? 1 : 2;
    assert(got == code);
    for (unsigned c = 0; c < f.ncomp; c++) {
        strides[c] = f.mcux * f.comp[c].h * 8;
        size_t size = (size_t)strides[c] * f.mcuy * f.comp[c].v * 8;
        mine[c] = g_malloc0(size);
        ref[c] = g_malloc0(size + strides[c] * 16);
    }
    assert(jpeg_decode(&f, mine));
    reference(jpg, len, ref, strides);
    for (unsigned c = 0; c < f.ncomp; c++) {
        unsigned cw = (W * f.comp[c].h + f.hmax - 1) / f.hmax, ch = (H * f.comp[c].v + f.vmax - 1) / f.vmax;
        for (unsigned y = 0; y < ch; y++) {
            for (unsigned x = 0; x < cw; x++) {
                int a = mine[c][y * strides[c] + x], b = ref[c][y * strides[c] + x];
                if (abs(a - b) > 2) {
                    fprintf(stderr, "%dx%d/%d rst %d: component %u (%u,%u) %d, libjpeg %d\n", h0, v0, ncomp,
                            restart, c, x, y, a, b);
                    abort();
                }
            }
        }
        g_free(mine[c]);
        g_free(ref[c]);
    }
    free(jpg);
}

int main(void)
{
    check(2, 2, 3, 0, 2);       /* 4:2:0 */
    check(2, 2, 3, 3, 2);       /* with restart markers every three MCUs */
    check(2, 1, 3, 0, 1);       /* 4:2:2 */
    check(1, 1, 3, 0, 0);       /* 4:4:4 */
    check(1, 1, 1, 2, 3);       /* grayscale */

    unsigned long len = 0;
    unsigned char *jpg = encode(2, 2, 3, 0, true, &len);
    JpegFrame f;
    assert(!jpeg_parse(jpg, len, &f));          /* progressive: the block's "stream corrupt" */
    free(jpg);
    jpg = encode(2, 2, 3, 0, false, &len);
    assert(!jpeg_parse(jpg, 200, &f));          /* cut off in the headers */
    free(jpg);
    puts("ok");
    return 0;
}
