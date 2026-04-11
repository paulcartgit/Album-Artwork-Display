#pragma once
#include <Arduino.h>

bool spotifyInit(const char* clientId, const char* clientSecret);
String spotifyGetAlbumArtUrl(const char* artist, const char* title);
