#include "image_pipeline.h"
#include "config.h"
#include "dither.h"
#include "tone_map.h"
#include "gamut.h"
#include "cover_match.h"
#include "cover_variants.h"

// Last pre-dither canvas, downsampled, so the simulator can be compared against
// what the device actually fed the dither rather than against a guess at it.
// Diagnosing a parity gap by elimination cost an evening; this answers it.
static uint8_t* g_canvasProbe = nullptr;
#define CANVAS_PROBE_DIV 4
const uint8_t* pipelineCanvasProbe() { return g_canvasProbe; }
size_t pipelineCanvasProbeSize() {
    return g_canvasProbe ? (size_t)(EPD_WIDTH / CANVAS_PROBE_DIV) *
                           (EPD_HEIGHT / CANVAS_PROBE_DIV) * 3 : 0;
}
#include "display.h"
#include "sd_manager.h"
#include "activity_log.h"
#include "fill_policy.h"
#include "app.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <TJpg_Decoder.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans18pt7b.h>

static const size_t JPEG_INITIAL_ALLOC = 64 * 1024;
static const size_t JPEG_MAX_DOWNLOAD  = 2 * 1024 * 1024;

// ─── TJpg_Decoder callback state ───
static uint8_t* g_decodeBuf = nullptr;
static int g_decodeW = 0;
static int g_decodeH = 0;

static bool tjpgCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!g_decodeBuf) return false;
    for (uint16_t dy = 0; dy < h; dy++) {
        for (uint16_t dx = 0; dx < w; dx++) {
            int dstX = x + dx;
            int dstY = y + dy;
            if (dstX >= g_decodeW || dstY >= g_decodeH) continue;

            uint16_t rgb565 = bitmap[dy * w + dx];
            int dstIdx = (dstY * g_decodeW + dstX) * 3;

            // RGB565 → RGB888
            g_decodeBuf[dstIdx]     = ((rgb565 >> 8) & 0xF8) | ((rgb565 >> 13) & 0x07);
            g_decodeBuf[dstIdx + 1] = ((rgb565 >> 3) & 0xFC) | ((rgb565 >> 9)  & 0x03);
            g_decodeBuf[dstIdx + 2] = ((rgb565 << 3) & 0xF8) | ((rgb565 >> 2)  & 0x07);
        }
    }
    return true;
}

// ─── Compute background fill color from image edges ───
// Saturation-weighted average: vibrant edge pixels dominate over dull/grey ones,
// preventing the common "muddy brown" result from simple averaging.
// A mild saturation boost afterward pushes the result toward the dominant hue.
static void averageEdgeColor(const uint8_t* src, int w, int h, uint8_t& rOut, uint8_t& gOut, uint8_t& bOut) {
    float rSum = 0, gSum = 0, bSum = 0, wSum = 0;

    auto addPixel = [&](int i) {
        uint8_t r = src[i], g = src[i+1], b = src[i+2];
        uint8_t mx = max(max(r, g), b);
        uint8_t mn = min(min(r, g), b);
        // HSV saturation (0..1), squared to strongly prefer vibrant pixels
        float sat = (mx > 0) ? (float)(mx - mn) / mx : 0.0f;
        float weight = 0.1f + sat * sat;
        rSum += r * weight;
        gSum += g * weight;
        bSum += b * weight;
        wSum += weight;
    };

    for (int x = 0; x < w; x++) {
        addPixel(x * 3);                     // top row
        addPixel(((h-1) * w + x) * 3);       // bottom row
    }
    for (int y = 1; y < h - 1; y++) {
        addPixel((y * w) * 3);               // left column
        addPixel((y * w + w - 1) * 3);       // right column
    }

    float r = rSum / wSum;
    float g = gSum / wSum;
    float b = bSum / wSum;

    // Boost saturation ~30% to push away from grey toward dominant hue
    float gray = 0.299f * r + 0.587f * g + 0.114f * b;
    r = gray + (r - gray) * 1.3f;
    g = gray + (g - gray) * 1.3f;
    b = gray + (b - gray) * 1.3f;

    rOut = constrain((int)(r + 0.5f), 0, 255);
    gOut = constrain((int)(g + 0.5f), 0, 255);
    bOut = constrain((int)(b + 0.5f), 0, 255);
}

// ─── Measure how "busy" the image edges are ───
// Samples the outermost pixel border of the image.  If the edge is uniform
// (low variance) a solid fill of that colour will look clean.  If it's
// varied / photographic we need the blur treatment.
static float edgeVariance(const uint8_t* src, int w, int h) {
    float rSum = 0, gSum = 0, bSum = 0;
    float r2Sum = 0, g2Sum = 0, b2Sum = 0;
    int count = 0;

    auto addPx = [&](int i) {
        float r = src[i], g = src[i+1], b = src[i+2];
        rSum += r; gSum += g; bSum += b;
        r2Sum += r*r; g2Sum += g*g; b2Sum += b*b;
        count++;
    };

    for (int x = 0; x < w; x++) {
        addPx(x * 3);                    // top row
        addPx(((h-1) * w + x) * 3);     // bottom row
    }
    for (int y = 1; y < h - 1; y++) {
        addPx((y * w) * 3);             // left col
        addPx((y * w + w - 1) * 3);     // right col
    }

    if (count == 0) return 0;
    float n = (float)count;
    float varR = r2Sum / n - (rSum / n) * (rSum / n);
    float varG = g2Sum / n - (gSum / n) * (gSum / n);
    float varB = b2Sum / n - (bSum / n) * (bSum / n);
    return (varR + varG + varB) / 3.0f;
}

// ─── Shared text fitting ───
// Both text renderers need the same behaviour: try the requested scale, step
// down while the string overflows the band, then truncate with an ellipsis.
// Returns the chosen scale and leaves `str` holding the text to draw.
static int fitTextToWidth(GFXcanvas1& canvas, const GFXfont* font,
                          String& str, int initScale, int minScale, int maxWidth) {
    int16_t x1, y1; uint16_t tw, th;
    canvas.setFont(font);

    int scale = initScale;
    canvas.setTextSize(scale);
    canvas.getTextBounds(str.c_str(), 0, 0, &x1, &y1, &tw, &th);
    while (tw > (uint16_t)maxWidth && scale > minScale) {
        scale--;
        canvas.setTextSize(scale);
        canvas.getTextBounds(str.c_str(), 0, 0, &x1, &y1, &tw, &th);
    }
    while (tw > (uint16_t)maxWidth && str.length() > 4) {
        str = str.substring(0, str.length() - 2);
        String test = str + "...";
        canvas.getTextBounds(test.c_str(), 0, 0, &x1, &y1, &tw, &th);
        if (tw <= (uint16_t)maxWidth) { str = test; break; }
    }
    return scale;
}

// Average brightness of a band of the RGB canvas, used to pick a text colour
// that contrasts with whatever is actually behind it.
static int bandBrightness(const uint8_t* rgb, int canvasW, int textAreaY, int textAreaH) {
    long rSum = 0, gSum = 0, bSum = 0;
    int samples = 0;
    const int step = 4; // sample every 4th pixel for speed
    for (int y = 0; y < textAreaH; y += step) {
        for (int x = 0; x < canvasW; x += step) {
            int di = ((textAreaY + y) * canvasW + x) * 3;
            rSum += rgb[di]; gSum += rgb[di + 1]; bSum += rgb[di + 2];
            samples++;
        }
    }
    if (samples == 0) return 255;
    int avgR = rSum / samples, avgG = gSum / samples, avgB = bSum / samples;
    return (avgR * 299 + avgG * 587 + avgB * 114) / 1000;
}

// Render text directly onto the packed (4-bit palette index) buffer,
// bypassing dithering for crisp text. Uses 2× supersampling for anti-aliasing
// mapped to palette indices: full coverage → text index, partial → threshold.
static void renderTextBandPacked(uint8_t* packed, const uint8_t* rgb,
                                 int canvasW, int canvasH,
                                 const char* text,
                                 const GFXfont* font, int initScale, int minScale,
                                 int textAreaY, int textAreaH) {
    int ssW = canvasW * 2;
    int ssH = textAreaH * 2;
    GFXcanvas1 canvas(ssW, ssH);
    canvas.fillScreen(0);
    canvas.setTextColor(1);
    canvas.setTextWrap(false);

    String str(text);
    int scale = fitTextToWidth(canvas, font, str, initScale, minScale, ssW - 60);

    int16_t x1, y1; uint16_t tw, th;
    canvas.setFont(font);
    canvas.setTextSize(scale);
    canvas.getTextBounds(str.c_str(), 0, 0, &x1, &y1, &tw, &th);
    canvas.setCursor((ssW - tw) / 2 - x1, (ssH - th) / 2 - y1);
    canvas.print(str);

    // White on dark, black on light — measured against the real background
    uint8_t textIdx = (bandBrightness(rgb, canvasW, textAreaY, textAreaH) < 128) ? 1 : 0;

    for (int y = 0; y < textAreaH; y++) {
        for (int x = 0; x < canvasW; x++) {
            int count = canvas.getPixel(x * 2,     y * 2)
                      + canvas.getPixel(x * 2 + 1, y * 2)
                      + canvas.getPixel(x * 2,     y * 2 + 1)
                      + canvas.getPixel(x * 2 + 1, y * 2 + 1);
            if (count < 2) continue; // skip low-coverage pixels (≤25%)
            int pi = (textAreaY + y) * canvasW + x;
            int byteIdx = pi / 2;
            if (pi & 1) {
                packed[byteIdx] = (packed[byteIdx] & 0xF0) | (textIdx & 0x0F);
            } else {
                packed[byteIdx] = (packed[byteIdx] & 0x0F) | (textIdx << 4);
            }
        }
    }
}

