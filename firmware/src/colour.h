#pragma once
#include <cmath>
#include <cstdint>

// ═══════════════════════════════════════════════════════════
// sRGB <-> CIELAB
//
// Shared between the dither (which matches in Lab) and the tone mapping
// (which compresses lightness in Lab so hue survives). Header-only and free
// of Arduino types so the native tests and parity check can use it.
// ═══════════════════════════════════════════════════════════

struct Lab { float L, a, b; };

// Float-accepting, because pixels carry accumulated error and can land
// outside [0,255] before clamping.
inline Lab rgbToLabF(float r, float g, float b) {
    r = fmaxf(0.0f, fminf(255.0f, r)) / 255.0f;
    g = fmaxf(0.0f, fminf(255.0f, g)) / 255.0f;
    b = fmaxf(0.0f, fminf(255.0f, b)) / 255.0f;
    auto decode = [](float v) -> float {
        return (v <= 0.04045f) ? v / 12.92f
                               : powf((v + 0.055f) / 1.055f, 2.4f);
    };
    float lr = decode(r), lg = decode(g), lb = decode(b);

    // Linear sRGB → XYZ (D65 illuminant)
    float x = lr * 0.4124564f + lg * 0.3575761f + lb * 0.1804375f;
    float y = lr * 0.2126729f + lg * 0.7151522f + lb * 0.0721750f;
    float z = lr * 0.0193339f + lg * 0.1191920f + lb * 0.9503041f;
    x /= 0.95047f;
    z /= 1.08883f;

    auto labf = [](float t) -> float {
        return (t > 0.008856f) ? cbrtf(t) : (7.787f * t + 16.0f / 116.0f);
    };
    float fx = labf(x), fy = labf(y), fz = labf(z);
    return { 116.0f * fy - 16.0f,
             500.0f * (fx - fy),
             200.0f * (fy - fz) };
}

inline void labToRgbF(const Lab& c, float* r, float* g, float* b) {
    float fy = (c.L + 16.0f) / 116.0f;
    float fx = fy + c.a / 500.0f;
    float fz = fy - c.b / 200.0f;
    auto inv = [](float t) -> float {
        float t3 = t * t * t;
        return (t3 > 0.008856f) ? t3 : (t - 16.0f / 116.0f) / 7.787f;
    };
    float x = inv(fx) * 0.95047f, y = inv(fy), z = inv(fz) * 1.08883f;

    float lr =  3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
    float lg = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
    float lb =  0.0556434f * x - 0.2040259f * y + 1.0572252f * z;

    auto encode = [](float v) -> float {
        v = fmaxf(0.0f, fminf(1.0f, v));
        return (v <= 0.0031308f) ? v * 12.92f
                                 : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    };
    *r = encode(lr) * 255.0f;
    *g = encode(lg) * 255.0f;
    *b = encode(lb) * 255.0f;
}
