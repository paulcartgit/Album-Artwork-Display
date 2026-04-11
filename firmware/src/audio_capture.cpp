#include "audio_capture.h"
#include "config.h"
#include <Wire.h>
#include <driver/i2s.h>

// ─── ES7210 minimal register init ───

static void es7210WriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static bool es7210Init() {
    // Software reset
    es7210WriteReg(0x00, 0xFF);
    delay(20);
    es7210WriteReg(0x00, 0x41);
    delay(20);

    // Clock configuration
    es7210WriteReg(0x01, 0x1F);  // Enable all clocks
    es7210WriteReg(0x02, 0xC3);  // MCLK from pin, analog power up
    es7210WriteReg(0x06, 0x00);  // Digital power on (all ADCs)
    es7210WriteReg(0x07, 0x20);  // ADC OSR = 32
    es7210WriteReg(0x09, 0x30);  // LRCK divider
    es7210WriteReg(0x0A, 0x30);  // Power management

    // I2S format: 16-bit, Philips standard
    es7210WriteReg(0x11, 0x60);
    es7210WriteReg(0x12, 0x00);

    // MIC gain (all channels ~30dB)
    es7210WriteReg(0x43, 0x1E);
    es7210WriteReg(0x44, 0x1E);
    es7210WriteReg(0x45, 0x1E);
    es7210WriteReg(0x46, 0x1E);

    Serial.println("[Audio] ES7210 initialized");
    return true;
}

// ─── I2S driver ───

bool audioInit() {
    // Configure ES7210 codec via I2C (Wire must be begun before calling this)
    es7210Init();

    // Enable power amplifier pin (active high)
    pinMode(PA_ENABLE, OUTPUT);
    digitalWrite(PA_ENABLE, LOW); // keep amp off during recording

    i2s_config_t i2s_cfg = {};
    i2s_cfg.mode            = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    i2s_cfg.sample_rate     = AUDIO_SAMPLE_RATE;
    i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_cfg.channel_format  = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    i2s_cfg.dma_buf_count   = 8;
    i2s_cfg.dma_buf_len     = 1024;
    i2s_cfg.use_apll        = false;
    i2s_cfg.tx_desc_auto_clear = false;
    i2s_cfg.fixed_mclk      = 0;

    i2s_pin_config_t pin_cfg = {};
    pin_cfg.mck_io_num   = I2S_MCLK;
    pin_cfg.bck_io_num   = I2S_BCLK;
    pin_cfg.ws_io_num    = I2S_WS;
    pin_cfg.data_out_num = I2S_PIN_NO_CHANGE;
    pin_cfg.data_in_num  = I2S_DIN;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[Audio] I2S install failed: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_cfg);
    if (err != ESP_OK) {
        Serial.printf("[Audio] I2S set_pin failed: %d\n", err);
        return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    Serial.println("[Audio] I2S RX initialized");
    return true;
}

bool audioRecord(uint8_t* buffer, size_t bufferSize, size_t& bytesRecorded) {
    bytesRecorded = 0;
    size_t remaining = bufferSize;
    uint8_t* ptr = buffer;

    Serial.printf("[Audio] Recording %d seconds...\n", AUDIO_RECORD_SECS);

    while (remaining > 0) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_NUM_0, ptr, remaining, &bytesRead, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            Serial.printf("[Audio] I2S read error: %d\n", err);
            return false;
        }
        ptr += bytesRead;
        remaining -= bytesRead;
        bytesRecorded += bytesRead;
    }

    Serial.printf("[Audio] Recorded %u bytes\n", bytesRecorded);
    return true;
}

void audioDeinit() {
    i2s_driver_uninstall(I2S_NUM_0);
    Serial.println("[Audio] I2S driver uninstalled");
}