// Uses 2× supersampling for anti-aliased text: renders at double resolution
// on a 1-bit canvas, then downsamples with a 2×2 box filter to get smooth
// 4-level alpha blending. setTextSize() scales the font up so it fills
// the text area nicely after the 2× downsample.
static void renderText(uint8_t* rgb, int canvasW, int canvasH,
                       const char* artist, const char* album,
                       int textAreaY, int textAreaH,
                       uint8_t bgR, uint8_t bgG, uint8_t bgB) {
    // 2× oversampled canvas
    int ssW = canvasW * 2;
    int ssH = textAreaH * 2;
    GFXcanvas1 canvas(ssW, ssH);
    canvas.fillScreen(0);
    canvas.setTextColor(1);
    canvas.setTextWrap(false);

    const int maxW = ssW - 60;
    int16_t x1, y1; uint16_t tw, th;
    int16_t ax1, ay1; uint16_t atw, ath;

    // Artist name — bold 24pt, scaled 4× on 2× canvas = ~66px effective
    String artistStr(artist);
    int artistScale = fitTextToWidth(canvas, &FreeSansBold24pt7b, artistStr, 4, 2, maxW);
    canvas.getTextBounds(artistStr.c_str(), 0, 0, &x1, &y1, &tw, &th);
    int artistH = th;

    // Album name — regular 18pt, scaled 3× on 2× canvas = ~36px effective
    String albumStr(album);
    int albumScale = fitTextToWidth(canvas, &FreeSans18pt7b, albumStr, 3, 2, maxW);
    canvas.getTextBounds(albumStr.c_str(), 0, 0, &ax1, &ay1, &atw, &ath);
    int albumH = ath;

    // Centre both lines vertically in the text area (at 2× scale)
    int gap = ssH / 8; // gap between artist and album
    int totalH = artistH + gap + albumH;
    int startY = (ssH - totalH) / 2;

    // Draw artist
    canvas.setFont(&FreeSansBold24pt7b);
    canvas.setTextSize(artistScale);
    canvas.getTextBounds(artistStr.c_str(), 0, 0, &x1, &y1, &tw, &th);
    canvas.setCursor((ssW - tw) / 2 - x1, startY - y1);
    canvas.print(artistStr);

    // Draw album
    canvas.setFont(&FreeSans18pt7b);
    canvas.setTextSize(albumScale);
    canvas.getTextBounds(albumStr.c_str(), 0, 0, &ax1, &ay1, &atw, &ath);
    canvas.setCursor((ssW - atw) / 2 - ax1, startY + artistH + gap - ay1);
    canvas.print(albumStr);

    // Determine text color: white on dark bg, black on light bg
    int brightness = (bgR * 299 + bgG * 587 + bgB * 114) / 1000;
    uint8_t textR = (brightness < 128) ? 255 : 0;
    uint8_t textG = textR, textB = textR;

    // Downsample 2×2 blocks → alpha (0..4) and blend text color with background
    for (int y = 0; y < textAreaH; y++) {
        for (int x = 0; x < canvasW; x++) {
            int count = canvas.getPixel(x * 2,     y * 2)
                      + canvas.getPixel(x * 2 + 1, y * 2)
                      + canvas.getPixel(x * 2,     y * 2 + 1)
                      + canvas.getPixel(x * 2 + 1, y * 2 + 1);
            if (count == 0) continue; // leave background as-is
            int di = ((textAreaY + y) * canvasW + x) * 3;
            if (count == 4) {
                // Fully covered — write text color directly
                rgb[di]     = textR;
                rgb[di + 1] = textG;
                rgb[di + 2] = textB;
            } else {
                // Partial coverage — blend with actual underlying pixel
                float alpha = count * 0.25f;
                rgb[di]     = (uint8_t)(rgb[di]     + (textR - rgb[di])     * alpha);
                rgb[di + 1] = (uint8_t)(rgb[di + 1] + (textG - rgb[di + 1]) * alpha);
                rgb[di + 2] = (uint8_t)(rgb[di + 2] + (textB - rgb[di + 2]) * alpha);
            }
        }
    }
}

// ─── Pre-dither image enhancement for e-ink output ───
// Applies mild unsharp-mask sharpening + contrast boost + gamma correction
// in a single pass using a 3-row rolling buffer (~4 KB working memory).

static void enhanceForEink(uint8_t* rgb, int w, int h, const RenderProfile& profile);

// Choose how far to compress lightness for THIS image, by rendering the
// candidates small and scoring each against the source. See tone_map.h for
// why this is measured rather than predicted.
//
// The trial must run the WHOLE pipeline. Dithering the darkened candidate
// directly scores a render that never happens: contrast, gamma and sharpening
// come after this in the real path, and they re-expand exactly what was just
// compressed. Skipping them here picked the wrong scale on the device while
// the simulator, which did enhance, picked the right one.
static float chooseLightnessScale(const uint8_t* rgb, int w, int h,
                                  const RenderProfile& profile, bool* useGamut) {
    const int dw = w / TONEMAP_TRIAL_DIV, dh = h / TONEMAP_TRIAL_DIV;
    const size_t npix = (size_t)dw * dh;

    // cand is reused to hold the unpacked render: the candidate pixels are
    // finished with the moment they have been dithered. One 280 KB buffer
    // fewer matters — the display's own frame copy competes for this PSRAM,
    // and when it lost, the portal silently served no image at all.
    uint8_t* small  = (uint8_t*)heap_caps_malloc(npix * 3, MALLOC_CAP_SPIRAM);
    uint8_t* cand   = (uint8_t*)heap_caps_malloc(npix * 3, MALLOC_CAP_SPIRAM);
    uint8_t* packed = (uint8_t*)heap_caps_malloc(npix / 2 + 1, MALLOC_CAP_SPIRAM);
    uint8_t* shown  = cand;
    if (!small || !cand || !packed) {
        heap_caps_free(small); heap_caps_free(cand); heap_caps_free(packed);
        Serial.println("[Pipeline] Tone-map alloc failed, leaving lightness alone");
        return 1.0f;
    }

    toneMapShrink(rgb, w, h, TONEMAP_TRIAL_DIV, small);

    // Gamut mapping is not a free win. It rescues the covers whose colour is
    // unreachable — the KPop sleeve goes from dE 14.5 / hue 19.4 to 9.1 / 5.8
    // — and costs on covers that were already inside the gamut, where the
    // worst regression measured was dE 38.9 to 48.8. So it is decided the same
    // way the tone scale is: try it, keep it if it scores better.
    float best = 1.0f, bestScore = 0.0f;
    bool bestGamut = false;
    for (int k = 0; k < TONEMAP_SCALES * 2; k++) {
        const float scale = TONEMAP_SCALE[k % TONEMAP_SCALES];
        const bool gamut = (k >= TONEMAP_SCALES);
        memcpy(cand, small, npix * 3);
        toneMapApply(cand, dw, dh, scale);
        if (gamut) gamutMapApply(cand, dw, dh);
        enhanceForEink(cand, dw, dh, profile);
        ditherFloydSteinberg(cand, packed, dw, dh, profile);

        // Unpack to the pigment colours the panel will actually show.
        for (size_t i = 0; i < npix; i++) {
            const uint8_t idx = (i & 1) ? (packed[i >> 1] & 0x0F)
                                        : (packed[i >> 1] >> 4);
            const PaletteColor& p = PALETTE[idx < EPD_COLORS ? idx : 1];
            shown[i * 3 + 0] = p.r; shown[i * 3 + 1] = p.g; shown[i * 3 + 2] = p.b;
        }

        float dE = 0.0f, hue = 0.0f;
        toneMapScore(small, shown, dw, dh, 8, &dE, &hue);
        const float score = dE + TONEMAP_HUE_WEIGHT * hue;
        Serial.printf("[Pipeline] tone x%.2f gamut %d  dE %.1f  hue %.1f  score %.1f\n",
                      scale, (int)gamut, dE, hue, score);
        if (k == 0 || score < bestScore) {
            bestScore = score; best = scale; bestGamut = gamut;
        }
    }
    if (useGamut) *useGamut = bestGamut;

    heap_caps_free(small); heap_caps_free(cand); heap_caps_free(packed);
    Serial.printf("[Pipeline] chose tone x%.2f, gamut map %s\n",
                  best, bestGamut ? "on" : "off");
    return best;
}

