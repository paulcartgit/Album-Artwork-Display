#pragma once
#include <Arduino.h>

struct SonosTrackInfo {
    String artist;
    String title;
    String album;
    String artUrl;
    bool isLineIn;
};

bool sonosGetTrackInfo(const char* sonosIp, SonosTrackInfo& info);
bool sonosIsPlaying(const char* sonosIp);
String sonosDiscover(int timeoutMs = 3000);
