/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest upload policy around Imagination's unmodified reference decoder.
 * The SDK assumes aligned native-endian words and spec-padded mip tails.
 * Guest bytes are little-endian and some legacy drivers send compact tails.
 */
#include "pvrtc.h"
#include "PVRTDecompress.cpp"
#include <new>
#include <atomic>
#include <thread>

/*
 * pvr::pvrtcDecompress's loop over word rows, for rows [row0, row1) of the -1 .. rows - 2 it walks. Each row writes
 * only its own pixels (the lower half of its words, the upper half of the next row's), so disjoint ranges can run on
 * separate threads and give the vendor's output bit for bit.
 */
static void decompress_rows(const uint32_t *words, pvr::Pixel32 *out, uint32_t width, uint32_t height, uint8_t bpp,
                            int32_t row0, int32_t row1)
{
    const uint32_t word_width = bpp == 2 ? 8 : 4, word_height = 4;
    const int32_t columns = int32_t(width / word_width), rows = int32_t(height / word_height);
    pvr::Pixel32 pixels[8 * 4];
    pvr::PVRTCWordIndices at;

    for (int32_t y = row0; y < row1; y++) {
        for (int32_t x = -1; x < columns - 1; x++) {
            at.P[0] = int(pvr::wrapWordIndex(columns, x));
            at.P[1] = int(pvr::wrapWordIndex(rows, y));
            at.Q[0] = int(pvr::wrapWordIndex(columns, x + 1));
            at.Q[1] = at.P[1];
            at.R[0] = at.P[0];
            at.R[1] = int(pvr::wrapWordIndex(rows, y + 1));
            at.S[0] = at.Q[0];
            at.S[1] = at.R[1];
            const uint32_t *w[4] = {
                words + pvr::TwiddleUV(columns, rows, at.P[0], at.P[1]) * 2,
                words + pvr::TwiddleUV(columns, rows, at.Q[0], at.Q[1]) * 2,
                words + pvr::TwiddleUV(columns, rows, at.R[0], at.R[1]) * 2,
                words + pvr::TwiddleUV(columns, rows, at.S[0], at.S[1]) * 2,
            };
            pvr::PVRTCWord P{w[0][0], w[0][1]}, Q{w[1][0], w[1][1]}, R{w[2][0], w[2][1]}, S{w[3][0], w[3][1]};
            pvr::pvrtcGetDecompressedPixels(P, Q, R, S, pixels, bpp);
            pvr::mapDecompressedData(out, width, pixels, at, bpp);
        }
    }
}

/*
 * The vendor decoder takes 20-40 ns a texel on one core: Epic Citadel's 2048x2048 levels held the guest's GL call
 * for 50-135 ms each. A level of at least 256x256 (where the vendor needs no padded copy) is decoded by up to 8
 * threads taking chunks of 4 word rows from a shared counter, so a slow core holds up one chunk, not a fixed share.
 * The caller's thread decodes too, and does all of it if no helper starts.
 */
static void decompress(const uint32_t *words, uint32_t w, uint32_t h, int bpp, uint8_t *dst)
{
    const uint32_t word_width = bpp == 2 ? 8 : 4;
    const int32_t rows = int32_t(h / 4), chunk = 4;
    static const bool off = [] {
        const char *v = getenv("LT_PVRTC_THREADS");     /* "off": one thread, for an A/B run */
        return v && !strcmp(v, "off");
    }();
    unsigned threads = off ? 1 : std::min(std::max(std::thread::hardware_concurrency(), 1u), 8u);

    if (uint64_t(w) * h < 256 * 256 || w < word_width * 2 || h < 8 || threads < 2) {
        pvr::PVRTDecompressPVRTC(words, bpp == 2, w, h, dst);
        return;
    }
    threads = std::min<unsigned>(threads, unsigned(rows / chunk));
    std::atomic<int32_t> next{-1};          /* the vendor's rows run from -1 to rows - 2 */
    auto work = [&] {
        for (int32_t row; (row = next.fetch_add(chunk)) < rows - 1;) {
            decompress_rows(words, (pvr::Pixel32 *)dst, w, h, uint8_t(bpp), row, std::min(row + chunk, rows - 1));
        }
    };
    std::thread pool[8];
    for (unsigned t = 1; t < threads; t++) {
        try {
            pool[t] = std::thread(work);
        } catch (...) {
            break;
        }
    }
    work();
    for (unsigned t = 1; t < threads; t++) {
        if (pool[t].joinable()) pool[t].join();
    }
}

extern "C" bool ltm_pvrtc_decode(const uint8_t *src, uint32_t w, uint32_t h,
                                 int bpp, bool alpha, bool padded, uint8_t *dst)
{
    if (!src || !dst || !w || !h || w > 4096 || h > 4096 ||
        (w & (w - 1)) || (h & (h - 1)) || (bpp != 2 && bpp != 4)) {
        return false;
    }
    const uint32_t block_width = bpp == 2 ? 8 : 4;
    const uint32_t minimum = padded ? 2 : 1;
    const uint32_t bw = std::max(w / block_width, minimum);
    const uint32_t bh = std::max(h / 4, minimum);
    const uint32_t out_bw = std::max(bw, 2u);
    const uint32_t out_bh = std::max(bh, 2u);
    try {
        std::vector<uint32_t> words(size_t(out_bw) * out_bh * 2);
        for (uint32_t y = 0; y < out_bh; ++y) {
            for (uint32_t x = 0; x < out_bw; ++x) {
                const uint8_t *in = src + pvr::TwiddleUV(bw, bh, x % bw, y % bh) * 8;
                const uint32_t offset = pvr::TwiddleUV(out_bw, out_bh, x, y) * 2;
                for (uint32_t i = 0; i < 2; ++i, in += 4) {
                    words[offset + i] = uint32_t(in[0]) | uint32_t(in[1]) << 8 |
                                        uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24;
                }
            }
        }
        decompress(words.data(), w, h, bpp, dst);
        if (!alpha) {
            for (size_t i = 3; i < size_t(w) * h * 4; i += 4) {
                dst[i] = 255;
            }
        }
        return true;
    } catch (const std::bad_alloc &) {
        return false;
    }
}
