"""
Read the firmware's own constants out of firmware/src/config.h.

The simulator used to hard-code its own palette and its own dithering
algorithm, and both drifted until the simulator predicted nothing about what
the device actually did (a seven-colour palette with an Orange that the panel
does not have, plain RGB matching, no virtual colours, a different layout).

Parsing the header means the palette and render profiles cannot drift again:
there is exactly one definition and it lives in the firmware.
"""

from pathlib import Path
import re

CONFIG_H = Path(__file__).resolve().parent.parent / "firmware" / "src" / "config.h"


def _read():
    if not CONFIG_H.exists():
        raise FileNotFoundError(
            f"Cannot find the firmware config at {CONFIG_H}. "
            "The simulator reads its constants from the firmware so the two "
            "cannot drift apart; run it from inside the repository."
        )
    return CONFIG_H.read_text()


def _parse_define(text, name):
    m = re.search(rf"^#define\s+{name}\s+(\S+)", text, re.M)
    if not m:
        raise ValueError(f"{name} not found in config.h")
    return int(m.group(1), 0)


def _parse_palette(text):
    block = re.search(
        r"static const PaletteColor PALETTE\[EPD_COLORS\]\s*=\s*\{(.*?)\n\};",
        text, re.S)
    if not block:
        raise ValueError("PALETTE not found in config.h")

    colors, names = [], []
    for line in block.group(1).splitlines():
        m = re.match(r"\s*\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,"
                     r"\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*\}", line)
        if not m:
            continue
        colors.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)))
        comment = re.search(r"//\s*(\w+)", line)
        names.append(comment.group(1) if comment else f"Color{len(names)}")
    if not colors:
        raise ValueError("PALETTE parsed but empty")
    return colors, names


def _parse_profiles(text):
    block = re.search(
        r"static const RenderProfile RENDER_PROFILES\[PROFILE_COUNT\]\s*=\s*\{(.*?)\n\};",
        text, re.S)
    if not block:
        raise ValueError("RENDER_PROFILES not found in config.h")

    profiles = []
    for line in block.group(1).splitlines():
        m = re.match(r'\s*\{\s*"([^"]+)"\s*,' + r"\s*([-\d.]+)f\s*," * 5 + r"\s*([-\d.]+)f\s*\}", line)
        if not m:
            continue
        profiles.append({
            "name": m.group(1),
            "sharpen": float(m.group(2)),
            "contrast": float(m.group(3)),
            "gamma": float(m.group(4)),
            "chroma_penalty_k": float(m.group(5)),
            "chroma_penalty_onset": float(m.group(6)),
            "edge_attenuation": float(m.group(7)),
        })
    if not profiles:
        raise ValueError("RENDER_PROFILES parsed but empty")
    return profiles


_TEXT = _read()

EPD_WIDTH = _parse_define(_TEXT, "EPD_WIDTH")
EPD_HEIGHT = _parse_define(_TEXT, "EPD_HEIGHT")
EPD_COLORS = _parse_define(_TEXT, "EPD_COLORS")

PALETTE_RGB, PALETTE_NAMES = _parse_palette(_TEXT)
RENDER_PROFILES = _parse_profiles(_TEXT)

PALETTE_HEX = ["#%02X%02X%02X" % c for c in PALETTE_RGB]

assert len(PALETTE_RGB) == EPD_COLORS, (
    f"config.h declares EPD_COLORS={EPD_COLORS} but PALETTE has {len(PALETTE_RGB)} entries")

def _parse_fill_policy():
    """
    Constants from firmware/src/fill_policy.h.

    The fill algorithm exists in both C++ and Python, and they have already
    disagreed once — the device cropped a sleeve the simulator said to leave
    alone, because the two sampled at different resolutions. Reading the
    thresholds from the header removes at least that class of drift.
    """
    path = CONFIG_H.parent / "fill_policy.h"
    if not path.exists():
        raise FileNotFoundError(f"Cannot find {path}")
    text = path.read_text()
    out = {}
    for name in ("FILL_MAX_ZOOM", "FILL_CUT_LIMIT", "FILL_SCAN_SIZE"):
        m = re.search(rf"#define\s+{name}\s+([0-9.]+)f?", text)
        if not m:
            raise ValueError(f"{name} not found in fill_policy.h")
        out[name] = float(m.group(1))
    return out


FILL = _parse_fill_policy()
FILL_MAX_ZOOM = FILL["FILL_MAX_ZOOM"]
FILL_CUT_LIMIT = FILL["FILL_CUT_LIMIT"]
FILL_SCAN_SIZE = int(FILL["FILL_SCAN_SIZE"])

DEFAULT_PROFILE = 1  # PROFILE_NATURAL


def profile(index=DEFAULT_PROFILE):
    if not 0 <= index < len(RENDER_PROFILES):
        index = DEFAULT_PROFILE
    return RENDER_PROFILES[index]

def _parse_fill_modes():
    """
    FillMode from config.h. The simulator used to restate this enum from
    memory and had ADAPTIVE and COVER the wrong way round, so it silently
    rendered a different fill from the device for the whole of a session.
    """
    m = re.search(r"enum FillMode \{(.*?)\};", _TEXT, re.S)
    if not m:
        raise ValueError("FillMode not found in config.h")
    out = {}
    for name, value in re.findall(r"(FILL_[A-Z]+)\s*=\s*(\d+)", m.group(1)):
        out[name] = int(value)
    for required in ("FILL_FIT", "FILL_ADAPTIVE", "FILL_BLEED", "FILL_COVER"):
        if required not in out:
            raise ValueError(f"{required} missing from FillMode")
    return out


FILL_MODES = _parse_fill_modes()
