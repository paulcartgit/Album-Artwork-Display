// Expose the firmware's gamut mapping so the simulator can check it agrees.
#define NATIVE_TEST 1
#include "config.h"
#include "gamut.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unistd.h>
int main(int argc, char** argv) {
    int w = argc > 1 ? atoi(argv[1]) : 0, h = argc > 2 ? atoi(argv[2]) : 0;
    float wgt = argc > 3 ? (float)atof(argv[3]) : GAMUT_LIGHTNESS_WEIGHT;
    if (w <= 0 || h <= 0) { fprintf(stderr, "usage: gamut_cli <w> <h> [weight]\n"); return 2; }
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    size_t got = 0;
    while (got < rgb.size()) { ssize_t n = read(0, rgb.data()+got, rgb.size()-got); if (n<=0) break; got += n; }
    if (got != rgb.size()) { fprintf(stderr, "short read\n"); return 3; }
    gamutMapApply(rgb.data(), w, h, wgt);
    size_t put = 0;
    while (put < rgb.size()) { ssize_t n = write(1, rgb.data()+put, rgb.size()-put); if (n<=0) break; put += n; }
    return 0;
}
