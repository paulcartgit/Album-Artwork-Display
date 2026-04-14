#include "spotify_client.h"
#include "url_utils.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

static String g_token;
static unsigned long g_tokenExpiry = 0;
static String g_clientId;
static String g_clientSecret;

// ─── Base64 via mbedtls ───
static String base64Encode(const String& input) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen,
                          (const uint8_t*)input.c_str(), input.length());
    uint8_t* buf = (uint8_t*)malloc(olen + 1);
    mbedtls_base64_encode(buf, olen + 1, &olen,
                          (const uint8_t*)input.c_str(), input.length());
    buf[olen] = 0;
    String result((char*)buf);
    free(buf);
    return result;
}

// ─── Token refresh ───
static bool refreshToken() {
    if (millis() < g_tokenExpiry && g_token.length() > 0) return true;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://accounts.spotify.com/api/token");

    String auth = base64Encode(g_clientId + ":" + g_clientSecret);
    http.addHeader("Authorization", "Basic " + auth);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int code = http.POST("grant_type=client_credentials");
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Spotify] Token HTTP %d\n", code);
        http.end();
        return false;
    }

    JsonDocument doc;
    deserializeJson(doc, http.getString());
    http.end();

    g_token = doc["access_token"].as<String>();
    int expiresIn = doc["expires_in"] | 3600;
    g_tokenExpiry = millis() + (unsigned long)(expiresIn - 60) * 1000UL;

    Serial.println("[Spotify] Token acquired");
    return true;
}

// ─── Public API ───

bool spotifyInit(const char* clientId, const char* clientSecret) {
    g_clientId     = clientId;
    g_clientSecret = clientSecret;
    g_token        = "";
    g_tokenExpiry  = 0;
    return true;
}

String spotifyGetAlbumArtUrl(const char* artist, const char* title) {
    if (!refreshToken()) return "";

    String query = urlEncode(String("artist:") + artist + " track:" + title);
    String url = "https://api.spotify.com/v1/search?q=" + query + "&type=track&limit=1";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Authorization", "Bearer " + g_token);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[Spotify] Search HTTP %d\n", code);
        http.end();
        return "";
    }

    JsonDocument doc;
    deserializeJson(doc, http.getString());
    http.end();

    const char* artUrl = doc["tracks"]["items"][0]["album"]["images"][0]["url"];
    if (!artUrl) {
        Serial.println("[Spotify] No art URL in response");
        return "";
    }

    Serial.printf("[Spotify] Art: %s\n", artUrl);
    return String(artUrl);
}
