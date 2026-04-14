#include "acrcloud_client.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include <time.h>

static String hmacSha1Base64(const char* key, const char* data) {
    uint8_t hmac[20];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
    mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)data, strlen(data));
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);

    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, hmac, 20);
    uint8_t* b64 = (uint8_t*)malloc(olen + 1);
    mbedtls_base64_encode(b64, olen + 1, &olen, hmac, 20);
    b64[olen] = 0;
    String result((char*)b64);
    free(b64);
    return result;
}

bool acrcloudIdentify(const char* host, const char* accessKey, const char* accessSecret,
                      const uint8_t* audioData, size_t audioLen, AcrResult& result) {
    result.found = false;

    String timestamp = String((unsigned long)time(nullptr));
    String httpMethod = "POST";
    String httpUri    = "/v1/identify";
    String dataType   = "audio";
    String sigVersion = "1";

    String stringToSign = httpMethod + "\n" + httpUri + "\n" + accessKey + "\n"
                        + dataType + "\n" + sigVersion + "\n" + timestamp;
    String signature = hmacSha1Base64(accessSecret, stringToSign.c_str());

    // Build multipart body
    String boundary = "----ACRBoundary9876543210";
    String url = String("https://") + host + httpUri;

    // Preamble fields
    String pre;
    auto addField = [&](const char* name, const String& value) {
        pre += "--" + boundary + "\r\n";
        pre += "Content-Disposition: form-data; name=\"" + String(name) + "\"\r\n\r\n";
        pre += value + "\r\n";
    };
    addField("access_key",        accessKey);
    addField("data_type",         dataType);
    addField("signature_version", sigVersion);
    addField("signature",         signature);
    addField("timestamp",         timestamp);
    addField("sample_bytes",      String(audioLen));

    // File part header
    pre += "--" + boundary + "\r\n";
    pre += "Content-Disposition: form-data; name=\"sample\"; filename=\"audio.wav\"\r\n";
    pre += "Content-Type: audio/wav\r\n\r\n";

    String post = "\r\n--" + boundary + "--\r\n";

    // Assemble in PSRAM
    size_t totalLen = pre.length() + audioLen + post.length();
    uint8_t* body = (uint8_t*)heap_caps_malloc(totalLen, MALLOC_CAP_SPIRAM);
    if (!body) {
        Serial.println("[ACRCloud] Body alloc failed");
        return false;
    }
    memcpy(body, pre.c_str(), pre.length());
    memcpy(body + pre.length(), audioData, audioLen);
    memcpy(body + pre.length() + audioLen, post.c_str(), post.length());

    // HTTPS POST
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    int code = http.POST(body, totalLen);
    heap_caps_free(body);

    if (code != HTTP_CODE_OK) {
        Serial.printf("[ACRCloud] HTTP %d\n", code);
        http.end();
        return false;
    }

    String response = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, response)) {
        Serial.println("[ACRCloud] JSON parse failed");
        return false;
    }

    int status = doc["status"]["code"] | -1;
    const char* statusMsg = doc["status"]["msg"] | "unknown";
    Serial.printf("[ACRCloud] Status %d: %s\n", status, statusMsg);
    if (status != 0) {
        Serial.printf("[ACRCloud] Full response: %s\n", response.c_str());
        return false;
    }

    result.artist = doc["metadata"]["music"][0]["artists"][0]["name"].as<String>();
    result.title  = doc["metadata"]["music"][0]["title"].as<String>();
    result.album  = doc["metadata"]["music"][0]["album"]["name"].as<String>();
    result.found  = result.artist.length() > 0 && result.title.length() > 0;

    Serial.printf("[ACRCloud] Found: %s — %s (%s)\n",
                  result.artist.c_str(), result.title.c_str(), result.album.c_str());
    return result.found;
}
