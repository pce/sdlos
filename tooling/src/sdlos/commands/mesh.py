"""
sdlos.commands.mesh
===================
``sdlos mesh`` — generate 3D region meshes from OpenStreetMap data.

Subcommands
-----------
  sdlos mesh generate   — fetch OSM buildings, extrude to 3D, export GLTF
  sdlos mesh info       — inspect a generated .gltf / .glb mesh file
  sdlos mesh presets    — list built-in city presets with bbox + notes

Dependencies (optional group "mesh")
-------------------------------------
  uv add --optional mesh osmnx trimesh shapely numpy requests

Install them first, then use this command:
  uv run --project tooling sdlos mesh generate --place "Mainz, Germany" --name mainz

Design
------
  Region data comes from OpenStreetMap via osmnx.
  Each building polygon is extruded with trimesh to a 3D prism.
  Building height is taken from the OSM ``height`` or ``building:levels``
  tag when available; falls back to --building-height default.

  Optional passes (all off by default, each toggled with a flag):

    --lod lowpoly     Quadratic mesh decimation to --face-count triangles.
                      Uses trimesh.simplify_quadric_decimation().

    --dem             Add a flat ground plane under the buildings.
                      Extent matches the bounding box; depth = --dem-depth m.
                      (Full SRTM/NASA elevation lookup is future work.)

  Output
  ------
    <out-dir>/<name>.gltf    (default format)
    <out-dir>/<name>.glb     (with --format glb)
    <out-dir>/<name>.obj     (with --format obj)

  When --app is given the mesh lands in
  <project-root>/examples/apps/<app>/data/models/<name>.<fmt>
  so sdlos run can find it immediately.

Examples
--------
  # Built-in city preset (fastest way to get started)
  sdlos mesh generate --preset rome
  sdlos mesh generate --preset paris --lod lowpoly --face-count 4000
  sdlos mesh generate --preset chicago --dem
  sdlos mesh generate --preset cologne --format glb
  sdlos mesh generate --preset hamburg
  sdlos mesh generate --preset osaka
  sdlos mesh generate --preset kyoto
  sdlos mesh generate --preset milan
  sdlos mesh generate --preset athens
  sdlos mesh generate --preset cappadocia

  # By place name (geocoded automatically)
  sdlos mesh generate --place "Mainz, Germany" --name mainz

  # By explicit bounding box (north south east west)
  sdlos mesh generate --bbox "50.002,49.995,8.278,8.268" --name mainz_bbox

  # Low-poly + ground plane, install into a running app
  sdlos mesh generate --preset amsterdam --lod lowpoly --dem --app flatshader

  # GLB + quiet for scripting
  sdlos mesh generate --preset detroit --format glb -q

  # List all presets grouped by region
  sdlos mesh presets

  # Cache management
  sdlos mesh cache list        # show cached fetches with size + bbox
  sdlos mesh cache clear       # delete all cached GeoJSON files
  sdlos mesh cache clear rome  # delete one preset's cache entry
"""
from __future__ import annotations

import hashlib
import sys
from dataclasses import dataclass
from pathlib import Path
import math as _math
from typing import Any, Callable, Iterator, Optional

import click


# ── City presets ──────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class CityPreset:
    """A named bounding box for a well-known city area.

    bbox is (north, south, east, west) in WGS-84 decimal degrees.
    All boxes are sized to a walkable downtown core (~0.5–1.5 km²) so the
    OSM fetch and extrusion complete in a few seconds on a normal connection.
    """
    key:             str    # CLI key, e.g. "rome"
    label:           str    # human label, e.g. "Rome — Historic Centre"
    bbox:            tuple[float, float, float, float]  # N, S, E, W
    building_height: float  # fallback height (m) when OSM tag is absent
    notes:           str    # one-line area description shown in `presets` table


