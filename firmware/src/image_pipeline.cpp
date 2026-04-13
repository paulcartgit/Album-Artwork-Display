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

// ─── Compute average color of image edges (for background fill) ───
static void averageEdgeColor(const uint8_t* src, int w, int h, uint8_t& rOut, uint8_t& gOut, uint8_t& bOut) {
    uint32_t rSum = 0, gSum = 0, bSum = 0, count = 0;
    // Sample top/bottom rows and left/right columns
    for (int x = 0; x < w; x++) {
        // Top row
        int i = x * 3;
        rSum += src[i]; gSum += src[i+1]; bSum += src[i+2]; count++;
        // Bottom row
        i = ((h-1) * w + x) * 3;
        rSum += src[i]; gSum += src[i+1]; bSum += src[i+2]; count++;
    }
    for (int y = 1; y < h - 1; y++) {
        // Left column
        int i = (y * w) * 3;
        rSum += src[i]; gSum += src[i+1]; bSum += src[i+2]; count++;
        // Right column
        i = (y * w + w - 1) * 3;
        rSum += src[i]; gSum += src[i+1]; bSum += src[i+2]; count++;
    }
    rOut = rSum / count;
    gOut = gSum / count;
    bOut = bSum / count;
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

    // Compute edge color for background fill
    uint8_t bgR, bgG, bgB;
    averageEdgeColor(g_decodeBuf, imgW, imgH, bgR, bgG, bgB);

    // Fill entire canvas with background color
    for (int i = 0; i < EPD_WIDTH * EPD_HEIGHT; i++) {
        scaledBuf[i * 3]     = bgR;
        scaledBuf[i * 3 + 1] = bgG;
        scaledBuf[i * 3 + 2] = bgB;
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

    // 4. Dither to 7-colour packed buffer
    size_t packedSize = (EPD_WIDTH * EPD_HEIGHT) / 2;
    uint8_t* packedBuf = (uint8_t*)heap_caps_calloc(packedSize, 1, MALLOC_CAP_SPIRAM);
    if (!packedBuf) {
        heap_caps_free(scaledBuf);
        Serial.println("[Pipeline] Packed buffer alloc failed");
        return false;
    }

    if (g_settings.use_dithering) {
        ditherFloydSteinberg(scaledBuf, packedBuf, EPD_WIDTH, EPD_HEIGHT);
    } else {
        quantizeNearest(scaledBuf, packedBuf, EPD_WIDTH, EPD_HEIGHT);
    }
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
