#include "image_pipeline.h"
#include "config.h"
#include "dither.h"
#include "display.h"
#include "sd_manager.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <TJpg_Decoder.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans18pt7b.h>

extern Settings g_settings;

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

static inline int triWave(int v, int period) {
    int m = v % period;
    if (m < 0) m += period;
    int half = period / 2;
    return (m < half) ? m : (period - m);
}

constexpr int NOISE_X_MULTIPLIER = 13;
constexpr int NOISE_Y_MULTIPLIER = 7;
constexpr int NOISE_MASK = 0x1F;
constexpr int NOISE_BIAS_OFFSET = 16;
constexpr uint32_t BASE_NEUTRAL_WEIGHT = 16u;
constexpr int PATTERN_COLOR_COUNT = 3;
constexpr int STRIPE_SLOPE_DIV = 2;
constexpr int STRIPE_WIDTH = 28;
constexpr int WAVE_A_X_SCALE = 1;
constexpr int WAVE_A_Y_SCALE = 3;
constexpr int WAVE_A_PERIOD = 96;
constexpr int WAVE_B_X_SCALE = 2;
constexpr int WAVE_B_Y_SCALE = 1;
constexpr int WAVE_B_PERIOD = 72;
constexpr int WAVE_BAND_DIV = 18;
constexpr int RING_SPACING = 24;

static void extractKeyColors(const uint8_t* src, int w, int h, uint8_t colors[3][3]) {
    uint8_t edgeR, edgeG, edgeB;
    averageEdgeColor(src, w, h, edgeR, edgeG, edgeB);
    for (int i = 0; i < 3; i++) {
        colors[i][0] = edgeR;
        colors[i][1] = edgeG;
        colors[i][2] = edgeB;
    }

    // Keep histogram buffers static to avoid large temporary stack usage.
    static uint32_t wSum[256];
    static uint32_t rSum[256];
    static uint32_t gSum[256];
    static uint32_t bSum[256];
    memset(wSum, 0, sizeof(wSum));
    memset(rSum, 0, sizeof(rSum));
    memset(gSum, 0, sizeof(gSum));
    memset(bSum, 0, sizeof(bSum));

    const int stepX = max(1, w / 48);
    const int stepY = max(1, h / 48);

    for (int y = 0; y < h; y += stepY) {
        for (int x = 0; x < w; x += stepX) {
            int i = (y * w + x) * 3;
            uint8_t r = src[i];
            uint8_t g = src[i + 1];
            uint8_t b = src[i + 2];
            uint8_t mx = max(max(r, g), b);
            uint8_t mn = min(min(r, g), b);
            int sat = (mx > 0) ? ((mx - mn) * 255) / mx : 0;
            // Base weight keeps neutrals represented; squared saturation strongly
            // favors vivid tones so the pattern uses the artwork's key colors.
            uint32_t weight = BASE_NEUTRAL_WEIGHT + (uint32_t)((sat * sat) / 255);

            // RGB332 quantization: top 3 bits red, top 3 bits green, top 2 bits blue.
            int bin = (r & 0xE0) | ((g & 0xE0) >> 3) | (b >> 6);
            wSum[bin] += weight;
            rSum[bin] += (uint32_t)r * weight;
            gSum[bin] += (uint32_t)g * weight;
            bSum[bin] += (uint32_t)b * weight;
        }
    }

    bool used[256] = {false};
    for (int c = 0; c < 3; c++) {
        int bestBin = -1;
        uint32_t bestWeight = 0;
        for (int i = 0; i < 256; i++) {
            if (used[i]) continue;
            if (wSum[i] > bestWeight) {
                bestWeight = wSum[i];
                bestBin = i;
            }
        }
        if (bestBin < 0 || bestWeight == 0) break;
        used[bestBin] = true;
        colors[c][0] = (uint8_t)(rSum[bestBin] / bestWeight);
        colors[c][1] = (uint8_t)(gSum[bestBin] / bestWeight);
        colors[c][2] = (uint8_t)(bSum[bestBin] / bestWeight);
    }
}

