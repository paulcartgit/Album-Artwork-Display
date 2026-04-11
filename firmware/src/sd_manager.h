#pragma once
#include "config.h"

bool sdInit();
bool sdReadWifiConfig(WifiConfig& cfg);
bool sdReadSettings(Settings& settings);
bool sdWriteSettings(const Settings& settings);
bool sdFileExists(const char* path);
String sdListGallery();
bool sdDeleteFile(const char* path);
String sdRandomGalleryFile();
