#pragma once
#include <Arduino.h>

// ─── Release metadata (MusicBrainz) ───
//
// Sonos and Shazam both give artist / title / album and nothing else. For a
// frame sitting next to a turntable, the pressing details are the interesting
// part: "1973 · Harvest · SHVL 804" says considerably more than the album name
// on its own.
//
// MusicBrainz is used rather than Discogs because it needs no key and no OAuth
// — it asks only for an honest User-Agent and no more than one request per
// second, both of which we honour. Results are cached per album in the history
// index, so a given record is looked up once and never again.

struct ReleaseInfo {
    String year;            // "1973"
    String label;           // "Harvest"
    String catalogNumber;   // "SHVL 804"
    String country;         // "GB"
    bool found = false;
};

// Blocking; typically under a second. Safe to call with anything — an empty or
// unmatched query simply returns found=false.
bool metadataLookup(const char* artist, const char* album, ReleaseInfo& out);

// "1973 · Harvest · SHVL 804", omitting whatever is missing. Empty if nothing
// useful is known.
String metadataSummary(const ReleaseInfo& info);
