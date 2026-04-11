#include "image_pipeline.h"
#include "config.h"
#include "dither.h"
#include "display.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <TJpg_Decoder.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

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

// ─── Nearest-neighbour scale & center-crop ───
static void scaleAndCrop(const uint8_t* src, int srcW, int srcH,
                         uint8_t* dst, int dstW, int dstH) {
    float scaleX = (float)dstW / srcW;
    float scaleY = (float)dstH / srcH;
    float scale  = (scaleX > scaleY) ? scaleX : scaleY; // fill & crop

    int scaledW  = (int)(srcW * scale);
    int scaledH  = (int)(srcH * scale);
    int offsetX  = (scaledW - dstW) / 2;
    int offsetY  = (scaledH - dstH) / 2;

    for (int y = 0; y < dstH; y++) {
        for (int x = 0; x < dstW; x++) {
            int sX = (int)((x + offsetX) / scale);
            int sY = (int)((y + offsetY) / scale);
            sX = constrain(sX, 0, srcW - 1);
            sY = constrain(sY, 0, srcH - 1);

            int si = (sY * srcW + sX) * 3;
            int di = (y * dstW + x) * 3;
            dst[di]     = src[si];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
        }
    }
}

// ─── Core: decode JPEG buffer → dither → display ───
static bool processJpegBuffer(uint8_t* jpegBuf, size_t jpegSize) {
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

    // 3. Scale & crop to display size
    uint8_t* scaledBuf = (uint8_t*)heap_caps_malloc((size_t)EPD_WIDTH * EPD_HEIGHT * 3, MALLOC_CAP_SPIRAM);
    if (!scaledBuf) {
        heap_caps_free(g_decodeBuf);
        g_decodeBuf = nullptr;
        Serial.println("[Pipeline] Scaled buffer alloc failed");
        return false;
    }

    if (imgW == EPD_WIDTH && imgH == EPD_HEIGHT) {
        memcpy(scaledBuf, g_decodeBuf, (size_t)EPD_WIDTH * EPD_HEIGHT * 3);
    } else {
        scaleAndCrop(g_decodeBuf, imgW, imgH, scaledBuf, EPD_WIDTH, EPD_HEIGHT);
    }
    heap_caps_free(g_decodeBuf);
    g_decodeBuf = nullptr;

    // 4. Dither to 7-colour packed buffer
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

bool pipelineProcessUrl(const char* url) {
    size_t jpegSize;
    uint8_t* jpegBuf = downloadJpeg(url, jpegSize);
    if (!jpegBuf || jpegSize == 0) return false;
    return processJpegBuffer(jpegBuf, jpegSize); // takes ownership of jpegBuf
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