static void enhanceForEink(uint8_t* rgb, int w, int h, const RenderProfile& profile) {
    const float sharpenAmt   = profile.sharpen;
    const float contrastFact = profile.contrast;
    const float gamma        = profile.gamma;
    const int   rowBytes     = w * 3;

    // Combined contrast + gamma LUT (one per intensity level)
    // Shadow protection: below shadowThresh, blend toward identity so darks
    // stay dark and map cleanly to black on e-ink (no dither noise in hair etc.)
    const int shadowThresh = 50;
    uint8_t lut[256];
    for (int i = 0; i < 256; i++) {
        float v = 128.0f + (i - 128.0f) * contrastFact;
        if (v < 0.0f)   v = 0.0f;
        if (v > 255.0f) v = 255.0f;
        v = 255.0f * powf(v / 255.0f, gamma);
        int enhanced = (int)(v + 0.5f);
        if (enhanced < 0)   enhanced = 0;
        if (enhanced > 255) enhanced = 255;

        // Blend: shadows keep original value, midtones/highlights get enhanced
        if (i < shadowThresh) {
            float t = (float)i / shadowThresh; // 0 at black → 1 at threshold
            lut[i] = (uint8_t)(i + t * (enhanced - i) + 0.5f);
        } else {
            lut[i] = (uint8_t)enhanced;
        }
    }

    // 3-row rolling buffer so we can read original values while writing back
    uint8_t* prev = (uint8_t*)malloc(rowBytes);
    uint8_t* curr = (uint8_t*)malloc(rowBytes);
    uint8_t* next = (uint8_t*)malloc(rowBytes);
    if (!prev || !curr || !next) {
        free(prev); free(curr); free(next);
        Serial.println("[Pipeline] Enhance alloc failed, contrast+gamma only");
        for (int i = 0; i < w * h * 3; i++) rgb[i] = lut[rgb[i]];
        return;
    }

    memcpy(prev, &rgb[0], rowBytes);                         // top-edge clamp
    memcpy(curr, &rgb[0], rowBytes);
    memcpy(next, (h > 1) ? &rgb[rowBytes] : &rgb[0], rowBytes);

    for (int y = 0; y < h; y++) {
        uint8_t* dst = &rgb[y * rowBytes];

        for (int x = 0; x < w; x++) {
            int xl = (x > 0)     ? x - 1 : 0;
            int xr = (x < w - 1) ? x + 1 : w - 1;

            for (int c = 0; c < 3; c++) {
                // 3×3 box-blur average
                int sum = prev[xl*3+c] + prev[x*3+c] + prev[xr*3+c]
                        + curr[xl*3+c] + curr[x*3+c] + curr[xr*3+c]
                        + next[xl*3+c] + next[x*3+c] + next[xr*3+c];
                int blur = sum / 9;
                int orig = curr[x*3+c];

                // Unsharp mask
                int sharp = orig + (int)(sharpenAmt * (orig - blur));
                if (sharp < 0)   sharp = 0;
                if (sharp > 255) sharp = 255;

                // Apply contrast + gamma via LUT
                dst[x*3+c] = lut[sharp];
            }
        }

        // Rotate rolling buffer
        uint8_t* tmp = prev;
        prev = curr;
        curr = next;
        next = tmp;

        if (y + 2 < h)
            memcpy(next, &rgb[(y + 2) * rowBytes], rowBytes);
        else
            memcpy(next, curr, rowBytes);   // bottom-edge clamp
    }

    free(prev);
    free(curr);
    free(next);
    Serial.printf("[Pipeline] Enhanced (%s: sharpen %.2f contrast %.2f gamma %.2f)\n",
                  profile.name, sharpenAmt, contrastFact, gamma);
}

// ─── Blurred background fill ───
// Scales source image to FILL the canvas (crop excess), then applies heavy box
// blur + slight dim. Creates the "blurred pillarbox" look seen on TV/YouTube.
static void fillBlurredBackground(uint8_t* canvas, int cW, int cH,
                                   const uint8_t* src, int sW, int sH,
                                   int fillH) {
    // Scale to fill (cover crop) the fill area, with extra 30% zoom
    // to focus on the central portion of the image (skip album borders)
    float scaleX = (float)cW / sW;
    float scaleY = (float)fillH / sH;
    float scale  = (scaleX > scaleY) ? scaleX : scaleY; // pick LARGER to fill
    scale *= 1.3f; // extra zoom into centre

    int scaledW = (int)(sW * scale);
    int scaledH = (int)(sH * scale);
    int cropX = (scaledW - cW) / 2;
    int cropY = (scaledH - fillH) / 2;

    // Blit scaled+cropped source into canvas
    for (int y = 0; y < fillH; y++) {
        for (int x = 0; x < cW; x++) {
            int sX = constrain((int)((x + cropX) / scale), 0, sW - 1);
            int sY = constrain((int)((y + cropY) / scale), 0, sH - 1);
            int si = (sY * sW + sX) * 3;
            int di = (y * cW + x) * 3;
            canvas[di]     = src[si];
            canvas[di + 1] = src[si + 1];
            canvas[di + 2] = src[si + 2];
        }
    }

    // Heavy box blur — 4 passes of radius-12 horizontal then vertical.
    // Uses a single row/col accumulator buffer (~1.5 KB).
    const int radius = 12;
    const int passes = 4;
    uint8_t* tmp = (uint8_t*)malloc(max(cW, fillH) * 3);
    if (!tmp) return; // degrade gracefully to unblurred

    for (int pass = 0; pass < passes; pass++) {
        // Horizontal pass
        for (int y = 0; y < fillH; y++) {
            uint8_t* row = &canvas[y * cW * 3];
            // Running sum for first pixel
            int rS = 0, gS = 0, bS = 0;
            for (int k = -radius; k <= radius; k++) {
                int xi = constrain(k, 0, cW - 1) * 3;
                rS += row[xi]; gS += row[xi + 1]; bS += row[xi + 2];
            }
            int diam = 2 * radius + 1;
            tmp[0] = rS / diam; tmp[1] = gS / diam; tmp[2] = bS / diam;

            for (int x = 1; x < cW; x++) {
                int addX = constrain(x + radius, 0, cW - 1) * 3;
                int subX = constrain(x - radius - 1, 0, cW - 1) * 3;
                rS += row[addX] - row[subX];
                gS += row[addX + 1] - row[subX + 1];
                bS += row[addX + 2] - row[subX + 2];
                tmp[x * 3]     = rS / diam;
                tmp[x * 3 + 1] = gS / diam;
                tmp[x * 3 + 2] = bS / diam;
            }
            memcpy(row, tmp, cW * 3);
        }

        // Vertical pass
        for (int x = 0; x < cW; x++) {
            int rS = 0, gS = 0, bS = 0;
            for (int k = -radius; k <= radius; k++) {
                int yi = constrain(k, 0, fillH - 1);
                int si = (yi * cW + x) * 3;
                rS += canvas[si]; gS += canvas[si + 1]; bS += canvas[si + 2];
            }
            int diam = 2 * radius + 1;
            tmp[0] = rS / diam; tmp[1] = gS / diam; tmp[2] = bS / diam;

            for (int y = 1; y < fillH; y++) {
                int addY = constrain(y + radius, 0, fillH - 1);
                int subY = constrain(y - radius - 1, 0, fillH - 1);
                int ai = (addY * cW + x) * 3;
                int si = (subY * cW + x) * 3;
                rS += canvas[ai] - canvas[si];
                gS += canvas[ai + 1] - canvas[si + 1];
                bS += canvas[ai + 2] - canvas[si + 2];
                tmp[y * 3]     = rS / diam;
                tmp[y * 3 + 1] = gS / diam;
                tmp[y * 3 + 2] = bS / diam;
            }

            // Write back column
            for (int y = 0; y < fillH; y++) {
                int di = (y * cW + x) * 3;
                canvas[di]     = tmp[y * 3];
                canvas[di + 1] = tmp[y * 3 + 1];
                canvas[di + 2] = tmp[y * 3 + 2];
            }
        }
    }
    free(tmp);

    // Darken or wash out depending on bg_style setting
    if (g_app.settings.bg_style == 1) {
        // Wash out: blend toward white
        for (int i = 0; i < fillH * cW * 3; i++)
            canvas[i] = (uint8_t)(canvas[i] + (255 - canvas[i]) * 45 / 100);
    } else {
        // Darken: dim to 55%
        for (int i = 0; i < fillH * cW * 3; i++)
            canvas[i] = (uint8_t)(canvas[i] * 55 / 100);
    }

    Serial.println("[Pipeline] Blurred background fill applied");
}

// ═══════════════════════════════════════════════════════════
// Filling the panel with square artwork
//
// The panel is 480x800; sleeves are square. Fitting to the width leaves 40% of
// the screen as background, and cover-cropping to fill it throws away 40% of
// the sleeve horizontally — which on album art usually means slicing through
// the artist's name.
//
// So the choice is not crop-or-don't, it is *how much*. `zoom` spans the whole
// range: 1.0 crops nothing and leaves a wide band to extend, FILL_MAX_ZOOM
// covers the panel outright. Adaptive walks it up and stops before the cut
// lines start passing through the sleeve's own detail.
// ═══════════════════════════════════════════════════════════

