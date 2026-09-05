#include "display.h"
#include "config.h"

#define GxEPD2_DISPLAY_CLASS GxEPD2_7c
#define GxEPD2_DRIVER_CLASS  GxEPD2_730c_GDEP073E01

#include <GxEPD2_7C.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

// Page buffer: HEIGHT/4 = 120 rows. Each row = 800 pixels × 4bpp / 8 = 400 bytes.
// Page buffer total = 120 × 400 = 48,000 bytes — fits in SRAM.
static GxEPD2_7C<GxEPD2_730c_GDEP073E01, GxEPD2_730c_GDEP073E01::HEIGHT / 4> epd(
    GxEPD2_730c_GDEP073E01(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// Set for the duration of a refresh so other tasks can see the panel is busy.
static volatile bool g_refreshing = false;
static uint32_t g_refreshCount = 0;
static unsigned long g_lastRefreshMs = 0;

static uint8_t* g_lastFrame = nullptr;   // packed 4bpp copy of what is on screen

const uint8_t* displayCurrentFrame() { return g_lastFrame; }

uint32_t displayRefreshCount() { return g_refreshCount; }
void displaySetRefreshCount(uint32_t n) { g_refreshCount = n; }
unsigned long displayLastRefreshMs() { return g_lastRefreshMs; }

bool displayIsBusy() {
    return g_refreshing;
}

bool displayInit() {
    SPI.begin(EPD_CLK, -1, EPD_MOSI, EPD_CS);
    epd.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    epd.init(115200, true, 50, false);
    epd.setRotation(3); // portrait: GFX viewport 480w × 800h (USB at top)
    Serial.println("[Display] Initialized 7.3\" 7-color GDEP073E01");
    return true;
}

// Callback invoked during GxEPD2's busy-wait polling — keeps WiFi alive
static void busyYieldCallback(const void*) {
    // The main loop is blocked in here for the whole 20-25s refresh, so the
    // watchdog has to be fed from inside it.
    esp_task_wdt_reset();
    delay(10);
    yield();
}

// GxEPD2 gives this panel a 20 s busy timeout (see the constructor in
// GxEPD2_730c_GDEP073E01.cpp) but a full Spectra 6 refresh actually takes
// longer than that, and longer still when cold — the serial log shows it
// hitting the timeout at _refresh: 20001027 us on every update. GxEPD2 then
// gives up and returns while the panel is still cycling, so we report the
// image as displayed before it is, and the next SPI transaction can land
// mid-refresh. Wait it out ourselves.
static const unsigned long PANEL_SETTLE_TIMEOUT_MS = 45000;

static void waitUntilPanelIdle() {
    unsigned long start = millis();
    // BUSY is active LOW on the GDEP073E01
    while (digitalRead(EPD_BUSY) == LOW) {
        if (millis() - start > PANEL_SETTLE_TIMEOUT_MS) {
            Serial.printf("[Display] Panel still busy after %lums — giving up\n",
                          PANEL_SETTLE_TIMEOUT_MS);
            return;
        }
        esp_task_wdt_reset();
        delay(50);
        yield();
    }
    unsigned long waited = millis() - start;
    if (waited > 50) {
        Serial.printf("[Display] Panel settled %lums after GxEPD2 returned\n", waited);
    }
}

void displayShowImage(const uint8_t* packedBuffer) {
    // packedBuffer: EPD_WIDTH×EPD_HEIGHT (480×800) at 4bpp, 2 pixels/byte
    // Panel native: 800×480.  Rotation 3: src(sx,sy) → native(sy, 479-sx)
    // writeNative() applies _convert_to_native internally, so we pass GxEPD2 indices as-is.

    const int panelW = 800;
    const int panelH = 480;
    size_t nativeSize = (size_t)panelW * panelH / 2;

    uint8_t* native = (uint8_t*)heap_caps_malloc(nativeSize, MALLOC_CAP_SPIRAM);
    if (!native) {
        Serial.println("[Display] Native buffer alloc failed");
        return;
    }

    g_refreshing = true;

    // Keep a copy so the portal can serve exactly what the panel shows.
    size_t packedSize = (size_t)EPD_WIDTH * EPD_HEIGHT / 2;
    if (!g_lastFrame) g_lastFrame = (uint8_t*)heap_caps_malloc(packedSize, MALLOC_CAP_SPIRAM);
    if (g_lastFrame) memcpy(g_lastFrame, packedBuffer, packedSize);

    memset(native, 0x11, nativeSize); // white fill (index 1)

    // Rotate portrait → native landscape
    for (int sy = 0; sy < EPD_HEIGHT; sy++) {
        for (int sx = 0; sx < EPD_WIDTH; sx++) {
            // Read source 4bpp pixel
            int srcIdx = sy * (EPD_WIDTH / 2) + sx / 2;
            uint8_t color = (sx % 2 == 0)
                ? (packedBuffer[srcIdx] >> 4)
                : (packedBuffer[srcIdx] & 0x0F);

            // Map to native coordinates (rotation 3)
            int nx = sy;
            int ny = 479 - sx;
            int dstIdx = ny * (panelW / 2) + nx / 2;
            if (nx % 2 == 0) {
                native[dstIdx] = (native[dstIdx] & 0x0F) | (color << 4);
            } else {
                native[dstIdx] = (native[dstIdx] & 0xF0) | color;
            }
        }
    }

    // Write image data to panel controller RAM (fast SPI transfer)
    epd.epd2.writeNative(native, nullptr, 0, 0, panelW, panelH);
    heap_caps_free(native);

    // Refresh with busy callback — yields during ~15s wait so WiFi stays alive
    epd.epd2.setBusyCallback(busyYieldCallback);
    epd.epd2.refresh();
    epd.epd2.setBusyCallback(nullptr);
    waitUntilPanelIdle();

    g_refreshing = false;
    g_refreshCount++;
    g_lastRefreshMs = millis();
    Serial.printf("[Display] Refresh complete (%u total)\n", g_refreshCount);
}

void displayShowMessage(const char* msg) {
    g_refreshing = true;
    epd.setFullWindow();
    epd.firstPage();
    do {
        epd.fillScreen(GxEPD_WHITE);
        epd.setTextColor(GxEPD_BLACK);
        epd.setTextSize(3);

        // Split message by newlines, centre each line
        String text(msg);
        int lineCount = 1;
        for (int i = 0; i < (int)text.length(); i++)
            if (text[i] == '\n') lineCount++;

        int16_t x1, y1;
        uint16_t tw, th;
        // Measure a single line height
        epd.getTextBounds("A", 0, 0, &x1, &y1, &tw, &th);
        int lineH = th + 8; // line height with spacing
        int totalH = lineH * lineCount;
        int startY = (EPD_HEIGHT - totalH) / 2 + th; // baseline of first line

        int lineIdx = 0;
        int start = 0;
        for (int i = 0; i <= (int)text.length(); i++) {
            if (i == (int)text.length() || text[i] == '\n') {
                String line = text.substring(start, i);
                epd.getTextBounds(line.c_str(), 0, 0, &x1, &y1, &tw, &th);
                int cx = (EPD_WIDTH - tw) / 2 - x1;
                int cy = startY + lineIdx * lineH;
                epd.setCursor(cx, cy);
                epd.print(line);
                lineIdx++;
                start = i + 1;
            }
        }
    } while (epd.nextPage());
    waitUntilPanelIdle();
    g_refreshing = false;
    g_refreshCount++;
    g_lastRefreshMs = millis();
    Serial.printf("[Display] Message: %s\n", msg);
}

void displayClear() {
    g_refreshing = true;
    epd.clearScreen(GxEPD_WHITE);
    g_refreshing = false;
}
