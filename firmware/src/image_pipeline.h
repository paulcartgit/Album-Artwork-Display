#pragma once

// Download JPEG from URL, decode, scale, dither, and push to display
// overlayArtist/overlayAlbum: text shown on display (null = no overlay)
// artist/title/album: track metadata for history (null = don't save to history)
bool pipelineProcessUrl(const char* url,
                        const char* overlayArtist = nullptr,
                        const char* overlayAlbum  = nullptr,
                        const char* artist = nullptr,
                        const char* title  = nullptr,
                        const char* album  = nullptr);

// Process a local JPEG file from SD card
bool pipelineProcessFile(const char* path);

// Display a test pattern showing all 7 palette colors
void pipelineShowTestPattern();
