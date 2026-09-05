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
//
// One row per pigment. Each row carries a black AND a white reference on BOTH
// sides, each side chip split into a black half (top) and a white half (bottom):
//
//     [K] [===== pigment 0 =====] [W]
//     [W]                          [K]
//     [K] [===== pigment 1 =====] [W]
//     [W]                          [K]
//     ... one such row per pigment
//
// Each row has a reference column on BOTH sides: black over white on the left,
// white over black on the right. That mirroring is the whole point. Averaging
// the two black halves puts the black reference at the row's exact centre in
// both x (left and right) and y (top and bottom) — the pigment's own centroid —
// and likewise for white. Any smooth illumination gradient or lens vignetting
// therefore affects references and pigment identically, and cancels out.
//
// Get this wrong and the correction is worse than none: references only at the
// row edges over-correct a pigment sampled from the centre, and references
// stacked above/below it skew under a vertical gradient.
//
// Halves rather than quarters because the photo may be low resolution. At a
// typical webcam distance the panel spans a few hundred pixels, so a 30px chip
// lands on ~17px and lens blur bleeds black into white; 60px chips survive it.
#define CAL_ROWS        6   // one per palette entry
#define CAL_MARGIN_X   10
#define CAL_MARGIN_Y   20
#define CAL_ROW_H     120
#define CAL_ROW_GAP     8
#define CAL_CHIP_W     70   // reference column width (each side)
#define CAL_CHIP_HALVES   2  // K/W down the left column, W/K down the right
#define CAL_CHIP_GAP   10
#define CAL_PATCH_W   300   // the pigment itself
#define CAL_KEYLINE     2   // outline thickness, drawn OUTSIDE each rectangle
