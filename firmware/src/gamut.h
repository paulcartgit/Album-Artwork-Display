#pragma once
#include <cstdint>
#include <cmath>
#include "colour.h"
#include "config.h"

// ═══════════════════════════════════════════════════════════
// Hue-preserving gamut mapping
//
// The dither matches each pixel to the nearest palette entry in CIELAB. When
// the pixel is outside anything the panel can reach, "nearest" gives away
// whatever is cheapest in Lab distance — and that is almost always HUE. The
// KPop Demon Hunters sleeve's skin, #E8A2C5, is a rose whose red channel is
// 233 where the panel's brightest pigment reaches 224. Nothing the panel can
// mix holds that hue, grey is genuinely the closest thing in Lab, and so the
// skin renders drab.
//
// This moves each out-of-gamut colour onto the gamut boundary along its own
// hue line, spending lightness and chroma and never hue. A pink that cannot
// be that pink becomes a darker, duller pink instead of a grey-brown.
//
// The reachable ceiling is strongly hue-dependent — at L*75 the panel can
// hold chroma 77 if the hue is yellow and 0 if it is purple — so it is a 2-D
// table over lightness and hue, built from the palette itself at startup. A
// coarse table measured slightly BETTER than a fine one as well as costing
// half as much: the interpolation it implies smooths the boundary, and an
// exact boundary is not what the eye is looking at.
//
// Measured over nine covers against the source, with lightness adaptation:
//   dither alone                     dE 11.9   hue 21.0 deg
//   + tone map                       dE  9.7   hue 13.3
//   + tone map + this                dE 10.3   hue  9.4
// ═══════════════════════════════════════════════════════════

#define GAMUT_NL 17          // lightness bins
#define GAMUT_NH 32          // hue bins
#define GAMUT_LIGHTNESS_WEIGHT 0.6f   // how readily L* is spent to hold hue

static float g_gamutCeiling[GAMUT_NL][GAMUT_NH];
static bool  g_gamutReady = false;

// Highest chroma the panel can average to, per lightness and hue.
inline void gamutBuild() {
    if (g_gamutReady) return;
    for (int i = 0; i < GAMUT_NL; i++)
        for (int j = 0; j < GAMUT_NH; j++) g_gamutCeiling[i][j] = 0.0f;

    // Every eighths mix of the six pigments. A dither can average to any of
    // these, so together they are the reachable set.
    for (int a = 0; a <= 8; a++)
    for (int b = 0; a + b <= 8; b++)
    for (int c = 0; a + b + c <= 8; c++)
    for (int d = 0; a + b + c + d <= 8; d++)
    for (int e = 0; a + b + c + d + e <= 8; e++) {
        const int wht = 8 - a - b - c - d - e;
        const float r = (a * PALETTE[0].r + b * PALETTE[2].r + c * PALETTE[3].r +
                         d * PALETTE[4].r + e * PALETTE[5].r + wht * PALETTE[1].r) / 8.0f;
        const float g = (a * PALETTE[0].g + b * PALETTE[2].g + c * PALETTE[3].g +
                         d * PALETTE[4].g + e * PALETTE[5].g + wht * PALETTE[1].g) / 8.0f;
        const float bl= (a * PALETTE[0].b + b * PALETTE[2].b + c * PALETTE[3].b +
                         d * PALETTE[4].b + e * PALETTE[5].b + wht * PALETTE[1].b) / 8.0f;
        const Lab lab = rgbToLabF(r, g, bl);
        const float chroma = sqrtf(lab.a * lab.a + lab.b * lab.b);
        int li = (int)(lab.L / 100.0f * (GAMUT_NL - 1) + 0.5f);
        if (li < 0) li = 0;
        if (li > GAMUT_NL - 1) li = GAMUT_NL - 1;
        float deg = atan2f(lab.b, lab.a) * 57.29578f;
        if (deg < 0) deg += 360.0f;
        int hi = (int)(deg / 360.0f * GAMUT_NH) % GAMUT_NH;
        if (chroma > g_gamutCeiling[li][hi]) g_gamutCeiling[li][hi] = chroma;
    }

    // A hue the panel cannot hold at one lightness is still worth aiming at
    // from its neighbours, so soften across hue rather than leaving holes for
    // pixels to fall into.
    for (int i = 0; i < GAMUT_NL; i++) {
        float row[GAMUT_NH];
        for (int j = 0; j < GAMUT_NH; j++) row[j] = g_gamutCeiling[i][j];
        for (int j = 0; j < GAMUT_NH; j++) {
            const float n = 0.5f * (row[(j + GAMUT_NH - 1) % GAMUT_NH] +
                                    row[(j + 1) % GAMUT_NH]);
            if (n > g_gamutCeiling[i][j]) g_gamutCeiling[i][j] = n;
        }
    }
    g_gamutReady = true;
}

inline void gamutMapApply(uint8_t* rgb, int w, int h,
                          float lightnessWeight = GAMUT_LIGHTNESS_WEIGHT) {
    gamutBuild();
    const int n = w * h;
    for (int i = 0; i < n; i++) {
        uint8_t* p = &rgb[i * 3];
        const Lab lab = rgbToLabF(p[0], p[1], p[2]);
        const float chroma = sqrtf(lab.a * lab.a + lab.b * lab.b);
        if (chroma < 1.0f) continue;                   // neutral: nothing to hold

        float deg = atan2f(lab.b, lab.a) * 57.29578f;
        if (deg < 0) deg += 360.0f;
        const int hi = (int)(deg / 360.0f * GAMUT_NH) % GAMUT_NH;

        int li = (int)(lab.L / 100.0f * (GAMUT_NL - 1) + 0.5f);
        if (li < 0) li = 0;
        if (li > GAMUT_NL - 1) li = GAMUT_NL - 1;
        if (chroma <= g_gamutCeiling[li][hi]) continue; // already reachable

        // Cheapest point on this hue's boundary: how far the lightness has to
        // move, against how much chroma survives once it is there.
        float bestCost = 1e30f, bestL = lab.L, bestC = chroma;
        for (int k = 0; k < GAMUT_NL; k++) {
            const float L = 100.0f * k / (GAMUT_NL - 1);
            const float reach = g_gamutCeiling[k][hi];
            const float c = (chroma < reach) ? chroma : reach;
            const float dL = lightnessWeight * (L - lab.L);
            const float dC = c - chroma;
            const float cost = dL * dL + dC * dC;
            if (cost < bestCost) { bestCost = cost; bestL = L; bestC = c; }
        }

        const float scale = bestC / chroma;
        Lab out = { bestL, lab.a * scale, lab.b * scale };
        float r, g, b;
        labToRgbF(out, &r, &g, &b);
        p[0] = (uint8_t)(r + 0.5f);
        p[1] = (uint8_t)(g + 0.5f);
        p[2] = (uint8_t)(b + 0.5f);
    }
}
