#include "dither.h"
#include "config.h"

#ifdef NATIVE_TEST
#include <cstdlib>
#include <cstdio>
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_free(ptr) free(ptr)
struct FakeSerial { template<typename... Args> void println(Args...) {} template<typename... Args> void printf(Args...) {} } Serial;
#else
#include <esp_heap_caps.h>
#endif

static uint8_t nearestPaletteColor(int16_t r, int16_t g, int16_t b) {
    uint32_t minDist = UINT32_MAX;
    uint8_t best = 0;
    for (uint8_t i = 0; i < EPD_COLORS; i++) {
        int16_t dr = r - PALETTE[i].r;
        int16_t dg = g - PALETTE[i].g;
        int16_t db = b - PALETTE[i].b;
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist < minDist) {
            minDist = dist;
            best = i;
        }
    }
    return best;
}

void ditherFloydSteinberg(const uint8_t* rgb888, uint8_t* packedOut, int w, int h) {
    // Allocate int16 working buffer in PSRAM for error diffusion
    size_t bufSize = (size_t)w * h * 3 * sizeof(int16_t);
    int16_t* buf = (int16_t*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM);
    if (!buf) {
        Serial.println("[Dither] PSRAM alloc failed");
        return;
    }

    // Copy RGB888 input into int16 buffer
    for (int i = 0; i < w * h * 3; i++) {
        buf[i] = rgb888[i];
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 3;

            // Clamp current pixel
            int16_t r = buf[idx];
            int16_t g = buf[idx + 1];
            int16_t b = buf[idx + 2];
            r = (r < 0) ? 0 : (r > 255 ? 255 : r);
            g = (g < 0) ? 0 : (g > 255 ? 255 : g);
            b = (b < 0) ? 0 : (b > 255 ? 255 : b);

            // Find nearest palette colour
            uint8_t ci = nearestPaletteColor(r, g, b);

            // Quantisation error
            int16_t er = r - PALETTE[ci].r;
            int16_t eg = g - PALETTE[ci].g;
            int16_t eb = b - PALETTE[ci].b;

            // Distribute error to neighbours
            #define SPREAD(dx, dy, frac) do { \
                int nx = x + (dx), ny = y + (dy); \
                if (nx >= 0 && nx < w && ny >= 0 && ny < h) { \
                    int ni = (ny * w + nx) * 3; \
                    buf[ni]     += (int16_t)(er * (frac) / 16); \
                    buf[ni + 1] += (int16_t)(eg * (frac) / 16); \
                    buf[ni + 2] += (int16_t)(eb * (frac) / 16); \
                } \
            } while (0)

            SPREAD( 1, 0, 7);   // right
            SPREAD(-1, 1, 3);   // below-left
            SPREAD( 0, 1, 5);   // below
            SPREAD( 1, 1, 1);   // below-right

            #undef SPREAD

            // Pack 4-bit colour index: high nibble = even pixel, low nibble = odd pixel
            int pixelIdx = y * w + x;
            int byteIdx  = pixelIdx / 2;
            if (pixelIdx % 2 == 0) {
                packedOut[byteIdx] = (ci << 4);
            } else {
                packedOut[byteIdx] |= (ci & 0x0F);
            }
        }
    }

    heap_caps_free(buf);
    Serial.printf("[Dither] Done — %dx%d → %d bytes packed\n", w, h, (w * h) / 2);
}
