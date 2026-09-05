// Expose the FIRMWARE dither to the simulator.
//
// The simulator had a second, independent implementation of the same
// algorithm in Python. Keeping two implementations honest needs a parity
// check for every constant, and one that was never written — the fill-mode
// enum — silently rendered a different image for a whole session. It was also
// ~100x slower, because Floyd-Steinberg is serial and cannot be vectorised, so
// a 480x800 frame is a 384,000-iteration Python loop.
//
// This runs the real dither.cpp. stdin: w, h, profile, then w*h*3 bytes of
// RGB888. stdout: w*h/2 packed bytes, 4bpp, high nibble first.
#define NATIVE_TEST 1
#include "config.h"
#include "dither.h"
#include "dither.cpp"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unistd.h>

int main(int argc, char** argv) {
    int w = 0, h = 0, profileId = 1;
    if (argc >= 4) { w = atoi(argv[1]); h = atoi(argv[2]); profileId = atoi(argv[3]); }
    if (w <= 0 || h <= 0 || (w * h) % 2) {
        fprintf(stderr, "usage: dither_cli <w> <h> <profile>  (w*h must be even)\n");
        return 2;
    }

    std::vector<uint8_t> rgb((size_t)w * h * 3);
    size_t got = 0;
    while (got < rgb.size()) {
        ssize_t n = read(STDIN_FILENO, rgb.data() + got, rgb.size() - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    if (got != rgb.size()) {
        fprintf(stderr, "short read: %zu of %zu\n", got, rgb.size());
        return 3;
    }

    std::vector<uint8_t> packed((size_t)w * h / 2);
    ditherFloydSteinberg(rgb.data(), packed.data(), w, h, renderProfile((uint8_t)profileId));

    size_t put = 0;
    while (put < packed.size()) {
        ssize_t n = write(STDOUT_FILENO, packed.data() + put, packed.size() - put);
        if (n <= 0) break;
        put += (size_t)n;
    }
    return put == packed.size() ? 0 : 4;
}
