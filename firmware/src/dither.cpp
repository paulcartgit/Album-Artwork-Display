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
// Palette helpers
// ═══════════════════════════════════════════════════════════

static bool g_palReady = false;

static void ensurePalette() {
    if (g_palReady) return;
    ensureLUT();
    g_palReady = true;
}

// ═══════════════════════════════════════════════════════════
// Extended matching palette: 6 real + 2 virtual RGB cube corners
//
// Our 6-colour palette is 6 of the 8 RGB cube corners, missing
// only Cyan(0,255,255) and Magenta(255,0,255).  For purple,
// the display MUST interleave Red and Blue pixels.  But in any
// perceptual colour space (Lab, YCbCr), Red is ~89° away from
// purple on the hue wheel — standard error diffusion can never
// naturally alternate Red↔Blue.
//
// Solution (caca.zoy.org §6.1–6.2, Reddit/Spectra-6 community):
// Add virtual Magenta + Cyan to the matching palette.  Purple
// pixels match Magenta, which gets mapped to alternating Red/Blue
// in a checkerboard — producing the interleaved pattern that the
// eye perceives as purple.
//
// Chroma-aware penalty prevents White/Black from absorbing
// chromatic pixels: if a pixel has significant chroma, achromatic
// palette entries get a distance penalty proportional to (chroma)².
// Without this, lavender(Lab L*=57, C*=48) is closer to White
// (L*=100) than to Magenta (a*=98) — White steals the purple.
// ═══════════════════════════════════════════════════════════
static constexpr int MATCH_COLORS = 8;

// Full matching palette (Lab matching searches all 8)
static const float MATCH_PAL[MATCH_COLORS][3] = {
    {   0.0f,   0.0f,   0.0f }, // 0  Black
    { 255.0f, 255.0f, 255.0f }, // 1  White
    {   0.0f, 255.0f,   0.0f }, // 2  Green
    {   0.0f,   0.0f, 255.0f }, // 3  Blue
    { 255.0f,   0.0f,   0.0f }, // 4  Red
    { 255.0f, 255.0f,   0.0f }, // 5  Yellow
    {   0.0f, 255.0f, 255.0f }, // 6  Cyan    (virtual)
    { 255.0f,   0.0f, 255.0f }, // 7  Magenta (virtual)
};

// Error reference: for real colours [0–5] = idealized value.
// For virtual colours [6–7] = average of the two alternating
// real colours.  This ensures correct total error accumulation
// (true error sums are identical to per-pixel true error sums)
// while avoiding the violent oscillation of per-pixel true error.
static const float ERROR_REF[MATCH_COLORS][3] = {
    {   0.0f,   0.0f,   0.0f }, // 0  Black
    { 255.0f, 255.0f, 255.0f }, // 1  White
    {   0.0f, 255.0f,   0.0f }, // 2  Green
    {   0.0f,   0.0f, 255.0f }, // 3  Blue
    { 255.0f,   0.0f,   0.0f }, // 4  Red
    { 255.0f, 255.0f,   0.0f }, // 5  Yellow
    {   0.0f, 127.5f, 127.5f }, // 6  Cyan    → avg(Green, Blue)
    { 127.5f,   0.0f, 127.5f }, // 7  Magenta → avg(Red, Blue)
};

// Float-accepting Lab conversion (pixels carry accumulated error)
static Lab rgbToLabF(float r, float g, float b) {
    // Clamp to [0,255] then gamma-decode
    r = fmaxf(0.0f, fminf(255.0f, r)) / 255.0f;
    g = fmaxf(0.0f, fminf(255.0f, g)) / 255.0f;
    b = fmaxf(0.0f, fminf(255.0f, b)) / 255.0f;
    auto decode = [](float v) -> float {
        return (v <= 0.04045f) ? v / 12.92f
                               : powf((v + 0.055f) / 1.055f, 2.4f);
    };
    float lr = decode(r), lg = decode(g), lb = decode(b);

    float x = lr * 0.4124564f + lg * 0.3575761f + lb * 0.1804375f;
    float y = lr * 0.2126729f + lg * 0.7151522f + lb * 0.0721750f;
    float z = lr * 0.0193339f + lg * 0.1191920f + lb * 0.9503041f;
    x /= 0.95047f;
    z /= 1.08883f;

    auto labf = [](float t) -> float {
        return (t > 0.008856f) ? cbrtf(t) : (7.787f * t + 16.0f / 116.0f);
    };
    float fx = labf(x), fy = labf(y), fz = labf(z);
    return { 116.0f * fy - 16.0f,
             500.0f * (fx - fy),
             200.0f * (fy - fz) };
}

