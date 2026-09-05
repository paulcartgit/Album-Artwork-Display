#pragma once
#include <cstdint>
#include <cstring>

// ─── Canonical 44-byte PCM WAV header ───
// Written in three different places before this existed, byte-for-byte
// identical each time and one typo away from silently corrupting uploads.
static const size_t WAV_HEADER_SIZE = 44;

inline void wavWriteHeader(uint8_t* dst, uint32_t dataLen,
                           uint16_t channels, uint32_t sampleRate,
                           uint16_t bitsPerSample = 16) {
    uint32_t byteRate   = sampleRate * channels * bitsPerSample / 8;
    uint16_t blockAlign = channels * bitsPerSample / 8;
    uint32_t riffSize   = dataLen + WAV_HEADER_SIZE - 8;
    uint32_t fmtSize    = 16;
    uint16_t audioFmt   = 1; // PCM

    memcpy(dst,      "RIFF", 4);       memcpy(dst + 4,  &riffSize,   4);
    memcpy(dst + 8,  "WAVEfmt ", 8);   memcpy(dst + 16, &fmtSize,    4);
    memcpy(dst + 20, &audioFmt, 2);    memcpy(dst + 22, &channels,   2);
    memcpy(dst + 24, &sampleRate, 4);  memcpy(dst + 28, &byteRate,   4);
    memcpy(dst + 32, &blockAlign, 2);  memcpy(dst + 34, &bitsPerSample, 2);
    memcpy(dst + 36, "data", 4);       memcpy(dst + 40, &dataLen,    4);
}