// Horizontal + vertical box blur over a band of rows, in place.
static void blurBand(uint8_t* canvas, int w, int y0, int y1, int radius) {
    if (radius < 1 || y1 - y0 < 1) return;
    int diam = 2 * radius + 1;
    uint8_t* tmp = (uint8_t*)malloc((size_t)w * 3);
    if (!tmp) return;
    for (int y = y0; y < y1; y++) {
        uint8_t* row = &canvas[(size_t)y * w * 3];
        int rS = 0, gS = 0, bS = 0;
        for (int k = -radius; k <= radius; k++) {
            int xi = constrain(k, 0, w - 1) * 3;
            rS += row[xi]; gS += row[xi+1]; bS += row[xi+2];
        }
        for (int x = 0; x < w; x++) {
            tmp[x*3] = rS / diam; tmp[x*3+1] = gS / diam; tmp[x*3+2] = bS / diam;
            int a = constrain(x + radius + 1, 0, w - 1) * 3;
            int b = constrain(x - radius,     0, w - 1) * 3;
            rS += row[a] - row[b]; gS += row[a+1] - row[b+1]; bS += row[a+2] - row[b+2];
        }
        memcpy(row, tmp, (size_t)w * 3);
    }
    free(tmp);
}

// Fill the strips above and below the artwork so it reaches the panel edges.
//
// Mirroring guarantees the colour matches exactly at the join — the reflected
// row beside the edge IS the edge row. But mirroring alone reflects *content*,
// and sleeves put type near their edges: the first version produced legible
// ghost text above the artwork, which reads as a fault rather than a design.
// So the extension starts already blurred past recognition and is pulled
// progressively toward a flat continuation of the artwork's edge colour.
static void extendEdges(uint8_t* canvas, int w, int h, int artY0, int artH) {
    const int artY1 = artY0 + artH;
    if (artY0 <= 0 && artY1 >= h) return;

    // Mirror the artwork outward.
    for (int y = 0; y < artY0; y++) {
        int src = artY0 + (artY0 - y);
        if (src >= artY1) src = artY1 - 1;
        memcpy(&canvas[(size_t)y * w * 3], &canvas[(size_t)src * w * 3], (size_t)w * 3);
    }
    for (int y = artY1; y < h; y++) {
        int src = artY1 - (y - artY1) - 1;
        if (src < artY0) src = artY0;
        memcpy(&canvas[(size_t)y * w * 3], &canvas[(size_t)src * w * 3], (size_t)w * 3);
    }

    // Flat wash: the artwork's own edge colour, per column, softened across.
    uint8_t* washTop = (uint8_t*)malloc((size_t)w * 3);
    uint8_t* washBot = (uint8_t*)malloc((size_t)w * 3);
    if (washTop && washBot) {
        const int SAMPLE = 24;
        for (int x = 0; x < w; x++) {
            int rt=0,gt=0,bt=0,rb=0,gb=0,bb=0,n=0;
            for (int k = 0; k < SAMPLE; k++) {
                const uint8_t* t = &canvas[((size_t)(artY0 + k) * w + x) * 3];
                const uint8_t* b = &canvas[((size_t)(artY1 - 1 - k) * w + x) * 3];
                rt+=t[0]; gt+=t[1]; bt+=t[2]; rb+=b[0]; gb+=b[1]; bb+=b[2]; n++;
            }
            washTop[x*3]=rt/n; washTop[x*3+1]=gt/n; washTop[x*3+2]=bt/n;
            washBot[x*3]=rb/n; washBot[x*3+1]=gb/n; washBot[x*3+2]=bb/n;
        }

        // Smear the wash sideways. It is sampled per column over rows that
        // include whatever type the sleeve carries near its edge, so a column
        // under a letter averages darker than its neighbours and the wash
        // itself keeps a trace of the text. Blurring across x removes that
        // while leaving the left-to-right colour change that makes the
        // extension look like a continuation.
        const int WASH_BLUR = 48;
        uint8_t* tmp = (uint8_t*)malloc((size_t)w * 3);
        if (tmp) {
            for (uint8_t* wash : { washTop, washBot }) {
                memcpy(tmp, wash, (size_t)w * 3);
                for (int x = 0; x < w; x++)
                    for (int c = 0; c < 3; c++) {
                        int sum = 0, n2 = 0;
                        for (int k = -WASH_BLUR; k <= WASH_BLUR; k++) {
                            const int xx = x + k;
                            if (xx < 0 || xx >= w) continue;
                            sum += tmp[xx * 3 + c]; n2++;
                        }
                        wash[x * 3 + c] = (uint8_t)(sum / (n2 ? n2 : 1));
                    }
            }
            free(tmp);
        }
    }

    const int BANDS = 6;
    for (int i = 0; i < BANDS; i++) {
        float f0 = (float)i / BANDS, f1 = (float)(i + 1) / BANDS;
        int radius = (int)(10 + 30 * powf(f1, 1.2f));
        // Reach the wash quickly. The ghost that survives is the one nearest
        // the join, because that band kept three quarters of the mirrored
        // content — an exponent of 0.8 only reached 24% wash in the first
        // band. The wash is the artwork's own edge colour, so converging on
        // it sooner improves the join rather than compromising it.
        float t = powf(f1, 0.35f);

        int ty1 = artY0 - (int)(artY0 * f0), ty0 = artY0 - (int)(artY0 * f1);
        int by0 = artY1 + (int)((h - artY1) * f0), by1 = artY1 + (int)((h - artY1) * f1);
        blurBand(canvas, w, max(0, ty0), max(0, ty1), radius);
        blurBand(canvas, w, min(h, by0), min(h, by1), radius);

        if (washTop && washBot) {
            for (int y = max(0, ty0); y < max(0, ty1); y++)
                for (int x = 0; x < w * 3; x++) {
                    uint8_t* p = &canvas[(size_t)y * w * 3 + x];
                    *p = (uint8_t)(*p * (1 - t) + washTop[x] * t);
                }
            for (int y = min(h, by0); y < min(h, by1); y++)
                for (int x = 0; x < w * 3; x++) {
                    uint8_t* p = &canvas[(size_t)y * w * 3 + x];
                    *p = (uint8_t)(*p * (1 - t) + washBot[x] * t);
                }
        }
    }
    free(washTop); free(washBot);
    Serial.printf("[Fill] edges extended (artwork rows %d-%d of %d)\n", artY0, artY1, h);
}

// ─── Core: decode JPEG buffer → scale → optional text → dither → display ───
// Does NOT take ownership of jpegBuf — the caller frees it.  Distinguishing
// "this JPEG is undecodable" from "we ran out of memory" matters: only the
// former should stop us caching the artwork to history.
enum PipelineResult {
    PIPE_OK = 0,
    PIPE_DECODE_FAILED,   // the JPEG itself is unusable — do not cache it
    PIPE_RESOURCE_FAILED  // transient (allocation) — the JPEG is fine
};

