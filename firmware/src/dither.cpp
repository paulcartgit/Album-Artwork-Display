#include "dither.h"
#include "config.h"
#include <cmath>

#ifdef NATIVE_TEST
#include <cstdlib>
#include <cstdio>
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_free(ptr) free(ptr)
struct FakeSerial { template<typename... Args> void println(Args...) {} template<typename... Args> void printf(Args...) {} } Serial;
#else
#include <esp_heap_caps.h>
#endif

// ═══════════════════════════════════════════════════════════
// sRGB → CIELAB colour-space pipeline
// ═══════════════════════════════════════════════════════════

// Precomputed sRGB → linear LUT (gamma decode)
static float g_srgbLUT[256];
static bool  g_lutReady = false;

static void ensureLUT() {
    if (g_lutReady) return;
    for (int i = 0; i < 256; i++) {
        float v = i / 255.0f;
        g_srgbLUT[i] = (v <= 0.04045f)
            ? v / 12.92f
            : powf((v + 0.055f) / 1.055f, 2.4f);
    }
    g_lutReady = true;
}

struct Lab { float L, a, b; };

static Lab rgbToLab(uint8_t R, uint8_t G, uint8_t B) {
    float lr = g_srgbLUT[R], lg = g_srgbLUT[G], lb = g_srgbLUT[B];

    // Linear sRGB → XYZ (D65 illuminant)
    float x = lr * 0.4124564f + lg * 0.3575761f + lb * 0.1804375f;
    float y = lr * 0.2126729f + lg * 0.7151522f + lb * 0.0721750f;
    float z = lr * 0.0193339f + lg * 0.1191920f + lb * 0.9503041f;

    // Normalise to D65 white point
    x /= 0.95047f;
    z /= 1.08883f;

    // XYZ → Lab
    auto labf = [](float t) -> float {
        return (t > 0.008856f) ? cbrtf(t) : (7.787f * t + 16.0f / 116.0f);
    };
    float fx = labf(x), fy = labf(y), fz = labf(z);

    return { 116.0f * fy - 16.0f,
             500.0f * (fx - fy),
             200.0f * (fy - fz) };
}

// ═══════════════════════════════════════════════════════════
// Palette in Lab space (computed once)
// ═══════════════════════════════════════════════════════════

static Lab  g_palLab[EPD_COLORS];
static bool g_palReady = false;

static void ensurePaletteLab() {
    if (g_palReady) return;
    ensureLUT();
    for (int i = 0; i < EPD_COLORS; i++)
        g_palLab[i] = rgbToLab(PALETTE[i].r, PALETTE[i].g, PALETTE[i].b);
    g_palReady = true;
}

// Nearest palette colour — Euclidean distance in CIELAB
static uint8_t nearestLab(float L, float a, float b) {
    float best = 1e30f;
    uint8_t ci = 0;
    for (uint8_t i = 0; i < EPD_COLORS; i++) {
        float dL = L - g_palLab[i].L;
        float da = a - g_palLab[i].a;
        float db = b - g_palLab[i].b;
        float d  = dL * dL + da * da + db * db;
        if (d < best) { best = d; ci = i; }
    }
    return ci;
}

// Nearest palette colour — RGB (for quantizeNearest fallback)
static uint8_t nearestPaletteColor(int16_t r, int16_t g, int16_t b) {
    uint32_t minDist = UINT32_MAX;
    uint8_t best = 0;
    for (uint8_t i = 0; i < EPD_COLORS; i++) {
        int16_t dr = r - PALETTE[i].r;
        int16_t dg = g - PALETTE[i].g;
        int16_t db = b - PALETTE[i].b;
        uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist < minDist) { minDist = dist; best = i; }
    }
    return best;
}

// ═══════════════════════════════════════════════════════════
// Perceptual dithering
//   • sRGB → CIELAB (perceptually uniform error diffusion)
//   • Stucki kernel (12 neighbours, 3 rows — smoother than F-S)
//   • Serpentine scan (eliminates directional streak artefacts)
//   • Edge-aware: suppresses error diffusion across hard edges
//   • 3-row rolling buffer (~17 KB for w=480)
// ═══════════════════════════════════════════════════════════

// Compute per-pixel edge strength map from source RGB.
// Returns a heap-allocated array of floats [0..1] where 1 = strong edge.
// Caller must free with heap_caps_free().
static float* buildEdgeMap(const uint8_t* rgb, int w, int h) {
    float* edge = (float*)heap_caps_malloc((size_t)w * h * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!edge) return nullptr;

    // Gradient magnitude using luminance (Sobel-like, simplified to central differences)
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            auto lum = [&](int px, int py) -> float {
                px = (px < 0) ? 0 : ((px >= w) ? w - 1 : px);
                py = (py < 0) ? 0 : ((py >= h) ? h - 1 : py);
                int i = (py * w + px) * 3;
                return 0.299f * rgb[i] + 0.587f * rgb[i+1] + 0.114f * rgb[i+2];
            };
            float gx = lum(x+1, y) - lum(x-1, y);
            float gy = lum(x, y+1) - lum(x, y-1);
            float mag = sqrtf(gx * gx + gy * gy);
            // Normalise: gradient of ~150+ = full edge (high threshold to
            // avoid treating photographic texture/grain as edges)
            float e = mag / 150.0f;
            edge[y * w + x] = (e > 1.0f) ? 1.0f : e;
        }
    }
    return edge;
}

