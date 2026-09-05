#include "dither.h"
#include "config.h"
#include <cmath>
#include <cstring>

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

struct Lab { float L, a, b; };

// Float-accepting Lab conversion (pixels carry accumulated error, so values
// can land outside [0,255] before clamping).
static Lab rgbToLabF(float r, float g, float b) {
    r = fmaxf(0.0f, fminf(255.0f, r)) / 255.0f;
    g = fmaxf(0.0f, fminf(255.0f, g)) / 255.0f;
    b = fmaxf(0.0f, fminf(255.0f, b)) / 255.0f;
    auto decode = [](float v) -> float {
        return (v <= 0.04045f) ? v / 12.92f
                               : powf((v + 0.055f) / 1.055f, 2.4f);
    };
    float lr = decode(r), lg = decode(g), lb = decode(b);

    // Linear sRGB → XYZ (D65 illuminant)
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

// ═══════════════════════════════════════════════════════════
// Extended matching palette: 6 real + 2 virtual entries
//
// Our 6 pigments are (roughly) 6 of the 8 RGB cube corners, missing Cyan and
// Magenta.  To render purple the panel MUST interleave Red and Blue pixels,
// but in any perceptual space Red sits ~89° from purple on the hue wheel, so
// plain error diffusion never alternates Red↔Blue on its own.
//
// Solution (caca.zoy.org §6.1–6.2): add *virtual* Magenta and Cyan to the
// matching palette, positioned at the midpoint of the two real pigments that
// will be interleaved.  Purple pixels then match virtual Magenta, which is
// emitted as a Red/Blue checkerboard the eye integrates back into purple.
//
// CRITICAL: every entry below is derived from the CALIBRATED pigment values in
// config.h — what the panel actually looks like — not from idealised RGB cube
// corners.  Matching against idealised corners tells the dither it just placed
// pure #00FF00 when the panel will show a dark teal, so every subsequent error
// term is wrong and the whole image drifts.
// ═══════════════════════════════════════════════════════════
static constexpr int MATCH_COLORS = EPD_COLORS + 2;
static constexpr uint8_t VIRTUAL_CYAN    = EPD_COLORS;     // 6 → Green + Blue
static constexpr uint8_t VIRTUAL_MAGENTA = EPD_COLORS + 1; // 7 → Red   + Blue

// The two real palette indices each virtual colour interleaves between.
static constexpr uint8_t VIRTUAL_PAIR[2][2] = {
    { 2, 3 },  // Cyan    → Green / Blue
    { 4, 3 },  // Magenta → Red   / Blue
};

// Matching palette in RGB, derived from PALETTE at startup.
static float MATCH_PAL[MATCH_COLORS][3];
static Lab   MATCH_PAL_LAB[MATCH_COLORS];
static float MATCH_PAL_CHROMA[MATCH_COLORS];
static bool  g_palReady = false;

static void ensureMatchPalette() {
    if (g_palReady) return;

    // Real pigments — straight from the calibrated table.
    for (int i = 0; i < EPD_COLORS; i++) {
        MATCH_PAL[i][0] = (float)PALETTE[i].r;
        MATCH_PAL[i][1] = (float)PALETTE[i].g;
        MATCH_PAL[i][2] = (float)PALETTE[i].b;
    }
    // Virtual colours — midpoint of the pigment pair they interleave.
    for (int v = 0; v < 2; v++) {
        const PaletteColor& a = PALETTE[VIRTUAL_PAIR[v][0]];
        const PaletteColor& b = PALETTE[VIRTUAL_PAIR[v][1]];
        MATCH_PAL[EPD_COLORS + v][0] = (a.r + b.r) * 0.5f;
        MATCH_PAL[EPD_COLORS + v][1] = (a.g + b.g) * 0.5f;
        MATCH_PAL[EPD_COLORS + v][2] = (a.b + b.b) * 0.5f;
    }

    for (int i = 0; i < MATCH_COLORS; i++) {
        MATCH_PAL_LAB[i] = rgbToLabF(MATCH_PAL[i][0], MATCH_PAL[i][1], MATCH_PAL[i][2]);
        MATCH_PAL_CHROMA[i] = sqrtf(MATCH_PAL_LAB[i].a * MATCH_PAL_LAB[i].a +
                                    MATCH_PAL_LAB[i].b * MATCH_PAL_LAB[i].b);
    }
    g_palReady = true;
}

// Chroma-aware penalty: when a pixel has significant chroma, penalise the
// achromatic palette entries (White, Black) so they don't absorb the colour
// signal.  Without this, light purple (L*≈57, C*≈48) always matches White
// because the lightness gap to White is smaller than the a* gap to Magenta.
static uint8_t nearestLab(float r, float g, float b, float penaltyK, float penaltyOnset) {
    Lab px = rgbToLabF(r, g, b);
    float pxChroma = sqrtf(px.a * px.a + px.b * px.b);
    float excess   = fmaxf(0.0f, pxChroma - penaltyOnset);
    float penalty  = excess * excess * penaltyK;

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

// ═══════════════════════════════════════════════════════════
// Lab-match + RGB-error dithering
//   • Nearest colour found in CIELAB (perceptual distance)
//   • Error computed & diffused in RGB (channel independence)
//   • Floyd-Steinberg kernel (4 neighbours, tight error spread)
//   • Serpentine scan (eliminates directional streak artefacts)
//   • Edge-aware: suppresses error diffusion across hard edges
// ═══════════════════════════════════════════════════════════

// Per-pixel edge strength map [0..1] where 1 = strong edge.
// Caller must free with heap_caps_free().
static float* buildEdgeMap(const uint8_t* rgb, int w, int h) {
    float* edge = (float*)heap_caps_malloc((size_t)w * h * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!edge) return nullptr;

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

void ditherFloydSteinberg(const uint8_t* rgb888, uint8_t* packedOut, int w, int h,
                          const RenderProfile& profile) {
    if (w <= 0 || h <= 0) return;
    ensureMatchPalette();

    const float edgeAtten = profile.edgeAttenuation;

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
        int xs = ltr ? 0 : w - 1;
        int xe = ltr ? w : -1;
        int xd = ltr ? 1 : -1;

        for (int x = xs; x != xe; x += xd) {
            int ri = x * 3;

            // Clamp to valid sRGB range [0, 255]
            float cr = fmaxf(0.0f, fminf(255.0f, row[0][ri]));
            float cg = fmaxf(0.0f, fminf(255.0f, row[0][ri + 1]));
            float cb = fmaxf(0.0f, fminf(255.0f, row[0][ri + 2]));

            // Match in CIELAB space against the calibrated palette
            uint8_t ci = nearestLab(cr, cg, cb,
                                    profile.chromaPenaltyK,
                                    profile.chromaPenaltyOnset);

            // Map virtual colours to alternating real pigments
            uint8_t displayIdx = ci;
            if (ci >= EPD_COLORS) {
                const uint8_t* pair = VIRTUAL_PAIR[ci - EPD_COLORS];
                displayIdx = ((x + y) & 1) ? pair[0] : pair[1];
            }

            // Error is measured against the pigment PHYSICALLY PLACED, not
            // against the virtual colour that was matched.  That is what makes
            // the interleave happen: after placing Red for a magenta pixel the
            // residual blue pushes the next pixel toward Blue, and vice versa.
            // (Diffusing against the virtual midpoint instead would under-
            // account for the error and wash the interleave out.)
            float er = cr - (float)PALETTE[displayIdx].r;
            float eg = cg - (float)PALETTE[displayIdx].g;
            float eb = cb - (float)PALETTE[displayIdx].b;

            // Shadow chroma suppression: in very dark regions humans can't
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

            // Edge-aware: attenuate error leaving a pixel that sits on an edge.
            // (Only applied here, at the source — attenuating again at each
            // target would discard the same energy twice and drift edges light.)
            if (edgeMap) {
                float atten = 1.0f - edgeMap[y * w + x] * edgeAtten;
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
    Serial.printf("[Dither] %s profile — %dx%d → %d bytes\n", profile.name, w, h, (w * h) / 2);
}