# Keys are the canonical short names accepted by --preset.
# Bboxes are sized to walkable downtown cores (~0.5–2 km²) so OSM fetch
# completes in a few seconds without hitting Overpass sub-query limits.
CITY_PRESETS: dict[str, CityPreset] = {

    # ── Germany ───────────────────────────────────────────────────────────────
    "mainz": CityPreset(
        key             = "mainz",
        label           = "Mainz — Altstadt / Dom",
        bbox            = (49.999, 49.993, 8.278, 8.266),
        building_height = 12.0,
        notes           = "Medieval old town, Rhine riverfront, Dom square",
    ),
    "cologne": CityPreset(
        key             = "cologne",
        label           = "Cologne — Altstadt / Dom",
        bbox            = (50.940, 50.932, 6.966, 6.952),
        building_height = 14.0,
        notes           = "Dom, Hohe Straße, Alter Markt, Rhine promenade",
    ),
    "hamburg": CityPreset(
        key             = "hamburg",
        label           = "Hamburg — Innenstadt / Rathaus",
        bbox            = (53.556, 53.546, 10.006, 9.982),
        building_height = 22.0,
        notes           = "Rathaus, Binnenalster, Mönckebergstraße, Jungfernstieg",
    ),
    "berlin": CityPreset(
        key             = "berlin",
        label           = "Berlin — Mitte",
        bbox            = (52.522, 52.514, 13.406, 13.388),
        building_height = 18.0,
        notes           = "Museumsinsel, Unter den Linden, Alexanderplatz edge",
    ),

    # ── Italy ─────────────────────────────────────────────────────────────────
    "rome": CityPreset(
        key             = "rome",
        label           = "Rome — Historic Centre",
        bbox            = (41.901, 41.893, 12.483, 12.468),
        building_height = 16.0,
        notes           = "Pantheon → Campo de' Fiori → Largo Argentina",
    ),
    "milan": CityPreset(
        key             = "milan",
        label           = "Milan — Duomo / Brera",
        bbox            = (45.468, 45.461, 9.196, 9.182),
        building_height = 20.0,
        notes           = "Duomo, Galleria Vittorio Emanuele, Scala, Brera",
    ),

    # ── France ────────────────────────────────────────────────────────────────
    "paris": CityPreset(
        key             = "paris",
        label           = "Paris — Île de la Cité / Marais",
        bbox            = (48.857, 48.848, 2.358, 2.338),
        building_height = 18.0,
        notes           = "Notre-Dame, Sainte-Chapelle, Place du Châtelet",
    ),

    # ── Netherlands ───────────────────────────────────────────────────────────
    "amsterdam": CityPreset(
        key             = "amsterdam",
        label           = "Amsterdam — Grachtengordel",
        bbox            = (52.377, 52.367, 4.902, 4.878),
        building_height = 11.0,
        notes           = "Canal ring, Jordaan, Leidseplein",
    ),
    "adam": CityPreset(          # short alias
        key             = "adam",
        label           = "Amsterdam — Centrum (alias for 'amsterdam')",
        bbox            = (52.377, 52.367, 4.902, 4.878),
        building_height = 11.0,
        notes           = "Same bbox as 'amsterdam'",
    ),

    # ── United Kingdom ────────────────────────────────────────────────────────
    "london": CityPreset(
        key             = "london",
        label           = "London — The City / Square Mile",
        bbox            = (51.516, 51.508, -0.080, -0.098),
        building_height = 30.0,
        notes           = "Bank, St Paul's, Leadenhall, Lloyd's cluster",
    ),

    # ── Greece ────────────────────────────────────────────────────────────────
    "athens": CityPreset(
        key             = "athens",
        label           = "Athens — Monastiraki / Plaka",
        bbox            = (37.979, 37.971, 23.731, 23.719),
        building_height = 10.0,
        notes           = "Monastiraki sq, Plaka, Thissio, foot of Acropolis",
    ),

    # ── Austria ───────────────────────────────────────────────────────────────
    "vienna": CityPreset(
        key             = "vienna",
        label           = "Vienna — Innere Stadt",
        bbox            = (48.212, 48.203, 16.377, 16.360),
        building_height = 22.0,
        notes           = "Stephansdom, Graben, Hofburg, Burgring",
    ),

    # ── Czech Republic ────────────────────────────────────────────────────────
    "prague": CityPreset(
        key             = "prague",
        label           = "Prague — Staré Město",
        bbox            = (50.090, 50.082, 14.428, 14.412),
        building_height = 16.0,
        notes           = "Old Town Square, Charles Bridge end, Josefov",
    ),

    # ── Hungary ───────────────────────────────────────────────────────────────
    "budapest": CityPreset(
        key             = "budapest",
        label           = "Budapest — Belváros / Inner City",
        bbox            = (47.502, 47.493, 19.062, 19.047),
        building_height = 18.0,
        notes           = "Vörösmarty tér, Váci utca, Ferenciek tere",
    ),

    # ── Portugal ──────────────────────────────────────────────────────────────
    "lisbon": CityPreset(
        key             = "lisbon",
        label           = "Lisbon — Alfama / Baixa",
        bbox            = (38.717, 38.708, -9.131, -9.144),
        building_height = 13.0,
        notes           = "Praça do Comércio, Rossio, Alfama quarter",
    ),

    # ── Spain ─────────────────────────────────────────────────────────────────
    "madrid": CityPreset(
        key             = "madrid",
        label           = "Madrid — Centro",
        bbox            = (40.421, 40.412, -3.697, -3.712),
        building_height = 18.0,
        notes           = "Puerta del Sol, Gran Vía, Plaza Mayor",
    ),
    "barcelona": CityPreset(
        key             = "barcelona",
        label           = "Barcelona — Barri Gòtic",
        bbox            = (41.388, 41.379, 2.180, 2.165),
        building_height = 20.0,
        notes           = "Gothic Quarter, La Rambla, Born district",
    ),

    # ── USA ───────────────────────────────────────────────────────────────────
    "nyc": CityPreset(
        key             = "nyc",
        label           = "New York City — Financial District",
        bbox            = (40.714, 40.703, -74.004, -74.020),
        building_height = 40.0,
        notes           = "Wall Street, World Trade, Battery Park area",
    ),
    "chicago": CityPreset(
        key             = "chicago",
        label           = "Chicago — The Loop",
        bbox            = (41.887, 41.877, -87.624, -87.640),
        building_height = 45.0,
        notes           = "Millennium Park, Willis Tower, State Street grid",
    ),
    "detroit": CityPreset(
        key             = "detroit",
        label           = "Detroit — Downtown",
        bbox            = (42.336, 42.326, -83.040, -83.058),
        building_height = 20.0,
        notes           = "Renaissance Center, Campus Martius, Greektown",
    ),
    "philadelphia": CityPreset(
        key             = "philadelphia",
        label           = "Philadelphia — Center City",
        bbox            = (39.956, 39.944, -75.146, -75.167),
        building_height = 18.0,
        notes           = "City Hall, Rittenhouse Sq, Old City corridor",
    ),

    # ── Japan ─────────────────────────────────────────────────────────────────
    "tokyo": CityPreset(
        key             = "tokyo",
        label           = "Tokyo — Shinjuku West",
        bbox            = (35.694, 35.686, 139.703, 139.692),
        building_height = 35.0,
        notes           = "Shinjuku skyscraper district, Tochō, Takashimaya",
    ),
    "osaka": CityPreset(
        key             = "osaka",
        label           = "Osaka — Dōtonbori / Namba",
        bbox            = (34.673, 34.663, 135.510, 135.497),
        building_height = 25.0,
        notes           = "Dōtonbori canal, Namba, Shinsaibashi, Kuromon Market",
    ),
    "kyoto": CityPreset(
        key             = "kyoto",
        label           = "Kyoto — Kinkaku-ji / Golden Pavilion",
        bbox            = (35.044, 35.034, 135.735, 135.723),
        building_height = 8.0,
        notes           = "Kinkaku-ji (Golden Pavilion), Kinugasa district, Ninna-ji vicinity",
    ),

    # ── Turkey ────────────────────────────────────────────────────────────────
    "cappadocia": CityPreset(
        key             = "cappadocia",
        label           = "Cappadocia — Göreme Fairy Chimneys",
        bbox            = (38.655, 38.633, 34.843, 34.817),
        building_height = 5.0,
        notes           = "Göreme town, fairy chimney rock formations, cave dwellings, Open Air Museum — organic non-rectangular geometry",
    ),
}

# Canonical display order for `sdlos mesh presets` — grouped by region
_PRESET_ORDER = [
    # Germany
    "mainz", "cologne", "hamburg", "berlin",
    # Italy
    "rome", "milan",
    # France
    "paris",
    # Netherlands
    "amsterdam",
    # UK
    "london",
    # Greece
    "athens",
    # Central Europe capitals
    "vienna", "prague", "budapest",
    # Iberia
    "lisbon", "madrid", "barcelona",
    # USA
    "nyc", "chicago", "detroit", "philadelphia",
    # Asia
    "tokyo", "osaka", "kyoto",
    # Turkey
    "cappadocia",
]

# Region grouping for the presets table display
_PRESET_REGIONS: list[tuple[str, list[str]]] = [
    ("Germany",         ["mainz", "cologne", "hamburg", "berlin"]),
    ("Italy",           ["rome", "milan"]),
    ("France",          ["paris"]),
    ("Netherlands",     ["amsterdam"]),
    ("United Kingdom",  ["london"]),
    ("Greece",          ["athens"]),
    ("Central Europe",  ["vienna", "prague", "budapest"]),
    ("Iberia",          ["lisbon", "madrid", "barcelona"]),
    ("USA",             ["nyc", "chicago", "detroit", "philadelphia"]),
    ("Japan",           ["tokyo", "osaka", "kyoto"]),
    ("Turkey",          ["cappadocia"]),
]


#  lazy import helper

def _require_mesh_deps() -> None:
    """Raise a clean UsageError when the mesh optional deps are absent."""
    missing: list[str] = []
    for pkg in ("osmnx", "trimesh", "shapely", "numpy", "mapbox_earcut"):
        try:
            __import__(pkg)
        except ModuleNotFoundError:
            missing.append(pkg)
    if missing:
        raise click.UsageError(
            f"Missing mesh dependencies: {', '.join(missing)}\n\n"
            "Install them with:\n"
            "  uv add --optional mesh osmnx trimesh shapely numpy requests mapbox-earcut\n"
            "\nThen re-run your command."
        )


#  bbox parsing

def _parse_bbox(raw: str) -> tuple[float, float, float, float]:
    """Parse ``"north,south,east,west"`` into a float 4-tuple.

    Also accepts space-separated values and forgives leading/trailing
    whitespace inside each token.

    Returns
    -------
    (north, south, east, west)  — matching osmnx 2.x bbox convention.
    """
    parts = [p.strip() for p in raw.replace(" ", ",").split(",") if p.strip()]
    if len(parts) != 4:
        raise click.BadParameter(
            f"Expected 4 values (north,south,east,west), got {len(parts)}: {raw!r}",
            param_hint="--bbox",
        )
    try:
        north, south, east, west = (float(p) for p in parts)
    except ValueError as exc:
        raise click.BadParameter(
            f"All bbox values must be floats: {exc}",
            param_hint="--bbox",
        ) from exc
    if north <= south:
        raise click.BadParameter("north must be > south", param_hint="--bbox")
    if east <= west:
        raise click.BadParameter("east must be > west",  param_hint="--bbox")
    return north, south, east, west