static PipelineResult processJpegBuffer(uint8_t* jpegBuf, size_t jpegSize,
                                        const char* artist = nullptr, const char* album = nullptr) {
    // 1. Get dimensions
    uint16_t imgW, imgH;
    JRESULT jr = TJpgDec.getJpgSize(&imgW, &imgH, jpegBuf, jpegSize);
    Serial.printf("[Pipeline] JPEG %dx%d (%u bytes) jd_prepare=%d\n", imgW, imgH, jpegSize, jr);
    Serial.printf("[Pipeline] Free PSRAM: %u, largest block: %u\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    if (jr != JDR_OK || imgW == 0 || imgH == 0) {
        Serial.printf("[Pipeline] JPEG parse failed (jr=%d)\n", (int)jr);
        activityLogf("Artwork decode failed: JPEG header (jr=%d)", (int)jr);
        return PIPE_DECODE_FAILED;
    }

    // 2. Decode to RGB888 in PSRAM
    g_decodeW = imgW;
    g_decodeH = imgH;
    g_decodeBuf = (uint8_t*)heap_caps_malloc((size_t)imgW * imgH * 3, MALLOC_CAP_SPIRAM);
    if (!g_decodeBuf) {
        Serial.printf("[Pipeline] Decode buffer alloc failed (%dx%dx3 = %u bytes)\n",
                      imgW, imgH, (unsigned)(imgW * imgH * 3));
        return PIPE_RESOURCE_FAILED;
    }

    TJpgDec.setCallback(tjpgCallback);
    TJpgDec.setJpgScale(1);
    JRESULT decodeResult = TJpgDec.drawJpg(0, 0, jpegBuf, jpegSize);

    if (decodeResult != JDR_OK) {
        Serial.printf("[Pipeline] JPEG decode failed (jr=%d)\n", (int)decodeResult);
        activityLogf("Artwork decode failed: unsupported JPEG format (jr=%d)", (int)decodeResult);
        heap_caps_free(g_decodeBuf);
        g_decodeBuf = nullptr;
        return PIPE_DECODE_FAILED;
    }

    // 3. Scale to display size
    bool showText = (artist && artist[0] && album && album[0]);
    // With text: artist band (top) | artwork (centre) | album band (bottom)
    // Without text: artwork fills entire canvas
    const int ARTIST_BAND_H = 100;
    const int ALBUM_BAND_H  = 80;
    int artAreaH = showText ? (EPD_HEIGHT - ARTIST_BAND_H - ALBUM_BAND_H) : EPD_HEIGHT;
    int artAreaW = EPD_WIDTH; // always 480

    uint8_t* scaledBuf = (uint8_t*)heap_caps_malloc((size_t)EPD_WIDTH * EPD_HEIGHT * 3, MALLOC_CAP_SPIRAM);
    if (!scaledBuf) {
        heap_caps_free(g_decodeBuf);
        g_decodeBuf = nullptr;
        Serial.println("[Pipeline] Scaled buffer alloc failed");
        return PIPE_RESOURCE_FAILED;
    }

    // ── Fill strategy ──
    // The text-overlay layout needs its bands, so it keeps the original
    // fit-and-background treatment. Without text we can use the whole panel.
    uint8_t fillMode = showText ? FILL_FIT : g_app.settings.fill_mode;

    if (fillMode != FILL_FIT) {
        float zoom = 1.0f;
        if (fillMode == FILL_COVER)         zoom = FILL_MAX_ZOOM;
        else if (fillMode == FILL_ADAPTIVE) zoom = fillAdaptiveZoom(g_decodeBuf, imgW, imgH);
        Serial.printf("[Fill] mode %d, zoom %.2f\n", fillMode, zoom);

        int side = (int)(EPD_WIDTH * zoom + 0.5f);
        if (side > EPD_HEIGHT) side = EPD_HEIGHT;
        int artY0 = (EPD_HEIGHT - side) / 2;
        if (artY0 < 0) artY0 = 0;

        // Sample the sleeve into a `side` x `side` square, cropped to the panel
        // width and clipped vertically to the panel.
        float sc = (float)side / imgW;
        int cropX = (side - EPD_WIDTH) / 2;
        int yStart = (artY0 < 0) ? -artY0 : 0;
        for (int y = 0; y < side; y++) {
            int cy = artY0 + y;
            if (cy < 0 || cy >= EPD_HEIGHT) continue;
            int sy = constrain((int)(y / sc), 0, imgH - 1);
            for (int x = 0; x < EPD_WIDTH; x++) {
                int sx = constrain((int)((x + cropX) / sc), 0, imgW - 1);
                const uint8_t* sp = &g_decodeBuf[((size_t)sy * imgW + sx) * 3];
                uint8_t* dp = &scaledBuf[((size_t)cy * EPD_WIDTH + x) * 3];
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
            }
        }
        (void)yStart;

        if (side < EPD_HEIGHT) extendEdges(scaledBuf, EPD_WIDTH, EPD_HEIGHT, artY0, side);
    } else {

    // Compute edge color for background fill (used as fallback)
    uint8_t bgR, bgG, bgB;
    averageEdgeColor(g_decodeBuf, imgW, imgH, bgR, bgG, bgB);

    // Background mode: 0 = always solid, 1 = always blur, 2 = auto-detect.
    bool useBlur;
    if (g_app.settings.bg_mode == 0) {
        useBlur = false;
        Serial.println("[Pipeline] Background: forced solid");
    } else if (g_app.settings.bg_mode == 1) {
        useBlur = true;
        Serial.println("[Pipeline] Background: forced blur");
    } else {
        float var = edgeVariance(g_decodeBuf, imgW, imgH);
        useBlur = var >= 800.0f;
        Serial.printf("[Pipeline] Edge variance: %.0f → %s fill\n", var,
                      useBlur ? "blur" : "solid");
    }

    if (useBlur) {
        // Blurred background: always fill entire canvas
        fillBlurredBackground(scaledBuf, EPD_WIDTH, EPD_HEIGHT,
                              g_decodeBuf, imgW, imgH, EPD_HEIGHT);

    } else {
        // Solid colour fill
        for (int i = 0; i < EPD_WIDTH * EPD_HEIGHT; i++) {
            scaledBuf[i * 3]     = bgR;
            scaledBuf[i * 3 + 1] = bgG;
            scaledBuf[i * 3 + 2] = bgB;
        }
    }

    // Scale artwork to fit art area, centred with shadow margin when blur is active.
    const int shadowMarginV = useBlur ? 22 : 0;
    int fitW = artAreaW;
    int fitH = artAreaH - shadowMarginV * 2;
    float scaleX = (float)fitW / imgW;
    float scaleY = (float)fitH / imgH;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;

    int scaledW = (int)(imgW * scale);
    int scaledH = (int)(imgH * scale);
    int offsetX = (artAreaW - scaledW) / 2;
    int offsetY;
    if (showText) {
        offsetY = ARTIST_BAND_H + (artAreaH - scaledH) / 2; // centred in middle zone
    } else {
        offsetY = (EPD_HEIGHT - scaledH) / 2; // centred on full canvas
    }

    // Drop shadow — when blur background is active.
    if (useBlur) {
        const int shadowPad = 12;
        // Top shadow band
        for (int dy = 1; dy <= shadowPad; dy++) {
            int py = offsetY - dy;
            if (py < 0) continue;
            float t = (float)dy / shadowPad;
            float alpha = 0.35f * (1.0f - t) * (1.0f - t);
            for (int x = 0; x < scaledW; x++) {
                int px = x + offsetX;
                if (px < 0 || px >= EPD_WIDTH) continue;
                int di = (py * EPD_WIDTH + px) * 3;
                scaledBuf[di]     = (uint8_t)(scaledBuf[di]     * (1.0f - alpha));
                scaledBuf[di + 1] = (uint8_t)(scaledBuf[di + 1] * (1.0f - alpha));
                scaledBuf[di + 2] = (uint8_t)(scaledBuf[di + 2] * (1.0f - alpha));
            }
        }
        // Bottom shadow band
        for (int dy = 1; dy <= shadowPad; dy++) {
            int py = offsetY + scaledH - 1 + dy;
            if (py >= EPD_HEIGHT) continue;
            float t = (float)dy / shadowPad;
            float alpha = 0.35f * (1.0f - t) * (1.0f - t);
            for (int x = 0; x < scaledW; x++) {
                int px = x + offsetX;
                if (px < 0 || px >= EPD_WIDTH) continue;
                int di = (py * EPD_WIDTH + px) * 3;
                scaledBuf[di]     = (uint8_t)(scaledBuf[di]     * (1.0f - alpha));
                scaledBuf[di + 1] = (uint8_t)(scaledBuf[di + 1] * (1.0f - alpha));
                scaledBuf[di + 2] = (uint8_t)(scaledBuf[di + 2] * (1.0f - alpha));
            }
        }
        Serial.println("[Pipeline] Drop shadow applied");
    }

    for (int y = 0; y < scaledH; y++) {
        for (int x = 0; x < scaledW; x++) {
            int sX = constrain((int)(x / scale), 0, imgW - 1);
            int sY = constrain((int)(y / scale), 0, imgH - 1);
            int si = (sY * imgW + sX) * 3;
            int di = ((y + offsetY) * EPD_WIDTH + (x + offsetX)) * 3;
            scaledBuf[di]     = g_decodeBuf[si];
            scaledBuf[di + 1] = g_decodeBuf[si + 1];
            scaledBuf[di + 2] = g_decodeBuf[si + 2];
        }
    }
    }

    heap_caps_free(g_decodeBuf);
    g_decodeBuf = nullptr;

    // 3.5. Pre-dither enhancement (sharpen + contrast + gamma)
    const RenderProfile& profile = renderProfile(g_app.settings.render_profile);
    bool useGamut = false;
    toneMapApply(scaledBuf, EPD_WIDTH, EPD_HEIGHT,
                 chooseLightnessScale(scaledBuf, EPD_WIDTH, EPD_HEIGHT,
                                      profile, &useGamut));
    // After the tone map, so the gamut is judged at the lightness the image
    // will actually be shown at, and before enhancement, so contrast and
    // gamma act on colours the panel can hold.
    if (useGamut) gamutMapApply(scaledBuf, EPD_WIDTH, EPD_HEIGHT);
    enhanceForEink(scaledBuf, EPD_WIDTH, EPD_HEIGHT, profile);

    if (!g_canvasProbe)
        g_canvasProbe = (uint8_t*)heap_caps_malloc(pipelineCanvasProbeSize()
                            ? pipelineCanvasProbeSize()
                            : (size_t)(EPD_WIDTH / CANVAS_PROBE_DIV) *
                              (EPD_HEIGHT / CANVAS_PROBE_DIV) * 3,
                            MALLOC_CAP_SPIRAM);
    if (g_canvasProbe)
        toneMapShrink(scaledBuf, EPD_WIDTH, EPD_HEIGHT, CANVAS_PROBE_DIV, g_canvasProbe);

    // 4. Dither to 6-colour packed buffer
    size_t packedSize = (EPD_WIDTH * EPD_HEIGHT) / 2;
    uint8_t* packedBuf = (uint8_t*)heap_caps_calloc(packedSize, 1, MALLOC_CAP_SPIRAM);
    if (!packedBuf) {
        heap_caps_free(scaledBuf);
        Serial.println("[Pipeline] Packed buffer alloc failed");
        return PIPE_RESOURCE_FAILED;
    }

    ditherFloydSteinberg(scaledBuf, packedBuf, EPD_WIDTH, EPD_HEIGHT, profile);

    // Render text directly onto packed buffer (after dithering for crisp text)
    if (showText) {
        renderTextBandPacked(packedBuf, scaledBuf, EPD_WIDTH, EPD_HEIGHT, artist,
                             &FreeSansBold24pt7b, 2, 1,
                             0, ARTIST_BAND_H);
        renderTextBandPacked(packedBuf, scaledBuf, EPD_WIDTH, EPD_HEIGHT, album,
                             &FreeSans18pt7b, 2, 1,
                             ARTIST_BAND_H + artAreaH, ALBUM_BAND_H);
    }
    heap_caps_free(scaledBuf);

    // 5. Push to display
    displayShowImage(packedBuf);
    heap_caps_free(packedBuf);

    return PIPE_OK;
}

