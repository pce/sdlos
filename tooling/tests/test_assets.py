"""
tests/test_assets.py
====================
Unit tests for sdlos.assets sub-modules:

  png
    - encode: signature, dimensions, valid PNG header, wrong-size raises
    - dot: returns bytes, alpha at centre == peak_alpha, edge alpha == 0,
           default radius, custom radius, color, grayscale correctness
    - solid: dimensions, color fill
    - gradient_h: left/right edge colours
    - gradient_radial: centre vs edge colour

  shaders
    - starter_msl("shader")  returns preset_a + preset_b Metal sources
    - starter_msl("camera")  returns cinematic Metal source
    - starter_msl("minimal") returns {}
    - get_msl known / unknown keys
    - every returned shader contains the required Metal identifiers
    - known_templates lists shader + camera

  gitignore
    - DATA_ROOT contains OS patterns and recursive glob markers
    - MODELS contains Git LFS hint
    - SPIRV contains *.spv rule
    - IMG, SHADERS are non-empty strings
    - no constant is empty
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

import pytest

from sdlos.assets import gitignore, png, shaders


# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _parse_ihdr(data: bytes) -> dict:
    """Return the IHDR fields of a PNG byte string as a dict."""
    assert data[:8] == _PNG_SIGNATURE, "Not a PNG"
    # First chunk starts at byte 8: length(4) + tag(4) + data(n) + crc(4)
    length = struct.unpack_from(">I", data, 8)[0]
    tag    = data[12:16]
    assert tag == b"IHDR", f"First chunk is not IHDR: {tag}"
    ihdr   = data[16 : 16 + length]
    width, height = struct.unpack_from(">II", ihdr, 0)
    bit_depth  = ihdr[8]
    color_type = ihdr[9]
    return {
        "width":      width,
        "height":     height,
        "bit_depth":  bit_depth,
        "color_type": color_type,   # 6 = RGBA
    }


def _read_pixel(data: bytes, x: int, y: int, width: int) -> tuple[int, int, int, int]:
    """Decode a single RGBA pixel from a PNG byte string (no interlacing assumed)."""
    # Collect IDAT chunks and decompress
    idat = bytearray()
    offset = 8
    while offset < len(data):
        length  = struct.unpack_from(">I", data, offset)[0]
        tag     = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + length]
        if tag == b"IDAT":
            idat.extend(payload)
        elif tag == b"IEND":
            break
        offset += 12 + length

    raw    = zlib.decompress(bytes(idat))
    stride = 1 + width * 4          # filter byte + RGBA per pixel
    row    = raw[y * stride : (y + 1) * stride]
    # filter byte is row[0]; pixel data starts at row[1]
    base   = 1 + x * 4
    return (row[base], row[base + 1], row[base + 2], row[base + 3])


# ─────────────────────────────────────────────────────────────────────────────
# png.encode
# ─────────────────────────────────────────────────────────────────────────────

class TestEncode:
    def test_starts_with_png_signature(self) -> None:
        data = png.encode(bytes([255, 0, 0, 255] * 4), 2, 2)
        assert data[:8] == _PNG_SIGNATURE

    def test_ihdr_dimensions(self) -> None:
        data   = png.encode(bytes([0] * 4 * 10 * 10), 10, 10)
        ihdr   = _parse_ihdr(data)
        assert ihdr["width"]  == 10
        assert ihdr["height"] == 10

    def test_ihdr_rgba_color_type(self) -> None:
        data = png.encode(bytes([0] * 4 * 4 * 4), 4, 4)
        ihdr = _parse_ihdr(data)
        assert ihdr["color_type"] == 6   # RGBA
        assert ihdr["bit_depth"]  == 8

    def test_returns_bytes(self) -> None:
        result = png.encode(bytes([0] * 4), 1, 1)
        assert isinstance(result, bytes)

    def test_wrong_size_raises(self) -> None:
        with pytest.raises(ValueError, match="does not match"):
            png.encode(bytes([0] * 3), 1, 1)   # 3 bytes instead of 4

    def test_wrong_size_too_large_raises(self) -> None:
        with pytest.raises(ValueError):
            png.encode(bytes([0] * 100), 2, 2)  # 100 instead of 16

    def test_pixel_round_trip_1x1(self) -> None:
        """A 1×1 pixel survives encode → PNG bytes."""
        pixels = bytes([200, 100, 50, 128])
        data   = png.encode(pixels, 1, 1)
        r, g, b, a = _read_pixel(data, 0, 0, 1)
        assert (r, g, b, a) == (200, 100, 50, 128)

    def test_pixel_round_trip_2x2(self) -> None:
        red   = bytes([255, 0,   0,   255])
        blue  = bytes([0,   0,   255, 255])
        green = bytes([0,   255, 0,   255])
        white = bytes([255, 255, 255, 255])
        pixels = red + blue + green + white
        data   = png.encode(pixels, 2, 2)
        assert _read_pixel(data, 0, 0, 2) == (255, 0,   0,   255)
        assert _read_pixel(data, 1, 0, 2) == (0,   0,   255, 255)
        assert _read_pixel(data, 0, 1, 2) == (0,   255, 0,   255)
        assert _read_pixel(data, 1, 1, 2) == (255, 255, 255, 255)

    def test_empty_1x1_transparent(self) -> None:
        pixels = bytes([0, 0, 0, 0])
        data   = png.encode(pixels, 1, 1)
        assert _read_pixel(data, 0, 0, 1) == (0, 0, 0, 0)


# ─────────────────────────────────────────────────────────────────────────────
# png.dot
# ─────────────────────────────────────────────────────────────────────────────

class TestDot:
    def test_returns_bytes(self) -> None:
        assert isinstance(png.dot(), bytes)

    def test_starts_with_png_signature(self) -> None:
        assert png.dot()[:8] == _PNG_SIGNATURE

    def test_default_dimensions(self) -> None:
        data = png.dot()
        ihdr = _parse_ihdr(data)
        assert ihdr["width"]  == 256
        assert ihdr["height"] == 256

    def test_custom_size(self) -> None:
        data = png.dot(size=64)
        ihdr = _parse_ihdr(data)
        assert ihdr["width"]  == 64
        assert ihdr["height"] == 64

    def test_centre_alpha_equals_peak_alpha(self) -> None:
        """The very centre pixel should carry peak_alpha."""
        size = 64
        cx   = size // 2
        data = png.dot(size=size, peak_alpha=200)
        _, _, _, a = _read_pixel(data, cx, cx, size)
        # Allow ±2 for floating-point rounding in the cosine falloff
        assert abs(a - 200) <= 2

    def test_corner_alpha_is_zero(self) -> None:
        """Corners are always outside any reasonable radius → alpha == 0."""
        size = 64
        data = png.dot(size=size, radius=20)
        _, _, _, a = _read_pixel(data, 0, 0, size)
        assert a == 0

    def test_peak_alpha_128_default(self) -> None:
        size = 64
        cx   = size // 2
        data = png.dot(size=size)
        _, _, _, a = _read_pixel(data, cx, cx, size)
        assert abs(a - 128) <= 2

    def test_peak_alpha_255(self) -> None:
        size = 32
        cx   = size // 2
        data = png.dot(size=size, peak_alpha=255)
        _, _, _, a = _read_pixel(data, cx, cx, size)
        assert abs(a - 255) <= 2

    def test_peak_alpha_0_produces_transparent(self) -> None:
        """peak_alpha=0 → every pixel is transparent."""
        size = 16
        data = png.dot(size=size, peak_alpha=0)
        for y in range(size):
            for x in range(size):
                _, _, _, a = _read_pixel(data, x, y, size)
                assert a == 0

    def test_color_white_default(self) -> None:
        size = 32
        cx   = size // 2
        data = png.dot(size=size, color=(255, 255, 255))
        r, g, b, _ = _read_pixel(data, cx, cx, size)
        assert r == 255
        assert g == 255
        assert b == 255

    def test_custom_color(self) -> None:
        size = 32
        cx   = size // 2
        data = png.dot(size=size, color=(100, 150, 200))
        r, g, b, _ = _read_pixel(data, cx, cx, size)
        assert r == 100
        assert g == 150
        assert b == 200

    def test_edge_alpha_less_than_centre(self) -> None:
        """Alpha must decrease monotonically from centre to radius."""
        size   = 64
        cx     = size // 2
        radius = 24
        data   = png.dot(size=size, radius=radius)
        _, _, _, a_centre = _read_pixel(data, cx, cx, size)
        _, _, _, a_mid    = _read_pixel(data, cx + radius // 2, cx, size)
        _, _, _, a_edge   = _read_pixel(data, cx + radius - 1, cx, size)
        assert a_centre >= a_mid >= a_edge

    def test_explicit_radius_smaller_than_default(self) -> None:
        """With a small radius the majority of the image should be transparent."""
        size = 64
        data = png.dot(size=size, radius=4)
        # Count transparent pixels
        transparent = sum(
            1 for y in range(size) for x in range(size)
            if _read_pixel(data, x, y, size)[3] == 0
        )
        # More than 90 % of pixels should be fully transparent
        assert transparent > size * size * 0.90

    def test_default_radius_covers_centre(self) -> None:
        """Default radius (40 % of size) should produce a visible centre."""
        size = 64
        cx   = size // 2
        data = png.dot(size=size, peak_alpha=128)
        _, _, _, a = _read_pixel(data, cx, cx, size)
        assert a > 64   # meaningfully non-zero


# ─────────────────────────────────────────────────────────────────────────────
# png.solid
# ─────────────────────────────────────────────────────────────────────────────

class TestSolid:
    def test_returns_bytes(self) -> None:
        assert isinstance(png.solid(4, 4), bytes)

    def test_png_signature(self) -> None:
        assert png.solid(4, 4)[:8] == _PNG_SIGNATURE

    def test_dimensions(self) -> None:
        data = png.solid(8, 12)
        ihdr = _parse_ihdr(data)
        assert ihdr["width"]  == 8
        assert ihdr["height"] == 12

    def test_opaque_black_default(self) -> None:
        data = png.solid(2, 2)
        for y in range(2):
            for x in range(2):
                assert _read_pixel(data, x, y, 2) == (0, 0, 0, 255)

    def test_custom_color(self) -> None:
        data = png.solid(3, 3, color=(255, 128, 0, 200))
        for y in range(3):
            for x in range(3):
                r, g, b, a = _read_pixel(data, x, y, 3)
                assert (r, g, b, a) == (255, 128, 0, 200)

    def test_transparent_fill(self) -> None:
        data = png.solid(2, 2, color=(0, 0, 0, 0))
        for y in range(2):
            for x in range(2):
                _, _, _, a = _read_pixel(data, x, y, 2)
                assert a == 0

    def test_1x1(self) -> None:
        data = png.solid(1, 1, color=(10, 20, 30, 40))
        assert _read_pixel(data, 0, 0, 1) == (10, 20, 30, 40)


# ─────────────────────────────────────────────────────────────────────────────
# png.gradient_h
# ─────────────────────────────────────────────────────────────────────────────

class TestGradientH:
    def test_returns_bytes(self) -> None:
        assert isinstance(png.gradient_h(4, 4), bytes)

    def test_png_signature(self) -> None:
        assert png.gradient_h(4, 4)[:8] == _PNG_SIGNATURE

    def test_dimensions(self) -> None:
        data = png.gradient_h(10, 5)
        ihdr = _parse_ihdr(data)
        assert ihdr["width"]  == 10
        assert ihdr["height"] == 5

    def test_left_edge_colour(self) -> None:
        data = png.gradient_h(
            8, 4,
            left=(255, 0, 0, 255),
            right=(0, 0, 255, 255),
        )
        r, g, b, a = _read_pixel(data, 0, 0, 8)
        assert r == 255
        assert b == 0

    def test_right_edge_colour(self) -> None:
        data = png.gradient_h(
            8, 4,
            left=(255, 0, 0, 255),
            right=(0, 0, 255, 255),
        )
        r, g, b, a = _read_pixel(data, 7, 0, 8)
        assert r == 0
        assert b == 255

    def test_midpoint_is_between_extremes(self) -> None:
        data = png.gradient_h(
            10, 1,
            left=(0, 0, 0, 255),
            right=(200, 0, 0, 255),
        )
        r_left,  _, _, _ = _read_pixel(data, 0, 0, 10)
        r_mid,   _, _, _ = _read_pixel(data, 5, 0, 10)
        r_right, _, _, _ = _read_pixel(data, 9, 0, 10)
        assert r_left < r_mid < r_right

    def test_alpha_gradient(self) -> None:
        data = png.gradient_h(
            4, 1,
            left=(255, 255, 255, 0),
            right=(255, 255, 255, 255),
        )
        _, _, _, a_left  = _read_pixel(data, 0, 0, 4)
        _, _, _, a_right = _read_pixel(data, 3, 0, 4)
        assert a_left  == 0
        assert a_right == 255


# ─────────────────────────────────────────────────────────────────────────────
# png.gradient_radial
# ─────────────────────────────────────────────────────────────────────────────

class TestGradientRadial:
    def test_returns_bytes(self) -> None:
        assert isinstance(png.gradient_radial(16), bytes)

    def test_dimensions(self) -> None:
        data = png.gradient_radial(32)
        ihdr = _parse_ihdr(data)
        assert ihdr["width"]  == 32
        assert ihdr["height"] == 32

    def test_centre_matches_inner(self) -> None:
        size = 32
        cx   = size // 2
        data = png.gradient_radial(
            size,
            inner=(255, 0, 0, 255),
            outer=(0,   0, 0, 255),
        )
        r, _, _, _ = _read_pixel(data, cx, cx, size)
        assert r > 200   # close to 255

    def test_corner_matches_outer(self) -> None:
        size = 32
        data = png.gradient_radial(
            size,
            inner=(255, 0, 0, 255),
            outer=(0,   0, 0, 255),
            radius=10,
        )
        r, _, _, _ = _read_pixel(data, 0, 0, size)
        assert r == 0   # saturated at outer colour


# ─────────────────────────────────────────────────────────────────────────────
# shaders.starter_msl
# ─────────────────────────────────────────────────────────────────────────────

# Required Metal identifiers that every valid starter shader must contain.
_REQUIRED_METAL = [
    "#include <metal_stdlib>",
    "struct NodeShaderParams",
    "fragment float4 main0(",
    "texture2d<float>",
    "sampler",
    "buffer(0)",
    "u_time",
    "u_param0",
]


class TestStarterMsl:
    # ── shader template ───────────────────────────────────────────────────────

    def test_shader_template_returns_two_files(self) -> None:
        result = shaders.starter_msl("shader")
        assert len(result) == 2

    def test_shader_template_filenames(self) -> None:
        result = shaders.starter_msl("shader")
        assert "preset_a.frag.metal" in result
        assert "preset_b.frag.metal" in result

    def test_shader_template_values_are_strings(self) -> None:
        for src in shaders.starter_msl("shader").values():
            assert isinstance(src, str)
            assert len(src) > 0

    @pytest.mark.parametrize("key", _REQUIRED_METAL)
    def test_preset_a_contains_required_identifier(self, key: str) -> None:
        src = shaders.starter_msl("shader")["preset_a.frag.metal"]
        assert key in src, f"preset_a missing: {key!r}"

    @pytest.mark.parametrize("key", _REQUIRED_METAL)
    def test_preset_b_contains_required_identifier(self, key: str) -> None:
        src = shaders.starter_msl("shader")["preset_b.frag.metal"]
        assert key in src, f"preset_b missing: {key!r}"

    def test_preset_a_references_u_focusX_or_animation(self) -> None:
        src = shaders.starter_msl("shader")["preset_a.frag.metal"]
        # plasma shader should animate via u_time
        assert "u_time" in src

    def test_preset_b_references_focus_point(self) -> None:
        src = shaders.starter_msl("shader")["preset_b.frag.metal"]
        # rings shader should use the focus point
        assert "u_focusX" in src or "u_focusY" in src

    # ── camera template ───────────────────────────────────────────────────────

    def test_camera_template_returns_one_file(self) -> None:
        result = shaders.starter_msl("camera")
        assert len(result) == 1

    def test_camera_template_filename(self) -> None:
        result = shaders.starter_msl("camera")
        assert "cinematic.frag.metal" in result

    @pytest.mark.parametrize("key", _REQUIRED_METAL)
    def test_cinematic_contains_required_identifier(self, key: str) -> None:
        src = shaders.starter_msl("camera")["cinematic.frag.metal"]
        assert key in src, f"cinematic missing: {key!r}"

    def test_cinematic_uses_param0_as_blend(self) -> None:
        src = shaders.starter_msl("camera")["cinematic.frag.metal"]
        assert "u_param0" in src

    # ── minimal template ──────────────────────────────────────────────────────

    def test_minimal_returns_empty(self) -> None:
        assert shaders.starter_msl("minimal") == {}

    def test_unknown_template_returns_empty(self) -> None:
        assert shaders.starter_msl("unknown_xyz") == {}

    # ── idempotency ───────────────────────────────────────────────────────────

    def test_starter_msl_is_idempotent(self) -> None:
        assert shaders.starter_msl("shader") == shaders.starter_msl("shader")


class TestGetMsl:
    def test_preset_a_known(self) -> None:
        src = shaders.get_msl("preset_a")
        assert src is not None
        assert "preset_a" in src.lower() or "plasma" in src.lower() or "main0" in src

    def test_preset_b_known(self) -> None:
        assert shaders.get_msl("preset_b") is not None

    def test_cinematic_known(self) -> None:
        assert shaders.get_msl("cinematic") is not None

    def test_unknown_returns_none(self) -> None:
        assert shaders.get_msl("nonexistent_shader_xyz") is None

    def test_empty_string_returns_none(self) -> None:
        assert shaders.get_msl("") is None


class TestKnownTemplates:
    def test_shader_listed(self) -> None:
        assert "shader" in shaders.known_templates()

    def test_camera_listed(self) -> None:
        assert "camera" in shaders.known_templates()

    def test_minimal_not_listed(self) -> None:
        # minimal has no associated shaders
        assert "minimal" not in shaders.known_templates()

    def test_returns_list(self) -> None:
        assert isinstance(shaders.known_templates(), list)


# ─────────────────────────────────────────────────────────────────────────────
# gitignore constants
# ─────────────────────────────────────────────────────────────────────────────

class TestGitignoreContent:
    # ── all constants are non-empty strings ──────────────────────────────────

    @pytest.mark.parametrize("constant", [
        gitignore.DATA_ROOT,
        gitignore.IMG,
        gitignore.MODELS,
        gitignore.SHADERS,
        gitignore.SPIRV,
    ])
    def test_constant_is_non_empty_string(self, constant: str) -> None:
        assert isinstance(constant, str)
        assert len(constant.strip()) > 0

    # ── DATA_ROOT ─────────────────────────────────────────────────────────────

    def test_data_root_contains_ds_store(self) -> None:
        assert ".DS_Store" in gitignore.DATA_ROOT

    def test_data_root_contains_thumbs(self) -> None:
        assert "Thumbs.db" in gitignore.DATA_ROOT

    def test_data_root_uses_recursive_glob(self) -> None:
        # Recursive **/ patterns cascade into subdirs without extra files
        assert "**/" in gitignore.DATA_ROOT

    def test_data_root_contains_desktop_ini(self) -> None:
        assert "desktop.ini" in gitignore.DATA_ROOT

    # ── MODELS ────────────────────────────────────────────────────────────────

    def test_models_contains_lfs_hint(self) -> None:
        assert "lfs" in gitignore.MODELS.lower() or "LFS" in gitignore.MODELS

    def test_models_mentions_glb(self) -> None:
        assert ".glb" in gitignore.MODELS or "glb" in gitignore.MODELS

    def test_models_contains_os_artifacts(self) -> None:
        assert ".DS_Store" in gitignore.MODELS

    # ── SPIRV ─────────────────────────────────────────────────────────────────

    def test_spirv_ignores_spv_files(self) -> None:
        assert "*.spv" in gitignore.SPIRV

    def test_spirv_contains_os_artifacts(self) -> None:
        assert ".DS_Store" in gitignore.SPIRV

    def test_spirv_mentions_compilation(self) -> None:
        # Should explain why .spv is excluded
        lower = gitignore.SPIRV.lower()
        assert "compil" in lower or "spirv-cross" in lower or "compiled" in lower

    # ── IMG ───────────────────────────────────────────────────────────────────

    def test_img_contains_os_artifacts(self) -> None:
        assert ".DS_Store" in gitignore.IMG

    # ── SHADERS ───────────────────────────────────────────────────────────────

    def test_shaders_contains_os_artifacts(self) -> None:
        assert ".DS_Store" in gitignore.SHADERS

    # ── no constant accidentally equals another ───────────────────────────────

    def test_constants_are_distinct(self) -> None:
        constants = [
            gitignore.DATA_ROOT,
            gitignore.IMG,
            gitignore.MODELS,
            gitignore.SHADERS,
            gitignore.SPIRV,
        ]
        # All five should be unique strings
        assert len(set(constants)) == len(constants)
