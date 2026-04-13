#pragma once

// Download JPEG from URL, decode, scale, dither, and push to display
// If artist/album are provided (non-null), renders text overlay below artwork
bool pipelineProcessUrl(const char* url, const char* artist = nullptr, const char* album = nullptr);

// Process a local JPEG file from SD card
bool pipelineProcessFile(const char* path);

// Display a test pattern showing all 7 palette colors
void pipelineShowTestPattern();
