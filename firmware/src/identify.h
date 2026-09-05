#pragma once
#include <Arduino.h>

// Why we are listening.  Only affects how long we record and how the activity
// log is worded — the signal path itself is identical either way, which is the
// point: the automatic and manual paths used to be two separate, divergent
// implementations and the automatic one was quietly the worse of the two.
enum IdentifyTrigger {
    IDENTIFY_VINYL,   // Sonos reports line-in — longer capture
    IDENTIFY_MANUAL   // user pressed "Listen" in the portal
};

enum IdentifyResult {
    IDENTIFY_OK = 0,      // matched, artwork displayed
    IDENTIFY_SILENT,      // room is quiet — didn't call Shazam
    IDENTIFY_NO_MATCH,    // Shazam had nothing
    IDENTIFY_ERROR        // hardware / allocation failure
};

// Record → mono-mix → auto-gain → WAV → Shazam → display artwork.
// On success, fills artist/title/album with what was identified.
IdentifyResult identifyNowPlaying(IdentifyTrigger trigger,
                                  String& artist, String& title, String& album);
