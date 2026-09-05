#pragma once
#include <cstdint>
#include "config.h"

// Floyd-Steinberg dithering of an RGB888 image to the calibrated 6-colour
// EPD palette (see PALETTE in config.h).
//
// Input:  rgb888 buffer (w * h * 3 bytes)
// Output: packed buffer (w * h / 2 bytes, 4 bits per pixel, high nibble first)
//
// The profile controls chroma-penalty strength and edge-aware attenuation;
// it defaults to PROFILE_NATURAL, which is the historical behaviour.
void ditherFloydSteinberg(const uint8_t* rgb888, uint8_t* packedOut, int w, int h,
                          const RenderProfile& profile = RENDER_PROFILES[PROFILE_NATURAL]);
