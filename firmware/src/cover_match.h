#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>

// ═══════════════════════════════════════════════════════════
// Is this the same sleeve?
//
// The same album is pressed dozens of times and the covers do not render
// alike: across six sleeves of Help! the best scored about twice as well as
// the worst on this panel, and that difference is in the source image where
// no amount of dithering can reach it. So it is worth fetching the
// alternatives and keeping the one that renders best.
//
// The obvious way to do that is also wrong. Ranking every cover MusicBrainz
// returns purely on how well it renders would hang an obscure reissue on the
// wall in place of the famous sleeve, because obscure reissues are often
// flatter and flatter renders better. The feature has to be "the best SCAN of
// the right artwork", never "a different artwork that happens to suit the
// panel".
//
// So a candidate is only considered if it is recognisably the same picture as
// the artwork we already have. A 16x16 thumbnail, per-channel normalised so
// that a colour cast, a darker scan or a heavier print does not count against
// it — those are exactly the differences we are shopping for — compared by
// cosine similarity. Measured on real Cover Art Archive data:
//
//   four scans of the Help! semaphore sleeve   +1.00  +0.90  +0.83  +0.79
//   two other sleeves also titled Help!        +0.03  -0.17
//   four covers of Abbey Road                  +0.13  +0.02  +0.00  -0.04
//
// which is a wide gap to put a threshold in.
// ═══════════════════════════════════════════════════════════

#define COVER_SIG_N     16                       // thumbnail edge
#define COVER_SIG_LEN   (COVER_SIG_N * COVER_SIG_N * 3)
#define COVER_SIG_MIN   0.60f                    // below this, a different sleeve

// Build the fingerprint from an RGB888 image of any size.
inline void coverSignature(const uint8_t* rgb, int w, int h, float* sig) {
    if (w <= 0 || h <= 0) {
        for (int i = 0; i < COVER_SIG_LEN; i++) sig[i] = 0.0f;
        return;
    }
    // Box-average into the thumbnail.
    for (int i = 0; i < COVER_SIG_LEN; i++) sig[i] = 0.0f;
    int counts[COVER_SIG_N][COVER_SIG_N] = {{0}};
    for (int y = 0; y < h; y++) {
        int ty = y * COVER_SIG_N / h;
        if (ty >= COVER_SIG_N) ty = COVER_SIG_N - 1;
        for (int x = 0; x < w; x++) {
            int tx = x * COVER_SIG_N / w;
            if (tx >= COVER_SIG_N) tx = COVER_SIG_N - 1;
            const uint8_t* p = &rgb[((size_t)y * w + x) * 3];
            float* s = &sig[(ty * COVER_SIG_N + tx) * 3];
            s[0] += p[0]; s[1] += p[1]; s[2] += p[2];
            counts[ty][tx]++;
        }
    }
    for (int ty = 0; ty < COVER_SIG_N; ty++)
        for (int tx = 0; tx < COVER_SIG_N; tx++) {
            const int n = counts[ty][tx] ? counts[ty][tx] : 1;
            float* s = &sig[(ty * COVER_SIG_N + tx) * 3];
            s[0] /= n; s[1] /= n; s[2] /= n;
        }

    // Normalise each channel to zero mean and unit deviation. This is the
    // whole point: it discards overall brightness, contrast and colour cast,
    // which are precisely the things that differ between pressings and
    // precisely what we are trying to choose between. What survives is the
    // arrangement of the picture.
    for (int c = 0; c < 3; c++) {
        float mean = 0.0f;
        for (int i = 0; i < COVER_SIG_N * COVER_SIG_N; i++) mean += sig[i * 3 + c];
        mean /= (COVER_SIG_N * COVER_SIG_N);
        float var = 0.0f;
        for (int i = 0; i < COVER_SIG_N * COVER_SIG_N; i++) {
            const float d = sig[i * 3 + c] - mean;
            var += d * d;
        }
        const float sd = sqrtf(var / (COVER_SIG_N * COVER_SIG_N));
        const float inv = (sd > 1e-4f) ? 1.0f / sd : 1.0f;
        for (int i = 0; i < COVER_SIG_N * COVER_SIG_N; i++)
            sig[i * 3 + c] = (sig[i * 3 + c] - mean) * inv;
    }
}

// Cosine similarity. 1 is the same picture, around 0 is an unrelated one.
inline float coverSimilarity(const float* a, const float* b) {
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (int i = 0; i < COVER_SIG_LEN; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    const float den = sqrtf(na) * sqrtf(nb);
    return (den > 1e-9f) ? dot / den : 0.0f;
}

inline bool coverIsSameArtwork(const float* a, const float* b) {
    return coverSimilarity(a, b) >= COVER_SIG_MIN;
}
