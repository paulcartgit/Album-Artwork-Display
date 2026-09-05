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

// Display a placeholder with artist/album text when artwork can't be decoded
bool pipelineShowPlaceholder(const char* artist, const char* album);

// Display a test pattern showing all 7 palette colors
void pipelineShowTestPattern();

// Display a dither-mix test pattern (pure colors + dithered blends)
void pipelineShowDitherTest();

// Display the palette calibration card: six flat, undithered pigment patches
// on a white field, in a fixed grid.  Photograph it and feed the photo to
// simulator/calibrate_from_photo.py to regenerate PALETTE[] in config.h.
void pipelineShowCalibrationCard();

// Geometry of the calibration card, shared with the photo-sampling script.
// Patches are laid out 2 columns x 3 rows, palette index 0..5 reading
// left-to-right, top-to-bottom.
#define CAL_MARGIN_X   30
#define CAL_MARGIN_Y   40
#define CAL_PATCH_W   190
#define CAL_PATCH_H   220
#define CAL_GUTTER_X   40
#define CAL_GUTTER_Y   30
#define CAL_COLS        2
#define CAL_ROWS        3
