#pragma once
#include "config.h"

bool wifiConnect(const WifiConfig& cfg);
bool wifiIsConnected();
String wifiGetIP();
