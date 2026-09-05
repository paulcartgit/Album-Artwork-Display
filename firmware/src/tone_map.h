#pragma once
#include <cstdint>
#include <cmath>
#include "colour.h"

// ═══════════════════════════════════════════════════════════
// Per-image lightness compression
//
// The panel only has chroma low down. Red, Blue and Green all sit near L*40;
// White is L*89 and nearly neutral. So a LIGHT colourful pixel has nothing to
// match against — at L*80 the only hue with any reachable chroma is yellow,
// and everything else there renders grey. Pastel artwork is mostly light and
// colourful, so it renders as a grey slab with occasional saturated flecks.
//
// Scaling L* down before dithering moves those pixels into the band where the
// pigments actually live, and the colour comes back. It costs lightness, so it
// is worth it only for images that are losing colour — Help! and Time Out get
// measurably WORSE when darkened, because they were already in gamut.
//
// The amount is chosen by TRYING it, not by predicting it. Three separate
// predictors were built and all three picked the wrong scale: chromatic share,
// gamut excess, and unreachable-chroma cost each said "leave it alone" for the
// sleeve that gains most. The panel takes 20-25s to refresh; a trial dither at
// half resolution costs a fraction of a second. So dither the candidates,
// score each against the source, and keep the winner. No model to be wrong.
// ═══════════════════════════════════════════════════════════

#define TONEMAP_SCALES      3
#define TONEMAP_TRIAL_DIV   2       // trial at half resolution in each axis
#define TONEMAP_HUE_WEIGHT  0.15f   // dE is in Lab units, hue in degrees

static constexpr float TONEMAP_SCALE[TONEMAP_SCALES] = { 1.00f, 0.90f, 0.80f };

// Scale L* while leaving a* and b* alone, so hue survives untouched.
inline void toneMapApply(uint8_t* rgb, int w, int h, float keep) {
    if (keep >= 0.999f) return;
    const int n = w * h;
    for (int i = 0; i < n; i++) {
        uint8_t* p = &rgb[i * 3];
        Lab c = rgbToLabF(p[0], p[1], p[2]);
        c.L *= keep;
        float r, g, b;
        labToRgbF(c, &r, &g, &b);
        p[0] = (uint8_t)(r + 0.5f);
        p[1] = (uint8_t)(g + 0.5f);
        p[2] = (uint8_t)(b + 0.5f);
    }
}

// Mean CIE76 distance and chroma-weighted hue error between two images,
// averaged over blocks at the scale the eye integrates a dither at.
//
// Blocks matter: compared pixel by pixel a dither always looks wrong, because
// each pixel is a pure pigment. The eye sees the local average, so that is
// what gets scored.
inline void toneMapScore(const uint8_t* src, const uint8_t* out, int w, int h,
                         int block, float* dE, float* hueErr) {
    double sumE = 0.0, sumH = 0.0, sumW = 0.0;
    int blocks = 0;
    for (int by = 0; by + block <= h; by += block) {
        for (int bx = 0; bx + block <= w; bx += block) {
            float sr = 0, sg = 0, sb = 0, orr = 0, og = 0, ob = 0;
            for (int y = by; y < by + block; y++) {
                for (int x = bx; x < bx + block; x++) {
                    const uint8_t* s = &src[(y * w + x) * 3];
                    const uint8_t* o = &out[(y * w + x) * 3];
                    sr += s[0]; sg += s[1]; sb += s[2];
                    orr += o[0]; og += o[1]; ob += o[2];
                }
            }
            const float inv = 1.0f / (block * block);
            Lab a = rgbToLabF(sr * inv, sg * inv, sb * inv);
            Lab c = rgbToLabF(orr * inv, og * inv, ob * inv);

            const float dL = a.L - c.L, da = a.a - c.a, db = a.b - c.b;
            sumE += sqrtf(dL * dL + da * da + db * db);
            blocks++;

            // Hue error, weighted by the SOURCE's chroma: a hue shift in a
            // near-grey block means nothing, one in saturated hair is the
            // whole complaint.
            const float ca = sqrtf(a.a * a.a + a.b * a.b);
            float ha = atan2f(a.b, a.a) * 57.29578f;
            float hc = atan2f(c.b, c.a) * 57.29578f;
            float dh = fmodf(fabsf(ha - hc), 360.0f);
            if (dh > 180.0f) dh = 360.0f - dh;
            sumH += dh * ca;
            sumW += ca;
        }
    }
    *dE     = blocks ? (float)(sumE / blocks) : 0.0f;
    *hueErr = (sumW > 1e-6) ? (float)(sumH / sumW) : 0.0f;
}

// Box-downscale by an integer factor, for the trial renders.
inline void toneMapShrink(const uint8_t* src, int w, int h, int div,
                          uint8_t* dst) {
    const int dw = w / div, dh = h / div;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int r = 0, g = 0, b = 0;
            for (int j = 0; j < div; j++)
                for (int i = 0; i < div; i++) {
                    const uint8_t* p = &src[((y * div + j) * w + (x * div + i)) * 3];
                    r += p[0]; g += p[1]; b += p[2];
                }
            const int m = div * div;
            uint8_t* q = &dst[(y * dw + x) * 3];
            q[0] = (uint8_t)(r / m); q[1] = (uint8_t)(g / m); q[2] = (uint8_t)(b / m);
        }
    }
}
