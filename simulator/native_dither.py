"""
Run the FIRMWARE's dither instead of reimplementing it.

The simulator carried a second implementation of Floyd-Steinberg in Python.
Two implementations of one algorithm need a parity assertion for every shared
constant, and the one nobody wrote — the fill-mode enum — quietly rendered a
different image for a whole session. This removes the second implementation
from the hot path: the same dither.cpp the device runs is compiled for the
desktop and driven over a pipe.

It is also about 100x faster. Floyd-Steinberg is serial — each pixel's error
feeds the next — so it cannot be vectorised, and a 480x800 frame is a 384,000
iteration Python loop at roughly 18us each. The compiled version does it in
about 10ms.
"""
import subprocess
from pathlib import Path

import numpy as np

_ROOT = Path(__file__).resolve().parents[1]
_SRC = _ROOT / "firmware" / "src"
_CLI = _ROOT / "firmware" / "tools" / "dither_cli"
_CPP = _ROOT / "firmware" / "tools" / "dither_cli.cpp"
_GAMUT_CLI = _ROOT / "firmware" / "tools" / "gamut_cli"
_GAMUT_CPP = _ROOT / "firmware" / "tools" / "gamut_cli.cpp"


def _build_one(binary, source, deps):
    # Rebuild whenever anything it compiles in has moved on, so the simulator
    # can never quietly run yesterday's firmware.
    if binary.exists():
        built = binary.stat().st_mtime
        if all(not f.exists() or f.stat().st_mtime <= built
               for f in (source,) + tuple(deps)):
            return
    subprocess.run(
        ["c++", "-O2", "-std=c++17", "-D", "NATIVE_TEST",
         "-I", str(_ROOT / "firmware" / "test" / "mocks"), "-I", str(_SRC),
         "-o", str(binary), str(source)],
        check=True)


def build(force=False):
    _build_one(_CLI, _CPP, (_SRC / "dither.cpp", _SRC / "dither.h",
                            _SRC / "config.h", _SRC / "colour.h"))


def gamut_map(rgb, weight=None):
    """Firmware gamut mapping, for the same reason as the dither: one
    implementation, so the simulator cannot disagree with the panel."""
    _build_one(_GAMUT_CLI, _GAMUT_CPP,
               (_SRC / "gamut.h", _SRC / "colour.h", _SRC / "config.h"))
    a = np.ascontiguousarray(np.asarray(rgb, dtype=np.uint8))
    h, w = a.shape[:2]
    args = [str(_GAMUT_CLI), str(w), str(h)]
    if weight is not None:
        args.append(str(weight))
    out = subprocess.run(args, input=a.tobytes(), stdout=subprocess.PIPE,
                         check=True).stdout
    return np.frombuffer(out, dtype=np.uint8).reshape(h, w, 3)


def dither(rgb, profile_index=1):
    """Returns an (h, w) array of palette indices, from the firmware itself."""
    build()
    a = np.ascontiguousarray(np.asarray(rgb, dtype=np.uint8))
    h, w = a.shape[:2]
    if (w * h) % 2:
        raise ValueError("width * height must be even (4bpp packing)")
    out = subprocess.run([str(_CLI), str(w), str(h), str(profile_index)],
                         input=a.tobytes(), stdout=subprocess.PIPE,
                         check=True).stdout
    packed = np.frombuffer(out, dtype=np.uint8)
    idx = np.empty(w * h, dtype=np.uint8)
    idx[0::2] = packed >> 4          # high nibble first
    idx[1::2] = packed & 0x0F
    return idx.reshape(h, w)
