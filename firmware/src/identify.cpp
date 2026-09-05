#include "identify.h"
#include "app.h"
#include "config.h"
#include "wav_utils.h"
#include "audio_capture.h"
#include "shazam_client.h"
#include "image_pipeline.h"
#include "activity_log.h"

#include <esp_heap_caps.h>
#include <cmath>

// 16-bit PCM; the ES7210 noise floor sits around 50–100 with the mic gain we set.
static const float SILENCE_RMS_THRESHOLD = 150.0f;

// Normalise the loudest sample to ~80 % of full scale, but never amplify more
// than 32× or we just amplify the noise floor into something Shazam chokes on.
static const int32_t AUTO_GAIN_TARGET_PEAK = 26000;
static const int32_t AUTO_GAIN_MAX         = 32;

// Collapse interleaved stereo to mono in place, keeping the left channel
// (MIC1 on the ES7210).  Returns the number of mono bytes.
static size_t downmixToMono(int16_t* samples, size_t totalSamples) {
    if (AUDIO_CHANNELS <= 1) return totalSamples * sizeof(int16_t);
    size_t monoSamples = totalSamples / AUDIO_CHANNELS;
    for (size_t i = 0; i < monoSamples; i++) {
        samples[i] = samples[i * AUDIO_CHANNELS];
    }
    return monoSamples * sizeof(int16_t);
}

static float rmsOf(const int16_t* samples, size_t count) {
    if (count == 0) return 0.0f;
    double sumSq = 0;
    for (size_t i = 0; i < count; i++) {
        double s = samples[i];
        sumSq += s * s;
    }
    return sqrtf((float)(sumSq / count));
}

static int32_t applyAutoGain(int16_t* samples, size_t count) {
    int32_t peak = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t a = abs((int32_t)samples[i]);
        if (a > peak) peak = a;
    }
    int32_t gain = (peak > 0) ? (AUTO_GAIN_TARGET_PEAK / peak) : 1;
    if (gain < 1) gain = 1;
    if (gain > AUTO_GAIN_MAX) gain = AUTO_GAIN_MAX;
    if (gain == 1) return 1;

    for (size_t i = 0; i < count; i++) {
        int32_t s = (int32_t)samples[i] * gain;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        samples[i] = (int16_t)s;
    }
    return gain;
}

// Keep a copy of the raw capture for the debug download endpoint.  The buffer
// is allocated once and never freed: the async web server may still be
// streaming from it when the next recording starts, and a stale-but-valid
// buffer is far better than a use-after-free.
static void stashDebugAudio(const uint8_t* data, size_t len) {
    if (!g_app.lastAudio) {
        g_app.lastAudio = (uint8_t*)heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_app.lastAudio) return;
    }
    size_t n = (len > AUDIO_BUFFER_SIZE) ? AUDIO_BUFFER_SIZE : len;
    g_app.lastAudioLen = 0;            // invalidate while we overwrite
    memcpy(g_app.lastAudio, data, n);
    g_app.lastAudioChannels   = 1;     // we stash the downmixed signal
    g_app.lastAudioSampleRate = AUDIO_SAMPLE_RATE;
    g_app.lastAudioLen = n;
}

IdentifyResult identifyNowPlaying(IdentifyTrigger trigger,
                                  String& artist, String& title, String& album) {
    const char* tag = (trigger == IDENTIFY_VINYL) ? "Vinyl" : "Listen";
    const size_t bufSize = (trigger == IDENTIFY_VINYL) ? AUDIO_BUFFER_SIZE : LISTEN_BUFFER_SIZE;
    const int    secs    = (trigger == IDENTIFY_VINYL) ? AUDIO_RECORD_SECS : LISTEN_RECORD_SECS;

    uint8_t* audioBuf = (uint8_t*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM);
    if (!audioBuf) {
        activityLogf("%s: audio buffer alloc failed", tag);
        return IDENTIFY_ERROR;
    }

    if (!audioInit()) {
        activityLogf("%s: audio init failed — check hardware", tag);
        heap_caps_free(audioBuf);
        return IDENTIFY_ERROR;
    }

    activityLogf("%s: recording %ds...", tag, secs);
    size_t recorded = 0;
    bool ok = audioRecord(audioBuf, bufSize, recorded);
    audioDeinit();

    if (!ok || recorded == 0) {
        activityLogf("%s: recording failed (got %u bytes)", tag, (unsigned)recorded);
        heap_caps_free(audioBuf);
        return IDENTIFY_ERROR;
    }

    // Downmix first, then measure.  Measuring the interleaved stereo stream
    // averaged in a second channel that may be near-silent, halving the
    // apparent level and triggering false "too quiet" skips.
    int16_t* samples = (int16_t*)audioBuf;
    size_t monoBytes   = downmixToMono(samples, recorded / sizeof(int16_t));
    size_t monoSamples = monoBytes / sizeof(int16_t);

    float rms = rmsOf(samples, monoSamples);
    activityLogf("%s: RMS %.0f (threshold %.0f)", tag, rms, SILENCE_RMS_THRESHOLD);
    if (rms < SILENCE_RMS_THRESHOLD) {
        activityLogf("%s: too quiet — skipping Shazam", tag);
        heap_caps_free(audioBuf);
        return IDENTIFY_SILENT;
    }

    int32_t gain = applyAutoGain(samples, monoSamples);
    activityLogf("%s: mono %uKB, gain %dx", tag, (unsigned)(monoBytes / 1024), (int)gain);

    stashDebugAudio(audioBuf, monoBytes);

    // Wrap in a WAV container for Shazam's file-upload endpoint
    size_t wavLen = WAV_HEADER_SIZE + monoBytes;
    uint8_t* wavBuf = (uint8_t*)heap_caps_malloc(wavLen, MALLOC_CAP_SPIRAM);
    if (!wavBuf) {
        activityLogf("%s: WAV buffer alloc failed", tag);
        heap_caps_free(audioBuf);
        return IDENTIFY_ERROR;
    }
    wavWriteHeader(wavBuf, monoBytes, 1, AUDIO_SAMPLE_RATE);
    memcpy(wavBuf + WAV_HEADER_SIZE, audioBuf, monoBytes);
    heap_caps_free(audioBuf);

    activityLogf("%s: identifying via Shazam...", tag);
    ShazamResult shazam;
    bool identified = shazamIdentify(g_app.settings.shazam_api_key, wavBuf, wavLen, shazam);
    heap_caps_free(wavBuf);

    if (!identified) {
        activityLogf("%s: Shazam — no match", tag);
        return IDENTIFY_NO_MATCH;
    }

    artist = shazam.artist;
    title  = shazam.title;
    album  = shazam.album;
    activityLogf("%s: %s — %s", tag, artist.c_str(), title.c_str());

    if (shazam.coverArtUrl.length() == 0) {
        activityLogf("%s: no album art in result", tag);
        return IDENTIFY_OK;
    }

    const char* overlayArtist = g_app.settings.show_track_info ? artist.c_str() : nullptr;
    const char* overlayAlbum  = g_app.settings.show_track_info ? album.c_str()  : nullptr;
    if (pipelineProcessUrl(shazam.coverArtUrl.c_str(), overlayArtist, overlayAlbum,
                           artist.c_str(), title.c_str(), album.c_str())) {
        g_app.lastArtUrl = shazam.coverArtUrl;
        activityLogf("%s: display updated", tag);
    } else {
        activityLogf("%s: artwork pipeline failed", tag);
    }
    return IDENTIFY_OK;
}
