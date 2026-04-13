#include "audio_capture.h"
#include "activity_log.h"
#include "config.h"
#include <Wire.h>
#include <driver/i2s.h>

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

    // Software reset
    es7210WriteReg(0x00, 0xFF);
    delay(20);
    es7210WriteReg(0x00, 0x41);
    delay(20);

    // Clock: initially keep clocks gated
    es7210WriteReg(0x01, 0x3F);

    // Timing
    es7210WriteReg(0x09, 0x30);  // Chip state cycle
    es7210WriteReg(0x0A, 0x30);  // Power-on state cycle

    // High-pass filter setup (DC offset removal)
    es7210WriteReg(0x23, 0x2A);
    es7210WriteReg(0x22, 0x0A);
    es7210WriteReg(0x20, 0x0A);
    es7210WriteReg(0x21, 0x2A);

    // Slave mode (ESP32 is I2S master)
    es7210WriteReg(0x08, 0x00);

    // Analog power: VDDA=3.3V, VMID 5kΩ start
    es7210WriteReg(0x40, 0x43);

    // MIC bias voltage = 2.87V
    es7210WriteReg(0x41, 0x70);  // MIC1/2 bias
    es7210WriteReg(0x42, 0x70);  // MIC3/4 bias

    // ADC oversampling ratio
    es7210WriteReg(0x07, 0x20);

    // Main clock: DLL + divider
    es7210WriteReg(0x02, 0xC1);

    // I2S format: 16-bit, I2S Philips standard
    es7210WriteReg(0x11, 0x60);
    // Normal I2S mode (not TDM)
    es7210WriteReg(0x12, 0x00);

    // ── Power-up sequence (MIC1 + MIC2 only — board has 2 physical mics) ──

    // Power up digital (all ADCs)
    es7210WriteReg(0x06, 0x00);

    // Analog
    es7210WriteReg(0x40, 0x43);

    // Individual MIC power on (MIC1/2 only, MIC3/4 off)
    es7210WriteReg(0x47, 0x08);  // MIC1 power on
    es7210WriteReg(0x48, 0x08);  // MIC2 power on
    es7210WriteReg(0x49, 0xFF);  // MIC3 power off
    es7210WriteReg(0x4A, 0xFF);  // MIC4 power off

    // MIC power rails: only MIC1/2
    es7210WriteReg(0x4B, 0x00);  // MIC1/2 power ON
    es7210WriteReg(0x4C, 0xFF);  // MIC3/4 power OFF

    // MIC gain: bit4=enable, bits[3:0]=gain value
    // 0x1A = 0x10 (enable) | 0x0A (30dB)
    es7210WriteReg(0x43, 0x1A);  // MIC1 gain enabled, 30dB
    es7210WriteReg(0x44, 0x1A);  // MIC2 gain enabled, 30dB
    es7210WriteReg(0x45, 0x00);  // MIC3 gain disabled
    es7210WriteReg(0x46, 0x00);  // MIC4 gain disabled

    // Enable clocks for MIC1/2 only (bits: 0x0b = ADC1/2 clocks)
    es7210WriteReg(0x01, 0x14);  // Keep MIC3/4 clocks gated

    // Final analog setup
    es7210WriteReg(0x40, 0x43);

    // Reset to start ADC conversion
    es7210WriteReg(0x00, 0x71);
    es7210WriteReg(0x00, 0x41);

    // Verify a register readback
    uint8_t regVal = es7210ReadReg(0x00);
    Serial.printf("[Audio] ES7210 initialized (reg0x00=0x%02X)\n", regVal);
    return true;
}

// ─── I2S driver ───

bool audioInit() {
    Serial.println("[Audio] audioInit starting...");

    // Set a timeout on Wire to prevent hangs on stuck I2C bus
    Wire.setTimeOut(100); // 100ms timeout

    // Enable power amplifier pin (active high)
    pinMode(PA_ENABLE, OUTPUT);
    digitalWrite(PA_ENABLE, LOW); // keep amp off during recording

    // ── Start I2S FIRST — the ES7210 needs MCLK running before its I2C works ──
    // Use MASTER | TX | RX to ensure MCLK is output on ESP32-S3
    i2s_config_t i2s_cfg = {};
    i2s_cfg.mode            = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    i2s_cfg.sample_rate     = AUDIO_SAMPLE_RATE;
    i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_cfg.channel_format  = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    i2s_cfg.dma_buf_count   = 8;
    i2s_cfg.dma_buf_len     = 1024;
    i2s_cfg.use_apll        = false;
    i2s_cfg.tx_desc_auto_clear = true;
    i2s_cfg.fixed_mclk      = 0;

    i2s_pin_config_t pin_cfg = {};
    pin_cfg.mck_io_num   = I2S_MCLK;
    pin_cfg.bck_io_num   = I2S_BCLK;
    pin_cfg.ws_io_num    = I2S_WS;
    pin_cfg.data_out_num = I2S_PIN_NO_CHANGE;
    pin_cfg.data_in_num  = I2S_DIN;

    activityLog("Audio: installing I2S driver...");
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL);
    if (err != ESP_OK) {
        activityLogf("Audio: I2S install failed: %d", err);
        Serial.printf("[Audio] I2S install failed: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_cfg);
    if (err != ESP_OK) {
        activityLogf("Audio: I2S set_pin failed: %d", err);
        Serial.printf("[Audio] I2S set_pin failed: %d\n", err);
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
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
        i2s_driver_uninstall(I2S_NUM_0);
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
    Serial.printf("[Audio] Recording %d seconds...\n", expectedSecs);

    unsigned long deadline = millis() + (expectedSecs + 5) * 1000UL; // generous timeout
    int stallCount = 0;

    while (remaining > 0) {
        if (millis() > deadline) {
            Serial.printf("[Audio] Timeout — got %u/%u bytes\n", (unsigned)bytesRecorded, (unsigned)bufferSize);
            return bytesRecorded > 0; // partial is OK if we got something
        }

        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_NUM_0, ptr, remaining, &bytesRead, pdMS_TO_TICKS(1000));
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

    // Audio level diagnostic
    int16_t* samples = (int16_t*)buffer;
    size_t numSamples = bytesRecorded / 2;
    int32_t minVal = 32767, maxVal = -32768;
    int64_t sumAbs = 0;
    size_t nonZero = 0;
    for (size_t i = 0; i < numSamples; i++) {
        int16_t s = samples[i];
        if (s < minVal) minVal = s;
        if (s > maxVal) maxVal = s;
        if (s != 0) nonZero++;
        sumAbs += abs(s);
    }
    int32_t avgAbs = numSamples > 0 ? (int32_t)(sumAbs / numSamples) : 0;
    Serial.printf("[Audio] Samples: %u total, %u non-zero, min=%d max=%d avgAbs=%d\n",
        (unsigned)numSamples, (unsigned)nonZero, (int)minVal, (int)maxVal, (int)avgAbs);
    activityLogf("Audio: min=%d max=%d avg=%d nz=%u%%",
        (int)minVal, (int)maxVal, (int)avgAbs,
        numSamples > 0 ? (unsigned)(nonZero * 100 / numSamples) : 0);

    return true;
}

void audioDeinit() {
    i2s_driver_uninstall(I2S_NUM_0);
    // Restore Wire to 400kHz for PMIC and other I2C devices
    Wire.end();
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Serial.println("[Audio] I2S driver uninstalled, Wire restored");
}
