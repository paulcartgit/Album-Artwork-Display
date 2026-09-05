#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

// ─── How far to enlarge square artwork on a portrait panel ───
//
// Pure functions, kept out of image_pipeline.cpp so they can be tested without
// a display, a decoder or a device. The one bug that reached the panel in this
// area — a percentile indexed from the wrong end, which sliced "THE BEATLES"
// in half — was exactly the kind a unit test catches in milliseconds.

// Enlarging beyond this covers the panel outright (panelH / panelW).
// Cropping a square to 480x800 discards 40% of its width.
#define FILL_MAX_ZOOM   1.6667f

// Peak detail permitted along the crop lines, relative to the sleeve overall.
// Calibrated against real covers at the resolution this actually runs on:
// sleeves with type across them score far above it, photographic sleeves far
// below. See test_fill_policy in the native tests.
#define FILL_CUT_LIMIT  7.0f

// Longest edge the severity scan works at. The measurement compares a local
// peak against a global mean, and both move with resolution — sampling a
// 1400px sleeve natively gives sharper peaks than a 600px one and reads as
// more severe for identical artwork. Normalising the scan to a fixed size
// makes the number mean the same thing whatever the source.
#define FILL_SCAN_SIZE  400

// Peak detail lying along the two vertical crop lines at a given zoom,
// relative to the image overall.
//
// Sensitivity: the 96th percentile over ~128 sampled rows detects anything
// occupying roughly 4% of the height or more. Type on album sleeves is
// comfortably above that; a single hairline rule is not, and will be cropped
// through. That is a deliberate trade — a lower percentile starts treating
// ordinary photographic texture as type and refuses to fill anything.
//
// Averaging along the cut was tried first and is wrong: a band of type is a
// small fraction of the height, so it washes out and sleeves that cropping
// visibly ruins score as safe. A high percentile asks the right question —
// is there ANY row where the cut passes through something strong.
//
// rgb is w*h*3 bytes. Sampling is strided, so passing the full-resolution
// decode is fine.
inline float fillCutSeverity(const uint8_t* rgb, int w, int h, float zoom) {
    if (zoom <= 1.0f || w < 16 || h < 16) return 0.0f;

    // Normalise the scan geometry so the result does not depend on how large
    // the source happens to be.
    int xStep = (w > FILL_SCAN_SIZE) ? (w / FILL_SCAN_SIZE) : 1;
    int scanW = w / xStep;
    int keep  = (int)(scanW / zoom + 0.5f);
    int x0    = (scanW - keep) / 2;
    int x1    = x0 + keep;
    if (x0 < 2 || x1 >= scanW - 2) return 0.0f;

    // Round the row stride UP, so the sample always spans the full height.
    // Rounding down means a height between MAX_ROWS and 2*MAX_ROWS gives a
    // stride of 1, and the scan stops at row 128 having never looked at the
    // bottom of the sleeve — type down there would be invisible to it.
    const int MAX_ROWS = 128;
    int yStep = (h + MAX_ROWS - 1) / MAX_ROWS;
    if (yStep < 1) yStep = 1;
    const int band = 2;

    float peaks[MAX_ROWS];
    int n = 0;
    double total = 0.0;
    long totalN = 0;

    for (int y = 0; y < h && n < MAX_ROWS; y += yStep) {
        const uint8_t* row = &rgb[(size_t)y * w * 3];
        float prev = 0.0f;
        float peak = 0.0f;
        for (int sx = 0; sx < scanW; sx++) {
            const uint8_t* p = &row[(size_t)(sx * xStep) * 3];
            float lum = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
            if (sx > 0) {
                float g = fabsf(lum - prev);
                total += g; totalN++;
                if ((sx >= x0 - band && sx <= x0 + band) ||
                    (sx >= x1 - band && sx <= x1 + band)) {
                    if (g > peak) peak = g;
                }
            }
            prev = lum;
        }
        peaks[n++] = peak;
    }
    if (n == 0 || totalN == 0) return 0.0f;

    // 96th percentile. The partial selection below orders DESCENDING, so the
    // 96th percentile sits near the FRONT — indexing at 0.96*n returns a
    // near-minimum instead, which is precisely the bug that shipped.
    int idx = (int)((n - 1) * 0.04f);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    for (int i = 0; i <= idx; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) if (peaks[j] > peaks[best]) best = j;
        float t = peaks[i]; peaks[i] = peaks[best]; peaks[best] = t;
    }

    float mean = (float)(total / totalN);
    return peaks[idx] / (mean > 0.001f ? mean : 0.001f);
}

// Walk the zoom up and stop before the crop lines start cutting the sleeve's
// own detail. A photographic sleeve reaches a full bleed; a sleeve with the
// artist's name across it stops at 1.0 and keeps its type.
inline float fillAdaptiveZoom(const uint8_t* rgb, int w, int h) {
    static const float STEPS[] = {1.0f, 1.1f, 1.2f, 1.3f, 1.45f, FILL_MAX_ZOOM};
    float best = 1.0f;
    for (int i = 1; i < 6; i++) {
        if (fillCutSeverity(rgb, w, h, STEPS[i]) < FILL_CUT_LIMIT) best = STEPS[i];
        else break;
    }
    return best;
}
