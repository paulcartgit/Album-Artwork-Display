#include "audio_capture.h"
#include "activity_log.h"
#include "config.h"
#include <Wire.h>
#include <driver/i2s_std.h>

// ─── ES7210 register init (based on official Espressif es7210.c driver) ───

static bool es7210WriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(reg);
    Wire.write(val);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Serial.printf("[Audio] ES7210 I2C write failed reg=0x%02X err=%d\n", reg, err);
        return false;
    }
    return true;
}

static uint8_t es7210ReadReg(uint8_t reg) {
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)ES7210_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

static bool es7210Init() {
    // Verify ES7210 is on the I2C bus
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(0x00);
    uint8_t err = Wire.endTransmission(true);
    if (err != 0) {
        Serial.printf("[Audio] ES7210 not found at 0x%02X (I2C err=%d)\n", ES7210_ADDR, err);
        return false;
    }
    Serial.printf("[Audio] ES7210 detected at 0x%02X\n", ES7210_ADDR);

    // ── Init sequence matching Espressif ESP-BSP es7210_config_codec() exactly ──

    // Software reset
    es7210WriteReg(0x00, 0xFF);
    delay(20);
    // Config/hold mode (0x32) — chip does NOT start running yet!
    // (Previously we used 0x41 which starts the chip before config is written)
    es7210WriteReg(0x00, 0x32);
    delay(20);

    // Initialization timing
    es7210WriteReg(0x09, 0x30);  // Chip state cycle
    es7210WriteReg(0x0A, 0x30);  // Power-on state cycle

    // High-pass filter (DC offset removal) for ADC1-4
    es7210WriteReg(0x23, 0x2A);
    es7210WriteReg(0x22, 0x0A);
    es7210WriteReg(0x21, 0x2A);
    es7210WriteReg(0x20, 0x0A);

    // I2S format: 16-bit, I2S Philips standard
    es7210WriteReg(0x11, 0x60);
    // Standard I2S mode (no TDM)
    es7210WriteReg(0x12, 0x00);

    // Analog power: keep analog OFF during config (bit 7 set = powered down)
    // ESP-BSP uses 0xC3; our old 0x43 started analog immediately
    es7210WriteReg(0x40, 0xC3);

    // MIC bias voltage = 2.87V
    es7210WriteReg(0x41, 0x70);  // MIC1/2 bias
    es7210WriteReg(0x42, 0x70);  // MIC3/4 bias

    // MIC gain: 36dB all channels (gain=0x0E | enable=0x10 = 0x1E)
    es7210WriteReg(0x43, 0x1E);
    es7210WriteReg(0x44, 0x1E);
    es7210WriteReg(0x45, 0x1E);
    es7210WriteReg(0x46, 0x1E);

    // MIC power on
    es7210WriteReg(0x47, 0x08);
    es7210WriteReg(0x48, 0x08);
    es7210WriteReg(0x49, 0x08);
    es7210WriteReg(0x4A, 0x08);

    // Clock coefficients for 44.1kHz @ 11.2896MHz MCLK (from Espressif coeff table)
    es7210WriteReg(0x07, 0x20);  // OSR
    es7210WriteReg(0x02, 0xC1);  // adc_div=1, doubler=1, dll_bypass=1
    es7210WriteReg(0x04, 0x01);  // LRCK_DIVH  (divider = 0x0100 = 256)
    es7210WriteReg(0x05, 0x00);  // LRCK_DIVL

    // Power down DLL (it's bypassed via reg 0x02 bit 7, so don't waste power)
    // ESP-BSP uses 0x04; our old 0x00 powered on everything including unused DLL
    es7210WriteReg(0x06, 0x04);

    // MIC power rails ON
    es7210WriteReg(0x4B, 0x0F);
    es7210WriteReg(0x4C, 0x0F);

    // NOW enable the chip — transitions from config mode to running
    es7210WriteReg(0x00, 0x71);
    es7210WriteReg(0x00, 0x41);

    // Verify register readback
    uint8_t regVal = es7210ReadReg(0x00);
    Serial.printf("[Audio] ES7210 initialized (reg0x00=0x%02X)\n", regVal);

    // Dump key registers to verify init
    uint8_t r01 = es7210ReadReg(0x01);  // clock
    uint8_t r06 = es7210ReadReg(0x06);  // digital power
    uint8_t r11 = es7210ReadReg(0x11);  // I2S format
    uint8_t r12 = es7210ReadReg(0x12);  // TDM
    uint8_t r40 = es7210ReadReg(0x40);  // analog power
    uint8_t r43 = es7210ReadReg(0x43);  // MIC1 gain
    uint8_t r47 = es7210ReadReg(0x47);  // MIC1 power
    uint8_t r4B = es7210ReadReg(0x4B);  // MIC12 rail
    Serial.printf("[Audio] Regs: 01=%02X 06=%02X 11=%02X 12=%02X 40=%02X 43=%02X 47=%02X 4B=%02X\n",
        r01, r06, r11, r12, r40, r43, r47, r4B);
    activityLogf("Regs:01=%02X 06=%02X 11=%02X 12=%02X",
        r01, r06, r11, r12);
    activityLogf("Regs:40=%02X 43=%02X 47=%02X 4B=%02X",
        r40, r43, r47, r4B);

    return true;
}

// ─── I2S driver (ESP-IDF 5.x new API) ───

static i2s_chan_handle_t rx_handle = NULL;