static void applyBackgroundStyle(uint8_t* canvas, int pixelCount) {
    if (g_settings.bg_style == 1) {
        // Wash out: blend toward white
        for (int i = 0; i < pixelCount * 3; i++)
            canvas[i] = (uint8_t)(canvas[i] + (255 - canvas[i]) * 45 / 100);
    } else {
        // Darken: dim to 55%
        for (int i = 0; i < pixelCount * 3; i++)
            canvas[i] = (uint8_t)(canvas[i] * 55 / 100);
    }
}

// ─── Render text into RGB888 buffer using Adafruit GFX ───
// Render a single line of text centred in a horizontal band.
// 2× supersampled for anti-aliased output on the 6-colour e-ink display.
static void renderTextBand(uint8_t* rgb, int canvasW, int canvasH,
                           const char* text,
                           const GFXfont* font, int initScale, int minScale,
                           int textAreaY, int textAreaH,
                           uint8_t bgR, uint8_t bgG, uint8_t bgB) {
    int ssW = canvasW * 2;
    int ssH = textAreaH * 2;
    GFXcanvas1 canvas(ssW, ssH);
    canvas.fillScreen(0);
    canvas.setTextColor(1);
    canvas.setTextWrap(false);

    canvas.setFont(font);
    canvas.setTextSize(initScale);
    int16_t x1, y1; uint16_t tw, th;

    String str(text);
    int scale = initScale;
    canvas.getTextBounds(str.c_str(), 0, 0, &x1, &y1, &tw, &th);
    while (tw > (uint16_t)(ssW - 60) && scale > minScale) {
        scale--;
        canvas.setTextSize(scale);
        canvas.getTextBounds(str.c_str(), 0, 0, &x1, &y1, &tw, &th);
    }
    while (tw > (uint16_t)(ssW - 60) && str.length() > 4) {
        str = str.substring(0, str.length() - 2);
        String test = str + "...";
        canvas.getTextBounds(test.c_str(), 0, 0, &x1, &y1, &tw, &th);
        if (tw <= (uint16_t)(ssW - 60)) { str = test; break; }
    }

    canvas.setFont(font);
    canvas.setTextSize(scale);
    canvas.getTextBounds(str.c_str(), 0, 0, &x1, &y1, &tw, &th);
    int textX = (ssW - tw) / 2 - x1;
    int textY = (ssH - th) / 2 - y1;
    canvas.setCursor(textX, textY);
    canvas.print(str);

    // Sample actual background pixels in the band to pick text colour
    long rSum = 0, gSum = 0, bSum = 0;
    int samples = 0;
    int step = 4; // sample every 4th pixel for speed
    for (int y = 0; y < textAreaH; y += step) {
        for (int x = 0; x < canvasW; x += step) {
            int di = ((textAreaY + y) * canvasW + x) * 3;
            rSum += rgb[di]; gSum += rgb[di + 1]; bSum += rgb[di + 2];
            samples++;
        }
    }
    int avgR = rSum / samples, avgG = gSum / samples, avgB = bSum / samples;
    int brightness = (avgR * 299 + avgG * 587 + avgB * 114) / 1000;
    uint8_t textR, textG, textB;
    if (brightness < 128) {
        textR = 255; textG = 255; textB = 255;
    } else {
        textR = 0; textG = 0; textB = 0;
    }

    for (int y = 0; y < textAreaH; y++) {
        for (int x = 0; x < canvasW; x++) {
            int count = canvas.getPixel(x * 2,     y * 2)
                      + canvas.getPixel(x * 2 + 1, y * 2)
                      + canvas.getPixel(x * 2,     y * 2 + 1)
                      + canvas.getPixel(x * 2 + 1, y * 2 + 1);
            if (count == 0) continue;
            int di = ((textAreaY + y) * canvasW + x) * 3;
            if (count == 4) {
                rgb[di]     = textR;
                rgb[di + 1] = textG;
                rgb[di + 2] = textB;
            } else {
                float alpha = count * 0.25f;
                rgb[di]     = (uint8_t)(rgb[di]     + (textR - rgb[di])     * alpha);
                rgb[di + 1] = (uint8_t)(rgb[di + 1] + (textG - rgb[di + 1]) * alpha);
                rgb[di + 2] = (uint8_t)(rgb[di + 2] + (textB - rgb[di + 2]) * alpha);
            }
        }
    }
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

    canvas.setFont(font);
    canvas.setTextSize(initScale);
    int16_t x1, y1; uint16_t tw, th;

    String str(text);
    int scale = initScale;
    canvas.getTextBounds(str.c_str(), 0, 0, &x1, &y1, &tw, &th);
    while (tw > (uint16_t)(ssW - 60) && scale > minScale) {
        scale--;
        canvas.setTextSize(scale);
        canvas.getTextBounds(str.c_str(), 0, 0, &x1, &y1, &tw, &th);
    }
    while (tw > (uint16_t)(ssW - 60) && str.length() > 4) {
        str = str.substring(0, str.length() - 2);
        String test = str + "...";
        canvas.getTextBounds(test.c_str(), 0, 0, &x1, &y1, &tw, &th);
        if (tw <= (uint16_t)(ssW - 60)) { str = test; break; }
    }

    canvas.setFont(font);
    canvas.setTextSize(scale);
    canvas.getTextBounds(str.c_str(), 0, 0, &x1, &y1, &tw, &th);
    int textX = (ssW - tw) / 2 - x1;
    int textY = (ssH - th) / 2 - y1;
    canvas.setCursor(textX, textY);
    canvas.print(str);

    // Sample actual background pixels in the band to pick text colour
    long rSum = 0, gSum = 0, bSum = 0;
    int samples = 0;
    int step = 4;
    for (int y = 0; y < textAreaH; y += step) {
        for (int x = 0; x < canvasW; x += step) {
            int di = ((textAreaY + y) * canvasW + x) * 3;
            rSum += rgb[di]; gSum += rgb[di + 1]; bSum += rgb[di + 2];
            samples++;
        }
    }
    int avgR = rSum / samples, avgG = gSum / samples, avgB = bSum / samples;
    int brightness = (avgR * 299 + avgG * 587 + avgB * 114) / 1000;
    uint8_t textIdx = (brightness < 128) ? 1 : 0; // White on dark, Black on light

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

    // Artist name — bold 24pt, scaled 4× on 2× canvas = ~66px effective
    canvas.setFont(&FreeSansBold24pt7b);
    canvas.setTextSize(4);
    int16_t x1, y1; uint16_t tw, th;

    // If artist name is too wide, try smaller scale, then truncate
    String artistStr(artist);
    int artistScale = 4;
    canvas.getTextBounds(artistStr.c_str(), 0, 0, &x1, &y1, &tw, &th);
    while (tw > (uint16_t)(ssW - 60) && artistScale > 2) {
        artistScale--;
        canvas.setTextSize(artistScale);
        canvas.getTextBounds(artistStr.c_str(), 0, 0, &x1, &y1, &tw, &th);
    }
    // Still too wide? Truncate with ellipsis
    while (tw > (uint16_t)(ssW - 60) && artistStr.length() > 4) {
        artistStr = artistStr.substring(0, artistStr.length() - 2);
        String test = artistStr + "...";
        canvas.getTextBounds(test.c_str(), 0, 0, &x1, &y1, &tw, &th);
        if (tw <= (uint16_t)(ssW - 60)) { artistStr = test; break; }
    }
    canvas.getTextBounds(artistStr.c_str(), 0, 0, &x1, &y1, &tw, &th);
    int artistH = th;

    // Album name — regular 18pt, scaled 3× on 2× canvas = ~36px effective
    canvas.setFont(&FreeSans18pt7b);
    canvas.setTextSize(3);
    String albumStr(album);
    int albumScale = 3;
    int16_t ax1, ay1; uint16_t atw, ath;
    canvas.getTextBounds(albumStr.c_str(), 0, 0, &ax1, &ay1, &atw, &ath);
    while (atw > (uint16_t)(ssW - 60) && albumScale > 2) {
        albumScale--;
        canvas.setTextSize(albumScale);
        canvas.getTextBounds(albumStr.c_str(), 0, 0, &ax1, &ay1, &atw, &ath);
    }
    while (atw > (uint16_t)(ssW - 60) && albumStr.length() > 4) {
        albumStr = albumStr.substring(0, albumStr.length() - 2);
        String test = albumStr + "...";
        canvas.getTextBounds(test.c_str(), 0, 0, &ax1, &ay1, &atw, &ath);
        if (atw <= (uint16_t)(ssW - 60)) { albumStr = test; break; }
    }
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
    int artistX = (ssW - tw) / 2 - x1;
    int artistY = startY - y1;
    canvas.setCursor(artistX, artistY);
    canvas.print(artistStr);

    // Draw album
    canvas.setFont(&FreeSans18pt7b);
    canvas.setTextSize(albumScale);
    canvas.getTextBounds(albumStr.c_str(), 0, 0, &ax1, &ay1, &atw, &ath);
    int albumX = (ssW - atw) / 2 - ax1;
    int albumY = startY + artistH + gap - ay1;
    canvas.setCursor(albumX, albumY);
    canvas.print(albumStr);

    // Determine text color: white on dark bg, black on light bg
    int brightness = (bgR * 299 + bgG * 587 + bgB * 114) / 1000;
    uint8_t textR, textG, textB;
    if (brightness < 128) {
        textR = 255; textG = 255; textB = 255;
    } else {
        textR = 0; textG = 0; textB = 0;
    }

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
static void enhanceForEink(uint8_t* rgb, int w, int h) {
    const float sharpenAmt   = 0.4f;   // unsharp mask strength
    const float contrastFact = 1.2f;   // 20 % contrast boost
    const float gamma        = 0.9f;   // < 1 lifts midtones slightly
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
    Serial.println("[Pipeline] Enhanced (sharpen+contrast+gamma)");
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

    applyBackgroundStyle(canvas, fillH * cW);

    Serial.println("[Pipeline] Blurred background fill applied");
}

static void fillPatternedBackground(uint8_t* canvas, int cW, int cH,
                                    const uint8_t* src, int sW, int sH,
                                    int fillH) {
    uint8_t colors[3][3];
    extractKeyColors(src, sW, sH, colors);
    float var = edgeVariance(src, sW, sH);

    int style = 0;
    if (var >= 1800.0f) style = 2;      // geometric ring style
    else if (var >= 800.0f) style = 1;  // wave bands
    else style = 0;                     // diagonal stripes

    for (int y = 0; y < fillH; y++) {
        for (int x = 0; x < cW; x++) {
            int band = 0;
            if (style == 0) {
                band = ((x + (y / STRIPE_SLOPE_DIV)) / STRIPE_WIDTH) % PATTERN_COLOR_COUNT;
            } else if (style == 1) {
                int wave = triWave(x * WAVE_A_X_SCALE + y * WAVE_A_Y_SCALE, WAVE_A_PERIOD)
                         + triWave(x * WAVE_B_X_SCALE - y * WAVE_B_Y_SCALE, WAVE_B_PERIOD);
                band = (wave / WAVE_BAND_DIV) % PATTERN_COLOR_COUNT;
            } else {
                int dx = abs(x - cW / 2);
                int dy = abs(y - fillH / 2);
                band = ((dx + dy) / RING_SPACING) % PATTERN_COLOR_COUNT;
            }

            int di = (y * cW + x) * 3;
            uint8_t r = colors[band][0];
            uint8_t g = colors[band][1];
            uint8_t b = colors[band][2];

            // Subtle paper-like texture to avoid flat digital bands
            int noise = ((x * NOISE_X_MULTIPLIER) ^ (y * NOISE_Y_MULTIPLIER)) & NOISE_MASK;
            int bias = noise - NOISE_BIAS_OFFSET; // -16..15
            canvas[di]     = constrain((int)r + bias, 0, 255);
            canvas[di + 1] = constrain((int)g + bias, 0, 255);
            canvas[di + 2] = constrain((int)b + bias, 0, 255);
        }
    }

    applyBackgroundStyle(canvas, fillH * cW);
    Serial.println("[Pipeline] Patterned background fill applied");
}

// ─── Core: decode JPEG buffer → scale → optional text → dither → display ───
static bool processJpegBuffer(uint8_t* jpegBuf, size_t jpegSize,
                              const char* artist = nullptr, const char* album = nullptr) {
    // 1. Get dimensions
    uint16_t imgW, imgH;
    JRESULT jr = TJpgDec.getJpgSize(&imgW, &imgH, jpegBuf, jpegSize);
    Serial.printf("[Pipeline] JPEG %dx%d (%u bytes) jd_prepare=%d\n", imgW, imgH, jpegSize, jr);
    Serial.printf("[Pipeline] Free PSRAM: %u, largest block: %u\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    if (imgW == 0 || imgH == 0) {
        Serial.println("[Pipeline] JPEG parse failed — invalid dimensions");
        heap_caps_free(jpegBuf);
        return false;
    }

    // 2. Decode to RGB888 in PSRAM
    g_decodeW = imgW;
    g_decodeH = imgH;
    g_decodeBuf = (uint8_t*)heap_caps_malloc((size_t)imgW * imgH * 3, MALLOC_CAP_SPIRAM);
    if (!g_decodeBuf) {
        Serial.printf("[Pipeline] Decode buffer alloc failed (%dx%dx3 = %u bytes)\n",
                      imgW, imgH, (unsigned)(imgW * imgH * 3));
        heap_caps_free(jpegBuf);
        return false;
    }

    TJpgDec.setCallback(tjpgCallback);
    TJpgDec.setJpgScale(1);
    TJpgDec.drawJpg(0, 0, jpegBuf, jpegSize);

    // JPEG buffer no longer needed
    heap_caps_free(jpegBuf);

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
        return false;
    }

    // Compute edge color for background fill (used as fallback)
    uint8_t bgR, bgG, bgB;
    averageEdgeColor(g_decodeBuf, imgW, imgH, bgR, bgG, bgB);

    // Background mode: 0 = always solid, 1 = always blur, 2 = auto-detect, 3 = patterned.
    bool useBlur = false;
    bool usePattern = false;
    if (g_settings.bg_mode == 0) {
        Serial.println("[Pipeline] Background: forced solid");
    } else if (g_settings.bg_mode == 1) {
        useBlur = true;
        Serial.println("[Pipeline] Background: forced blur");
    } else if (g_settings.bg_mode == 3) {
        usePattern = true;
        Serial.println("[Pipeline] Background: forced patterned");
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
    } else if (usePattern) {
        // Patterned background: palette-derived texture fill
        fillPatternedBackground(scaledBuf, EPD_WIDTH, EPD_HEIGHT,
                                g_decodeBuf, imgW, imgH, EPD_HEIGHT);
    } else {
        // Solid colour fill
        for (int i = 0; i < EPD_WIDTH * EPD_HEIGHT; i++) {
            scaledBuf[i * 3]     = bgR;
            scaledBuf[i * 3 + 1] = bgG;
            scaledBuf[i * 3 + 2] = bgB;
        }
    }

    // Scale artwork to fit art area, centred with shadow margin for decorated backgrounds.
    bool useDecoratedBg = useBlur || usePattern;
    const int shadowMarginV = useDecoratedBg ? 22 : 0;
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

    // Drop shadow — when blur/pattern background is active.
    if (useDecoratedBg) {
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
    heap_caps_free(g_decodeBuf);
    g_decodeBuf = nullptr;

    // 3.5. Pre-dither enhancement (sharpen + contrast + gamma)
    enhanceForEink(scaledBuf, EPD_WIDTH, EPD_HEIGHT);

    // 4. Dither to 6-colour packed buffer
    size_t packedSize = (EPD_WIDTH * EPD_HEIGHT) / 2;
    uint8_t* packedBuf = (uint8_t*)heap_caps_calloc(packedSize, 1, MALLOC_CAP_SPIRAM);
    if (!packedBuf) {
        heap_caps_free(scaledBuf);
        Serial.println("[Pipeline] Packed buffer alloc failed");
        return false;
    }

    ditherFloydSteinberg(scaledBuf, packedBuf, EPD_WIDTH, EPD_HEIGHT);

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

    return true;
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

    enhanceForEink(scaledBuf, EPD_WIDTH, EPD_HEIGHT);

    size_t packedSize = (EPD_WIDTH * EPD_HEIGHT) / 2;
    uint8_t* packedBuf = (uint8_t*)heap_caps_calloc(packedSize, 1, MALLOC_CAP_SPIRAM);
    if (!packedBuf) {
        heap_caps_free(scaledBuf);
        Serial.println("[Pipeline] Placeholder packed alloc failed");
        return false;
    }

    ditherFloydSteinberg(scaledBuf, packedBuf, EPD_WIDTH, EPD_HEIGHT);
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

    if (strncmp(url, "https", 5) == 0) {
        WiFiClientSecure* client = new WiFiClientSecure;
        client->setInsecure(); // album art — no cert verification needed
        http.begin(*client, url);
    } else {
        http.begin(url);
    }

    http.setTimeout(15000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Pipeline] HTTP %d from %s\n", code, url);
        http.end();
        return nullptr;
    }

    int contentLen = http.getSize();
    size_t allocSize = (contentLen > 0) ? (size_t)contentLen : 256 * 1024;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM);
    if (!buf) {
        Serial.println("[Pipeline] PSRAM alloc failed for download");
        http.end();
        return nullptr;
    }

    WiFiClient* stream = http.getStreamPtr();
    size_t total = 0;
    unsigned long deadline = millis() + 15000;
    while (http.connected() && total < allocSize && millis() < deadline) {
        size_t avail = stream->available();
        if (avail) {
            size_t chunk = min(avail, allocSize - total);
            size_t got = stream->readBytes(buf + total, chunk);
            total += got;
        } else {
            delay(1);
        }
        if (contentLen > 0 && (int)total >= contentLen) break;
    }
    http.end();

    outSize = total;
    Serial.printf("[Pipeline] Downloaded %u bytes\n", total);
    return buf;
}

