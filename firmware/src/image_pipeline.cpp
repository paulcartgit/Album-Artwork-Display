#include "image_pipeline.h"
#include "config.h"
#include "dither.h"
#include "display.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <TJpg_Decoder.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

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

// ─── Render text into RGB888 buffer using Adafruit GFX ───
static void renderText(uint8_t* rgb, int canvasW, int canvasH,
                       const char* artist, const char* album,
                       int textAreaY, int textAreaH,
                       uint8_t bgR, uint8_t bgG, uint8_t bgB) {
    // Use a 1-bit canvas for text, then composite into RGB buffer
    GFXcanvas1 canvas(canvasW, textAreaH);
    canvas.fillScreen(0); // black background
    canvas.setTextColor(1);
    canvas.setTextWrap(false);

    // Artist name — larger font, centered
    canvas.setFont(&FreeSansBold12pt7b);
    int16_t x1, y1; uint16_t tw, th;
    canvas.getTextBounds(artist, 0, 0, &x1, &y1, &tw, &th);
    int artistX = (canvasW - tw) / 2 - x1;
    int artistY = 50 - y1; // ~50px from top of text area
    canvas.setCursor(artistX, artistY);
    canvas.print(artist);

    // Album name — smaller font, centered below artist
    canvas.setFont(&FreeSans9pt7b);
    canvas.getTextBounds(album, 0, 0, &x1, &y1, &tw, &th);
    int albumX = (canvasW - tw) / 2 - x1;
    int albumY = artistY + th + 30 - y1; // 30px gap
    canvas.setCursor(albumX, albumY);
    canvas.print(album);

    // Determine text color: white on dark bg, black on light bg
    int brightness = (bgR * 299 + bgG * 587 + bgB * 114) / 1000;
    uint8_t textR, textG, textB;
    if (brightness < 128) {
        textR = 255; textG = 255; textB = 255;
    } else {
        textR = 0; textG = 0; textB = 0;
    }

    // Composite 1-bit text into RGB888 buffer
    for (int y = 0; y < textAreaH; y++) {
        for (int x = 0; x < canvasW; x++) {
            int di = ((textAreaY + y) * canvasW + x) * 3;
            if (canvas.getPixel(x, y)) {
                rgb[di]     = textR;
                rgb[di + 1] = textG;
                rgb[di + 2] = textB;
            }
            // else: leave the background fill color as-is
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

    // Heavy box blur — 4 passes of radius-8 horizontal then vertical.
    // Uses a single row/col accumulator buffer (~1.5 KB).
    const int radius = 8;
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

    // Slight dim (70%) so sharp overlay pops
    for (int i = 0; i < fillH * cW * 3; i++) {
        canvas[i] = (uint8_t)(canvas[i] * 7 / 10);
    }

    Serial.println("[Pipeline] Blurred background fill applied");
}

// ─── Core: decode JPEG buffer → scale → optional text → dither → display ───
static bool processJpegBuffer(uint8_t* jpegBuf, size_t jpegSize,
                              const char* artist = nullptr, const char* album = nullptr) {
    // 1. Get dimensions
    uint16_t imgW, imgH;
    TJpgDec.getJpgSize(&imgW, &imgH, jpegBuf, jpegSize);
    Serial.printf("[Pipeline] JPEG %dx%d (%u bytes)\n", imgW, imgH, jpegSize);

    // 2. Decode to RGB888 in PSRAM
    g_decodeW = imgW;
    g_decodeH = imgH;
    g_decodeBuf = (uint8_t*)heap_caps_malloc((size_t)imgW * imgH * 3, MALLOC_CAP_SPIRAM);
    if (!g_decodeBuf) {
        Serial.println("[Pipeline] Decode buffer alloc failed");
        return false;
    }

    TJpgDec.setCallback(tjpgCallback);
    TJpgDec.setJpgScale(1);
    TJpgDec.drawJpg(0, 0, jpegBuf, jpegSize);

    // JPEG buffer no longer needed
    heap_caps_free(jpegBuf);

    // 3. Scale to display size
    bool showText = (artist && artist[0] && album && album[0]);
    // With text: artwork in top portion, text below
    // Without text: artwork fills entire canvas
    int artAreaH = showText ? EPD_WIDTH : EPD_HEIGHT; // 480 for text mode (square), 800 for full
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

    if (g_settings.blur_background) {
        // Blurred pillarbox: scale source to fill canvas, blur heavily, dim
        int fillH = showText ? artAreaH : EPD_HEIGHT;
        fillBlurredBackground(scaledBuf, EPD_WIDTH, EPD_HEIGHT,
                              g_decodeBuf, imgW, imgH, fillH);
    } else {
        // Solid colour fill
        for (int i = 0; i < EPD_WIDTH * EPD_HEIGHT; i++) {
            scaledBuf[i * 3]     = bgR;
            scaledBuf[i * 3 + 1] = bgG;
            scaledBuf[i * 3 + 2] = bgB;
        }
    }

    // Scale artwork to fit art area (no crop)
    float scaleX = (float)artAreaW / imgW;
    float scaleY = (float)artAreaH / imgH;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;

    int scaledW = (int)(imgW * scale);
    int scaledH = (int)(imgH * scale);
    int offsetX = (artAreaW - scaledW) / 2;
    int offsetY = showText ? (artAreaH - scaledH) / 2 : (EPD_HEIGHT - scaledH) / 2;

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

    // Render text overlay if requested
    if (showText) {
        int textAreaY = artAreaH; // starts right below artwork area
        int textAreaH = EPD_HEIGHT - artAreaH; // remaining 320px
        renderText(scaledBuf, EPD_WIDTH, EPD_HEIGHT, artist, album,
                   textAreaY, textAreaH, bgR, bgG, bgB);
    }

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
    heap_caps_free(scaledBuf);

    // 5. Push to display
    displayShowImage(packedBuf);
    heap_caps_free(packedBuf);

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

bool pipelineProcessUrl(const char* url, const char* artist, const char* album) {
    size_t jpegSize;
    uint8_t* jpegBuf = downloadJpeg(url, jpegSize);
    if (!jpegBuf || jpegSize == 0) return false;
    return processJpegBuffer(jpegBuf, jpegSize, artist, album); // takes ownership of jpegBuf
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