// Pre-computed CIELAB values and chroma for matching palette
static Lab   MATCH_PAL_LAB[MATCH_COLORS];
static float MATCH_PAL_CHROMA[MATCH_COLORS];
static bool  g_labReady = false;

static void ensureLabPalette() {
    if (g_labReady) return;
    for (int i = 0; i < MATCH_COLORS; i++) {
        MATCH_PAL_LAB[i] = rgbToLabF(
            MATCH_PAL[i][0], MATCH_PAL[i][1], MATCH_PAL[i][2]);
        MATCH_PAL_CHROMA[i] = sqrtf(
            MATCH_PAL_LAB[i].a * MATCH_PAL_LAB[i].a +
            MATCH_PAL_LAB[i].b * MATCH_PAL_LAB[i].b);
    }
    g_labReady = true;
}

// Chroma-aware penalty: when pixel has significant chroma,
// penalize achromatic palette entries (White, Black) so they
// don't absorb the colour signal.  Without this, light purple
// (Lab L*=57, C*=48) always matches White (L*=100) because the
// lightness gap is smaller than the a* gap to Magenta.
static constexpr float CHROMA_PENALTY_K     = 5.0f;
static constexpr float CHROMA_PENALTY_ONSET = 12.0f;

static uint8_t nearestLab(float r, float g, float b) {
    Lab px = rgbToLabF(r, g, b);
    float pxChroma = sqrtf(px.a * px.a + px.b * px.b);
    float excess   = fmaxf(0.0f, pxChroma - CHROMA_PENALTY_ONSET);
    float penalty  = excess * excess * CHROMA_PENALTY_K;

    float best = 1e30f;
    uint8_t ci = 0;
    for (uint8_t i = 0; i < MATCH_COLORS; i++) {
        float dL = px.L - MATCH_PAL_LAB[i].L;
        float da = px.a - MATCH_PAL_LAB[i].a;
        float db = px.b - MATCH_PAL_LAB[i].b;
        float d  = dL * dL + da * da + db * db;
        if (MATCH_PAL_CHROMA[i] < 5.0f) d += penalty;
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
// Lab-match + RGB-error dithering  ("Lab errRGB")
//   • Nearest colour found in CIELAB (perceptual distance)
//   • Error computed & diffused in RGB (channel independence)
//   • Floyd-Steinberg kernel (4 neighbours, tight error spread)
//   • Serpentine scan (eliminates directional streak artefacts)
//   • Edge-aware: suppresses error diffusion across hard edges
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
    ensurePalette();
    ensureLabPalette();

    // Build edge map for edge-aware error diffusion
    float* edgeMap = buildEdgeMap(rgb888, w, h);

    // 2 rolling rows of RGB error accumulation (sRGB 0-255 scale)
    const size_t rowFloats = (size_t)w * 3;
    const size_t rowBytes  = rowFloats * sizeof(float);
    float* row[2];
    for (int i = 0; i < 2; i++) {
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
        // Load this row as sRGB (0-255), adding accumulated error
        for (int x = 0; x < w; x++) {
            int si = (y * w + x) * 3;
            int ri = x * 3;
            row[0][ri]     += (float)rgb888[si];
            row[0][ri + 1] += (float)rgb888[si + 1];
            row[0][ri + 2] += (float)rgb888[si + 2];
        }

        // Serpentine: even rows L→R, odd rows R→L
        bool ltr = (y & 1) == 0;
        int xs = ltr ? 0     : w - 1;
        int xe = ltr ? w     : -1;
        int xd = ltr ? 1     : -1;

        for (int x = xs; x != xe; x += xd) {
            int ri = x * 3;

            // Clamp to valid sRGB range [0, 255]
            float cr = fmaxf(0.0f, fminf(255.0f, row[0][ri]));
            float cg = fmaxf(0.0f, fminf(255.0f, row[0][ri + 1]));
            float cb = fmaxf(0.0f, fminf(255.0f, row[0][ri + 2]));

            // Match in CIELAB space (8-colour palette with chroma penalty)
            uint8_t ci = nearestLab(cr, cg, cb);

            // Map virtual colours to alternating real display colours
            uint8_t displayIdx = ci;
            if (ci == 6) {        // Cyan → alternate Green/Blue
                displayIdx = ((x + y) & 1) ? 2 : 3;
            } else if (ci == 7) { // Magenta → alternate Red/Blue
                displayIdx = ((x + y) & 1) ? 4 : 3;
            }

            // Error in RGB against the ACTUAL placed colour.
            // For virtual colours, error is computed vs the real colour
            // that was physically placed (Red or Blue for Magenta, etc).
            // This causes error to naturally oscillate: after placing Red,
            // the residual blue pushes the next pixel toward Blue/Magenta,
            // creating the interleaved pattern that reads as purple.
            float refR = MATCH_PAL[displayIdx][0];
            float refG = MATCH_PAL[displayIdx][1];
            float refB = MATCH_PAL[displayIdx][2];
            float er = cr - refR;
            float eg = cg - refG;
            float eb = cb - refB;

            // Shadow chroma suppression: in very dark regions, humans can't
            // perceive colour, but error diffusion accumulates chrominance
            // error across black pixels until it flips one to blue/red.
            float lum = 0.299f * cr + 0.587f * cg + 0.114f * cb;
            if (lum < 8.0f) {
                float chromaScale = lum / 8.0f;
                chromaScale *= chromaScale;
                float eLum = 0.299f * er + 0.587f * eg + 0.114f * eb;
                er = eLum + (er - eLum) * chromaScale;
                eg = eLum + (eg - eLum) * chromaScale;
                eb = eLum + (eb - eLum) * chromaScale;
            }

            // Edge-aware: attenuate error at source pixel when on/near an edge
            if (edgeMap) {
                float srcEdge = edgeMap[y * w + x];
                float atten = 1.0f - srcEdge * 0.85f;
                er *= atten;
                eg *= atten;
                eb *= atten;
            }

            // Floyd-Steinberg kernel (/16)
            //        *   7/16
            //  3/16  5/16  1/16
            #define FS(dx, dy, wt) do { \
                int nx = x + (ltr ? (dx) : -(dx)); \
                if (nx >= 0 && nx < w) { \
                    int ni = nx * 3; \
                    float f = (wt) / 16.0f; \
                    if (edgeMap && (y + (dy)) < h) { \
                        float tgtEdge = edgeMap[(y + (dy)) * w + nx]; \
                        f *= (1.0f - tgtEdge * 0.85f); \
                    } \
                    row[dy][ni]     += er * f; \
                    row[dy][ni + 1] += eg * f; \
                    row[dy][ni + 2] += eb * f; \
                } \
            } while(0)

            FS(1, 0, 7);
            if (y + 1 < h) { FS(-1,1,3); FS(0,1,5); FS(1,1,1); }

            #undef FS

            // Pack 4-bit colour index (displayIdx maps virtual→real)
            int pi = y * w + x;
            int bi = pi / 2;
            if (pi % 2 == 0)
                packedOut[bi] = (packedOut[bi] & 0x0F) | (displayIdx << 4);
            else
                packedOut[bi] = (packedOut[bi] & 0xF0) | (displayIdx & 0x0F);
        }

        // Roll rows: 0←1, 1←cleared
        float* tmp = row[0];
        row[0] = row[1];
        row[1] = tmp;
        memset(row[1], 0, rowBytes);
    }

    for (int i = 0; i < 2; i++) heap_caps_free(row[i]);
    if (edgeMap) heap_caps_free(edgeMap);
    Serial.printf("[Dither] Lab+chroma-penalty+virtual-Mag/Cyn F-S — %dx%d → %d bytes\n", w, h, (w * h) / 2);
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