bool audioInit() {
    Serial.println("[Audio] audioInit starting...");

    // Set a timeout on Wire to prevent hangs on stuck I2C bus
    Wire.setTimeOut(100); // 100ms timeout

    // Enable power amplifier pin (active high)
    pinMode(PA_ENABLE, OUTPUT);
    digitalWrite(PA_ENABLE, LOW); // keep amp off during recording

    // ── Start I2S FIRST — the ES7210 needs MCLK running before its I2C works ──
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1024;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK) {
        activityLogf("Audio: I2S channel create failed: %d", err);
        Serial.printf("[Audio] I2S channel create failed: %d\n", err);
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK,
            .bclk = (gpio_num_t)I2S_BCLK,
            .ws   = (gpio_num_t)I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    // MCLK = sample_rate * mclk_multiple = 44100 * 256 = 11,289,600 Hz (default)

    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        activityLogf("Audio: I2S init std mode failed: %d", err);
        Serial.printf("[Audio] I2S init std mode failed: %d\n", err);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return false;
    }

    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        activityLogf("Audio: I2S channel enable failed: %d", err);
        Serial.printf("[Audio] I2S channel enable failed: %d\n", err);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return false;
    }

    activityLog("Audio: I2S started (MCLK running)");
    Serial.println("[Audio] I2S RX started — MCLK now active");

    // Give the ES7210 time to wake up with MCLK present
    delay(200);

    // Reinitialize Wire fresh — PMIC init may have left it in a bad state
    Wire.end();
    delay(10);
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);  // ES7210 safer at 100kHz
    Wire.setTimeOut(50);    // 50ms timeout per transaction

    activityLog("Audio: configuring ES7210...");
    if (!es7210Init()) {
        activityLog("Audio: ES7210 init FAILED");
        Serial.println("[Audio] ES7210 init FAILED — recording will not work");
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return false;
    }
    activityLog("Audio: ES7210 configured OK");

    Serial.println("[Audio] Audio init complete");
    return true;
}

bool audioRecord(uint8_t* buffer, size_t bufferSize, size_t& bytesRecorded) {
    bytesRecorded = 0;
    size_t remaining = bufferSize;
    uint8_t* ptr = buffer;

    int expectedSecs = bufferSize / (AUDIO_SAMPLE_RATE * (AUDIO_BITS / 8) * AUDIO_CHANNELS);
    Serial.printf("[Audio] Recording %d seconds (16-bit stereo)...\n", expectedSecs);

    unsigned long deadline = millis() + (expectedSecs + 5) * 1000UL;
    int stallCount = 0;

    while (remaining > 0) {
        if (millis() > deadline) {
            Serial.printf("[Audio] Timeout — got %u/%u bytes\n", (unsigned)bytesRecorded, (unsigned)bufferSize);
            return bytesRecorded > 0;
        }

        size_t bytesRead = 0;
        esp_err_t err = i2s_channel_read(rx_handle, ptr, remaining, &bytesRead, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            Serial.printf("[Audio] I2S read error: %d\n", err);
            return false;
        }

        if (bytesRead == 0) {
            stallCount++;
            if (stallCount > 3) {
                Serial.printf("[Audio] I2S stalled — no data after %d attempts (%u bytes so far)\n",
                              stallCount, (unsigned)bytesRecorded);
                return false;
            }
            continue;
        }
        stallCount = 0;
        ptr += bytesRead;
        remaining -= bytesRead;
        bytesRecorded += bytesRead;
    }

    Serial.printf("[Audio] Recorded %u bytes\n", bytesRecorded);

    // Audio level diagnostic — stereo 16-bit: [L, R, L, R, ...]
    int16_t* samples = (int16_t*)buffer;
    size_t numSamples = bytesRecorded / 2;
    int32_t maxL = 0, maxR = 0;
    int64_t sumL = 0, sumR = 0;
    size_t nzL = 0, nzR = 0;
    size_t perCh = numSamples / 2;
    for (size_t i = 0; i + 1 < numSamples; i += 2) {
        int32_t aL = abs(samples[i]);
        int32_t aR = abs(samples[i + 1]);
        if (aL > maxL) maxL = aL;
        if (aR > maxR) maxR = aR;
        if (samples[i] != 0) nzL++;
        if (samples[i + 1] != 0) nzR++;
        sumL += aL;
        sumR += aR;
    }
    int32_t avgL = perCh > 0 ? (int32_t)(sumL / perCh) : 0;
    int32_t avgR = perCh > 0 ? (int32_t)(sumR / perCh) : 0;
    Serial.printf("[Audio] L: max=%d avg=%d nz=%u/%u (%u%%)\n",
        (int)maxL, (int)avgL, (unsigned)nzL, (unsigned)perCh,
        perCh > 0 ? (unsigned)(nzL*100/perCh) : 0);
    Serial.printf("[Audio] R: max=%d avg=%d nz=%u/%u (%u%%)\n",
        (int)maxR, (int)avgR, (unsigned)nzR, (unsigned)perCh,
        perCh > 0 ? (unsigned)(nzR*100/perCh) : 0);
    activityLogf("L: max=%d avg=%d nz=%u%%", (int)maxL, (int)avgL, perCh > 0 ? (unsigned)(nzL*100/perCh) : 0);
    activityLogf("R: max=%d avg=%d nz=%u%%", (int)maxR, (int)avgR, perCh > 0 ? (unsigned)(nzR*100/perCh) : 0);

    return true;
}

void audioDeinit() {
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
    // Restore Wire to 400kHz for PMIC and other I2C devices
    Wire.end();
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Serial.println("[Audio] I2S driver uninstalled, Wire restored");
}
