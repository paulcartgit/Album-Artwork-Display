#include "wifi_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>

static bool s_apMode = false;

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
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
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

bool wifiStartAP(const char* apName) {
    // WIFI_AP_STA allows scanning for nearby networks while the AP is active
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(apName)) {
        Serial.println("[WiFi] AP start failed");
        return false;
    }
    s_apMode = true;
    Serial.printf("[WiFi] AP started — SSID: %s  IP: %s\n",
                  apName, WiFi.softAPIP().toString().c_str());
    return true;
}

void wifiStopAP() {
    WiFi.softAPdisconnect(true);
    s_apMode = false;
}

bool wifiIsAPMode() {
    return s_apMode;
}

String wifiGetAPIP() {
    return WiFi.softAPIP().toString();
}
