#pragma once
#include <Arduino.h>

struct AcrResult {
    String artist;
    String title;
    String album;
    bool found;
};

bool acrcloudIdentify(const char* host, const char* accessKey, const char* accessSecret,
                      const uint8_t* audioData, size_t audioLen, AcrResult& result);
