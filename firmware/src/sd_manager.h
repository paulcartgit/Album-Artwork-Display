#pragma once
#include "config.h"

bool sdInit();
bool sdReadWifiConfig(WifiConfig& cfg);
bool sdWriteWifiConfig(const WifiConfig& cfg);
bool sdReadSettings(Settings& settings);
bool sdWriteSettings(const Settings& settings);
bool sdFileExists(const char* path);

// ─── Album art history (replaces manual gallery) ───
bool sdHistorySave(const char* artist, const char* title, const char* album,
                   const uint8_t* jpegBuf, size_t jpegSize);
String sdHistoryList();                           // JSON array for web UI
bool sdHistorySetEnabled(const char* file, bool on);
String sdHistoryRandomFile();                     // random enabled entry path
