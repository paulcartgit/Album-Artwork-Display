#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ─── Minimal indexed-PNG writer ───
//
// The panel buffer is already 4 bits per pixel, two pixels per byte, high
// nibble first — which is exactly PNG's 4-bit indexed layout. So the encoder is
// a header, a palette, and the rows copied through with a filter byte in front.
//
// This replaced a BMP of the same data. The BMP was correct and decoded
// perfectly in Python, which is how it passed review; Safari simply does not
// render 4-bit indexed BMPs, so the portal showed a placeholder instead of the
// artwork. Verifying with a decoder rather than the actual consumer is what
// hid it. PNG is rendered by everything.
//
// Deflate is used in "stored" mode — no compression. The data is a dithered
// image, so it would barely compress anyway, and this keeps the encoder to
// arithmetic that cannot fail.

inline uint32_t pngCrc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc;
}

class PngWriter {
public:
    PngWriter(uint8_t* out, size_t cap) : _p(out), _cap(cap), _n(0) {}

    size_t size() const { return _n; }
    bool overflowed() const { return _over; }

    void u8(uint8_t v)  { if (_n < _cap) _p[_n++] = v; else _over = true; }
    void u32(uint32_t v) { u8(v >> 24); u8(v >> 16); u8(v >> 8); u8(v); }
    void raw(const uint8_t* d, size_t len) {
        for (size_t i = 0; i < len; i++) u8(d[i]);
    }

    void chunk(const char* type, const uint8_t* data, size_t len) {
        u32((uint32_t)len);
        size_t start = _n;
        raw((const uint8_t*)type, 4);
        raw(data, len);
        if (_n >= start) u32(pngCrc32(_p + start, _n - start) ^ 0xFFFFFFFFu);
    }

private:
    uint8_t* _p; size_t _cap; size_t _n; bool _over = false;
};

// Writes a complete 4-bit indexed PNG. `rows` is height * (width/2) bytes.
// `palette` is paletteCount RGB triples. Returns bytes written, or 0.
inline size_t pngWriteIndexed4(uint8_t* out, size_t cap,
                               const uint8_t* rows, int width, int height,
                               const uint8_t* palette, int paletteCount) {
    if (width % 2) return 0;                 // 4bpp packs two pixels per byte
    const size_t rowBytes = (size_t)width / 2;

    PngWriter w(out, cap);
    static const uint8_t SIG[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    w.raw(SIG, 8);

    uint8_t ihdr[13];
    ihdr[0]=width>>24; ihdr[1]=width>>16; ihdr[2]=width>>8; ihdr[3]=width;
    ihdr[4]=height>>24; ihdr[5]=height>>16; ihdr[6]=height>>8; ihdr[7]=height;
    ihdr[8]=4;    // bit depth
    ihdr[9]=3;    // colour type: indexed
    ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;      // deflate, adaptive filter, no interlace
    w.chunk("IHDR", ihdr, 13);
    w.chunk("PLTE", palette, (size_t)paletteCount * 3);

    // IDAT: zlib around stored deflate blocks. Length is known up front, so the
    // chunk header can be written before the payload is assembled.
    const size_t rawLen = (size_t)height * (rowBytes + 1);   // +1 filter byte
    size_t blocks = (rawLen + 65534) / 65535;
    const size_t zlibLen = 2 + blocks * 5 + rawLen + 4;

    w.u32((uint32_t)zlibLen);
    size_t idatStart = w.size();
    w.raw((const uint8_t*)"IDAT", 4);
    w.u8(0x78); w.u8(0x01);                  // zlib header, no preset dictionary

    uint32_t a = 1, b = 0;                   // adler32, computed as we go
    auto feed = [&](uint8_t v) {
        a = (a + v) % 65521; b = (b + a) % 65521; w.u8(v);
    };

    size_t emitted = 0;
    int row = 0;
    size_t col = 0;                          // 0 = filter byte, then row data
    while (emitted < rawLen) {
        size_t remaining = rawLen - emitted;
        uint16_t blockLen = (uint16_t)(remaining > 65535 ? 65535 : remaining);
        bool last = (size_t)blockLen == remaining;
        w.u8(last ? 1 : 0);
        w.u8(blockLen & 0xFF); w.u8(blockLen >> 8);
        w.u8(~blockLen & 0xFF); w.u8((~blockLen >> 8) & 0xFF);

        for (uint16_t i = 0; i < blockLen; i++) {
            if (col == 0) feed(0);           // filter type: None
            else          feed(rows[(size_t)row * rowBytes + (col - 1)]);
            if (++col > rowBytes) { col = 0; row++; }
        }
        emitted += blockLen;
    }
    w.u32((b << 16) | a);                    // adler32

    // CRC over "IDAT" plus the zlib stream
    uint32_t crc = pngCrc32(out + idatStart, w.size() - idatStart) ^ 0xFFFFFFFFu;
    w.u32(crc);

    w.chunk("IEND", nullptr, 0);
    return w.overflowed() ? 0 : w.size();
}
