#pragma once
#include "config.h"

bool sdInit();
bool sdReadWifiConfig(WifiConfig& cfg);
bool sdWriteWifiConfig(const WifiConfig& cfg);
bool sdReadSettings(Settings& settings);
bool sdWriteSettings(const Settings& settings);
bool sdFileExists(const char* path);

// ─── Album art history (replaces manual gallery) ───
static const int HISTORY_FNAME_LEN = 20; // "XXXXXXXX.jpg" + NUL fits in 14; 20 gives headroom
bool sdHistorySave(const char* artist, const char* title, const char* album,
                   const uint8_t* jpegBuf, size_t jpegSize);
String sdHistoryList();                           // JSON array for web UI
bool sdHistorySetEnabled(const char* file, bool on);
bool sdHistorySetPinned(const char* file, bool pinned);
bool sdHistoryDelete(const char* file);            // remove entry + JPEG from SD
String sdHistoryRandomFile();                     // shuffle-bag enabled entry path
