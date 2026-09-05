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
//     [K]                              [K]
//     [W] [===== pigment 0 =====]      [W]
//     [W]                              [W]
//     [K]                              [K]
//     ... one such row per pigment
//
// Each reference column is quartered K/W/W/K, on BOTH sides of the pigment.
// That geometry is the whole point: averaging the two black quarters and the
// two white quarters puts BOTH references at exactly the same centroid as the
// pigment they flank — same x (left and right averaged), same y (top and bottom
// averaged). Any smooth illumination gradient or lens vignetting therefore
// affects the references and the pigment identically, and cancels out.
//
// Get this wrong and the correction is worse than none: references only at the
// row edges over-correct a pigment sampled from the centre, and references
// stacked above/below it skew under a vertical gradient.
#define CAL_ROWS        6   // one per palette entry
#define CAL_MARGIN_X   10
#define CAL_MARGIN_Y   20
#define CAL_ROW_H     120
#define CAL_ROW_GAP     8
#define CAL_CHIP_W     70   // reference column width (each side)
#define CAL_CHIP_QUARTERS 4  // K / W / W / K down each column
#define CAL_CHIP_GAP   10
#define CAL_PATCH_W   300   // the pigment itself
#define CAL_KEYLINE     2   // outline thickness, drawn OUTSIDE each rectangle
