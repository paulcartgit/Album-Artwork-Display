#include "display.h"
#include "config.h"

#define GxEPD2_DISPLAY_CLASS GxEPD2_7c
#define GxEPD2_DRIVER_CLASS  GxEPD2_730c_GDEP073E01

#include <GxEPD2_7C.h>
#include <SPI.h>
#include <esp_heap_caps.h>

// Page buffer: HEIGHT/4 = 120 rows. Each row = 800 pixels × 4bpp / 8 = 400 bytes.
// Page buffer total = 120 × 400 = 48,000 bytes — fits in SRAM.
static GxEPD2_7C<GxEPD2_730c_GDEP073E01, GxEPD2_730c_GDEP073E01::HEIGHT / 4> epd(
    GxEPD2_730c_GDEP073E01(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

bool displayInit() {
    SPI.begin(EPD_CLK, -1, EPD_MOSI, EPD_CS);
    epd.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    epd.init(115200, true, 50, false);
    epd.setRotation(3); // portrait: GFX viewport 480w × 800h (USB at top)
    Serial.println("[Display] Initialized 7.3\" 7-color GDEP073E01");
    return true;
}

void displayShowImage(const uint8_t* packedBuffer) {
    // packedBuffer: EPD_WIDTH×EPD_HEIGHT (480×800) at 4bpp, 2 pixels/byte
    // Panel native: 800×480.  Rotation 3: src(sx,sy) → native(sy, 479-sx)

    const int panelW = 800;
    const int panelH = 480;
    size_t nativeSize = (size_t)panelW * panelH / 2;

    uint8_t* native = (uint8_t*)heap_caps_malloc(nativeSize, MALLOC_CAP_SPIRAM);
    if (!native) {
        Serial.println("[Display] Native buffer alloc failed");
        return;
    }
    memset(native, 0x11, nativeSize); // white fill

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

    epd.epd2.writeNative(native, nullptr, 0, 0, panelW, panelH);
    epd.epd2.refresh();
    Serial.println("[Display] Refresh complete");

    heap_caps_free(native);
}

void displayShowMessage(const char* msg) {
    epd.setFullWindow();
    epd.firstPage();
    do {
        epd.fillScreen(GxEPD_WHITE);
        epd.setTextColor(GxEPD_BLACK);
        epd.setTextSize(3);
        epd.setCursor(20, EPD_HEIGHT / 2);
        epd.print(msg);
    } while (epd.nextPage());
    Serial.printf("[Display] Message: %s\n", msg);
}

void displayClear() {
    epd.clearScreen(GxEPD_WHITE);
}
