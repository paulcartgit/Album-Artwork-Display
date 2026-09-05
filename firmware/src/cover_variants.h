#pragma once
#include <Arduino.h>

// ─── Choosing between pressings ───
//
// Sonos hands over one cover image, whichever the streaming service happens to
// hold. The same album has been pressed dozens of times and the sleeves do not
// render alike on six pigments: across six covers of Help! the best scored
// about twice as well as the worst, and that gap is in the source image where
// no amount of dithering can reach it.
//
// The obvious version of this feature is a trap, and worth restating wherever
// someone might loosen it: ranking every cover MusicBrainz returns purely on
// how well it renders puts an obscure reissue on the wall instead of the
// famous sleeve, because obscure reissues are often flatter and flatter
// renders better. Every candidate must therefore pass cover_match.h first —
// it has to be the same picture, only a better scan of it.

// Replaces `url` with a better-rendering scan of the SAME artwork, if one
// exists. Returns true if it changed anything. Blocking, several seconds; call
// it on the controller task, never from the web server.
bool coverChooseVariant(const char* artist, const char* album,
                        const uint8_t* currentJpeg, size_t currentLen,
                        String& url);
