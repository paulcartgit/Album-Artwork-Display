// Expose the firmware's artwork gate so the simulator can validate it against
// real Cover Art Archive images rather than synthetic patterns.
#define NATIVE_TEST 1
#include "cover_match.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unistd.h>
int main(int argc, char** argv) {
    // stdin: two raw RGB888 images of the given size, back to back.
    int w = argc > 1 ? atoi(argv[1]) : 0, h = argc > 2 ? atoi(argv[2]) : 0;
    if (w <= 0 || h <= 0) { fprintf(stderr, "usage: cover_cli <w> <h>\n"); return 2; }
    const size_t one = (size_t)w * h * 3;
    std::vector<uint8_t> buf(one * 2);
    size_t got = 0;
    while (got < buf.size()) { ssize_t n = read(0, buf.data()+got, buf.size()-got); if (n<=0) break; got += n; }
    if (got != buf.size()) { fprintf(stderr, "short read\n"); return 3; }
    float a[COVER_SIG_LEN], b[COVER_SIG_LEN];
    coverSignature(buf.data(), w, h, a);
    coverSignature(buf.data() + one, w, h, b);
    printf("%.6f\n", coverSimilarity(a, b));
    return 0;
}
