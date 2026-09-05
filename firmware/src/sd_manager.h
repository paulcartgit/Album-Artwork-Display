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

// ─── Rendered frame cache ───
//
// Everything between the JPEG and the panel — the fill, six trial renders, the
// tone map, the gamut map, the enhancement and the dither — produces the same
// 192 KB packed frame every time for the same artwork and settings. Before
// this it was all redone on every display: on the idle rotation, on a history
// tap, and on the boot restore. That is minutes of work per hour thrown away,
// and it is what pushed a single render close enough to the watchdog to reset
// the frame.
//
// The signature covers everything that changes the output — palette, profile,
// fill mode and an algorithm version — so a cached frame is only used when it
// would be identical to re-rendering.
bool sdRenderCacheLoad(const char* file, uint32_t signature, uint8_t* packed);
bool sdRenderCacheSave(const char* file, uint32_t signature, const uint8_t* packed);
void sdRenderCacheDrop(const char* file);

// ─── What was last on the panel ───
// Recorded explicitly rather than inferred. The first guess at this read the
// first entry of the history index, assuming it was ordered newest first — it
// is not: pinned entries lead, and the timestamps are not ordered either, so
// the frame restored a pinned Beatles sleeve on every boot regardless of what
// had actually been showing.
bool sdSetLastShown(const char* path);
String sdGetLastShown();

// The most recently saved entry. Used to put something back on the panel after
// a restart: e-ink keeps showing whatever was there, but the firmware has no
// copy of it, so the portal has nothing to serve and the frame is stale in a
// way nobody can see. Returns "" if the history is empty.
String sdHistoryNewestFile();

// ─── Release metadata cache ───
// Looked up once per album from MusicBrainz and kept in the history index, so
// a record is never queried twice.
// Artist and album for a stored entry.
bool sdHistoryLookup(const char* file, String& artist, String& album);

bool sdHistoryGetRelease(const char* artist, const char* album, String& summary);
bool sdHistorySetRelease(const char* artist, const char* album, const char* summary);

// ─── Chosen cover art, per album ───
// Searching for a better-rendering scan costs a MusicBrainz query and several
// Cover Art Archive downloads, and the answer never changes for a given album.
// Stored beside the release details, keyed the same way. An empty string is a
// real answer meaning "looked, nothing better" — so it is remembered too,
// rather than searching again every time the record comes round.
bool sdHistoryGetCoverChoice(const char* artist, const char* album, String& url);
bool sdHistorySetCoverChoice(const char* artist, const char* album, const char* url);


