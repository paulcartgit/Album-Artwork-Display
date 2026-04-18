# Dithering on 6-Colour E-Ink Displays

## A practical guide to Floyd-Steinberg dithering for limited-palette e-paper

This document captures the findings from building a real-time album artwork display on a **GDEP073E01** 6-colour e-paper panel (480 × 800, driven by an ESP32-S3). The palette is **Black, White, Green, Blue, Red, Yellow** — six of the eight RGB cube corners, missing only **Cyan** and **Magenta**. These findings should be applicable to any similar limited-palette e-ink display (Waveshare Spectra 6, Good Display ACeP, etc.).

---

## Table of Contents

1. [The Hardware Palette](#the-hardware-palette)
2. [Calibrated vs Idealised Palette Values](#calibrated-vs-idealised-palette-values)
3. [Colour Space Selection](#colour-space-selection)
4. [The Missing-Colour Problem](#the-missing-colour-problem)
5. [Virtual Palette Entries](#virtual-palette-entries)
6. [Chroma-Aware Penalty](#chroma-aware-penalty)
7. [Error Reference Strategy](#error-reference-strategy)
8. [Edge-Aware Diffusion](#edge-aware-diffusion)
9. [Shadow Chroma Suppression](#shadow-chroma-suppression)
10. [Serpentine Scanning](#serpentine-scanning)
11. [Error Capping](#error-capping)
12. [Saturation Boosting (What Didn't Work)](#saturation-boosting-what-didnt-work)
13. [Dual Match Palette (What Didn't Work)](#dual-match-palette-what-didnt-work)
14. [Summary of the Final Algorithm](#summary-of-the-final-algorithm)
15. [Key References](#key-references)

---

## The Hardware Palette

The GDEP073E01 (and similar Spectra 6 panels) use 6 physical pigments:

| Index | Name   | Appearance              |
|-------|--------|-------------------------|
| 0     | Black  | Near-black charcoal     |
| 1     | White  | Light grey, cool tint   |
| 2     | Green  | Dark teal-green         |
| 3     | Blue   | Medium-bright, saturated|
| 4     | Red    | Dark brick-crimson      |
| 5     | Yellow | Warm golden             |

Crucially, there is **no Cyan** and **no Magenta**. Any purple, pink, teal, or orange must be produced entirely through dithering — interleaving physical pigment dots so the eye blends them perceptually.

---

## Calibrated vs Idealised Palette Values

### The calibration trap

Our first instinct was to photograph the display showing each pure colour, then colour-pick the actual RGB values:

```
Black  (0x10, 0x10, 0x12)   White  (0xD8, 0xDA, 0xD4)
Green  (0x30, 0x66, 0x58)   Blue   (0x38, 0x68, 0xC0)
Red    (0x9C, 0x30, 0x2C)   Yellow (0xC8, 0xB8, 0x30)
```

These "calibrated" values were used for both nearest-colour matching AND error computation. The result was **catastrophic for dithering**:

- **White (216, 218, 212)** sits close to light lavender in RGB space, so the matching algorithm constantly picks White for pastel inputs, washing them out
- **Red (156, 48, 44)** has a blue component of only 44, making it astronomically distant from any purple input — the algorithm never picks Red when it should be alternating Red↔Blue to create purple
- The **gamut collapses**: calibrated Green (48, 102, 88) and calibrated White (216, 218, 212) are both desaturated and close together, eliminating the colour space that dithering needs to work in

### The solution: idealised RGB for matching

Use pure RGB cube corners for the matching palette:

```
Black   (0,   0,   0)     White   (255, 255, 255)
Green   (0,   255, 0)     Blue    (0,   0,   255)
Red     (255, 0,   0)     Yellow  (255, 255, 0)
```

This gives maximum gamut separation and ensures the dithering algorithm "thinks" in a full-gamut space. Error diffusion still works correctly because it drives the algorithm toward placing the right mix of physical pigments over an area — the quantisation error automatically compensates for the mismatch between idealised and physical appearance.

> **Key insight**: The palette values used for colour matching should represent the *intended* colour, not the physical pigment appearance. The physical appearance only matters for what the viewer sees — the algorithm needs clean separation to make good mixing decisions.

For error computation, we also used idealised values rather than calibrated ones. Using calibrated values for error created large artificial errors (e.g., matching White but computing error against (216, 218, 212) instead of (255, 255, 255)) that destabilised the diffusion.

---

## Colour Space Selection

### What we tried

| Space | Matching | Error Diffusion | Result |
|-------|----------|-----------------|--------|
| sRGB  | sRGB     | sRGB            | Pink→blue, skin tones grey. White dominates light colours. |
| CIELAB| CIELAB   | CIELAB          | Better perceptual matching, but error diffusion in Lab couples channels (L error creates a/b artefacts). |
| CIELAB| CIELAB   | RGB             | **Best overall.** Perceptual matching + independent channel error. |
| YCbCr | YCbCr    | YCbCr           | Collapsed purple signals, everything grey. Abandoned. |

### Why "Lab matching + RGB error" works

- **CIELAB matching** is perceptually uniform: the Euclidean distance in Lab space corresponds much better to how humans perceive colour differences. This means the algorithm makes visually sensible choices about which palette colour is "closest."

- **RGB error diffusion** keeps the three colour channels independent. In Lab space, lightness error leaks into chroma and vice versa, creating subtle colour casts. In RGB, a red deficit stays a red deficit — it doesn't accidentally create a blue push.

This hybrid approach ("Lab errRGB") consistently outperformed pure Lab or pure RGB across test images with skin tones, saturated colours, and subtle gradients.

### L-channel weighting

Early iterations suffered from skin tones turning blue. The fix was applying a **1.5× weight on the L (lightness) channel** during Lab distance calculation. This prioritises brightness matching, which prevents the algorithm from substituting a colour that has similar chroma but wildly different brightness.

However, we later moved to a chroma-aware penalty system (see below) which made the L-weight unnecessary.

---

## The Missing-Colour Problem

This is the central challenge. With 6 colours that are 6 of the 8 RGB cube corners, the missing corners are **Cyan (0, 255, 255)** and **Magenta (255, 0, 255)**.

For **purple** on the display, the algorithm must interleave Red and Blue pixels. But standard error diffusion cannot achieve this:

1. Purple input like (128, 0, 128) is much closer to Blue (0, 0, 255) than to Red (255, 0, 0) in almost any colour space
2. The algorithm picks Blue. The error propagated is (+128, 0, −127) in RGB
3. The next pixel sees roughly (256, 0, 1), which matches Red
4. But the error from Red is (−127, 0, +127), pushing the *next* pixel back toward Blue
5. In theory this should oscillate... but the Green and White palette entries interfere

The problem is that in **any perceptual colour space** (Lab, YCbCr, etc.), Red sits ~89° away from purple on the hue wheel. The error diffusion path from purple goes through Blue, then overshoots past various colours (Yellow, White) before eventually reaching Red — if it reaches Red at all. Standard error diffusion simply cannot reliably create the Red↔Blue alternation needed for purple.

### What we tried that failed

- **Boosting Red's blue component** in a separate "match palette" (Red at (156, 48, 100) instead of (156, 48, 44)). This pulled Red closer to purple for selection, but the massive error between the biased match value and real value caused blown-out whites and unstable diffusion.

- **Shrinking White's catchment area** by shifting White to (245, 245, 240) in the match palette. Same problem — the mismatch between match value and error value created cascading over-corrections.

- **Saturation boosting** the input image to push pastels further from White. This helped purple but destroyed skin tones by pushing them toward Yellow and Red.

- **Chrominance-preserving edge detection** that only attenuated luminance error at edges while passing chrominance through. This helped purple in textured areas but caused text to blur into backgrounds.

---

## Virtual Palette Entries

The breakthrough came from the [libcaca study on colour quantisation](https://caca.zoy.org/study/part6.html) (§6.1–6.2) and the Waveshare/Spectra 6 community on Reddit.

### The idea

Add **virtual palette entries** for the missing RGB cube corners: **Cyan (0, 255, 255)** and **Magenta (255, 0, 255)**. The matching palette becomes 8 colours:

```
0: Black   (0,   0,   0)      4: Red     (255, 0,   0)
1: White   (255, 255, 255)    5: Yellow  (255, 255, 0)
2: Green   (0,   255, 0)      6: Cyan    (0,   255, 255)  ← virtual
3: Blue    (0,   0,   255)    7: Magenta (255, 0,   255)  ← virtual
```

When the algorithm selects Cyan or Magenta, they are **mapped to alternating real colours** using a spatial checkerboard:

```c
if (ci == 6) {        // Cyan → alternate Green/Blue
    displayIdx = ((x + y) & 1) ? GREEN : BLUE;
} else if (ci == 7) { // Magenta → alternate Red/Blue
    displayIdx = ((x + y) & 1) ? RED : BLUE;
}
```

### Why this works

Purple input (128, 0, 128) is now closest to **Magenta (255, 0, 255)** in Lab space. The algorithm selects Magenta, which gets physically rendered as either Red or Blue depending on position. The `(x + y) & 1` checkerboard ensures a 50/50 mix over any area, and from normal viewing distance, the eye blends this into purple.

This completely sidesteps the error diffusion path problem. Instead of relying on error to eventually cycle through Red and Blue, we directly select both via the virtual entry.

### Teal / cyan tones

The same trick works for the other missing corner. Teal input (0, 128, 128) matches Cyan, which alternates Green and Blue, producing the teal that would otherwise be impossible.

---

## Chroma-Aware Penalty

Even with virtual palette entries, White and Black "steal" chromatic pixels. Light purple (Lab L\*=57, C\*=48) is perceptually closer to White (L\*=100, C\*=0) than to Magenta (L\*=60, a\*=+98) because the lightness gap to White is smaller than the a\* gap to Magenta.

### The penalty function

When a pixel has significant **chroma** (saturation in Lab space), achromatic palette entries (White, Black — those with chroma < 5) receive a distance penalty:

```c
float pxChroma = sqrt(px.a² + px.b²);
float excess   = max(0, pxChroma - CHROMA_PENALTY_ONSET);
float penalty  = excess² * CHROMA_PENALTY_K;

// Applied during matching:
if (paletteChroma[i] < 5.0) distance += penalty;
```

### Parameter tuning

| Parameter | Value | Effect |
|-----------|-------|--------|
| `CHROMA_PENALTY_K` | 5.0 | Strength of penalty. Too low (2.0) → Black still steals dark purples. Too high (10.0) → even neutral greys get forced toward colours. |
| `CHROMA_PENALTY_ONSET` | 12.0 | Chroma threshold below which no penalty applies. Prevents neutral/near-neutral pixels from being forced toward colours. |

The penalty is **quadratic** in chroma excess: mildly chromatic pixels get a gentle nudge, while strongly saturated pixels get a heavy push away from White/Black.

### The effect on purple

For dark purple (80, 0, 80) with chroma ~47:
- Without penalty: Black wins (small lightness gap)
- With penalty (K=5.0, onset=12): penalty = (47−12)² × 5.0 = 6125 added to Black's distance → Magenta wins decisively

---

## Error Reference Strategy

When a virtual colour (Cyan/Magenta) is placed, what RGB value do we compute the quantisation error against?

### Option 1: True per-pixel error (the actual colour placed)

If Magenta is placed as Red on this pixel: `error = pixel − Red(255, 0, 0)`.
If Magenta is placed as Blue on the next: `error = pixel − Blue(0, 0, 255)`.

**Problem**: The error oscillates violently. After Red, the blue-channel error is enormous (+128), which pushes the next pixel hard toward Blue. After Blue, the red-channel error is enormous (+128), pushing back toward Red. While this sounds correct, it creates visible high-frequency noise in practice.

### Option 2: Average error reference

Use the average of the two alternating colours:
- Magenta → avg(Red, Blue) = **(127.5, 0, 127.5)**
- Cyan → avg(Green, Blue) = **(0, 127.5, 127.5)**

**Advantage**: Smooth error accumulation. The total error over a 2-pixel pair is identical to the true per-pixel approach, but the individual pixel errors are moderate rather than violent.

**Disadvantage**: For dark purples (say 80, 0, 80), the average reference (127.5, 0, 127.5) is brighter than the input, so the error is negative, which can push neighbouring pixels toward Black — exactly the contamination we're trying to avoid.

### What we chose

We tested both approaches and found that **per-pixel true error** produced slightly more vivid purples but with more visible noise, while **average error** produced smoother results with slightly more Black contamination.

The final implementation uses per-pixel true error, combined with the strong chroma penalty (K=5.0) to prevent Black from absorbing the dark purple pixels that per-pixel error tends to push toward darkness.

---

## Edge-Aware Diffusion

Standard Floyd-Steinberg diffusion bleeds error across hard edges, creating colour fringing around text and sharp boundaries (e.g., album artwork text, sharp colour transitions).

### Implementation

1. **Build an edge map** from the source image using luminance gradient magnitude:
   ```
   gx = luminance(x+1, y) − luminance(x−1, y)
   gy = luminance(x, y+1) − luminance(x, y−1)
   edge = sqrt(gx² + gy²) / 150.0   // normalised to [0, 1]
   ```

2. **Attenuate error** at both source and target pixels:
   ```
   atten = 1.0 − edge × 0.85
   ```
   - At the source pixel: multiply the quantisation error by `atten`
   - In the F-S kernel: multiply each neighbour's share by the target pixel's `atten`

### Threshold tuning

The normalisation divisor (150.0) acts as an edge sensitivity threshold:
- **Too low** (e.g., 50): photographic texture and grain are treated as edges, killing dithering in detailed areas
- **Too high** (e.g., 300): only the sharpest edges are detected, losing the benefit
- **150** was the sweet spot for photographic album artwork

### Chrominance-preserving edge detection (abandoned)

We tried an approach where edge detection only attenuated **luminance** error while passing chrominance through at full strength. The idea was to preserve colour mixing (Red+Blue→purple) through textured areas while still preventing brightness bleed.

**Result**: It helped purple in fur/fabric textures but caused text to lose sharpness against coloured backgrounds, as chrominance error bled freely across text boundaries. Reverted to uniform attenuation.

---

## Shadow Chroma Suppression

In very dark regions (luminance < 8), humans can barely perceive colour, but error diffusion accumulates chrominance error across consecutive Black pixels until it flips one to Blue or Red — creating isolated coloured pixels in shadows that look like noise.

### Fix

Below a luminance threshold of 8.0, progressively suppress the chrominance component of the error:

```c
float chromaScale = (lum / 8.0)²;  // quadratic ramp
// Decompose error into luminance + chrominance
eLum = 0.299*er + 0.587*eg + 0.114*eb;
er = eLum + (er − eLum) * chromaScale;
eg = eLum + (eg − eLum) * chromaScale;
eb = eLum + (eb − eLum) * chromaScale;
```

This keeps error flowing for brightness correction (smooth dark gradients) while preventing colour noise in shadows.

---

## Serpentine Scanning

Standard left-to-right scanning creates visible directional streaks — error accumulates consistently toward the right, producing a horizontal bias in dithering patterns.

**Serpentine scan** alternates direction each row:
- Even rows: left → right
- Odd rows: right → left

The F-S kernel must be mirrored when scanning right-to-left (the `(ltr ? (dx) : -(dx))` in our implementation). This eliminates directional artefacts completely at zero quality cost.

---

## Error Capping

Early iterations suffered from **error explosion**: a single pixel far from all palette colours generates enormous error that cascades through neighbours, creating large single-colour blobs.

### Soft error cap

Rather than hard-clamping error (which creates its own artefacts), apply a **soft dampening**:

```c
float errMag = er² + eg² + eb²;
if (errMag > threshold²) {
    float scale = dampening;  // e.g. 0.6
    er *= scale; eg *= scale; eb *= scale;
}
```

Parameters that worked:
- **Threshold**: 2500 (squared magnitude ~50 per channel)
- **Dampening**: 0.6

**Too aggressive** (threshold=900): Skin tones lose subtle colour variation, appearing grey-blue.
**Too gentle** (threshold=5000): Error explosion still visible in high-contrast transitions.

In our final implementation, we removed explicit error capping because the chroma penalty system and edge-aware diffusion together prevented the worst error explosion cases.

---

## Saturation Boosting (What Didn't Work)

To push light pastels away from White, we tried boosting saturation of the input image before dithering — scaling each RGB channel away from the per-pixel grey midpoint by 1.3–1.5×.

**Problem**: Saturation boosting is a blunt instrument:
- Purple/lavender → pushed toward vivid purple ✓
- Skin tones → pushed toward vivid orange/yellow ✗
- Light blues → pushed toward vivid blue, losing the white interleave that creates "light" ✗

A hue-selective boost (only for cool/purple hues) would have been more targeted, but added complexity. The virtual palette + chroma penalty approach solved the problem more cleanly.

---

## Dual Match Palette (What Didn't Work)

We created a separate "match palette" with biased values for selection (Red shifted bluer, White shifted brighter) while using real values for error computation. The theory was sound — bias the selection without corrupting the error math.

**Problem in practice**: Any mismatch between "what the algorithm thinks it placed" and "what the error says was placed" creates systematic drift. For example:
- Red matched at (156, 48, **100**) but error computed against (156, 48, **44**)
- The 56-unit blue discrepancy accumulates as phantom blue error
- This pushes subsequent pixels toward White to compensate → blown-out whites

The magnitude of this drift depends on how far the bias shifts the match values, but even modest shifts (28 units) produced visible artefacts.

**Lesson**: Match and error palettes must be consistent. The virtual palette approach achieves the same goal (getting Red+Blue to appear together) without any match/error mismatch.

---

## Summary of the Final Algorithm

```
FOR each row (serpentine: alternating L→R / R→L):
    Load source RGB + accumulated error
    FOR each pixel:
        1. Convert pixel to CIELAB
        2. Find nearest colour in 8-entry palette (6 real + Cyan + Magenta)
           → Apply chroma penalty (K=5.0) to Black/White when pixel is chromatic
        3. Map virtual colours to physical checkerboard:
           Cyan → Green/Blue, Magenta → Red/Blue  [based on (x+y) & 1]
        4. Compute RGB error against ACTUAL placed colour (idealized values)
        5. In shadows (lum < 8): suppress chrominance component of error
        6. At edges: attenuate error by (1 − edge×0.85)
        7. Distribute error via Floyd-Steinberg kernel (7/16, 3/16, 5/16, 1/16)
           → Also attenuate each kernel target by its own edge strength
        8. Pack 4-bit palette index into output buffer
```

This produces visually compelling results across a wide range of photographic content: skin tones, saturated album artwork, subtle gradients, text overlays, and (critically) purple/magenta/teal tones that are impossible to produce with naïve 6-colour dithering.

---

## Key References

- **libcaca colour study**: [caca.zoy.org/study/part6.html](https://caca.zoy.org/study/part6.html) — The virtual palette/checkerboard approach for missing colours (§6.1–6.2). Essential reading for anyone working with limited palettes.

- **Reddit Spectra 6 thread**: Community discussion of dithering approaches for Waveshare 6-colour e-ink panels, including calibrated palette values and error diffusion strategies.

- **Floyd-Steinberg dithering**: Floyd, R.W. and Steinberg, L. (1976). "An Adaptive Algorithm for Spatial Greyscale." The classic error-diffusion kernel. The 7-3-5-1 weights (/16) provide tight error locality — larger kernels (Stucki, Burkes) spread error too wide for the coarse e-ink palette.

- **CIELAB colour space**: Developed by the International Commission on Illumination (CIE) in 1976 for perceptual uniformity. Critical for correct colour matching in limited palettes where Euclidean distance must correlate with perceived difference.

---

## Appendix: Test Pattern Approach

Building a **dither test pattern** was invaluable for diagnosing issues. Rather than testing with real photographs (too many variables), we generated a synthetic RGB888 image with flat-colour swatches:

- Row 0: Pure palette colours (undithered reference)
- Row 1: 50/50 mixes (Red+Blue target, Red+White target, etc.)
- Rows 2–3: Purple and pink gradients (light → dark)
- Row 4: Violet/magenta shades
- Row 5: Grey gradient (luminance dithering only)
- Rows 6–7: Warm and cool tones

This image was passed through the actual dither pipeline and displayed on hardware. Photographing the result immediately revealed which colours were working and which were being absorbed by White or Black.

**Recommendation**: Always build a test pattern generator early when working on e-ink dithering. It eliminates variables from JPEG decoding, colour space conversion, and image content, letting you see the dithering algorithm's behaviour in isolation.

---

*This document was produced during the development of an ESP32-S3-based album artwork display using the GDEP073E01 6-colour e-paper panel. The full source code is available in the project repository.*
