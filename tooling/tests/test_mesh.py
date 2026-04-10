"""
tests/test_mesh.py
==================
Unit and integration tests for sdlos.commands.mesh.

Topics
------
  TestCityPresetStructure   — data integrity across all presets in CITY_PRESETS
  TestHamburgPreset         — Hamburg-specific assertions (newly added preset)
  TestOsakaPreset           — Osaka-specific assertions
  TestKyotoPreset           — Kyoto/Kinkaku-ji-specific assertions
  TestCappadociaPreset      — Cappadocia/Göreme preset (organic non-rectangular geometry)
  TestPresetRegistry        — consistency of CITY_PRESETS / _PRESET_ORDER / _PRESET_REGIONS
  TestSplitBbox             — split_bbox tiling geometry correctness
  TestParseBbox             — _parse_bbox valid and invalid inputs
  TestBuildingHeight        — _building_height OSM tag extraction
  TestPresetsCLI            — `sdlos mesh presets` via Click CliRunner
  TestGltfCityMeshes        — structural validation of any .gltf / .glb files
                               present in assets/city-meshes/  (skipped when empty)
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

import click
import pytest
from click.testing import CliRunner

from sdlos.commands.mesh import (
    CITY_PRESETS,
    CityPreset,
    _PRESET_ORDER,
    _PRESET_REGIONS,
    _building_height,
    _fetch_buildings_raw,
    _parse_bbox,
    _with_overpass_fallback,
    cmd_mesh,
    split_bbox,
)


# ── Helpers / fixtures ────────────────────────────────────────────────────────

_CITY_MESHES_DIR = Path(__file__).parents[2] / "assets" / "city-meshes"
_GLTF_FILES = sorted(_CITY_MESHES_DIR.glob("*.gltf")) if _CITY_MESHES_DIR.is_dir() else []
_GLB_FILES  = sorted(_CITY_MESHES_DIR.glob("*.glb"))  if _CITY_MESHES_DIR.is_dir() else []

# All preset keys (including aliases such as "adam")
_ALL_KEYS = list(CITY_PRESETS.keys())

# Keys that appear in _PRESET_REGIONS (excludes aliases)
_REGION_KEYS: set[str] = {k for _, keys in _PRESET_REGIONS for k in keys}


# ── TestCityPresetStructure ───────────────────────────────────────────────────

class TestCityPresetStructure:
    """Every entry in CITY_PRESETS must be internally consistent."""

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_north_greater_than_south(self, key: str) -> None:
        p = CITY_PRESETS[key]
        n, s, _e, _w = p.bbox
        assert n > s, f"{key}: bbox north ({n}) must be > south ({s})"

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_east_greater_than_west(self, key: str) -> None:
        p = CITY_PRESETS[key]
        _n, _s, e, w = p.bbox
        assert e > w, f"{key}: bbox east ({e}) must be > west ({w})"

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_bbox_is_four_tuple(self, key: str) -> None:
        p = CITY_PRESETS[key]
        assert len(p.bbox) == 4

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_bbox_values_are_floats(self, key: str) -> None:
        p = CITY_PRESETS[key]
        for val in p.bbox:
            assert isinstance(val, float), f"{key}: bbox value {val!r} is not a float"

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_building_height_positive(self, key: str) -> None:
        p = CITY_PRESETS[key]
        assert p.building_height > 0, f"{key}: building_height must be positive"

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_key_field_matches_dict_key(self, key: str) -> None:
        p = CITY_PRESETS[key]
        # Aliases may intentionally differ; check that .key is a non-empty string
        assert isinstance(p.key, str) and p.key, f"{key}: .key field must be a non-empty string"

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_non_empty_label(self, key: str) -> None:
        p = CITY_PRESETS[key]
        assert p.label.strip(), f"{key}: label must not be empty"

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_non_empty_notes(self, key: str) -> None:
        p = CITY_PRESETS[key]
        assert p.notes.strip(), f"{key}: notes must not be empty"

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_bbox_within_world_bounds(self, key: str) -> None:
        p = CITY_PRESETS[key]
        n, s, e, w = p.bbox
        assert -90  <= s < n <= 90,  f"{key}: latitudes out of range"
        assert -180 <= w < e <= 180, f"{key}: longitudes out of range"

    @pytest.mark.parametrize("key", _ALL_KEYS)
    def test_bbox_area_not_degenerate(self, key: str) -> None:
        """Bounding box must cover at least 0.005° in each axis (~500 m)."""
        p = CITY_PRESETS[key]
        n, s, e, w = p.bbox
        assert (n - s) >= 0.005, f"{key}: bbox is too narrow (N-S < 0.005°)"
        assert (e - w) >= 0.005, f"{key}: bbox is too narrow (E-W < 0.005°)"


# ── TestHamburgPreset ─────────────────────────────────────────────────────────

class TestHamburgPreset:
    """Hamburg-specific assertions for the newly added preset."""

    def test_hamburg_exists(self) -> None:
        assert "hamburg" in CITY_PRESETS, "'hamburg' key missing from CITY_PRESETS"

    def test_hamburg_is_city_preset(self) -> None:
        assert isinstance(CITY_PRESETS["hamburg"], CityPreset)

    def test_hamburg_label_contains_hamburg(self) -> None:
        label = CITY_PRESETS["hamburg"].label
        assert "Hamburg" in label, f"Label {label!r} should contain 'Hamburg'"

    def test_hamburg_bbox_north_gt_south(self) -> None:
        n, s, _e, _w = CITY_PRESETS["hamburg"].bbox
        assert n > s

    def test_hamburg_bbox_east_gt_west(self) -> None:
        _n, _s, e, w = CITY_PRESETS["hamburg"].bbox
        assert e > w

    def test_hamburg_latitude_range(self) -> None:
        """Hamburg is around 53.5° N."""
        n, s, _e, _w = CITY_PRESETS["hamburg"].bbox
        assert 53.0 < s < n < 54.0, f"Hamburg latitudes look wrong: {s}–{n}"

    def test_hamburg_longitude_range(self) -> None:
        """Hamburg is around 10° E."""
        _n, _s, e, w = CITY_PRESETS["hamburg"].bbox
        assert 9.0 < w < e < 11.0, f"Hamburg longitudes look wrong: {w}–{e}"

    def test_hamburg_in_preset_order(self) -> None:
        assert "hamburg" in _PRESET_ORDER, "'hamburg' missing from _PRESET_ORDER"

    def test_hamburg_in_germany_region(self) -> None:
        germany_keys: list[str] = []
        for label, keys in _PRESET_REGIONS:
            if label == "Germany":
                germany_keys = keys
                break
        assert "hamburg" in germany_keys, \
            f"'hamburg' not in Germany region group; got {germany_keys}"

    def test_hamburg_height_reasonable(self) -> None:
        """Hamburg Innenstadt has medium-rise buildings; expect 10–40 m fallback."""
        h = CITY_PRESETS["hamburg"].building_height
        assert 10.0 <= h <= 40.0, f"Unexpected fallback height for hamburg: {h}"


# ── TestOsakaPreset ───────────────────────────────────────────────────────────

class TestOsakaPreset:
    """Osaka-specific assertions for the Dōtonbori / Namba preset."""

    def test_osaka_exists(self) -> None:
        assert "osaka" in CITY_PRESETS, "'osaka' key missing from CITY_PRESETS"

    def test_osaka_is_city_preset(self) -> None:
        assert isinstance(CITY_PRESETS["osaka"], CityPreset)

    def test_osaka_label_contains_osaka(self) -> None:
        label = CITY_PRESETS["osaka"].label
        assert "Osaka" in label, f"Label {label!r} should contain 'Osaka'"

    def test_osaka_bbox_north_gt_south(self) -> None:
        n, s, _e, _w = CITY_PRESETS["osaka"].bbox
        assert n > s

    def test_osaka_bbox_east_gt_west(self) -> None:
        _n, _s, e, w = CITY_PRESETS["osaka"].bbox
        assert e > w

    def test_osaka_latitude_range(self) -> None:
        """Osaka is around 34.7° N."""
        n, s, _e, _w = CITY_PRESETS["osaka"].bbox
        assert 34.0 < s < n < 35.5, f"Osaka latitudes look wrong: {s}–{n}"

    def test_osaka_longitude_range(self) -> None:
        """Osaka is around 135.5° E."""
        _n, _s, e, w = CITY_PRESETS["osaka"].bbox
        assert 135.0 < w < e < 136.0, f"Osaka longitudes look wrong: {w}–{e}"

    def test_osaka_in_preset_order(self) -> None:
        assert "osaka" in _PRESET_ORDER, "'osaka' missing from _PRESET_ORDER"

    def test_osaka_in_japan_region(self) -> None:
        japan_keys: list[str] = []
        for label, keys in _PRESET_REGIONS:
            if label == "Japan":
                japan_keys = keys
                break
        assert "osaka" in japan_keys, \
            f"'osaka' not in Japan region group; got {japan_keys}"

    def test_osaka_height_reasonable(self) -> None:
        h = CITY_PRESETS["osaka"].building_height
        assert 10.0 <= h <= 50.0, f"Unexpected fallback height for osaka: {h}"


# ── TestKyotoPreset ───────────────────────────────────────────────────────────

class TestKyotoPreset:
    """Kyoto-specific assertions — bbox centred on Kinkaku-ji (Golden Pavilion)."""

    def test_kyoto_exists(self) -> None:
        assert "kyoto" in CITY_PRESETS, "'kyoto' key missing from CITY_PRESETS"

    def test_kyoto_is_city_preset(self) -> None:
        assert isinstance(CITY_PRESETS["kyoto"], CityPreset)

    def test_kyoto_label_contains_kyoto(self) -> None:
        label = CITY_PRESETS["kyoto"].label
        assert "Kyoto" in label, f"Label {label!r} should contain 'Kyoto'"

    def test_kyoto_label_mentions_golden_pavilion(self) -> None:
        """The preset is centred on Kinkaku-ji; the label should say so."""
        label = CITY_PRESETS["kyoto"].label
        notes = CITY_PRESETS["kyoto"].notes
        combined = label + " " + notes
        assert "Kinkaku" in combined or "Golden" in combined, (
            f"Expected 'Kinkaku' or 'Golden' in label/notes; got: {combined!r}"
        )

    def test_kyoto_notes_mention_kinkakuji(self) -> None:
        notes = CITY_PRESETS["kyoto"].notes
        assert "Kinkaku" in notes, \
            f"Notes should mention Kinkaku-ji; got: {notes!r}"

    def test_kyoto_bbox_north_gt_south(self) -> None:
        n, s, _e, _w = CITY_PRESETS["kyoto"].bbox
        assert n > s

    def test_kyoto_bbox_east_gt_west(self) -> None:
        _n, _s, e, w = CITY_PRESETS["kyoto"].bbox
        assert e > w

    def test_kyoto_latitude_range(self) -> None:
        """Kinkaku-ji is at ~35.04° N."""
        n, s, _e, _w = CITY_PRESETS["kyoto"].bbox
        assert 34.5 < s < n < 35.5, f"Kyoto latitudes look wrong: {s}–{n}"

    def test_kyoto_longitude_range(self) -> None:
        """Kinkaku-ji is at ~135.73° E."""
        _n, _s, e, w = CITY_PRESETS["kyoto"].bbox
        assert 135.5 < w < e < 136.0, f"Kyoto longitudes look wrong: {w}–{e}"

    def test_kyoto_in_preset_order(self) -> None:
        assert "kyoto" in _PRESET_ORDER, "'kyoto' missing from _PRESET_ORDER"

    def test_kyoto_in_japan_region(self) -> None:
        japan_keys: list[str] = []
        for label, keys in _PRESET_REGIONS:
            if label == "Japan":
                japan_keys = keys
                break
        assert "kyoto" in japan_keys, \
            f"'kyoto' not in Japan region group; got {japan_keys}"

    def test_kyoto_height_low_for_temple_district(self) -> None:
        """Temple district is mostly low-rise; expect fallback height <= 15 m."""
        h = CITY_PRESETS["kyoto"].building_height
        assert h <= 15.0, (
            f"Kyoto (temple district) fallback height should be <= 15 m; got {h}"
        )

    def test_kyoto_bbox_centred_near_kinkakuji(self) -> None:
        """Bbox midpoint should be within ~1 km of Kinkaku-ji (35.0394°N, 135.7292°E)."""
        n, s, e, w = CITY_PRESETS["kyoto"].bbox
        mid_lat = (n + s) / 2
        mid_lon = (e + w) / 2
        # Allow ±0.015° (~1.5 km) tolerance
        assert abs(mid_lat - 35.0394) < 0.015, \
            f"Kyoto bbox midpoint lat {mid_lat:.4f} is far from Kinkaku-ji (35.0394)"
        assert abs(mid_lon - 135.7292) < 0.015, \
            f"Kyoto bbox midpoint lon {mid_lon:.4f} is far from Kinkaku-ji (135.7292)"


# ── TestCappadociaPreset ──────────────────────────────────────────────────────

class TestCappadociaPreset:
    """Cappadocia / Göreme-specific assertions.

    Cappadocia is added specifically to test mesh generation for organic,
    non-rectangular geometry: fairy chimney rock formations, cave dwellings,
    and rock-cut houses produce highly irregular building footprints unlike
    the grid-like urban forms of European or US city presets.
    """

    def test_cappadocia_exists(self) -> None:
        assert "cappadocia" in CITY_PRESETS, "'cappadocia' key missing from CITY_PRESETS"

    def test_cappadocia_is_city_preset(self) -> None:
        assert isinstance(CITY_PRESETS["cappadocia"], CityPreset)

    def test_cappadocia_label_contains_cappadocia_or_goreme(self) -> None:
        label = CITY_PRESETS["cappadocia"].label
        assert "Cappadocia" in label or "Göreme" in label, (
            f"Label {label!r} should contain 'Cappadocia' or 'Göreme'"
        )

    def test_cappadocia_notes_mention_fairy_chimneys(self) -> None:
        notes = CITY_PRESETS["cappadocia"].notes
        assert "fairy" in notes.lower() or "chimney" in notes.lower(), (
            f"Notes should mention fairy chimneys; got: {notes!r}"
        )

    def test_cappadocia_notes_mention_cave_or_organic(self) -> None:
        """Notes should flag the non-standard geometry for test-awareness."""
        notes = CITY_PRESETS["cappadocia"].notes.lower()
        keywords = ("cave", "organic", "rock", "non-rectangular")
        assert any(kw in notes for kw in keywords), (
            f"Notes should describe the irregular geometry; got: {notes!r}"
        )

    def test_cappadocia_bbox_north_gt_south(self) -> None:
        n, s, _e, _w = CITY_PRESETS["cappadocia"].bbox
        assert n > s

    def test_cappadocia_bbox_east_gt_west(self) -> None:
        _n, _s, e, w = CITY_PRESETS["cappadocia"].bbox
        assert e > w

    def test_cappadocia_latitude_range(self) -> None:
        """Göreme is at ~38.64° N — must sit in central Turkey latitude band."""
        n, s, _e, _w = CITY_PRESETS["cappadocia"].bbox
        assert 38.0 < s < n < 39.5, f"Cappadocia latitudes look wrong: {s}–{n}"

    def test_cappadocia_longitude_range(self) -> None:
        """Göreme is at ~34.83° E."""
        _n, _s, e, w = CITY_PRESETS["cappadocia"].bbox
        assert 34.0 < w < e < 36.0, f"Cappadocia longitudes look wrong: {w}–{e}"

    def test_cappadocia_in_preset_order(self) -> None:
        assert "cappadocia" in _PRESET_ORDER, "'cappadocia' missing from _PRESET_ORDER"

    def test_cappadocia_in_turkey_region(self) -> None:
        turkey_keys: list[str] = []
        for label, keys in _PRESET_REGIONS:
            if label == "Turkey":
                turkey_keys = keys
                break
        assert "cappadocia" in turkey_keys, (
            f"'cappadocia' not in Turkey region group; got {turkey_keys}"
        )

    def test_cappadocia_height_low_for_cave_dwellings(self) -> None:
        """Cave dwellings and fairy chimneys are low-rise; expect fallback <= 8 m."""
        h = CITY_PRESETS["cappadocia"].building_height
        assert h <= 8.0, (
            f"Cappadocia (cave dwellings) fallback height should be <= 8 m; got {h}"
        )

    def test_cappadocia_bbox_centred_near_goreme(self) -> None:
        """Bbox midpoint should be within ~2 km of Göreme centre (38.644°N, 34.830°E)."""
        n, s, e, w = CITY_PRESETS["cappadocia"].bbox
        mid_lat = (n + s) / 2
        mid_lon = (e + w) / 2
        # Allow ±0.020° (~2 km) tolerance
        assert abs(mid_lat - 38.644) < 0.020, (
            f"Cappadocia bbox midpoint lat {mid_lat:.4f} is far from Göreme (38.644)"
        )
        assert abs(mid_lon - 34.830) < 0.020, (
            f"Cappadocia bbox midpoint lon {mid_lon:.4f} is far from Göreme (34.830)"
        )


# ── TestWithOverpassFallback ──────────────────────────────────────────────────

class TestWithOverpassFallback:
    """Tests for the endpoint-iteration logic in _with_overpass_fallback.

    The function's contract:
      - fn receives the endpoint URL string as its sole argument.
      - Returns on the first successful fn(ep) call.
      - Advances to the next endpoint on any exception.
      - Raises the *last* exception if every endpoint fails.
    """

    def test_fn_receives_endpoint_string(self) -> None:
        received: list[str] = []

        def fn(ep: str) -> str:
            received.append(ep)
            return "ok"

        _with_overpass_fallback(["https://ep1.example/"], fn, quiet=True)
        assert received == ["https://ep1.example/"]

    def test_returns_fn_result(self) -> None:
        def fn(ep: str) -> int:
            return 42

        result = _with_overpass_fallback(["https://ep1.example/"], fn, quiet=True)
        assert result == 42

    def test_advances_to_next_on_failure(self) -> None:
        call_log: list[str] = []

        def fn(ep: str) -> str:
            call_log.append(ep)
            if ep == "https://ep1.example/":
                raise RuntimeError("ep1 down")
            return "ok"

        result = _with_overpass_fallback(
            ["https://ep1.example/", "https://ep2.example/"], fn, quiet=True
        )
        assert result == "ok"
        assert call_log == ["https://ep1.example/", "https://ep2.example/"]

    def test_raises_last_exception_when_all_fail(self) -> None:
        def fn(ep: str) -> None:
            raise ValueError(f"failed: {ep}")

        with pytest.raises(ValueError, match="failed: https://ep2.example/"):
            _with_overpass_fallback(
                ["https://ep1.example/", "https://ep2.example/"], fn, quiet=True
            )

    def test_raises_correct_type_on_single_endpoint_failure(self) -> None:
        def fn(ep: str) -> None:
            raise ConnectionError("timeout")

        with pytest.raises(ConnectionError):
            _with_overpass_fallback(["https://ep1.example/"], fn, quiet=True)

    def test_stops_after_first_success(self) -> None:
        called: list[str] = []

        def fn(ep: str) -> str:
            called.append(ep)
            return "done"

        _with_overpass_fallback(
            ["https://ep1.example/", "https://ep2.example/", "https://ep3.example/"],
            fn,
            quiet=True,
        )
        assert called == ["https://ep1.example/"]

    def test_skips_failing_and_uses_third(self) -> None:
        call_log: list[str] = []

        def fn(ep: str) -> str:
            call_log.append(ep)
            if ep in ("https://ep1.example/", "https://ep2.example/"):
                raise OSError("down")
            return "third"

        result = _with_overpass_fallback(
            ["https://ep1.example/", "https://ep2.example/", "https://ep3.example/"],
            fn,
            quiet=True,
        )
        assert result == "third"
        assert call_log == [
            "https://ep1.example/",
            "https://ep2.example/",
            "https://ep3.example/",
        ]


# ── TestFetchBuildingsRawParser ───────────────────────────────────────────────

class TestFetchBuildingsRawParser:
    """Tests for _fetch_buildings_raw — parser correctness with mocked HTTP.

    No network required: requests.post is monkeypatched to return a
    hand-crafted Overpass JSON payload for each test case.
    """

    # ── Minimal Overpass JSON payloads ────────────────────────────────────────

    # A single closed way (5 nodes, first == last) with building=yes
    _SIMPLE_WAY = {
        "elements": [
            {"type": "node", "id": 1, "lat": 52.370, "lon": 4.880},
            {"type": "node", "id": 2, "lat": 52.371, "lon": 4.880},
            {"type": "node", "id": 3, "lat": 52.371, "lon": 4.881},
            {"type": "node", "id": 4, "lat": 52.370, "lon": 4.881},
            {
                "type": "way", "id": 100,
                "nodes": [1, 2, 3, 4, 1],
                "tags": {"building": "yes", "name": "Test House"},
            },
        ]
    }

    # A way with building tag + a node-only element (node should not become a row)
    _WAY_WITH_EXTRA_NODE = {
        "elements": [
            {"type": "node", "id": 1, "lat": 52.370, "lon": 4.880},
            {"type": "node", "id": 2, "lat": 52.371, "lon": 4.880},
            {"type": "node", "id": 3, "lat": 52.371, "lon": 4.881},
            {"type": "node", "id": 4, "lat": 52.370, "lon": 4.881},
            {"type": "node", "id": 99, "lat": 52.375, "lon": 4.890},  # standalone node
            {
                "type": "way", "id": 200,
                "nodes": [1, 2, 3, 4, 1],
                "tags": {"building": "apartments"},
            },
        ]
    }

    # A way WITHOUT building tag — must be ignored
    _NO_BUILDING_TAG = {
        "elements": [
            {"type": "node", "id": 1, "lat": 52.370, "lon": 4.880},
            {"type": "node", "id": 2, "lat": 52.371, "lon": 4.880},
            {"type": "node", "id": 3, "lat": 52.371, "lon": 4.881},
            {"type": "node", "id": 4, "lat": 52.370, "lon": 4.881},
            {
                "type": "way", "id": 300,
                "nodes": [1, 2, 3, 4, 1],
                "tags": {"amenity": "cafe"},   # no building tag
            },
        ]
    }

    # Empty response — no elements
    _EMPTY_RESPONSE: dict = {"elements": []}

    # Two ways — used to check multi-row DataFrames
    _TWO_WAYS = {
        "elements": [
            {"type": "node", "id": 1, "lat": 52.370, "lon": 4.880},
            {"type": "node", "id": 2, "lat": 52.371, "lon": 4.880},
            {"type": "node", "id": 3, "lat": 52.371, "lon": 4.881},
            {"type": "node", "id": 4, "lat": 52.370, "lon": 4.881},
            {"type": "node", "id": 5, "lat": 52.372, "lon": 4.882},
            {"type": "node", "id": 6, "lat": 52.373, "lon": 4.882},
            {"type": "node", "id": 7, "lat": 52.373, "lon": 4.883},
            {"type": "node", "id": 8, "lat": 52.372, "lon": 4.883},
            {
                "type": "way", "id": 101,
                "nodes": [1, 2, 3, 4, 1],
                "tags": {"building": "yes"},
            },
            {
                "type": "way", "id": 102,
                "nodes": [5, 6, 7, 8, 5],
                "tags": {"building": "residential"},
            },
        ]
    }

    # ── Fixture ───────────────────────────────────────────────────────────────

    @pytest.fixture
    def mock_post(self, monkeypatch: pytest.MonkeyPatch):
        """Return a factory that patches requests.post with a given payload."""
        import json as _json
        import types

        def _make_mock(payload: dict):
            fake_resp = types.SimpleNamespace(
                status_code=200,
                json=lambda: payload,
                raise_for_status=lambda: None,
            )

            def _post(*args, **kwargs):
                return fake_resp

            monkeypatch.setattr("requests.post", _post)

        return _make_mock

    # ── Tests ─────────────────────────────────────────────────────────────────

    def test_returns_geodataframe(self, mock_post) -> None:
        import geopandas as gpd
        mock_post(self._SIMPLE_WAY)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert isinstance(result, gpd.GeoDataFrame)

    def test_crs_is_epsg_4326(self, mock_post) -> None:
        mock_post(self._SIMPLE_WAY)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert result.crs is not None
        assert result.crs.to_epsg() == 4326

    def test_simple_way_produces_one_row(self, mock_post) -> None:
        mock_post(self._SIMPLE_WAY)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert len(result) == 1

    def test_osmid_preserved(self, mock_post) -> None:
        mock_post(self._SIMPLE_WAY)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert result.iloc[0]["osmid"] == 100

    def test_element_type_is_way(self, mock_post) -> None:
        mock_post(self._SIMPLE_WAY)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert result.iloc[0]["element_type"] == "way"

    def test_osm_tags_are_columns(self, mock_post) -> None:
        mock_post(self._SIMPLE_WAY)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert "building" in result.columns
        assert result.iloc[0]["building"] == "yes"
        assert "name" in result.columns
        assert result.iloc[0]["name"] == "Test House"

    def test_geometry_is_polygon(self, mock_post) -> None:
        from shapely.geometry import Polygon
        mock_post(self._SIMPLE_WAY)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert isinstance(result.iloc[0].geometry, Polygon)

    def test_geometry_is_not_empty(self, mock_post) -> None:
        mock_post(self._SIMPLE_WAY)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert not result.iloc[0].geometry.is_empty

    def test_standalone_node_not_returned_as_row(self, mock_post) -> None:
        mock_post(self._WAY_WITH_EXTRA_NODE)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        # Only the way should appear — standalone nodes are not building footprints
        assert len(result) == 1
        assert result.iloc[0]["element_type"] == "way"

    def test_way_without_building_tag_ignored(self, mock_post) -> None:
        mock_post(self._NO_BUILDING_TAG)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert len(result) == 0

    def test_empty_response_returns_empty_geodataframe(self, mock_post) -> None:
        mock_post(self._EMPTY_RESPONSE)
        result = _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                      endpoint="https://ep.example/", timeout=10)
        assert len(result) == 0
        assert "geometry" in result.columns

    def test_two_ways_returns_two_rows(self, mock_post) -> None:
        mock_post(self._TWO_WAYS)
        result = _fetch_buildings_raw(52.374, 52.369, 4.884, 4.879,
                                      endpoint="https://ep.example/", timeout=10)
        assert len(result) == 2

    def test_two_ways_osmids_correct(self, mock_post) -> None:
        mock_post(self._TWO_WAYS)
        result = _fetch_buildings_raw(52.374, 52.369, 4.884, 4.879,
                                      endpoint="https://ep.example/", timeout=10)
        assert set(result["osmid"]) == {101, 102}

    def test_http_error_propagates(self, monkeypatch: pytest.MonkeyPatch) -> None:
        import requests
        import types

        fake_resp = types.SimpleNamespace(
            status_code=429,
            raise_for_status=lambda: (_ for _ in ()).throw(
                requests.HTTPError("429 Too Many Requests")
            ),
        )
        monkeypatch.setattr("requests.post", lambda *a, **kw: fake_resp)
        with pytest.raises(requests.HTTPError):
            _fetch_buildings_raw(52.371, 52.370, 4.881, 4.880,
                                 endpoint="https://ep.example/", timeout=10)


# ── TestSplitBbox ─────────────────────────────────────────────────────────────

class TestSplitBbox:
    """Geometry correctness tests for the split_bbox tiler."""

    def test_returns_iterator(self) -> None:
        result = split_bbox(52.377, 52.367, 4.902, 4.878)
        # Should be iterable (generator)
        assert hasattr(result, "__iter__")

    def test_tiles_cover_full_area(self) -> None:
        """Union of all tiles must equal the original bbox."""
        n, s, e, w = 52.377, 52.367, 4.902, 4.878
        tiles = list(split_bbox(n, s, e, w, step=0.005))
        assert tiles, "Expected at least one tile"
        tile_s_min = min(t[1] for t in tiles)
        tile_n_max = max(t[0] for t in tiles)
        tile_w_min = min(t[3] for t in tiles)
        tile_e_max = max(t[2] for t in tiles)
        assert tile_s_min == pytest.approx(s)
        assert tile_n_max == pytest.approx(n)
        assert tile_w_min == pytest.approx(w)
        assert tile_e_max == pytest.approx(e)

    def test_every_tile_north_gt_south(self) -> None:
        for tile_n, tile_s, tile_e, tile_w in split_bbox(52.377, 52.367, 4.902, 4.878):
            assert tile_n > tile_s

    def test_every_tile_east_gt_west(self) -> None:
        for tile_n, tile_s, tile_e, tile_w in split_bbox(52.377, 52.367, 4.902, 4.878):
            assert tile_e > tile_w

    def test_no_tile_exceeds_step_lat(self) -> None:
        step = 0.005
        for tile_n, tile_s, _e, _w in split_bbox(52.377, 52.367, 4.902, 4.878, step=step):
            assert (tile_n - tile_s) <= step + 1e-9

    def test_no_tile_exceeds_step_lon(self) -> None:
        step = 0.005
        for _n, _s, tile_e, tile_w in split_bbox(52.377, 52.367, 4.902, 4.878, step=step):
            assert (tile_e - tile_w) <= step + 1e-9

    def test_single_tile_when_bbox_smaller_than_step(self) -> None:
        """A bbox already smaller than the step must produce exactly one tile."""
        tiles = list(split_bbox(52.372, 52.370, 4.882, 4.880, step=0.01))
        assert len(tiles) == 1
        tile_n, tile_s, tile_e, tile_w = tiles[0]
        assert tile_n == pytest.approx(52.372)
        assert tile_s == pytest.approx(52.370)
        assert tile_e == pytest.approx(4.882)
        assert tile_w == pytest.approx(4.880)

    def test_tile_count_is_reasonable_for_amsterdam(self) -> None:
        """Amsterdam preset (~0.010° × 0.024°) with step=0.005 → 2×5 = 10 tiles."""
        tiles = list(split_bbox(52.377, 52.367, 4.902, 4.878, step=0.005))
        # Expect 2 rows × 5 cols = 10 tiles (give a little slack for float edges)
        assert 8 <= len(tiles) <= 12

    def test_large_bbox_produces_many_tiles(self) -> None:
        """A 1° × 1° box with step=0.005 should produce ~200 × 200 = 40 000 tiles."""
        tiles = list(split_bbox(1.0, 0.0, 1.0, 0.0, step=0.005))
        # 1.0 / 0.005 = 200 steps per axis
        assert len(tiles) == 200 * 200

    def test_negative_longitude_bbox(self) -> None:
        """NYC bbox has negative longitudes — tiler must handle them correctly."""
        n, s, e, w = 40.714, 40.703, -74.004, -74.020
        tiles = list(split_bbox(n, s, e, w, step=0.005))
        assert tiles
        for tile_n, tile_s, tile_e, tile_w in tiles:
            assert tile_n > tile_s
            assert tile_e > tile_w

    def test_tiles_four_tuple(self) -> None:
        for tile in split_bbox(52.377, 52.367, 4.902, 4.878):
            assert len(tile) == 4

    def test_default_step_is_small_enough(self) -> None:
        """Default step (0.005°) should produce multiple tiles for Amsterdam."""
        tiles = list(split_bbox(52.377, 52.367, 4.902, 4.878))
        assert len(tiles) > 1


# ── TestPresetRegistry ────────────────────────────────────────────────────────

class TestPresetRegistry:
    """Consistency checks across CITY_PRESETS, _PRESET_ORDER, _PRESET_REGIONS."""

    def test_preset_order_keys_exist_in_city_presets(self) -> None:
        for key in _PRESET_ORDER:
            assert key in CITY_PRESETS, f"_PRESET_ORDER key '{key}' not in CITY_PRESETS"

    def test_preset_region_keys_exist_in_city_presets(self) -> None:
        for _, keys in _PRESET_REGIONS:
            for key in keys:
                assert key in CITY_PRESETS, \
                    f"_PRESET_REGIONS key '{key}' not in CITY_PRESETS"

    def test_preset_order_no_duplicates(self) -> None:
        seen: set[str] = set()
        for key in _PRESET_ORDER:
            assert key not in seen, f"Duplicate key '{key}' in _PRESET_ORDER"
            seen.add(key)

    def test_preset_regions_no_duplicates_within_group(self) -> None:
        for label, keys in _PRESET_REGIONS:
            seen: set[str] = set()
            for key in keys:
                assert key not in seen, \
                    f"Duplicate key '{key}' in _PRESET_REGIONS group '{label}'"
                seen.add(key)

    def test_preset_regions_no_key_in_two_groups(self) -> None:
        all_region_keys: list[str] = []
        for _, keys in _PRESET_REGIONS:
            all_region_keys.extend(keys)
        assert len(all_region_keys) == len(set(all_region_keys)), \
            "A key appears in more than one _PRESET_REGIONS group"

    def test_order_and_regions_cover_same_keys(self) -> None:
        order_set  = set(_PRESET_ORDER)
        region_set = {k for _, keys in _PRESET_REGIONS for k in keys}
        assert order_set == region_set, (
            f"Keys only in _PRESET_ORDER: {order_set - region_set}\n"
            f"Keys only in _PRESET_REGIONS: {region_set - order_set}"
        )

    def test_aliases_not_in_preset_order(self) -> None:
        """Alias keys (e.g. 'adam') should not appear in _PRESET_ORDER."""
        assert "adam" not in _PRESET_ORDER, "'adam' alias must not be in _PRESET_ORDER"

    def test_city_presets_has_at_least_fifteen_entries(self) -> None:
        assert len(CITY_PRESETS) >= 17, "Expected at least 17 presets (including aliases)"


# ── TestParseBbox ─────────────────────────────────────────────────────────────

class TestParseBbox:
    """Unit tests for the _parse_bbox helper."""

    def test_valid_comma_separated(self) -> None:
        result = _parse_bbox("50.002,49.995,8.278,8.266")
        assert result == (50.002, 49.995, 8.278, 8.266)

    def test_returns_four_tuple(self) -> None:
        result = _parse_bbox("50.002,49.995,8.278,8.266")
        assert len(result) == 4

    def test_all_values_are_floats(self) -> None:
        result = _parse_bbox("50.002,49.995,8.278,8.266")
        for v in result:
            assert isinstance(v, float)

    def test_whitespace_around_values_is_stripped(self) -> None:
        result = _parse_bbox(" 50.002 , 49.995 , 8.278 , 8.266 ")
        assert result == (50.002, 49.995, 8.278, 8.266)

    def test_negative_longitude_accepted(self) -> None:
        """NYC bbox has negative longitudes."""
        result = _parse_bbox("40.714,40.703,-74.004,-74.020")
        assert result == (40.714, 40.703, -74.004, -74.020)

    def test_three_values_raises_bad_parameter(self) -> None:
        with pytest.raises(click.BadParameter):
            _parse_bbox("50.0,49.9,8.2")

    def test_five_values_raises_bad_parameter(self) -> None:
        with pytest.raises(click.BadParameter):
            _parse_bbox("50.0,49.9,8.2,8.1,0.0")

    def test_empty_string_raises_bad_parameter(self) -> None:
        with pytest.raises(click.BadParameter):
            _parse_bbox("")

    def test_non_float_raises_bad_parameter(self) -> None:
        with pytest.raises(click.BadParameter):
            _parse_bbox("north,south,east,west")

    def test_north_equal_south_raises(self) -> None:
        with pytest.raises(click.BadParameter):
            _parse_bbox("50.0,50.0,8.2,8.1")

    def test_north_less_than_south_raises(self) -> None:
        with pytest.raises(click.BadParameter):
            _parse_bbox("49.9,50.0,8.2,8.1")

    def test_east_equal_west_raises(self) -> None:
        with pytest.raises(click.BadParameter):
            _parse_bbox("50.0,49.9,8.2,8.2")

    def test_east_less_than_west_raises(self) -> None:
        with pytest.raises(click.BadParameter):
            _parse_bbox("50.0,49.9,8.1,8.2")


# ── TestBuildingHeight ────────────────────────────────────────────────────────

class TestBuildingHeight:
    """Unit tests for the _building_height OSM tag extractor."""

    def test_explicit_height_tag(self) -> None:
        assert _building_height({"height": "15"}, default=10.0) == 15.0

    def test_height_tag_with_m_suffix(self) -> None:
        assert _building_height({"height": "20m"}, default=10.0) == 20.0

    def test_height_tag_with_whitespace(self) -> None:
        assert _building_height({"height": " 12.5 m "}, default=10.0) == 12.5

    def test_float_string_height(self) -> None:
        assert _building_height({"height": "8.75"}, default=10.0) == 8.75

    def test_levels_tag_used_when_no_height(self) -> None:
        result = _building_height({"building:levels": "4"}, default=10.0)
        assert result == pytest.approx(4 * 3.5)

    def test_levels_float_string(self) -> None:
        result = _building_height({"building:levels": "3.0"}, default=10.0)
        assert result == pytest.approx(3.0 * 3.5)

    def test_height_takes_priority_over_levels(self) -> None:
        result = _building_height(
            {"height": "25", "building:levels": "10"}, default=10.0
        )
        assert result == 25.0

    def test_no_tags_returns_default(self) -> None:
        assert _building_height({}, default=7.0) == 7.0

    def test_none_height_tag_falls_through(self) -> None:
        # OSM GeoDataFrame rows often have None for absent tags
        assert _building_height({"height": None}, default=9.0) == 9.0

    def test_invalid_height_string_falls_through_to_levels(self) -> None:
        result = _building_height(
            {"height": "varies", "building:levels": "3"}, default=10.0
        )
        assert result == pytest.approx(3 * 3.5)

    def test_invalid_height_and_invalid_levels_returns_default(self) -> None:
        result = _building_height(
            {"height": "unknown", "building:levels": "several"}, default=5.5
        )
        assert result == 5.5

    def test_missing_keys_returns_default(self) -> None:
        assert _building_height({"amenity": "cafe"}, default=3.0) == 3.0


# ── TestPresetsCLI ────────────────────────────────────────────────────────────

class TestPresetsCLI:
    """Tests for `sdlos mesh presets` via Click CliRunner.

    These invoke the CLI without network access; no mesh deps required.
    """

    def test_presets_exits_zero(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets"])
        assert result.exit_code == 0, result.output

    def test_presets_output_contains_hamburg(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets"])
        assert "hamburg" in result.output

    def test_presets_output_contains_germany_header(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets"])
        assert "Germany" in result.output

    def test_presets_output_contains_rome(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets"])
        assert "rome" in result.output

    def test_presets_output_contains_tokyo(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets"])
        assert "tokyo" in result.output

    def test_presets_output_contains_osaka(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets"])
        assert "osaka" in result.output

    def test_presets_output_contains_kyoto(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets"])
        assert "kyoto" in result.output

    def test_presets_output_contains_cappadocia(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets"])
        assert "cappadocia" in result.output.lower() or "Cappadocia" in result.output, (
            f"'cappadocia' not found in presets output: {result.output[:300]}"
        )

    def test_presets_bbox_only_exits_zero(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets", "--bbox-only"])
        assert result.exit_code == 0, result.output

    def test_presets_bbox_only_contains_hamburg(self) -> None:
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets", "--bbox-only"])
        assert "hamburg" in result.output

    def test_presets_bbox_only_tab_separated(self) -> None:
        """Each line in --bbox-only output is: key<TAB>N,S,E,W"""
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets", "--bbox-only"])
        for line in result.output.strip().splitlines():
            parts = line.split("\t")
            assert len(parts) == 2, f"Expected TAB-separated line, got: {line!r}"
            key, bbox = parts
            assert key in CITY_PRESETS or key in _PRESET_ORDER, \
                f"Unexpected key in --bbox-only output: {key!r}"
            n, s, e, w = bbox.split(",")
            float(n); float(s); float(e); float(w)  # must all parse as floats

    def test_presets_bbox_only_row_count(self) -> None:
        """--bbox-only should emit exactly as many lines as _PRESET_ORDER."""
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["presets", "--bbox-only"])
        lines = [l for l in result.output.strip().splitlines() if l]
        assert len(lines) == len(_PRESET_ORDER)

    def test_generate_without_mesh_deps_shows_error(self) -> None:
        """Invoking generate with no preset/place/bbox should fail gracefully."""
        runner = CliRunner()
        result = runner.invoke(cmd_mesh, ["generate"])
        # Should exit non-zero — either missing-option or missing-dep error
        assert result.exit_code != 0


# ── TestGltfCityMeshes ────────────────────────────────────────────────────────
# These tests automatically activate as soon as .gltf / .glb files are placed
# in assets/city-meshes/.  They are silently skipped when the directory is empty.

def _gltf_ids(files: list[Path]) -> list[str]:
    return [f.name for f in files]


@pytest.mark.skipif(not _GLTF_FILES, reason="No .gltf files in assets/city-meshes/")
class TestGltfFiles:
    """Structural validation of .gltf (JSON) files in assets/city-meshes/."""

    @pytest.fixture(params=_GLTF_FILES, ids=_gltf_ids(_GLTF_FILES))
    def gltf_path(self, request: pytest.FixtureRequest) -> Path:
        return request.param  # type: ignore[return-value]

    @pytest.fixture
    def gltf_doc(self, gltf_path: Path) -> dict:
        return json.loads(gltf_path.read_text(encoding="utf-8"))

    def test_is_valid_json(self, gltf_path: Path) -> None:
        text = gltf_path.read_text(encoding="utf-8")
        doc = json.loads(text)
        assert isinstance(doc, dict)

    def test_asset_key_present(self, gltf_doc: dict) -> None:
        assert "asset" in gltf_doc, "GLTF document missing top-level 'asset' key"

    def test_asset_version_is_2_0(self, gltf_doc: dict) -> None:
        assert gltf_doc["asset"].get("version") == "2.0", \
            f"Expected asset.version '2.0', got {gltf_doc['asset'].get('version')!r}"

    def test_meshes_key_present(self, gltf_doc: dict) -> None:
        assert "meshes" in gltf_doc, "GLTF document missing 'meshes' array"

    def test_meshes_is_non_empty(self, gltf_doc: dict) -> None:
        assert len(gltf_doc["meshes"]) > 0, "GLTF 'meshes' array is empty"

    def test_each_mesh_has_primitives(self, gltf_doc: dict) -> None:
        for i, mesh in enumerate(gltf_doc["meshes"]):
            assert "primitives" in mesh, f"mesh[{i}] missing 'primitives'"
            assert len(mesh["primitives"]) > 0, f"mesh[{i}].primitives is empty"

    def test_nodes_key_present(self, gltf_doc: dict) -> None:
        assert "nodes" in gltf_doc, "GLTF document missing 'nodes' array"

    def test_accessors_key_present(self, gltf_doc: dict) -> None:
        assert "accessors" in gltf_doc, "GLTF document missing 'accessors' array"

    def test_buffer_views_present_when_buffers_exist(self, gltf_doc: dict) -> None:
        if gltf_doc.get("buffers"):
            assert "bufferViews" in gltf_doc, \
                "GLTF has 'buffers' but no 'bufferViews'"

    def test_file_size_is_nonzero(self, gltf_path: Path) -> None:
        assert gltf_path.stat().st_size > 0


@pytest.mark.skipif(not _GLB_FILES, reason="No .glb files in assets/city-meshes/")
class TestGlbFiles:
    """Structural validation of .glb (binary GLTF) files in assets/city-meshes/."""

    # GLB binary layout (little-endian):
    # bytes 0-3   : magic  0x46546C67  ("glTF")
    # bytes 4-7   : version (uint32) — must be 2
    # bytes 8-11  : total file length (uint32)
    _MAGIC = 0x46546C67  # "glTF" as little-endian uint32

    @pytest.fixture(params=_GLB_FILES, ids=_gltf_ids(_GLB_FILES))
    def glb_path(self, request: pytest.FixtureRequest) -> Path:
        return request.param  # type: ignore[return-value]

    @pytest.fixture
    def glb_header(self, glb_path: Path) -> tuple[int, int, int]:
        """Return (magic, version, length) from the GLB header."""
        data = glb_path.read_bytes()
        magic, version, length = struct.unpack_from("<III", data, 0)
        return magic, version, length

    def test_magic_bytes(self, glb_header: tuple[int, int, int]) -> None:
        magic, _ver, _len = glb_header
        assert magic == self._MAGIC, \
            f"GLB magic bytes wrong: expected 0x{self._MAGIC:08X}, got 0x{magic:08X}"

    def test_version_is_2(self, glb_header: tuple[int, int, int]) -> None:
        _magic, version, _len = glb_header
        assert version == 2, f"GLB version must be 2, got {version}"

    def test_declared_length_matches_file_size(
        self, glb_path: Path, glb_header: tuple[int, int, int]
    ) -> None:
        _magic, _ver, declared_len = glb_header
        actual_len = glb_path.stat().st_size
        assert declared_len == actual_len, (
            f"GLB header says {declared_len} bytes, file is {actual_len} bytes"
        )

    def test_file_size_nonzero(self, glb_path: Path) -> None:
        assert glb_path.stat().st_size > 0
