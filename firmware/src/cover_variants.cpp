#include "cover_variants.h"
#include "cover_match.h"
#include "image_pipeline.h"
#include "metadata_client.h"
#include "activity_log.h"

#include <esp_task_wdt.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

// Cover Art Archive redirects to archive.org and is frequently slow, and most
// releases have no art at all. Both are normal: skip and move on rather than
// letting one candidate hold up a refresh.
#define CAA_TIMEOUT_MS   5000
#define CAA_MAX_BYTES    (400 * 1024)
#define COVER_MIN_GAIN   0.75f   // must be this much better to be worth swapping

// A hard ceiling on the whole search. Each candidate is a download plus a
// decode plus six trial renders, and this runs on the loop task inside
// pipelineProcessUrl — which does not feed the watchdog while it works. With
// six candidates it blew the 90s watchdog and reset-looped the frame the first
// time it ran for real. The budget is checked between candidates and the
// watchdog is fed as we go, so a slow archive costs a worse cover choice
// rather than a reboot.
#define COVER_BUDGET_MS  25000
#define COVER_MAX_TRIED  4

static uint8_t* fetchCover(const String& url, size_t& lenOut) {
    lenOut = 0;
    WiFiClientSecure client;
    client.setInsecure();               // public images, no credentials sent
    HTTPClient http;
    http.begin(client, url);
    http.setConnectTimeout(CAA_TIMEOUT_MS);
    http.setTimeout(CAA_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("User-Agent", MB_USER_AGENT);

    if (http.GET() != HTTP_CODE_OK) { http.end(); return nullptr; }
    const int len = http.getSize();
    if (len <= 0 || len > CAA_MAX_BYTES) { http.end(); return nullptr; }

    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) { http.end(); return nullptr; }
    const int got = http.getStream().readBytes(buf, len);
    http.end();
    if (got != len) { heap_caps_free(buf); return nullptr; }
    lenOut = (size_t)len;
    return buf;
}

bool coverChooseVariant(const char* artist, const char* album,
                        const uint8_t* currentJpeg, size_t currentLen,
                        String& url) {
    if (!artist || !*artist || !album || !*album || !currentJpeg) return false;

    esp_task_wdt_reset();
    float refSig[COVER_SIG_LEN], refScore = 0.0f;
    if (!pipelineAssessJpeg(currentJpeg, currentLen, &refScore, refSig)) return false;
    esp_task_wdt_reset();

    ReleaseInfo info;
    metadataLookup(artist, album, info);
    if (info.candidateCount == 0) return false;

    float bestScore = refScore;
    String bestUrl;
    int considered = 0, sameArtwork = 0;
    const unsigned long started = millis();

    for (int i = 0; i < info.candidateCount; i++) {
        esp_task_wdt_reset();
        if (considered >= COVER_MAX_TRIED) break;
        if (millis() - started > COVER_BUDGET_MS) {
            Serial.println("[Cover] budget spent, keeping what we have");
            break;
        }
        const String candidate = "https://coverartarchive.org/release/" +
                                 info.candidates[i] + "/front-500";
        size_t len = 0;
        uint8_t* buf = fetchCover(candidate, len);
        if (!buf) continue;
        considered++;

        esp_task_wdt_reset();
        float sig[COVER_SIG_LEN], score = 0.0f;
        const bool ok = pipelineAssessJpeg(buf, len, &score, sig);
        esp_task_wdt_reset();
        heap_caps_free(buf);
        if (!ok) continue;

        if (!coverIsSameArtwork(refSig, sig)) continue;   // a different sleeve
        sameArtwork++;
        if (score < bestScore - COVER_MIN_GAIN) {
            bestScore = score;
            bestUrl = candidate;
        }
    }

    Serial.printf("[Cover] %d fetched, %d same artwork, best %.2f vs %.2f\n",
                  considered, sameArtwork, bestScore, refScore);

    if (bestUrl.length() == 0) return false;
    activityLogf("Better scan of this sleeve found (%.1f vs %.1f)",
                 bestScore, refScore);
    url = bestUrl;
    return true;
}