#  OSM height helpers

def _building_height(row: "Any", default: float) -> float:  # noqa: F821
    """Return the best available building height from an OSM feature row.

    Priority
    --------
    1. ``height`` tag (metres, may include "m" suffix)
    2. ``building:levels`` × 3.5 m per floor
    3. *default* parameter
    """
    # 1. explicit height tag
    h = row.get("height")
    if h and h is not None:
        try:
            return float(str(h).replace("m", "").strip())
        except (ValueError, TypeError):
            pass

    # 2. levels × 3.5 m
    levels = row.get("building:levels")
    if levels and levels is not None:
        try:
            return float(str(levels).strip()) * 3.5
        except (ValueError, TypeError):
            pass

    return default



# Overpass endpoint fallback list
# Tried in order at query time; first successful response wins.
# Users can override with --endpoint or SDLOS_OVERPASS_ENDPOINT env var.
_OVERPASS_ENDPOINTS = [
    "https://overpass-api.de/api/interpreter",          # official (can be slow/down)
    "https://overpass.openstreetmap.fr/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",    # VERY reliable
    "https://lz4.overpass-api.de/api/interpreter",      # fast mirror
    "https://z.overpass-api.de/api/interpreter",        # alt cluster
]


def _with_overpass_fallback(
    endpoints: list[str],
    fn: "Callable[[str], Any]",
    quiet: bool,
) -> "Any":
    """Try *fn(endpoint)* against each Overpass endpoint in turn.

    For every candidate endpoint the function:
      1. Sets ``osmnx.settings.overpass_endpoint`` (for any residual osmnx calls).
      2. Calls ``fn(ep)`` — *fn* receives the endpoint URL as its only argument
         so it can embed it directly in its own HTTP request.
      3. On any exception, logs the failure and advances to the next endpoint.

    Raises the *last* exception if every endpoint fails.

    Passing the endpoint into *fn* (rather than relying on the osmnx global)
    lets callers use raw ``requests`` queries that bypass osmnx's internal
    subdivision heuristic entirely.
    """
    import osmnx as ox  # optional dep — only present when mesh group is installed

    last_exc: Optional[Exception] = None
    for ep in endpoints:
        if not quiet:
            click.echo(f"  trying    {ep}")
        try:
            ox.settings.overpass_endpoint = ep  # keep in sync for any osmnx calls
            return fn(ep)
        except Exception as exc:
            last_exc = exc
            if not quiet:
                click.echo(f"  [fail]    {ep}: {exc}")
    raise last_exc  # type: ignore[misc]


def split_bbox(
    north: float, south: float, east: float, west: float,
    step: float = 0.005,
) -> Iterator[tuple[float, float, float, float]]:
    """Tile a bounding box into sub-boxes of at most *step* degrees per side.

    Yields ``(north, south, east, west)`` tuples that together cover the
    full input area without gaps or overlaps.  Smaller tiles keep individual
    Overpass queries within server memory limits.

    A step of 0.005° is roughly 550 m × 350 m at mid-latitudes — well
    inside Overpass's comfort zone even under heavy load.

    Examples
    --------
    >>> list(split_bbox(52.377, 52.367, 4.902, 4.878))   # Amsterdam core
    [(52.372, 52.367, 4.883, 4.878), ...]  # 2 × 5 tiles
    """
    lat = south
    while lat < north:
        lon = west
        while lon < east:
            yield (
                min(lat + step, north),
                lat,
                min(lon + step, east),
                lon,
            )
            lon = round(lon + step, 9)   # avoid float-drift creep
        lat = round(lat + step, 9)


def _fetch_buildings_raw(
    north: float, south: float, east: float, west: float,
    endpoint: str,
    timeout: int = 90,
) -> "gpd.GeoDataFrame":
    """Fetch building footprints via a direct Overpass QL POST request.

    Bypasses osmnx's ``features_from_bbox`` / ``_consolidate_subdivide_geometry``
    code path entirely, which is the source of the "area is N times your
    configured max_query_area_size" infinite-subdivision bug in osmnx 2.x.

    One precisely-scoped Overpass QL query is sent per call; the server-side
    ``[timeout:N]`` directive ensures a clean error is returned rather than a
    hung connection when the server is under load.

    Parameters
    ----------
    north, south, east, west : float
        Bounding box in WGS-84 decimal degrees.
    endpoint : str
        Full Overpass API interpreter URL, e.g.
        ``"https://overpass.kumi.systems/api/interpreter"``.
    timeout : int
        Server-side **and** socket timeout in seconds.  The socket timeout
        is ``timeout + 15`` to give the server room to send its response.

    Returns
    -------
    geopandas.GeoDataFrame
        CRS ``EPSG:4326``.  Columns are OSM tag names plus ``osmid`` and
        ``element_type`` (``"way"`` or ``"relation"``).
    """
    import geopandas as gpd
    import requests
    from shapely.geometry import MultiPolygon, Polygon
    from shapely.ops import unary_union

    # Overpass QL bbox order is (south, west, north, east) — note the swap
    # from the (north, south, east, west) convention used everywhere else.
    query = (
        f"[out:json][timeout:{timeout}];\n"
        f"(\n"
        f'  way["building"]({south},{west},{north},{east});\n'
        f'  relation["building"]({south},{west},{north},{east});\n'
        f");\n"
        f"out body;\n"
        f">;\n"
        f"out skel qt;\n"
    )

    resp = requests.post(
        endpoint,
        data={"data": query},
        timeout=timeout + 15,   # socket timeout > server timeout so server error arrives
    )
    resp.raise_for_status()
    elements: list[dict] = resp.json().get("elements", [])

    #  Index raw elements by type
    nodes:     dict[int, tuple[float, float]] = {}   # id → (lon, lat)
    ways:      dict[int, dict]                = {}
    relations: dict[int, dict]                = {}

    for el in elements:
        t = el["type"]
        if t == "node" and "lat" in el:
            nodes[el["id"]] = (el["lon"], el["lat"])
        elif t == "way":
            ways[el["id"]] = el
        elif t == "relation":
            relations[el["id"]] = el

    def _way_polygon(way: dict) -> Optional[Polygon]:
        """Reconstruct a closed Polygon from a way's node-id list."""
        coords = [nodes[nid] for nid in way.get("nodes", []) if nid in nodes]
        if len(coords) < 4:
            return None
        try:
            poly = Polygon(coords)
            return poly if poly.is_valid else poly.buffer(0)
        except Exception:
            return None

    rows: list[dict] = []

    #  Ways → simple building polygons
    for way in ways.values():
        tags = way.get("tags", {})
        if "building" not in tags:
            continue
        poly = _way_polygon(way)
        if poly is None or poly.is_empty:
            continue
        row: dict = {"geometry": poly, "osmid": way["id"], "element_type": "way"}
        row.update(tags)
        rows.append(row)

    #  Relations → multipolygon buildings
    for rel in relations.values():
        tags = rel.get("tags", {})
        if "building" not in tags:
            continue
        outer_polys: list[Polygon] = []
        inner_polys: list[Polygon] = []
        for member in rel.get("members", []):
            if member.get("type") != "way":
                continue
            way = ways.get(member["ref"])
            if way is None:
                continue
            poly = _way_polygon(way)
            if poly is None or poly.is_empty:
                continue
            role = member.get("role", "")
            if role == "outer":
                outer_polys.append(poly)
            elif role == "inner":
                inner_polys.append(poly)
        if not outer_polys:
            continue
        try:
            geom = unary_union(outer_polys)
            for hole in inner_polys:
                geom = geom.difference(hole)
            if geom.is_empty:
                continue
            row = {"geometry": geom, "osmid": rel["id"], "element_type": "relation"}
            row.update(tags)
            rows.append(row)
        except Exception:
            continue

    if not rows:
        import pandas as _pd
        return gpd.GeoDataFrame(
            _pd.DataFrame(columns=["geometry", "osmid", "element_type"]),
            geometry="geometry",
            crs="EPSG:4326",
        )

    return gpd.GeoDataFrame(rows, geometry="geometry", crs="EPSG:4326")


