#pragma once
#include <Arduino.h>

struct ShazamResult {
    String artist;
    String title;
    String album;
    String coverArtUrl;
    bool found;
};

bool shazamIdentify(const char* rapidApiKey,
                    const uint8_t* audioData, size_t audioLen,
                    ShazamResult& result);