// ─── Public API ───

bool pipelineProcessUrl(const char* url,
                        const char* overlayArtist, const char* overlayAlbum,
                        const char* artist, const char* title, const char* album) {
    size_t jpegSize;
    uint8_t* jpegBuf = downloadJpeg(url, jpegSize);
    if (!jpegBuf || jpegSize == 0) return false;

    // Save to album art history (before processJpegBuffer frees the buffer)
    if (artist && artist[0] && title && title[0]) {
        sdHistorySave(artist, title, album, jpegBuf, jpegSize);
    }

    if (processJpegBuffer(jpegBuf, jpegSize, overlayArtist, overlayAlbum))
        return true; // takes ownership of jpegBuf

    // JPEG wasn't decodable — show placeholder with track info
    Serial.println("[Pipeline] Artwork decode failed — showing placeholder");
    if (artist && artist[0]) {
        return pipelineShowPlaceholder(artist, album);
    }
    return false;
}

bool pipelineProcessFile(const char* path) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[Pipeline] Cannot open %s\n", path);
        return false;
    }

    size_t fSize = f.size();
    uint8_t* jpegBuf = (uint8_t*)heap_caps_malloc(fSize, MALLOC_CAP_SPIRAM);
    if (!jpegBuf) {
        f.close();
        Serial.println("[Pipeline] PSRAM alloc failed for file");
        return false;
    }
    f.readBytes((char*)jpegBuf, fSize);
    f.close();

    return processJpegBuffer(jpegBuf, fSize); // takes ownership
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
