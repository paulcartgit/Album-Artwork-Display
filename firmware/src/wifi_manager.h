#pragma once
#include "config.h"

bool wifiConnect(const WifiConfig& cfg);
bool wifiIsConnected();
String wifiGetIP();

// Access-point (setup) mode
bool   wifiStartAP(const char* apName = "NowPlaying-Setup");
void   wifiStopAP();
bool   wifiIsAPMode();
String wifiGetAPIP();