// ─── Placeholder display when artwork can't be decoded ───
bool pipelineShowPlaceholder(const char* artist, const char* album) {
    uint8_t* scaledBuf = (uint8_t*)heap_caps_malloc((size_t)EPD_WIDTH * EPD_HEIGHT * 3, MALLOC_CAP_SPIRAM);
    if (!scaledBuf) {
        Serial.println("[Pipeline] Placeholder alloc failed");
        return false;
    }

    // Dark charcoal background
    uint8_t bgR = 35, bgG = 35, bgB = 35;
    for (int i = 0; i < EPD_WIDTH * EPD_HEIGHT; i++) {
        scaledBuf[i * 3]     = bgR;
        scaledBuf[i * 3 + 1] = bgG;
        scaledBuf[i * 3 + 2] = bgB;
    }

    // Render text centred on entire display
    renderText(scaledBuf, EPD_WIDTH, EPD_HEIGHT, artist, album,
               0, EPD_HEIGHT, bgR, bgG, bgB);

    const RenderProfile& profile = renderProfile(g_app.settings.render_profile);
    toneMapApply(scaledBuf, EPD_WIDTH, EPD_HEIGHT,
                 chooseLightnessScale(scaledBuf, EPD_WIDTH, EPD_HEIGHT, profile, nullptr));
    enhanceForEink(scaledBuf, EPD_WIDTH, EPD_HEIGHT, profile);

    size_t packedSize = (EPD_WIDTH * EPD_HEIGHT) / 2;
    uint8_t* packedBuf = (uint8_t*)heap_caps_calloc(packedSize, 1, MALLOC_CAP_SPIRAM);
    if (!packedBuf) {
        heap_caps_free(scaledBuf);
        Serial.println("[Pipeline] Placeholder packed alloc failed");
        return false;
    }

    ditherFloydSteinberg(scaledBuf, packedBuf, EPD_WIDTH, EPD_HEIGHT, profile);
    heap_caps_free(scaledBuf);

    displayShowImage(packedBuf);
    heap_caps_free(packedBuf);

    Serial.println("[Pipeline] Placeholder displayed");
    return true;
}

// ─── Download JPEG from URL into PSRAM ───
static uint8_t* downloadJpeg(const char* url, size_t& outSize) {
    outSize = 0;
    HTTPClient http;
    activityLogf("Artwork fetch: %s", url);
    const char* hdrKeys[] = {"Content-Type"};
    http.collectHeaders(hdrKeys, 1);

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    if (strncmp(url, "https", 5) == 0) {
        secureClient.setInsecure(); // album art — no cert verification needed
        http.begin(secureClient, url);
    } else {
        http.begin(plainClient, url);
    }

    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    int code = http.GET();
    String contentType = http.header("Content-Type");
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Pipeline] HTTP %d from %s\n", code, url);
        activityLogf("Artwork fetch failed: HTTP %d", code);
        http.end();
        return nullptr;
    }
    if (contentType.length() == 0) contentType = "unknown";
    activityLogf("Artwork HTTP 200 (%s)", contentType.c_str());

    int contentLen = http.getSize();
    if (contentLen < 0) contentLen = 0; // unknown length (chunked/no header)
    if (contentLen > 0 && (size_t)contentLen > JPEG_MAX_DOWNLOAD) {
        activityLogf("Artwork too large: %d bytes", contentLen);
        http.end();
        return nullptr;
    }
    size_t allocSize = (contentLen > 0) ? (size_t)contentLen : JPEG_INITIAL_ALLOC;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM);
    if (!buf) {
        Serial.println("[Pipeline] PSRAM alloc failed for download");
        activityLog("Artwork fetch failed: PSRAM alloc");
        http.end();
        return nullptr;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t total = 0;
    unsigned long deadline = millis() + 15000;
    while ((http.connected() || stream->available()) && millis() < deadline) {
        size_t avail = stream->available();
        if (avail) {
            size_t required = total + avail;
            if (required > allocSize) {
                size_t newSize = allocSize;
                while (newSize < required && newSize < JPEG_MAX_DOWNLOAD) {
                    newSize *= 2;
                }
                if (newSize > JPEG_MAX_DOWNLOAD) newSize = JPEG_MAX_DOWNLOAD;
                if (newSize <= allocSize || newSize < required) {
                    activityLogf("Artwork fetch failed: exceeds %u bytes", (unsigned)JPEG_MAX_DOWNLOAD);
                    heap_caps_free(buf);
                    http.end();
                    return nullptr;
                }
                uint8_t* grown = (uint8_t*)heap_caps_realloc(buf, newSize, MALLOC_CAP_SPIRAM);
                if (!grown) {
                    activityLog("Artwork fetch failed: PSRAM realloc");
                    heap_caps_free(buf);
                    http.end();
                    return nullptr;
                }
                buf = grown;
                allocSize = newSize;
            }
            size_t chunk = min(avail, allocSize - total);
            size_t got = stream->readBytes(buf + total, chunk);
            total += got;
        } else {
            delay(1);
        }
        if (contentLen > 0 && total >= (size_t)contentLen) break;
    }
    http.end();

    if (contentLen > 0 && total != (size_t)contentLen) {
        activityLogf("Artwork fetch incomplete: %u/%d bytes", (unsigned)total, contentLen);
        heap_caps_free(buf);
        return nullptr;
    }
    if (total < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
        activityLog("Artwork fetch failed: not a JPEG payload");
        heap_caps_free(buf);
        return nullptr;
    }
    bool hasEoi = false;
    if (total >= 2) {
        size_t eoiStart = (total > 64) ? total - 64 : 0;
        size_t i = total - 2;
        while (true) {
            if (buf[i] == 0xFF && buf[i + 1] == 0xD9) {
                hasEoi = true;
                break;
            }
            if (i == eoiStart) break;
            --i;
        }
    }
    if (!hasEoi) {
        activityLog("Artwork fetch failed: JPEG appears truncated");
        heap_caps_free(buf);
        return nullptr;
    }

    outSize = total;
    Serial.printf("[Pipeline] Downloaded %u bytes\n", total);
    activityLogf("Artwork downloaded: %u bytes", (unsigned)total);
    return buf;
}

// ─── Public API ───