void ditherFloydSteinberg(const uint8_t* rgb888, uint8_t* packedOut, int w, int h) {
    ensurePaletteLab();

    // Build edge map for edge-aware error diffusion
    float* edgeMap = buildEdgeMap(rgb888, w, h);
    // If alloc fails, proceed without edge awareness (graceful degradation)

    // 3 rolling rows of Lab error accumulation
    const size_t rowFloats = (size_t)w * 3;
    const size_t rowBytes  = rowFloats * sizeof(float);
    float* row[3];
    for (int i = 0; i < 3; i++) {
        row[i] = (float*)heap_caps_malloc(rowBytes, MALLOC_CAP_SPIRAM);
        if (!row[i]) {
            Serial.println("[Dither] row alloc failed");
            for (int j = 0; j < i; j++) heap_caps_free(row[j]);
            if (edgeMap) heap_caps_free(edgeMap);
            return;
        }
        memset(row[i], 0, rowBytes);
    }

    for (int y = 0; y < h; y++) {
        // Convert this row from sRGB → Lab, adding accumulated error
        for (int x = 0; x < w; x++) {
            int si = (y * w + x) * 3;
            Lab c  = rgbToLab(rgb888[si], rgb888[si + 1], rgb888[si + 2]);
            int ri = x * 3;
            row[0][ri]     += c.L;
            row[0][ri + 1] += c.a;
            row[0][ri + 2] += c.b;
        }

        // Serpentine: even rows L→R, odd rows R→L
        bool ltr = (y & 1) == 0;
        int xs = ltr ? 0     : w - 1;
        int xe = ltr ? w     : -1;
        int xd = ltr ? 1     : -1;

        for (int x = xs; x != xe; x += xd) {
            int ri = x * 3;

            // Clamp to valid Lab range
            float L = fmaxf(0.0f,    fminf(100.0f, row[0][ri]));
            float a = fmaxf(-128.0f, fminf(127.0f, row[0][ri + 1]));
            float b = fmaxf(-128.0f, fminf(127.0f, row[0][ri + 2]));

            uint8_t ci = nearestLab(L, a, b);

            // Quantisation error in Lab space
            float eL = L - g_palLab[ci].L;
            float ea = a - g_palLab[ci].a;
            float eb = b - g_palLab[ci].b;

            // Boost error for poor palette matches on HIGH-CHROMA pixels only
            // (e.g. purple → blue has large residual toward red). Low-chroma
            // colours like brown/skin must NOT be boosted or they shift toward red.
            float chroma = sqrtf(a * a + b * b);
            float dist2 = eL * eL + ea * ea + eb * eb;
            if (dist2 > 400.0f && chroma > 40.0f) {
                float boost = 1.0f + 0.25f * fminf(sqrtf(dist2) / 50.0f, 1.0f);
                eL *= boost;
                ea *= boost;
                eb *= boost;
            }

            // Edge-aware: attenuate error at source pixel when on/near an edge.
            // This prevents error from bleeding across sharp boundaries.
            if (edgeMap) {
                float srcEdge = edgeMap[y * w + x];
                float atten = 1.0f - srcEdge * 0.85f;  // up to 85% reduction at hard edges
                eL *= atten;
                ea *= atten;
                eb *= atten;
            }

            // Stucki kernel (/42) — dx mirrored for R→L scan
            // Edge-aware: also attenuate by target pixel's edge strength
            //            *    8/42  4/42
            //  2/42  4/42  8/42  4/42  2/42
            //  1/42  2/42  4/42  2/42  1/42
            #define SP(dx, dy, wt) do { \
                int nx = x + (ltr ? (dx) : -(dx)); \
                if (nx >= 0 && nx < w) { \
                    int ni = nx * 3; \
                    float f = (wt) / 42.0f; \
                    if (edgeMap && (y + (dy)) < h) { \
                        float tgtEdge = edgeMap[(y + (dy)) * w + nx]; \
                        f *= (1.0f - tgtEdge * 0.85f); \
                    } \
                    row[dy][ni]     += eL * f; \
                    row[dy][ni + 1] += ea * f; \
                    row[dy][ni + 2] += eb * f; \
                } \
            } while(0)

            SP(1, 0, 8); SP(2, 0, 4);
            if (y + 1 < h) { SP(-2,1,2); SP(-1,1,4); SP(0,1,8); SP(1,1,4); SP(2,1,2); }
            if (y + 2 < h) { SP(-2,2,1); SP(-1,2,2); SP(0,2,4); SP(1,2,2); SP(2,2,1); }

            #undef SP

            // Pack 4-bit colour index (preserve other nibble for serpentine)
            int pi = y * w + x;
            int bi = pi / 2;
            if (pi % 2 == 0)
                packedOut[bi] = (packedOut[bi] & 0x0F) | (ci << 4);
            else
                packedOut[bi] = (packedOut[bi] & 0xF0) | (ci & 0x0F);
        }

        // Roll rows: 0←1, 1←2, 2←cleared
        float* tmp = row[0];
        row[0] = row[1];
        row[1] = row[2];
        row[2] = tmp;
        memset(row[2], 0, rowBytes);
    }

    for (int i = 0; i < 3; i++) heap_caps_free(row[i]);
    if (edgeMap) heap_caps_free(edgeMap);
    Serial.printf("[Dither] Lab+Stucki serpentine edge-aware — %dx%d → %d bytes\n", w, h, (w * h) / 2);
}

void quantizeNearest(const uint8_t* rgb888, uint8_t* packedOut, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 3;
            uint8_t ci = nearestPaletteColor(rgb888[idx], rgb888[idx + 1], rgb888[idx + 2]);

            int pixelIdx = y * w + x;
            int byteIdx  = pixelIdx / 2;
            if (pixelIdx % 2 == 0)
                packedOut[byteIdx] = (ci << 4);
            else
                packedOut[byteIdx] |= (ci & 0x0F);
        }
    }
    Serial.printf("[Quantize] Done — %dx%d → %d bytes (no dither)\n", w, h, (w * h) / 2);
}