def _generate(
    *,
    name: str,
    bbox: Optional[tuple[float, float, float, float]],
    place: Optional[str],
    lod: str,
    face_count: int,
    dem: bool,
    dem_depth: float,
    building_height: float,
    fmt: str,
    out_dir: Path,
    no_cache: bool = False,
    endpoint: Optional[str] = None,
    timeout: int = 180,
    quiet: bool,
) -> Path:
    """Run the full generation pipeline and return the output path.

    This function is intentionally free of Click so it can be called
    programmatically from tests or other tooling commands.
    """
    import numpy as np
    import osmnx as ox
    import trimesh
    from shapely.geometry import MultiPolygon, Polygon

    # osmnx query settings
    # max_query_area_size: osmnx 2.x subdivides any area larger than this (m²)
    # before sending to Overpass.  50 km² is a comfortable single-query ceiling;
    # our preset bboxes are all ≪ 5 km² and are never subdivided by osmnx, but
    # a --place that geocodes to a large district will be split automatically.
    ox.settings.max_query_area_size = 50_000_000   # 50 km²

    # Cap the HTTP socket timeout.  Values > 90 s risk a silently hung
    # connection on a stalled server; we also embed the same value as a
    # server-side Overpass QL directive (below) so the server gives up cleanly.
    _http_timeout = min(timeout, 90)
    ox.settings.requests_timeout = _http_timeout

    # Server-side timeout directive prepended to every Overpass QL query.
    # Without this, Overpass may queue the request indefinitely and the
    # Python socket hangs.  With it the server returns a clean error after
    # _http_timeout seconds, allowing _with_overpass_fallback to advance to
    # the next mirror immediately.
    ox.settings.overpass_settings = f"[out:json][timeout:{_http_timeout}]"

    # HTTP-level response cache — osmnx stores raw Overpass responses on disk.
    # A successful fetch is never re-sent to the API; subsequent calls return
    # the cached XML/JSON instantly.  Stored under ~/.sdlos/osmnx_http_cache/
    # separately from our GeoJSON cache so both layers are active.
    import os as _os
    _cache_folder = str(Path.home() / ".sdlos" / "osmnx_http_cache")
    Path(_cache_folder).mkdir(parents=True, exist_ok=True)
    ox.settings.use_cache    = True
    ox.settings.cache_folder = _cache_folder

    # Identify ourselves to the Overpass API.  Servers use the User-Agent to
    # rate-limit anonymous scrapers; a descriptive string gets better treatment.
    ox.settings.http_accept_language = "en"

    # Overpass endpoint selection
    # --endpoint / SDLOS_OVERPASS_ENDPOINT pin to exactly one URL.
    # Otherwise the full _OVERPASS_ENDPOINTS list is tried at query time by
    # _with_overpass_fallback, which advances to the next mirror on any error.
    import os
    env_ep = os.environ.get("SDLOS_OVERPASS_ENDPOINT")
    if endpoint:
        endpoints_to_try = [endpoint]
        if not quiet:
            click.echo(f"  endpoint  {endpoint}  (--endpoint)")
    elif env_ep:
        endpoints_to_try = [env_ep]
        if not quiet:
            click.echo(f"  endpoint  {env_ep}  (SDLOS_OVERPASS_ENDPOINT)")
    else:
        endpoints_to_try = _OVERPASS_ENDPOINTS
        if not quiet:
            click.echo(
                f"  will try {len(endpoints_to_try)} Overpass endpoints "
                f"(first to succeed wins)"
            )

    if not quiet:
        click.echo(
            "  tip  Results cached in ~/.sdlos/osmnx_http_cache/  "
            "(re-runs are instant)"
        )

    def _log(msg: str) -> None:
        if not quiet:
            click.echo(f"  {msg}")


    # 0. Cache helpers
    def _bbox_cache_key(n: float, s: float, e: float, w: float) -> str:
        """Short hex key derived from the bbox values (6 decimal places)."""
        raw = f"{n:.6f},{s:.6f},{e:.6f},{w:.6f}"
        return hashlib.sha256(raw.encode()).hexdigest()[:16]

    def _cache_dir() -> Path:
        d = Path.home() / ".sdlos" / "mesh_cache"
        d.mkdir(parents=True, exist_ok=True)
        return d

    def _cache_path_for(n: float, s: float, e: float, w: float) -> Path:
        return _cache_dir() / f"{_bbox_cache_key(n, s, e, w)}.geojson"

    # 1. Resolve bounding box
    if bbox is not None:
        north, south, east, west = bbox
        _log(f"bbox      {north:.4f},{south:.4f},{east:.4f},{west:.4f}")
    elif place is not None:
        _log(f"geocoding '{place}' …")
        gdf_place = ox.geocode_to_gdf(place)
        bounds = gdf_place.total_bounds  # (minx, miny, maxx, maxy) = (W, S, E, N)
        west, south, east, north = (
            float(bounds[0]),
            float(bounds[1]),
            float(bounds[2]),
            float(bounds[3]),
        )
        _log(f"bbox      N={north:.4f} S={south:.4f} E={east:.4f} W={west:.4f}")
    else:
        raise ValueError("Either bbox or place must be provided.")

    # 1b. Decide whether to tile the bbox
    # Preset bboxes are all ≪ 5 km² and are sent as a single Overpass query.
    # A geocoded --place area (e.g. a whole arrondissement or district) can be
    # much larger; tiling it into ≤ 0.005° × 0.005° cells keeps each
    # individual query well within Overpass server memory limits.
    _lat_m   = (north - south) * 111_000
    _lon_m   = (east  - west)  * 111_000 * _math.cos(_math.radians((north + south) / 2))
    _area_m2 = _lat_m * _lon_m
    _TILE_THRESHOLD_M2 = 5_000_000   # 5 km²

    if _area_m2 > _TILE_THRESHOLD_M2:
        _tiles = list(split_bbox(north, south, east, west, step=0.005))
        _log(
            f"  area {_area_m2 / 1e6:.1f} km² — tiling into "
            f"{len(_tiles)} sub-queries (threshold 5 km²)"
        )
    else:
        _tiles = [(north, south, east, west)]
        _log(f"  area {_area_m2 / 1e6:.2f} km² — single query")

    #  2. Fetch OSM buildings (with local GeoJSON cache)
    import geopandas as gpd

    cp = _cache_path_for(north, south, east, west)

    if cp.exists() and not no_cache:
        _log(f"cache hit  {cp.name}  (use --no-cache to re-fetch)")
        try:
            gdf = gpd.read_file(str(cp))
        except Exception:
            _log("  [warn] cache read failed — re-fetching from OSM")
            cp.unlink(missing_ok=True)
            gdf = None
    else:
        gdf = None

    if gdf is None:
        _log("fetching OSM buildings …")
        try:
            _all_gdfs: list["gpd.GeoDataFrame"] = []
            for _i, (_tn, _ts, _te, _tw) in enumerate(_tiles):
                if len(_tiles) > 1:
                    _log(
                        f"  tile {_i + 1}/{len(_tiles)}  "
                        f"N={_tn:.4f} S={_ts:.4f} E={_te:.4f} W={_tw:.4f}"
                    )
                def _fetch_tile(
                    ep: str,
                    tn: float = _tn, ts: float = _ts,
                    te: float = _te, tw: float = _tw,
                ) -> "gpd.GeoDataFrame":
                    return _fetch_buildings_raw(
                        north=tn, south=ts, east=te, west=tw,
                        endpoint=ep,
                        timeout=_http_timeout,
                    )
                _all_gdfs.append(
                    _with_overpass_fallback(endpoints_to_try, _fetch_tile, quiet)
                )

            if len(_all_gdfs) == 1:
                gdf = _all_gdfs[0]
            else:
                import pandas as _pd
                gdf = gpd.GeoDataFrame(
                    _pd.concat(_all_gdfs, ignore_index=True),
                    crs=_all_gdfs[0].crs,
                )
                # Drop duplicate features that straddle tile boundaries.
                _before = len(gdf)
                gdf = gdf.drop_duplicates(subset=["geometry"])
                if len(gdf) < _before:
                    _log(
                        f"  deduped  {_before - len(gdf)} duplicate features "
                        f"across tile boundaries"
                    )

        except Exception as exc:
            n_tried = len(endpoints_to_try)
            raise click.ClickException(
                f"OSM fetch failed on all {n_tried} endpoint(s): {exc}\n\n"
                "Possible causes:\n"
                "  • All Overpass mirrors are overloaded or down\n"
                "      → check https://status.overpass-api.de/\n"
                "      → wait a few minutes and retry\n"
                "      → pin a specific mirror:  --endpoint URL\n"
                "  • No internet connection\n"
                "  • Bounding box is outside an area with OSM coverage\n\n"
                "Public Overpass mirrors to try with --endpoint:\n"
                "  https://overpass.kumi.systems/api/interpreter    (reliable)\n"
                "  https://lz4.overpass-api.de/api/interpreter      (fast mirror)\n"
                "  https://overpass.openstreetmap.fr/api/interpreter\n\n"
                "Status page:  https://status.overpass-api.de/"
            ) from exc

        # Persist to cache (GeoJSON; columns with list values are dropped
        # because GeoJSON does not support them).
        try:
            saveable = gdf.copy()
            for col in saveable.columns:
                if col == "geometry":
                    continue
                if saveable[col].apply(lambda x: isinstance(x, list)).any():
                    saveable = saveable.drop(columns=[col])
            saveable.to_file(str(cp), driver="GeoJSON")
            _log(f"  cached   {cp.name}")
        except Exception as exc:
            _log(f"  [warn] could not write cache: {exc}")

    _log(f"  {len(gdf)} features returned")

    # 3. Project to metric CRS (UTM)
    # osmnx returns WGS-84 (degrees). We need metres for extrusion height.
    try:
        gdf_proj = ox.projection.project_gdf(gdf)
    except Exception:
        # Fallback: use a simple equirectangular approximation if projection fails
        gdf_proj = gdf

    # 3b. Simplify + filter noise (after projection — coords now in metres)
    # simplify(1.0) — removes sub-metre vertex noise from dense OSM traces
    #                 without visibly changing building outlines at city scale.
    # area > 10 m²  — drops OSM noise: manholes, pillar bases, sub-polygon
    #                 artefacts that survive relation parsing.
    _n_before_filter = len(gdf_proj)
    gdf_proj["geometry"] = gdf_proj["geometry"].simplify(1.0)
    gdf_proj = gdf_proj[
        ~gdf_proj["geometry"].is_empty & (gdf_proj.area > 10.0)
    ].copy()
    _n_filtered = _n_before_filter - len(gdf_proj)
    if _n_filtered:
        _log(f"  filtered  {_n_filtered} tiny / degenerate features (< 10 m²)")

    # 4. Extrude each polygon to a 3D mesh
    # Guard: trimesh requires a polygon triangulator (mapbox-earcut or triangle)
    # to extrude footprints.  Without one it prints the same warning once per
    # building — thousands of times — then silently skips everything.  Check
    # once up front and raise a clean, actionable error instead.
    _triangulator: str | None = None
    for _tri_pkg, _tri_name in (("mapbox_earcut", "mapbox-earcut"), ("triangle", "triangle")):
        try:
            __import__(_tri_pkg)
            _triangulator = _tri_name
            break
        except ImportError:
            pass
    if _triangulator is None:
        raise click.ClickException(
            "No polygon triangulator found — trimesh cannot extrude building "
            "footprints without one.\n\n"
            "Install the recommended open-source engine:\n"
            "  uv add --optional mesh mapbox-earcut\n\n"
            "Then re-run the command."
        )

    import warnings as _warnings

    # LOD pre-processing: building count cap + 2D footprint simplification
    # A rectangular prism (4-sided footprint) extrudes to 12 triangles minimum.
    # If face_count / 12 < n_buildings the budget cannot show every building —
    # we keep the N largest by projected area, then simplify each 2D footprint
    # polygon before extrusion (Douglas-Peucker, 3 m tolerance).  This directly
    # controls extruded vertex count without relying on mesh decimation, which
    # cannot reduce a solid below its minimum face count (~12) and silently
    # no-ops on disconnected multi-building scenes.
    _MIN_FACES_PER_BLDG = 12   # physical minimum triangles for a rectangular prism
    _lod_simplify_tol   = 3.0  # 2-D footprint D-P tolerance in projected metres

    if lod == "lowpoly":
        _max_bldgs   = max(1, face_count // _MIN_FACES_PER_BLDG)
        _total_bldgs = len(gdf_proj)
        if _max_bldgs < _total_bldgs:
            _log(
                f"  face budget {face_count:,} → keeping {_max_bldgs:,} largest "
                f"buildings of {_total_bldgs:,} (by projected area)"
            )
            _gdf_tmp = gdf_proj.copy()
            _gdf_tmp["_area"] = _gdf_tmp.area
            gdf_proj = _gdf_tmp.nlargest(_max_bldgs, "_area").drop(columns=["_area"])
        else:
            _log(
                f"  {_total_bldgs:,} buildings × {_MIN_FACES_PER_BLDG} min faces = "
                f"~{_total_bldgs * _MIN_FACES_PER_BLDG:,} faces floor  "
                f"(target {face_count:,})"
            )

    _log("extruding building polygons …")
    meshes: list[trimesh.Trimesh] = []
    skipped = 0

    for idx, row in gdf_proj.iterrows():
        geom = row.geometry
        h = _building_height(row, building_height)

        polys: list[Polygon] = []
        if isinstance(geom, Polygon) and not geom.is_empty:
            polys = [geom]
        elif isinstance(geom, MultiPolygon):
            polys = [p for p in geom.geoms if isinstance(p, Polygon) and not p.is_empty]

        for poly in polys:
            # 2-D footprint simplification (lowpoly mode)
            # Simplify the flat polygon before extrusion.  D-P at 3 m collapses
            # complex canal-house outlines to 4–8 vertices; the extruded solid
            # then has 10–18 triangles instead of 40–100+.  This is the correct
            # LOD lever — mesh decimation cannot go below ~12 faces per solid.
            if lod == "lowpoly":
                poly = poly.simplify(_lod_simplify_tol, preserve_topology=True)
                if poly.is_empty:
                    skipped += 1
                    continue

            # Ensure exterior ring is valid and has enough vertices
            if len(poly.exterior.coords) < 4:
                skipped += 1
                continue
            try:
                # Suppress trimesh's per-polygon triangulator warning — we
                # already verified a working engine is present above.
                with _warnings.catch_warnings():
                    _warnings.simplefilter("ignore")
                    mesh = trimesh.creation.extrude_polygon(poly, h)
                meshes.append(mesh)
            except Exception:
                skipped += 1
                continue

    _log(f"  {len(meshes)} meshes built  ({skipped} skipped / invalid)")

    if not meshes:
        raise click.ClickException(
            "No valid building polygons could be extruded.\n\n"
            "This usually means all footprints in the cached GeoJSON are\n"
            "degenerate after projection.  Try:\n"
            "  sdlos mesh generate --preset amsterdam --no-cache\n"
            "to re-fetch fresh data from Overpass."
        )

    # 5. Merge into one scene
    _log("merging scene …")
    scene: trimesh.Trimesh = trimesh.util.concatenate(meshes)
    # center geometry around origin
    # rebase coords GIS data uses: global coordinate systems (UTM / WGS84)
    # 3D tools expect: local coordinates near origin
    centroid = scene.vertices.mean(axis=0)
    scene.vertices -= centroid

    _log(f"  total faces: {len(scene.faces):,}   vertices: {len(scene.vertices):,}")

    # 6. Optional: ground plane (DEM placeholder)
    if dem:
        _log("adding ground plane …")
        # Compute extent from projected bounding box
        vmin = scene.vertices.min(axis=0)
        vmax = scene.vertices.max(axis=0)
        extent_x = float(vmax[0] - vmin[0])
        extent_y = float(vmax[1] - vmin[1])

        ground = trimesh.creation.box(extents=(extent_x, extent_y, dem_depth))
        # Centre ground under the scene, sink it by half its depth
        ground.apply_translation([
            (vmin[0] + vmax[0]) * 0.5,
            (vmin[1] + vmax[1]) * 0.5,
            vmin[2] - dem_depth * 0.5,
        ])
        scene = trimesh.util.concatenate([scene, ground])
        _log(f"  ground plane  {extent_x:.0f} × {extent_y:.0f} × {dem_depth:.1f} m")

    # 7. LOD result summary
    # The real LOD work happened in step 4: building count cap + 2-D footprint
    # D-P simplification.  Report what we ended up with and give a useful tip
    # if the result is far above the requested budget (which happens when the
    # preset area contains more buildings than face_count // 12 allows).
    if lod == "lowpoly":
        current = len(scene.faces)
        target  = face_count
        _log(f"lod result {current:,} faces  (target {target:,})")
        if current > target * 3:
            floor_faces = (len(gdf_proj) if hasattr(gdf_proj, "__len__") else "?")
            _log(
                f"  tip  this area has too many buildings for {target:,} faces;\n"
                f"       try --face-count {current // 2:,} or a smaller preset bbox"
            )

    # 8. Export
    out_dir.mkdir(parents=True, exist_ok=True)
    ext_map = {"gltf": ".gltf", "glb": ".glb", "obj": ".obj"}
    ext = ext_map.get(fmt, ".gltf")
    out_path = out_dir / f"{name}{ext}"

    _log(f"exporting  {out_path} …")
    try:
        scene.export(str(out_path))
    except Exception as exc:
        raise click.ClickException(f"Export failed: {exc}") from exc

    size_kb = out_path.stat().st_size / 1024
    _log(f"  done  {size_kb:.1f} KB")

    return out_path


# ── Click command group ───────────────────────────────────────────────────────

@click.group("mesh")
def cmd_mesh() -> None:
    """Generate and inspect 3D region meshes from OpenStreetMap data.

    \b
    Requires the mesh optional dependencies:
      uv add --optional mesh osmnx trimesh shapely numpy requests

    \b
    Examples
    --------
      sdlos mesh generate --place "Mainz, Germany" --name mainz
      sdlos mesh generate --bbox "50.05,49.90,8.35,8.15" --name mainz_bbox
      sdlos mesh generate --place "Altstadt, Mainz" --name mainz_lp \\
              --lod lowpoly --face-count 3000 --dem
      sdlos mesh info output/mainz.gltf
    """


# ── sdlos mesh generate ───────────────────────────────────────────────────────

@cmd_mesh.command("generate")
@click.option(
    "--endpoint",
    default=None, metavar="URL",
    help=(
        "Overpass API endpoint URL.  "
        "When omitted the command probes a list of public mirrors and uses "
        "the first that responds.  "
        "Can also be set via the SDLOS_OVERPASS_ENDPOINT environment variable.  "
        "e.g. --endpoint https://overpass-api.de/api/interpreter"
    ),
)
@click.option(
    "--timeout",
    type=int, default=180, show_default=True, metavar="SEC",
    help="Per-request timeout in seconds for Overpass API calls.",
)
@click.option(
    "--preset", "preset_key",
    default=None, metavar="PRESET",
    help=(
        "Use a built-in city preset.  Sets --name and --bbox automatically.  "
        "Run `sdlos mesh presets` to list all available presets.  "
        "e.g. --preset rome"
    ),
)
@click.option(
    "--name", "-n",
    default=None,
    metavar="NAME",
    help=(
        "Output file stem, e.g. 'mainz'.  Final file: <out-dir>/<name>.<fmt>  "
        "Inferred from --preset when not set."
    ),
)
@click.option(
    "--place", "-p",
    default=None, metavar="PLACE",
    help=(
        "Place name for OSM geocoding, e.g. \"Mainz, Germany\".  "
        "Mutually exclusive with --bbox and --preset."
    ),
)
@click.option(
    "--bbox", "-b",
    default=None, metavar="N,S,E,W",
    help=(
        "Explicit bounding box as north,south,east,west in WGS-84 decimal degrees.  "
        "e.g. \"50.002,49.995,8.278,8.268\".  Mutually exclusive with --place and --preset."
    ),
)
@click.option(
    "--lod",
    type=click.Choice(["full", "lowpoly"], case_sensitive=False),
    default="full",
    show_default=True,
    help=(
        "Level of detail.  "
        "'lowpoly' runs quadratic mesh decimation to --face-count triangles."
    ),
)
@click.option(
    "--face-count",
    type=int, default=5000, show_default=True, metavar="N",
    help="Target triangle count for --lod lowpoly.",
)
@click.option(
    "--dem",
    is_flag=True, default=False,
    help=(
        "Add a flat ground plane beneath the buildings.  "
        "Extent matches the building bounding box.  "
        "Full SRTM/NASA elevation is planned; this inserts a box placeholder."
    ),
)
@click.option(
    "--dem-depth",
    type=float, default=1.0, show_default=True, metavar="M",
    help="Thickness of the ground plane in metres.",
)
@click.option(
    "--building-height",
    type=float, default=10.0, show_default=True, metavar="M",
    help=(
        "Fallback building height in metres when OSM has no 'height' or "
        "'building:levels' tag."
    ),
)
@click.option(
    "--format", "fmt",
    type=click.Choice(["gltf", "glb", "obj"], case_sensitive=False),
    default="gltf", show_default=True,
    help="Output file format.",
)
@click.option(
    "--out-dir",
    type=click.Path(path_type=Path),
    default=None, metavar="DIR",
    help=(
        "Output directory.  "
        "Default: ./output/ when --app is not set, "
        "or <project-root>/examples/apps/<app>/data/models/ when --app is set."
    ),
)
@click.option(
    "--app",
    default=None, metavar="APP",
    help=(
        "Install the mesh into an sdlos app's data/models/ directory.  "
        "e.g. --app flatshader  →  examples/apps/flatshader/data/models/<name>.<fmt>"
    ),
)
@click.option(
    "--no-cache",
    is_flag=True, default=False,
    help=(
        "Skip the local GeoJSON cache and re-fetch from Overpass.  "
        "Useful after OSM data has been updated in the region."
    ),
)
@click.option(
    "--quiet", "-q",
    is_flag=True, default=False,
    help="Suppress progress output.",
)
def cmd_generate(
    endpoint: Optional[str],
    timeout: int,
    preset_key: Optional[str],
    name: Optional[str],
    place: Optional[str],
    bbox: Optional[str],
    lod: str,
    face_count: int,
    dem: bool,
    dem_depth: float,
    building_height: float,
    fmt: str,
    out_dir: Optional[Path],
    app: Optional[str],
    no_cache: bool,
    quiet: bool,
) -> None:
    """Fetch OSM buildings and export a 3D region mesh.

    Provide one of --preset, --place, or --bbox to define the region.

    \b
    Examples
    --------
      sdlos mesh generate --preset rome
      sdlos mesh generate --preset nyc --lod lowpoly --face-count 4000
      sdlos mesh generate --place "Mainz, Germany" --name mainz
      sdlos mesh generate --bbox "50.002,49.995,8.278,8.268" --name mainz_bbox
      sdlos mesh generate --preset amsterdam --lod lowpoly --dem --app flatshader
    """
    _require_mesh_deps()

    # ── Resolve preset ────────────────────────────────────────────────────────
    preset: Optional[CityPreset] = None
    if preset_key is not None:
        preset = CITY_PRESETS.get(preset_key.lower())
        if preset is None:
            available = ", ".join(_PRESET_ORDER)
            raise click.BadParameter(
                f"Unknown preset '{preset_key}'.  Available: {available}\n"
                "Run `sdlos mesh presets` to see details.",
                param_hint="--preset",
            )

    # ── Validate mutual exclusivity ───────────────────────────────────────────
    sources = sum([preset is not None, place is not None, bbox is not None])
    if sources == 0:
        raise click.UsageError(
            "Provide one of:\n"
            "  --preset PRESET   (e.g. --preset rome)\n"
            "  --place  PLACE    (e.g. --place \"Mainz, Germany\")\n"
            "  --bbox   N,S,E,W  (e.g. --bbox \"50.002,49.995,8.278,8.268\")\n\n"
            "Run `sdlos mesh presets` to list built-in city presets."
        )
    if sources > 1:
        raise click.UsageError(
            "--preset, --place, and --bbox are mutually exclusive.  Use only one."
        )

    # ── Resolve name (required unless --preset supplies it) ───────────────────
    resolved_name: str
    if name:
        resolved_name = name
    elif preset is not None:
        resolved_name = preset.key
    else:
        raise click.UsageError(
            "--name is required when using --place or --bbox.\n"
            "e.g.  sdlos mesh generate --place \"Mainz\" --name mainz"
        )

    # ── Apply preset defaults (can be overridden by explicit flags) ───────────
    if preset is not None:
        parsed_bbox: Optional[tuple[float, float, float, float]] = preset.bbox
        # Only apply preset building_height if the user didn't pass the option
        # explicitly.  Click doesn't give us "was this set?" easily, so we
        # compare to the declared default (10.0) as a heuristic.
        if building_height == 10.0:
            building_height = preset.building_height
    else:
        parsed_bbox = None

    # ── Parse explicit bbox string ────────────────────────────────────────────
    if bbox is not None:
        parsed_bbox = _parse_bbox(bbox)

    # ── Resolve output directory ──────────────────────────────────────────────
    if out_dir is None:
        if app is not None:
            from ..core.cmake import find_project_root
            try:
                root = find_project_root(Path.cwd())
            except Exception:
                root = Path.cwd()
            out_dir = root / "examples" / "apps" / app / "data" / "models"
        else:
            out_dir = Path.cwd() / "output"

    # ── Banner ────────────────────────────────────────────────────────────────
    if not quiet:
        click.echo()
        click.echo(f"sdlos mesh generate  '{resolved_name}'")
        if preset:
            click.echo(f"  preset:   {preset.label}")
            n, s, e, w = preset.bbox
            click.echo(f"  bbox:     N={n} S={s} E={e} W={w}")
            click.echo(f"            {preset.notes}")
        elif place:
            click.echo(f"  place:    {place}")
        click.echo(f"  lod:      {lod}" + (f"  (target {face_count:,} faces)" if lod == "lowpoly" else ""))
        click.echo(f"  dem:      {'yes' if dem else 'no'}")
        click.echo(f"  format:   {fmt}")
        click.echo(f"  out-dir:  {out_dir}")
        click.echo()

    # ── Run ───────────────────────────────────────────────────────────────────
    try:
        out_path = _generate(
            name=resolved_name,
            bbox=parsed_bbox,
            place=place,
            lod=lod,
            face_count=face_count,
            dem=dem,
            dem_depth=dem_depth,
            building_height=building_height,
            fmt=fmt,
            out_dir=out_dir,
            no_cache=no_cache,
            endpoint=endpoint,
            timeout=timeout,
            quiet=quiet,
        )
    except click.ClickException:
        raise
    except Exception as exc:
        raise click.ClickException(str(exc)) from exc

    # ── Success ───────────────────────────────────────────────────────────────
    if not quiet:
        click.echo()
        click.echo(f"  ✓  {out_path}")
        if app:
            click.echo()
            click.echo(f"  Next steps")
            click.echo(f"    sdlos run {app} --reconfigure")
        else:
            click.echo()
            click.echo(f"  Next steps")
            click.echo(f"    sdlos mesh info {out_path}")

    sys.exit(0)


# ── sdlos mesh presets ────────────────────────────────────────────────────────

@cmd_mesh.command("presets")
@click.option(
    "--bbox-only",
    is_flag=True, default=False,
    help="Print only the preset key and raw bbox (N,S,E,W) — useful for scripting.",
)
def cmd_presets(bbox_only: bool) -> None:
    """List all built-in city presets with bounding boxes and area notes.

    Use a preset with:  sdlos mesh generate --preset KEY

    \b
    Examples
    --------
      sdlos mesh presets
      sdlos mesh presets --bbox-only
      sdlos mesh generate --preset rome
      sdlos mesh generate --preset nyc --lod lowpoly --face-count 4000
    """
    if bbox_only:
        for key in _PRESET_ORDER:
            p = CITY_PRESETS[key]
            n, s, e, w = p.bbox
            click.echo(f"{key}\t{n},{s},{e},{w}")
        return

    col_key  = 14
    col_bbox = 40
    col_h    = 6

    click.echo()
    for region_label, keys in _PRESET_REGIONS:
        click.echo(f"  ── {region_label} {'─' * max(0, 54 - len(region_label))}")
        for key in keys:
            p = CITY_PRESETS[key]
            n, s, e, w = p.bbox
            bbox_str = f"{n},{s},{e},{w}"
            click.echo(
                f"    {key:<{col_key}}  {bbox_str:<{col_bbox}}  "
                f"H={p.building_height:<{col_h}.0f}  {p.notes}"
            )
        click.echo()

    click.echo("  Aliases:  adam → amsterdam")
    click.echo()
    click.echo("  Usage:")
    click.echo("    sdlos mesh generate --preset paris")
    click.echo("    sdlos mesh generate --preset chicago --lod lowpoly --face-count 4000")
    click.echo("    sdlos mesh generate --preset tokyo --dem --app flatshader")
    click.echo()


# ── sdlos mesh cache ──────────────────────────────────────────────────────────

@cmd_mesh.group("cache")
def cmd_cache() -> None:
    """Manage the local OSM fetch cache (~/.sdlos/mesh_cache/).

    OSM building data is cached as GeoJSON after the first fetch so
    subsequent generate runs are instant.  Use these subcommands to
    inspect or clear cached data.

    \b
    Examples
    --------
      sdlos mesh cache list          # show all cached entries
      sdlos mesh cache clear         # delete everything
      sdlos mesh cache clear rome    # delete one preset by key or hex id
    """


@cmd_cache.command("list")
def cmd_cache_list() -> None:
    """List all cached OSM fetch files with size, age, and bbox."""
    import json
    import time

    cache_dir = Path.home() / ".sdlos" / "mesh_cache"
    if not cache_dir.exists():
        click.echo("  Cache is empty (directory does not exist yet).")
        return

    files = sorted(cache_dir.glob("*.geojson"))
    if not files:
        click.echo("  Cache is empty.")
        return

    # Build a reverse lookup: cache-key → preset key(s)
    key_to_preset: dict[str, list[str]] = {}
    for preset_key, preset in CITY_PRESETS.items():
        if preset_key == "adam":   # skip alias
            continue
        n, s, e, w = preset.bbox
        raw = f"{n:.6f},{s:.6f},{e:.6f},{w:.6f}"
        h = hashlib.sha256(raw.encode()).hexdigest()[:16]
        key_to_preset.setdefault(h, []).append(preset_key)

    now = time.time()
    total_kb = 0.0

    click.echo()
    click.echo(f"  {'File':<20}  {'Preset':<14}  {'Size':>8}  {'Age':>10}  Bbox snippet")
    click.echo(f"  {'-'*20}  {'-'*14}  {'-'*8}  {'-'*10}  {'-'*24}")

    for f in files:
        stat   = f.stat()
        kb     = stat.st_size / 1024
        total_kb += kb
        age_s  = now - stat.st_mtime
        age    = (
            f"{int(age_s // 3600)}h {int((age_s % 3600) // 60)}m"
            if age_s >= 3600
            else f"{int(age_s // 60)}m {int(age_s % 60)}s"
        )
        hex_id = f.stem

        # Try to read bbox from GeoJSON bounds
        bbox_snip = "—"
        try:
            import geopandas as gpd
            gdf = gpd.read_file(str(f))
            b = gdf.total_bounds  # minx miny maxx maxy
            bbox_snip = f"N{b[3]:.3f} S{b[1]:.3f}"
        except Exception:
            pass

        preset_names = ", ".join(key_to_preset.get(hex_id, ["—"]))
        click.echo(
            f"  {f.name:<20}  {preset_names:<14}  {kb:>7.1f}K  {age:>10}  {bbox_snip}"
        )

    click.echo()
    click.echo(f"  Total: {len(files)} file(s)  {total_kb:.1f} KB  →  {cache_dir}")
    click.echo()


@cmd_cache.command("clear")
@click.argument("target", metavar="[TARGET]", required=False, default=None)
def cmd_cache_clear(target: Optional[str]) -> None:
    """Delete cached OSM fetch files.

    TARGET may be a preset key (e.g. 'rome') or the hex cache id shown
    by `sdlos mesh cache list`.  Omit TARGET to clear everything.

    \b
    Examples
    --------
      sdlos mesh cache clear            # wipe entire cache
      sdlos mesh cache clear rome       # wipe only the 'rome' bbox
      sdlos mesh cache clear a1b2c3d4   # wipe by hex id
    """
    cache_dir = Path.home() / ".sdlos" / "mesh_cache"
    if not cache_dir.exists():
        click.echo("  Cache is already empty.")
        return

    if target is None:
        files = list(cache_dir.glob("*.geojson"))
        if not files:
            click.echo("  Cache is already empty.")
            return
        for f in files:
            f.unlink()
        click.echo(f"  Cleared {len(files)} cached file(s).")
        return

    # Resolve target → hex id
    hex_id: Optional[str] = None

    # 1. Check if target is a known preset key
    preset = CITY_PRESETS.get(target.lower())
    if preset:
        n, s, e, w = preset.bbox
        raw = f"{n:.6f},{s:.6f},{e:.6f},{w:.6f}"
        hex_id = hashlib.sha256(raw.encode()).hexdigest()[:16]

    # 2. Treat target as a literal hex id
    if hex_id is None:
        hex_id = target

    candidate = cache_dir / f"{hex_id}.geojson"
    if candidate.exists():
        candidate.unlink()
        click.echo(f"  Deleted  {candidate.name}")
    else:
        click.echo(
            f"  No cache entry found for '{target}'.\n"
            "  Run `sdlos mesh cache list` to see what is cached."
        )


# ── sdlos mesh info ───────────────────────────────────────────────────────────

@cmd_mesh.command("info")
@click.argument("path", metavar="PATH", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--verbose", "-v",
    is_flag=True, default=False,
    help="Show per-geometry breakdown in addition to the summary.",
)
def cmd_info(path: Path, verbose: bool) -> None:
    """Inspect a generated mesh file and print geometry statistics.

    PATH may be a .gltf, .glb, or .obj file.

    \b
    Examples
    --------
      sdlos mesh info output/mainz.gltf
      sdlos mesh info output/mainz.glb --verbose
    """
    _require_mesh_deps()

    import trimesh

    click.echo(f"\nsdlos mesh info  {path}")
    click.echo()

    try:
        loaded = trimesh.load(str(path), force="scene")
    except Exception as exc:
        raise click.ClickException(f"Failed to load '{path}': {exc}") from exc

    # ── Collect geometry objects ──────────────────────────────────────────────
    if isinstance(loaded, trimesh.Scene):
        geometries: list[trimesh.Trimesh] = [
            g for g in loaded.geometry.values()
            if isinstance(g, trimesh.Trimesh)
        ]
        scene_bounds = loaded.bounds
        scene_extents = loaded.extents
    elif isinstance(loaded, trimesh.Trimesh):
        geometries = [loaded]
        scene_bounds  = loaded.bounds
        scene_extents = loaded.extents
    else:
        raise click.ClickException(
            f"Unrecognised trimesh type: {type(loaded).__name__}"
        )

    total_faces    = sum(len(g.faces)    for g in geometries)
    total_vertices = sum(len(g.vertices) for g in geometries)
    total_geom     = len(geometries)

    file_kb = path.stat().st_size / 1024

    # ── Summary table ─────────────────────────────────────────────────────────
    click.echo(f"  File         {path.name}  ({file_kb:.1f} KB)")
    click.echo(f"  Format       {path.suffix.lstrip('.')}")
    click.echo(f"  Geometries   {total_geom}")
    click.echo(f"  Total faces  {total_faces:,}")
    click.echo(f"  Total verts  {total_vertices:,}")

    if scene_bounds is not None and scene_extents is not None:
        click.echo()
        click.echo(f"  Scene bounds")
        click.echo(f"    min  ({scene_bounds[0][0]:.2f}, {scene_bounds[0][1]:.2f}, {scene_bounds[0][2]:.2f})")
        click.echo(f"    max  ({scene_bounds[1][0]:.2f}, {scene_bounds[1][1]:.2f}, {scene_bounds[1][2]:.2f})")
        click.echo(f"    size ({scene_extents[0]:.2f} × {scene_extents[1]:.2f} × {scene_extents[2]:.2f}) m")

    # ── Per-geometry breakdown (--verbose) ────────────────────────────────────
    if verbose and geometries:
        click.echo()
        click.echo(f"  {'#':<4}  {'Faces':>8}  {'Vertices':>10}  {'Bounds (x y z extent)':}")
        click.echo(f"  {'-'*4}  {'-'*8}  {'-'*10}  {'-'*32}")
        for i, g in enumerate(geometries):
            ext = g.extents
            click.echo(
                f"  {i:<4}  {len(g.faces):>8,}  {len(g.vertices):>10,}  "
                f"{ext[0]:.1f} × {ext[1]:.1f} × {ext[2]:.1f}"
            )

    click.echo()