// ─── How well would this cover render? ───
//
// Used to choose between pressings of the same album. Decodes at a quarter
// scale and runs the same trial the real path runs, so the answer is the
// pipeline's own opinion rather than a proxy for it — three separate proxies
// were tried for the tone map and all three chose wrong.
//
// Also returns the artwork signature, because a candidate that renders
// beautifully is no use if it is a different sleeve. See cover_match.h.
bool pipelineAssessJpeg(const uint8_t* jpeg, size_t len,
                        float* scoreOut, float* sigOut) {
    uint16_t imgW = 0, imgH = 0;
    if (TJpgDec.getJpgSize(&imgW, &imgH, (uint8_t*)jpeg, len) != JDR_OK ||
        !imgW || !imgH) return false;

    const int div = 4;                       // plenty for both jobs
    const int dw = imgW / div, dh = imgH / div;
    if (dw < 16 || dh < 16) return false;

    uint8_t* prevBuf = g_decodeBuf;
    const int prevW = g_decodeW, prevH = g_decodeH;

    g_decodeW = dw; g_decodeH = dh;
    g_decodeBuf = (uint8_t*)heap_caps_malloc((size_t)dw * dh * 3, MALLOC_CAP_SPIRAM);
    if (!g_decodeBuf) {
        g_decodeBuf = prevBuf; g_decodeW = prevW; g_decodeH = prevH;
        return false;
    }

    TJpgDec.setCallback(tjpgCallback);
    TJpgDec.setJpgScale(div);
    const bool ok = TJpgDec.drawJpg(0, 0, (uint8_t*)jpeg, len) == JDR_OK;
    TJpgDec.setJpgScale(1);

    if (ok) {
        if (sigOut) coverSignature(g_decodeBuf, dw, dh, sigOut);

        if (scoreOut) {
            // A square canvas of the sleeve alone. The real path also extends
            // the artwork to fill the panel, but every candidate is treated
            // alike so the comparison holds, and the extension is derived from
            // the sleeve anyway.
            const int cw = 120, ch = 200;
            uint8_t* canvas = (uint8_t*)heap_caps_malloc((size_t)cw * ch * 3,
                                                         MALLOC_CAP_SPIRAM);
            uint8_t* packed = (uint8_t*)heap_caps_malloc((size_t)cw * ch / 2,
                                                         MALLOC_CAP_SPIRAM);
            uint8_t* shown  = (uint8_t*)heap_caps_malloc((size_t)cw * ch * 3,
                                                         MALLOC_CAP_SPIRAM);
            if (canvas && packed && shown) {
                for (int y = 0; y < ch; y++)
                    for (int x = 0; x < cw; x++) {
                        const int sx = x * dw / cw, sy = y * dh / ch;
                        const uint8_t* sp = &g_decodeBuf[((size_t)sy * dw + sx) * 3];
                        uint8_t* dp = &canvas[((size_t)y * cw + x) * 3];
                        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
                    }
                const RenderProfile& profile =
                    renderProfile(g_app.settings.render_profile);
                uint8_t* ref = (uint8_t*)heap_caps_malloc((size_t)cw * ch * 3,
                                                          MALLOC_CAP_SPIRAM);
                if (ref) memcpy(ref, canvas, (size_t)cw * ch * 3);

                float best = 1e30f;
                for (int k = 0; k < TONEMAP_SCALES * 2; k++) {
                    if (ref) memcpy(canvas, ref, (size_t)cw * ch * 3);
                    toneMapApply(canvas, cw, ch, TONEMAP_SCALE[k % TONEMAP_SCALES]);
                    if (k >= TONEMAP_SCALES) gamutMapApply(canvas, cw, ch);
                    enhanceForEink(canvas, cw, ch, profile);
                    ditherFloydSteinberg(canvas, packed, cw, ch, profile);
                    for (size_t i = 0; i < (size_t)cw * ch; i++) {
                        const uint8_t idx = (i & 1) ? (packed[i >> 1] & 0x0F)
                                                    : (packed[i >> 1] >> 4);
                        const PaletteColor& pc = PALETTE[idx < EPD_COLORS ? idx : 1];
                        shown[i*3] = pc.r; shown[i*3+1] = pc.g; shown[i*3+2] = pc.b;
                    }
                    float dE = 0.0f, hue = 0.0f;
                    toneMapScore(canvas, shown, cw, ch, 8, &dE, &hue);
                    const float sc = dE + TONEMAP_HUE_WEIGHT * hue;
                    if (sc < best) best = sc;
                }
                heap_caps_free(ref);
                *scoreOut = best;
            } else {
                *scoreOut = 1e30f;
            }
            heap_caps_free(canvas); heap_caps_free(packed); heap_caps_free(shown);
        }
    }

    heap_caps_free(g_decodeBuf);
    g_decodeBuf = prevBuf; g_decodeW = prevW; g_decodeH = prevH;
    return ok;
}

bool pipelineProcessUrl(const char* url,
                        const char* overlayArtist, const char* overlayAlbum,
                        const char* artist, const char* title, const char* album) {
    size_t jpegSize = 0;
    uint8_t* jpegBuf = downloadJpeg(url, jpegSize);
    if (!jpegBuf || jpegSize == 0) {
        activityLog("Artwork fetch failed");
        return false;
    }

    // Look for a better-rendering scan of the SAME sleeve. Only ever a
    // different scan — cover_match.h refuses anything that is not recognisably
    // the same picture, because ranking on render quality alone would hang an
    // obscure reissue on the wall in place of the famous cover.
    if (g_app.settings.cover_variants && artist && artist[0] && album && album[0]) {
        String better;
        if (coverChooseVariant(artist, album, jpegBuf, jpegSize, better)) {
            size_t altSize = 0;
            uint8_t* altBuf = downloadJpeg(better.c_str(), altSize);
            if (altBuf && altSize) {
                heap_caps_free(jpegBuf);
                jpegBuf = altBuf;
                jpegSize = altSize;
            } else if (altBuf) {
                heap_caps_free(altBuf);
            }
        }
    }

    PipelineResult res = processJpegBuffer(jpegBuf, jpegSize, overlayArtist, overlayAlbum);

    // Cache to history only once we know the JPEG actually decodes.  Caching
    // first would leave undecodable artwork on the SD card forever, where the
    // idle gallery would pick it and fail on every rotation.
    if (res != PIPE_DECODE_FAILED && artist && artist[0] && title && title[0]) {
        sdHistorySave(artist, title, album, jpegBuf, jpegSize);
    }
    heap_caps_free(jpegBuf);

    if (res == PIPE_OK) {
        activityLog("Artwork render complete");
        return true;
    }

    if (res == PIPE_RESOURCE_FAILED) {
        activityLog("Artwork render failed: out of memory");
        return false;
    }

    // JPEG wasn't decodable — show placeholder with track info
    Serial.println("[Pipeline] Artwork decode failed — showing placeholder");
    if (artist && artist[0]) {
        bool shown = pipelineShowPlaceholder(artist, album);
        activityLog(shown ? "Artwork fallback: placeholder shown"
                          : "Artwork fallback failed: placeholder");
        return shown;
    }
    activityLog("Artwork decode failed (no fallback metadata)");
    return false;
}

bool pipelineProcessFile(const char* path) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[Pipeline] Cannot open %s\n", path);
        return false;
    }

    size_t fSize = f.size();
    if (fSize == 0) {
        f.close();
        Serial.printf("[Pipeline] %s is empty\n", path);
        return false;
    }

    uint8_t* jpegBuf = (uint8_t*)heap_caps_malloc(fSize, MALLOC_CAP_SPIRAM);
    if (!jpegBuf) {
        f.close();
        Serial.println("[Pipeline] PSRAM alloc failed for file");
        return false;
    }
    size_t got = f.readBytes((char*)jpegBuf, fSize);
    f.close();
    if (got != fSize) {
        Serial.printf("[Pipeline] Short read on %s (%u/%u bytes)\n",
                      path, (unsigned)got, (unsigned)fSize);
        heap_caps_free(jpegBuf);
        return false;
    }

    PipelineResult res = processJpegBuffer(jpegBuf, fSize);
    heap_caps_free(jpegBuf);
    return res == PIPE_OK;
}

void pipelineShowTestPattern() {
    // 7 horizontal color bands across the 480×800 display
    static const char* COLOR_NAMES[EPD_COLORS] = {
        "Black", "White", "Green", "Blue", "Red", "Yellow"
    };

    size_t packedSize = (EPD_WIDTH * EPD_HEIGHT) / 2;
    uint8_t* packedBuf = (uint8_t*)heap_caps_malloc(packedSize, MALLOC_CAP_SPIRAM);
    if (!packedBuf) {
        Serial.println("[Pipeline] Test pattern alloc failed");
        return;
    }

    int bandH = EPD_HEIGHT / EPD_COLORS; // ~114px per band
    for (int c = 0; c < EPD_COLORS; c++) {
        int yStart = c * bandH;
        int yEnd   = (c == EPD_COLORS - 1) ? EPD_HEIGHT : yStart + bandH;
        uint8_t packed = (c << 4) | c; // both nibbles same color
        for (int y = yStart; y < yEnd; y++) {
            int rowStart = (y * EPD_WIDTH) / 2;
            memset(packedBuf + rowStart, packed, EPD_WIDTH / 2);
        }
        Serial.printf("[Test] Band %d: %s (index %d, y %d-%d)\n", c, COLOR_NAMES[c], c, yStart, yEnd - 1);
    }

    displayShowImage(packedBuf);
    heap_caps_free(packedBuf);
    Serial.println("[Test] Color test pattern displayed");
}

