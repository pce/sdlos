"""
sdlos.assets.png
================
Pure-Python RGBA PNG encoder — no Pillow, no numpy required.

All functions return ``bytes`` ready to be written to a ``.png`` file or
passed directly to an image loader.

Public API
----------
encode(pixels, width, height) → bytes
    Encode raw RGBA8 bytes (width × height × 4) as a PNG stream.

dot(size, *, radius, peak_alpha, color) → bytes
    Soft radial dot on a transparent background.
    Alpha follows a cosine falloff from *peak_alpha* at the centre to 0
    at *radius* pixels.  Everything outside *radius* is fully transparent.

solid(width, height, *, color) → bytes
    Flat single-colour fill.

gradient_h(width, height, *, left, right) → bytes
    Horizontal linear gradient between two RGBA colours.

gradient_radial(size, *, inner, outer, radius) → bytes
    Radial gradient from *inner* (centre) to *outer* (edge).

Format details
--------------
All output is an IHDR + single IDAT + IEND PNG.
Colour type 6 (RGBA), bit depth 8, no interlacing.
IDAT payload is zlib-compressed at level 9.
Each row uses filter type 0 (None) — no per-row delta filter.
"""

from __future__ import annotations

import math
import struct
import zlib
from typing import Union

# ── Type aliases ──────────────────────────────────────────────────────────────

RGBA = tuple[int, int, int, int]
RGB  = tuple[int, int, int]


# ── Low-level encoder ─────────────────────────────────────────────────────────

def encode(pixels: bytes, width: int, height: int) -> bytes:
    """Encode *width* × *height* raw RGBA8 bytes as a PNG stream.

    Parameters
    ----------
    pixels:
        Raw pixel data in R G B A order, row-major.
        Must be exactly ``width * height * 4`` bytes.
    width:
        Image width in pixels.
    height:
        Image height in pixels.

    Returns
    -------
    bytes
        A complete, self-contained PNG file.

    Raises
    ------
    ValueError
        If ``len(pixels) != width * height * 4``.
    """
    expected = width * height * 4
    if len(pixels) != expected:
        raise ValueError(
            f"pixels length {len(pixels)} does not match "
            f"width={width} × height={height} × 4 = {expected}"
        )

    stride = width * 4

    # Prepend PNG filter byte 0x00 (None) to each scanline.
    raw = bytearray(height * (1 + stride))
    for y in range(height):
        raw[y * (1 + stride)] = 0                      # filter byte
        base = y * (1 + stride) + 1
        raw[base : base + stride] = pixels[y * stride : (y + 1) * stride]

    compressed = zlib.compress(bytes(raw), level=9)

    def _chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    # IHDR: width(4) height(4) bitdepth(1=8) colortype(1=6/RGBA)
    #       compress(1=0) filter(1=0) interlace(1=0)  — 13 bytes total
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)

    return (
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", ihdr)
        + _chunk(b"IDAT", compressed)
        + _chunk(b"IEND", b"")
    )


# ── Primitives ────────────────────────────────────────────────────────────────

def dot(
    size: int = 256,
    *,
    radius: Union[int, None] = None,
    peak_alpha: int = 128,
    color: RGB = (255, 255, 255),
) -> bytes:
    """Soft radial dot on a fully transparent background.

    The alpha channel follows a smooth cosine falloff::

        α(d) = peak_alpha × ½ × (1 + cos(π × d / radius))

    yielding *peak_alpha* at the centre and 0 at the edge, with no
    hard boundary.

    Parameters
    ----------
    size:
        Width and height of the square image in pixels.
    radius:
        Dot radius in pixels.  Defaults to 40 % of *size*
        (i.e. the dot fills ~80 % of the canvas width).
    peak_alpha:
        Maximum alpha value (0–255) at the dot centre.
        128 → half-opaque; 255 → fully opaque.
    color:
        RGB colour of the dot.  Default: white ``(255, 255, 255)``.

    Returns
    -------
    bytes
        A *size* × *size* RGBA PNG.
    """
    if radius is None:
        radius = max(1, int(size * 0.40))

    r, g, b = color
    cx = cy = size // 2

    pixels = bytearray(size * size * 4)

    for y in range(size):
        for x in range(size):
            dist = math.sqrt((x - cx) ** 2 + (y - cy) ** 2)
            if dist < radius:
                t = dist / radius
                # Cosine falloff: peak at centre (t=0), zero at edge (t=1)
                a = int(peak_alpha * 0.5 * (1.0 + math.cos(math.pi * t)))
                a = max(0, min(255, a))
            else:
                a = 0
            i = (y * size + x) * 4
            pixels[i]     = r
            pixels[i + 1] = g
            pixels[i + 2] = b
            pixels[i + 3] = a

    return encode(bytes(pixels), size, size)


def solid(
    width: int,
    height: int,
    *,
    color: RGBA = (0, 0, 0, 255),
) -> bytes:
    """Flat single-colour fill.

    Parameters
    ----------
    width:
        Image width in pixels.
    height:
        Image height in pixels.
    color:
        RGBA fill colour.  Default: opaque black.

    Returns
    -------
    bytes
        A *width* × *height* RGBA PNG.
    """
    r, g, b, a = color
    pixels = bytes([r, g, b, a] * (width * height))
    return encode(pixels, width, height)


def gradient_h(
    width: int,
    height: int,
    *,
    left:  RGBA = (0,   0,   0,   255),
    right: RGBA = (255, 255, 255, 255),
) -> bytes:
    """Horizontal linear gradient between two RGBA colours.

    Parameters
    ----------
    width:
        Image width in pixels.
    height:
        Image height in pixels.
    left:
        RGBA colour at the left edge (x = 0).
    right:
        RGBA colour at the right edge (x = width − 1).

    Returns
    -------
    bytes
        A *width* × *height* RGBA PNG.
    """
    pixels = bytearray(width * height * 4)
    denom  = max(width - 1, 1)

    for y in range(height):
        for x in range(width):
            t = x / denom
            i = (y * width + x) * 4
            for c in range(4):
                pixels[i + c] = int(left[c] + (right[c] - left[c]) * t)

    return encode(bytes(pixels), width, height)


def gradient_radial(
    size: int,
    *,
    inner: RGBA = (255, 255, 255, 255),
    outer: RGBA = (0,   0,   0,   255),
    radius: Union[int, None] = None,
) -> bytes:
    """Radial gradient from *inner* colour at the centre to *outer* at the edge.

    Parameters
    ----------
    size:
        Width and height of the square image in pixels.
    inner:
        RGBA colour at the centre.
    outer:
        RGBA colour at *radius* pixels from the centre and beyond.
    radius:
        Gradient radius in pixels.  Defaults to half of *size*.

    Returns
    -------
    bytes
        A *size* × *size* RGBA PNG.
    """
    if radius is None:
        radius = size // 2

    cx = cy = size // 2
    pixels = bytearray(size * size * 4)

    for y in range(size):
        for x in range(size):
            dist = math.sqrt((x - cx) ** 2 + (y - cy) ** 2)
            t    = min(dist / max(radius, 1), 1.0)
            i    = (y * size + x) * 4
            for c in range(4):
                pixels[i + c] = int(inner[c] + (outer[c] - inner[c]) * t)

    return encode(bytes(pixels), size, size)
