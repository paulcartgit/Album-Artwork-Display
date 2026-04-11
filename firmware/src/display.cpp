#include "display.h"
#include "config.h"

#define GxEPD2_DISPLAY_CLASS GxEPD2_7c
#define GxEPD2_DRIVER_CLASS  GxEPD2_730c_ACeP_730

#include <GxEPD2_7C.h>
#include <SPI.h>

// Page buffer: HEIGHT/4 = 200 rows. Each row = 480 pixels × 4bpp / 8 = 240 bytes.
// Page buffer total = 200 × 240 = 48,000 bytes — fits in SRAM.
static GxEPD2_7C<GxEPD2_730c_ACeP_730, GxEPD2_730c_ACeP_730::HEIGHT / 4> epd(
    GxEPD2_730c_ACeP_730(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

bool displayInit() {
    SPI.begin(EPD_CLK, -1, EPD_MOSI, EPD_CS);
    epd.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    epd.init(115200, true, 50, false);
    epd.setRotation(0);
    Serial.println("[Display] Initialized 7.3\" 7-color ACeP");
    return true;
}

void displayShowImage(const uint8_t* packedBuffer) {
    // packedBuffer: 480×800 at 4 bits per pixel, 2 pixels per byte (high nibble first)
    // Total size: 480 * 800 / 2 = 192,000 bytes
    epd.epd2.writeImage(packedBuffer, 0, 0, EPD_WIDTH, EPD_HEIGHT);
    epd.epd2.refresh();
    Serial.println("[Display] Refresh complete");
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