void pipelineShowDitherTest() {
    // Generate an RGB888 image with color swatches, then run it through
    // the actual dither pipeline. This reveals exactly how the dithering
    // algorithm handles various colors including out-of-gamut ones.
    //
    // Layout (480 wide × 800 tall):
    //   Row 0 (0-99):     6 pure palette colors (undithered reference)
    //   Row 1 (100-199):  Dithered mixes: R+B, R+W, B+W, R+Y, G+W, B+R dark
    //   Row 2 (200-299):  Purple gradient (light → dark)
    //   Row 3 (300-399):  Pink/skin gradient (light → dark)
    //   Row 4 (400-499):  Violet/magenta shades
    //   Row 5 (500-599):  Grey gradient (tests luminance dithering)
    //   Row 6 (600-699):  Orange / brown / warm tones
    //   Row 7 (700-799):  Teal / cyan / cool tones

    const int W = EPD_WIDTH;   // 480
    const int H = EPD_HEIGHT;  // 800
    const int ROW_H = 100;
    const int COLS = 6;
    const int COL_W = W / COLS; // 80

    size_t rgbSize = (size_t)W * H * 3;
    uint8_t* rgb = (uint8_t*)heap_caps_malloc(rgbSize, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        Serial.println("[DitherTest] RGB alloc failed");
        return;
    }

    // Helper to fill a rectangular swatch
    auto fillRect = [&](int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        for (int y = y0; y < y0 + h && y < H; y++) {
            for (int x = x0; x < x0 + w && x < W; x++) {
                int i = (y * W + x) * 3;
                rgb[i] = r; rgb[i+1] = g; rgb[i+2] = b;
            }
        }
    };

    // Clear to white
    memset(rgb, 0xD8, rgbSize);

    // ── Row 0: Pure palette colors (flat RGB matching palette values) ──
    fillRect(0*COL_W, 0, COL_W, ROW_H, 0x10, 0x10, 0x12); // Black
    fillRect(1*COL_W, 0, COL_W, ROW_H, 0xD8, 0xDA, 0xD4); // White
    fillRect(2*COL_W, 0, COL_W, ROW_H, 0x30, 0x66, 0x58); // Green
    fillRect(3*COL_W, 0, COL_W, ROW_H, 0x38, 0x68, 0xC0); // Blue
    fillRect(4*COL_W, 0, COL_W, ROW_H, 0x9C, 0x30, 0x2C); // Red
    fillRect(5*COL_W, 0, COL_W, ROW_H, 0xC8, 0xB8, 0x30); // Yellow

    // ── Row 1: Dithered 50/50 mixes (target RGB = average of two palette colors) ──
    fillRect(0*COL_W, 100, COL_W, ROW_H, 0x6A, 0x4C, 0x76); // Red+Blue avg (purple)
    fillRect(1*COL_W, 100, COL_W, ROW_H, 0xBA, 0x85, 0x80); // Red+White avg (pink)
    fillRect(2*COL_W, 100, COL_W, ROW_H, 0x88, 0xA1, 0xCA); // Blue+White avg (light blue)
    fillRect(3*COL_W, 100, COL_W, ROW_H, 0xB2, 0x74, 0x2E); // Red+Yellow avg (orange)
    fillRect(4*COL_W, 100, COL_W, ROW_H, 0x84, 0xA0, 0x96); // Green+White avg
    fillRect(5*COL_W, 100, COL_W, ROW_H, 0x34, 0x67, 0x8C); // Blue+Green avg (teal)

    // ── Row 2: Purple gradient (light purple → deep purple → dark purple) ──
    for (int c = 0; c < COLS; c++) {
        // Vary from light lavender to deep purple
        uint8_t r = 200 - c * 25;  // 200 → 75
        uint8_t g = 180 - c * 30;  // 180 → 30
        uint8_t b = 220 - c * 15;  // 220 → 145
        fillRect(c * COL_W, 200, COL_W, ROW_H, r, g, b);
    }

    // ── Row 3: Pink/skin gradient ──
    for (int c = 0; c < COLS; c++) {
        uint8_t r = 240 - c * 20;  // 240 → 140
        uint8_t g = 200 - c * 25;  // 200 → 75
        uint8_t b = 190 - c * 20;  // 190 → 90
        fillRect(c * COL_W, 300, COL_W, ROW_H, r, g, b);
    }

    // ── Row 4: Violet/magenta shades ──
    fillRect(0*COL_W, 400, COL_W, ROW_H, 180,  80, 180); // Magenta
    fillRect(1*COL_W, 400, COL_W, ROW_H, 140,  60, 160); // Deep violet
    fillRect(2*COL_W, 400, COL_W, ROW_H, 160, 100, 200); // Lavender
    fillRect(3*COL_W, 400, COL_W, ROW_H, 120,  40, 140); // Dark purple
    fillRect(4*COL_W, 400, COL_W, ROW_H, 200, 100, 180); // Light magenta
    fillRect(5*COL_W, 400, COL_W, ROW_H, 100,  20, 120); // Very dark purple

    // ── Row 5: Grey gradient (tests luminance dithering) ──
    for (int c = 0; c < COLS; c++) {
        uint8_t v = 30 + c * 40;  // 30 → 230
        fillRect(c * COL_W, 500, COL_W, ROW_H, v, v, v);
    }

    // ── Row 6: Orange / brown / warm tones ──
    fillRect(0*COL_W, 600, COL_W, ROW_H, 220, 160,  60); // Warm orange
    fillRect(1*COL_W, 600, COL_W, ROW_H, 180, 120,  40); // Brown
    fillRect(2*COL_W, 600, COL_W, ROW_H, 240, 200, 140); // Cream
    fillRect(3*COL_W, 600, COL_W, ROW_H, 160,  80,  30); // Dark brown
    fillRect(4*COL_W, 600, COL_W, ROW_H, 240, 120,  60); // Bright orange
    fillRect(5*COL_W, 600, COL_W, ROW_H, 200, 180, 160); // Beige/skin

    // ── Row 7: Teal / cyan / cool tones ──
    fillRect(0*COL_W, 700, COL_W, ROW_H,  60, 180, 180); // Cyan
    fillRect(1*COL_W, 700, COL_W, ROW_H,  40, 120, 140); // Dark teal
    fillRect(2*COL_W, 700, COL_W, ROW_H, 140, 200, 220); // Light sky
    fillRect(3*COL_W, 700, COL_W, ROW_H,  80, 140, 100); // Sage green
    fillRect(4*COL_W, 700, COL_W, ROW_H,  40,  80, 120); // Navy
    fillRect(5*COL_W, 700, COL_W, ROW_H, 100, 160, 200); // Steel blue

    Serial.println("[DitherTest] RGB test image generated, dithering...");

    // Dither through the actual pipeline
    size_t packedSize = (size_t)(W * H) / 2;
    uint8_t* packed = (uint8_t*)heap_caps_malloc(packedSize, MALLOC_CAP_SPIRAM);
    if (!packed) {
        Serial.println("[DitherTest] Packed alloc failed");
        heap_caps_free(rgb);
        return;
    }
    memset(packed, 0, packedSize);

    ditherFloydSteinberg(rgb, packed, W, H, renderProfile(g_app.settings.render_profile));
    heap_caps_free(rgb);

    displayShowImage(packed);
    heap_caps_free(packed);
    Serial.println("[DitherTest] Dither test pattern displayed");
}

// ─── Palette calibration card ───
//
// One row per pigment, each flanked by its own black and white reference chips.
// Everything is written straight to palette indices, bypassing the dither, so
// each area is exactly one pigment: what you photograph is ground truth rather
// than an optical mix.
//
//     [K] [====== pigment 0 (black)  ======] [W]
//     [K] [====== pigment 1 (white)  ======] [W]
//     [K] [====== pigment 2 (green)  ======] [W]
//     ...
//
// The per-row references are the important part: they let the sampler correct
// exposure and white balance *locally*, cancelling any illumination gradient or
// lens vignetting across the card. See simulator/calibrate_from_photo.py.
void pipelineShowCalibrationCard() {
    size_t packedSize = (EPD_WIDTH * EPD_HEIGHT) / 2;
    uint8_t* packed = (uint8_t*)heap_caps_malloc(packedSize, MALLOC_CAP_SPIRAM);
    if (!packed) {
        Serial.println("[Calibration] Packed alloc failed");
        return;
    }
    memset(packed, 0x11, packedSize); // white field

    auto setPixel = [&](int x, int y, uint8_t idx) {
        if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
        int pi = y * EPD_WIDTH + x;
        int bi = pi / 2;
        if (pi & 1) packed[bi] = (packed[bi] & 0xF0) | (idx & 0x0F);
        else        packed[bi] = (packed[bi] & 0x0F) | (idx << 4);
    };

    auto fillRect = [&](int x0, int y0, int w, int h, uint8_t idx) {
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++)
                setPixel(x, y, idx);
        // Keyline outside the rectangle, so the white chip and the white field
        // stay distinguishable and a bad crop is visible in the photo.
        for (int t = 0; t < CAL_KEYLINE; t++) {
            for (int x = x0 - t - 1; x <= x0 + w + t; x++) {
                setPixel(x, y0 - t - 1, 0);
                setPixel(x, y0 + h + t, 0);
            }
            for (int y = y0 - t - 1; y <= y0 + h + t; y++) {
                setPixel(x0 - t - 1, y, 0);
                setPixel(x0 + w + t, y, 0);
            }
        }
    };

    const int xLeft  = CAL_MARGIN_X;
    const int xPatch = xLeft + CAL_CHIP_W + CAL_CHIP_GAP;
    const int xRight = xPatch + CAL_PATCH_W + CAL_CHIP_GAP;
    const int half = CAL_ROW_H / CAL_CHIP_HALVES;

    for (int c = 0; c < EPD_COLORS; c++) {
        int y0 = CAL_MARGIN_Y + c * (CAL_ROW_H + CAL_ROW_GAP);

        // Mirrored: black over white on the left, white over black on the
        // right, so each reference averages to the pigment's own centroid.
        fillRect(xLeft,  y0,        CAL_CHIP_W, half, 0);
        fillRect(xLeft,  y0 + half, CAL_CHIP_W, half, 1);
        fillRect(xRight, y0,        CAL_CHIP_W, half, 1);
        fillRect(xRight, y0 + half, CAL_CHIP_W, half, 0);

        fillRect(xPatch, y0, CAL_PATCH_W, CAL_ROW_H, (uint8_t)c);
    }

    Serial.printf("[Calibration] Card: %d rows, pigment x=%d w=%d, "
                  "reference columns at x=%d and x=%d\n",
                  EPD_COLORS, xPatch, CAL_PATCH_W, xLeft, xRight);

    displayShowImage(packed);
    heap_caps_free(packed);
    Serial.println("[Calibration] Card displayed — photograph it square-on in even light");
    activityLog("Calibration card displayed");
}
