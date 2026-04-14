#pragma once
#include <cstdint>

// Floyd-Steinberg dithering of RGB888 image to 7-color EPD palette
// Input:  rgb888 buffer (w * h * 3 bytes)
// Output: packed buffer (w * h / 2 bytes, 4 bits per pixel, high nibble first)
void ditherFloydSteinberg(const uint8_t* rgb888, uint8_t* packedOut, int w, int h);

// Nearest-color quantization (no dithering)
void quantizeNearest(const uint8_t* rgb888, uint8_t* packedOut, int w, int h);
