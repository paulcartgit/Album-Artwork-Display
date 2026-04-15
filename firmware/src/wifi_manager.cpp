#include "wifi_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>

bool wifiConnect(const WifiConfig& cfg) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.password);

    Serial.printf("[WiFi] Connecting to %s", cfg.ssid);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Connection failed");
        return false;
    }

    Serial.printf("[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());

    if (!MDNS.begin("nowplaying")) {
        Serial.println("[WiFi] mDNS init failed");
    } else {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[WiFi] mDNS: nowplaying.local");
    }

    return true;
}

bool wifiIsConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String wifiGetIP() {
    return WiFi.localIP().toString();
}
