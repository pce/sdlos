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
    "mount_etna": CityPreset(
        key             = "mount_etna",
        label           = "Mount Etna — Sicily, Italy",
        bbox            = (37.800, 37.700, 15.100, 14.900),
        building_height = 5.0,
        notes           = "Active stratovolcano on Sicily (3,326 m) — use --volcano --volcano-height 3326 --volcano-radius 20000",
    ),
    "stromboli": CityPreset(
        key             = "stromboli",
        label           = "Stromboli — Aeolian Islands, Italy",
        bbox            = (38.805, 38.785, 15.225, 15.200),
        building_height = 5.0,
        notes           = "The 'Lighthouse of the Mediterranean' (924 m) — use --volcano --volcano-height 924 --volcano-radius 3000",
    ),
    "chania_lefka_ori": CityPreset(
        key             = "chania_lefka_ori",
        label           = "Lefka Ori (White Mountains) — Chania, Crete",
        bbox            = (35.350, 35.250, 24.100, 23.950),
        building_height = 6.0,
        notes           = "Towering limestone peaks near Chania; distinct geometric appearance — use --dem for the karst topography",
    ),
    "teide_volcano": CityPreset(
        key             = "teide_volcano",
        label           = "Mount Teide — Tenerife, Canary Islands",
        bbox            = (28.320, 28.220, -16.580, -16.700),
        building_height = 5.0,
        notes           = "Highest point in Spain (3,715 m) — use --volcano --volcano-height 3715 --volcano-radius 15000",
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
    "mainz": CityPreset(
        key             = "mainz",
        label           = "Mainz — Altstadt (full)",
        bbox            = (50.002, 49.986, 8.287, 8.257),
        building_height = 12.0,
        notes           = "Full Altstadt: Dom, Markt, Kaiserstraße, Holzturm, Kaisertor, Rheinufer (~1.8 × 2.1 km)",
    ),
    "mainz-core": CityPreset(
        key             = "mainz-core",
        label           = "Mainz — Dom / Markt core",
        bbox            = (49.999, 49.993, 8.278, 8.266),
        building_height = 12.0,
        notes           = "Compact core: Dom square, Markt, Rheinufer (~666 × 857 m). Fast fetch.",
    ),
    "munich": CityPreset(
        key             = "munich",
        label           = "Munich — Marienplatz / Altstadt",
        bbox            = (48.140, 48.131, 11.581, 11.566),
        building_height = 16.0,
        notes           = "Marienplatz, Frauenkirche, Kaufingerstraße, Neues Rathaus",
    ),
    "nurnberg": CityPreset(
        key             = "nurnberg",
        label           = "Nürnberg — Altstadt",
        bbox            = (49.460, 49.451, 11.086, 11.071),
        building_height = 12.0,
        notes           = "Hauptmarkt, Kaiserburg, St. Lorenz, Weißgerbergasse",
    ),
    "frankfurt": CityPreset(
        key             = "frankfurt",
        label           = "Frankfurt am Main — Bankenviertel / Römerberg",
        bbox            = (50.115, 50.104, 8.692, 8.674),
        building_height = 40.0,
        notes           = "European banking skyline: Commerzbank Tower, Messeturm, Römerberg old town, Main riverfront — tallest cluster in continental Europe",
    ),
    "freiburg": CityPreset(
        key             = "freiburg",
        label           = "Freiburg im Breisgau — Altstadt / Münsterplatz",
        bbox            = (47.999, 47.988, 7.862, 7.839),
        building_height = 12.0,
        notes           = "Münster (rose window, market), Rathausplatz, Schwabentor, Martinstor, arcaded Bächle-stream streets — Black Forest gateway city, fine late-Gothic Altstadt",
    ),
    "lübeck": CityPreset(
        key             = "lübeck",
        label           = "Lübeck — Holstentor / Altstadt",
        bbox            = (53.871, 53.860, 10.696, 10.677),
        building_height = 14.0,
        notes           = "Holstentor, Marktplatz, seven-spire skyline, Buddenbrookhaus, Petrikirche — UNESCO, best-preserved Hanseatic brick-Gothic city",
    ),
    "regensburg": CityPreset(
        key             = "regensburg",
        label           = "Regensburg — Altstadt / Dom St. Peter",
        bbox            = (49.022, 49.013, 12.102, 12.084),
        building_height = 14.0,
        notes           = "Steinerne Brücke (12th-c. stone bridge), Dom St Peter, Altes Rathaus, Haidplatz — UNESCO, best-preserved medieval city in Germany north of the Alps",
    ),
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
    "paris": CityPreset(
        key             = "paris",
        label           = "Paris — Île de la Cité / Marais",
        bbox            = (48.857, 48.848, 2.358, 2.338),
        building_height = 18.0,
        notes           = "Notre-Dame, Sainte-Chapelle, Place du Châtelet",
    ),
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
    "london": CityPreset(
        key             = "london",
        label           = "London — The City / Square Mile",
        bbox            = (51.516, 51.508, -0.080, -0.098),
        building_height = 30.0,
        notes           = "Bank, St Paul's, Leadenhall, Lloyd's cluster",
    ),
    "athens": CityPreset(
        key             = "athens",
        label           = "Athens — Monastiraki / Plaka",
        bbox            = (37.979, 37.971, 23.731, 23.719),
        building_height = 10.0,
        notes           = "Monastiraki sq, Plaka, Thissio, foot of Acropolis",
    ),
    "vienna": CityPreset(
        key             = "vienna",
        label           = "Vienna — Innere Stadt",
        bbox            = (48.212, 48.203, 16.377, 16.360),
        building_height = 22.0,
        notes           = "Stephansdom, Graben, Hofburg, Burgring",
    ),
    "prague": CityPreset(
        key             = "prague",
        label           = "Prague — Staré Město",
        bbox            = (50.090, 50.082, 14.428, 14.412),
        building_height = 16.0,
        notes           = "Old Town Square, Charles Bridge end, Josefov",
    ),
    "budapest": CityPreset(
        key             = "budapest",
        label           = "Budapest — Belváros / Inner City",
        bbox            = (47.502, 47.493, 19.062, 19.047),
        building_height = 18.0,
        notes           = "Vörösmarty tér, Váci utca, Ferenciek tere",
    ),
    "lisbon": CityPreset(
        key             = "lisbon",
        label           = "Lisbon — Alfama / Baixa",
        bbox            = (38.717, 38.708, -9.131, -9.144),
        building_height = 13.0,
        notes           = "Praça do Comércio, Rossio, Alfama quarter",
    ),
    "madrid": CityPreset(
        key             = "madrid",
        label           = "Madrid — Centro / Gran Vía",
        bbox            = (40.426, 40.408, -3.690, -3.718),
        building_height = 18.0,
        notes           = "Gran Vía full length, Puerta del Sol, Plaza Mayor, La Latina, Huertas — ~2 × 2 km block grid; add --dem for ground plane",
    ),
    "barcelona": CityPreset(
        key             = "barcelona",
        label           = "Barcelona — Barri Gòtic",
        bbox            = (41.388, 41.379, 2.180, 2.165),
        building_height = 20.0,
        notes           = "Gothic Quarter, La Rambla, Born district",
    ),
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
    "kyoto-hiei": CityPreset(
        key             = "kyoto-hiei",
        label           = "Mt. Hiei — Hieizan above Kyoto",
        bbox            = (35.120, 34.990, 135.900, 135.750),
        building_height = 6.0,
        notes           = "Sacred Mt. Hiei (848 m) on the NE skyline of Kyoto, site of Enryaku-ji temple — use --volcano --volcano-height 848 --volcano-radius 6000 for the mountain cone",
    ),
    "cappadocia": CityPreset(
        key             = "cappadocia",
        label           = "Cappadocia — Göreme Fairy Chimneys",
        bbox            = (38.655, 38.633, 34.843, 34.817),
        building_height = 5.0,
        notes           = "Göreme town, fairy chimney rock formations, cave dwellings, Open Air Museum — organic non-rectangular geometry; for the volcanic backdrop use --preset erciyes or --preset mt-hasan with --volcano",
    ),
    "erciyes": CityPreset(
        key             = "erciyes",
        label           = "Mt. Erciyes — Erciyes Dağı, Cappadocia",
        bbox            = (38.630, 38.440, 35.570, 35.320),
        building_height = 5.0,
        notes           = "Dominant stratovolcano (3,916 m) near Kayseri whose Pliocene eruptions deposited the tuff layer that eroded into Cappadocia's fairy chimneys — use --volcano --volcano-height 3916 --volcano-radius 20000",
    ),
    "mt-hasan": CityPreset(
        key             = "mt-hasan",
        label           = "Mt. Hasan — Hassan Dağı, Cappadocia",
        bbox            = (38.220, 38.050, 34.270, 34.060),
        building_height = 5.0,
        notes           = "Twin-peaked stratovolcano (3,268 m) 40 km W of Göreme, visible from the Cappadocian valleys — use --volcano --volcano-height 3268 --volcano-radius 15000",
    ),
    "quito": CityPreset(
        key             = "quito",
        label           = "Quito — Centro Histórico",
        bbox            = (- 0.218, -0.232, -78.507, -78.522),
        building_height = 12.0,
        notes           = "UNESCO colonial centre: Plaza Grande, La Catedral, La Compañía, La Ronda — dense 2–4 storey Spanish-colonial blocks",
    ),
    "austin": CityPreset(
        key             = "austin",
        label           = "Austin TX — Downtown / 6th Street",
        bbox            = (30.271, 30.261, -97.735, -97.750),
        building_height = 28.0,
        notes           = "Congress Ave, 6th Street entertainment district, Republic Square — mix of high-rises and historic 2-storey blocks",
    ),
    "sydney": CityPreset(
        key             = "sydney",
        label           = "Sydney — CBD / Circular Quay",
        bbox            = (-33.857, -33.870, 151.216, 151.200),
        building_height = 30.0,
        notes           = "Opera House precinct, Circular Quay ferry terminal, The Rocks historic district, CBD high-rise core",
    ),
    "toronto": CityPreset(
        key             = "toronto",
        label           = "Toronto — Bay Street Financial District",
        bbox            = (43.654, 43.641, -79.371, -79.390),
        building_height = 40.0,
        notes           = "Bay St towers, Union Station, CN Tower base, St Lawrence Market — tallest Canadian skyline",
    ),
    "vancouver": CityPreset(
        key             = "vancouver",
        label           = "Vancouver — Downtown / Coal Harbour",
        bbox            = (49.291, 49.279, -123.101, -123.124),
        building_height = 30.0,
        notes           = "Canada Place, Gastown, Granville Street, Robson Square — 2010 Winter Olympics host",
    ),
    "montreal": CityPreset(
        key             = "montreal",
        label           = "Montreal — Vieux-Montréal",
        bbox            = (45.511, 45.499, -73.548, -73.563),
        building_height = 16.0,
        notes           = "Notre-Dame Basilica, Place d'Armes, Old Port, Rue Saint-Paul — 1976 Summer Olympics host",
    ),
    "calgary": CityPreset(
        key             = "calgary",
        label           = "Calgary — Downtown Core",
        bbox            = (51.053, 51.041, -114.055, -114.079),
        building_height = 35.0,
        notes           = "Stephen Avenue Walk, Bow Tower, Eau Claire, energy-sector high-rises — 1988 Winter Olympics host",
    ),
    "oslo": CityPreset(
        key             = "oslo",
        label           = "Oslo — Sentrum / Aker Brygge",
        bbox            = (59.919, 59.906, 10.753, 10.721),
        building_height = 18.0,
        notes           = "Rådhusplassen (City Hall), Aker Brygge waterfront, National Theatre, Karl Johans gate — 1952 Winter Olympics host",
    ),
    "copenhagen": CityPreset(
        key             = "copenhagen",
        label           = "Copenhagen — Nyhavn / Indre By",
        bbox            = (55.684, 55.672, 12.592, 12.561),
        building_height = 20.0,
        notes           = "Nyhavn coloured gabled houses, Strøget pedestrian street, Christiansborg Palace, Rådhuspladsen, Tivoli Gardens — Danish capital, compact 17th–19th c. streetscape",
    ),
    "stockholm": CityPreset(
        key             = "stockholm",
        label           = "Stockholm — Gamla Stan / Royal Palace",
        bbox            = (59.334, 59.320, 18.086, 18.057),
        building_height = 14.0,
        notes           = "Gamla Stan medieval island, Royal Palace, Storkyrkan, Riddarholmen, Stortorget — best-preserved medieval city core in Northern Europe",
    ),
    "malmo": CityPreset(
        key             = "malmo",
        label           = "Malmö — Stortorget / Gamla Staden",
        bbox            = (55.609, 55.598, 13.016, 12.991),
        building_height = 11.0,
        notes           = "Stortorget (Sweden's oldest surviving square), Lilla Torg, St Peter's Church, Gamla Väster — historic Danish-era city (ceded 1658), Öresund Bridge approach",
    ),
    "moscow": CityPreset(
        key             = "moscow",
        label           = "Moscow — Red Square / Kitai-gorod",
        bbox            = (55.760, 55.748, 37.638, 37.607),
        building_height = 16.0,
        notes           = "Red Square, St Basil's Cathedral, GUM, Zaryadye Park, Kremlin walls — 1980 Summer Olympics host",
    ),
    "seoul": CityPreset(
        key             = "seoul",
        label           = "Seoul — Myeongdong / City Hall",
        bbox            = (37.572, 37.557, 126.993, 126.971),
        building_height = 28.0,
        notes           = "Seoul City Hall, Myeongdong shopping district, Deoksugung Palace, Namdaemun — 1988 Summer Olympics host",
    ),
    "beijing": CityPreset(
        key             = "beijing",
        label           = "Beijing — Wangfujing / Dongcheng",
        bbox            = (39.922, 39.907, 116.421, 116.399),
        building_height = 30.0,
        notes           = "Wangfujing commercial street, Oriental Plaza, Dongdan, Chongwenmen — 2008 Summer / 2022 Winter Olympics host",
    ),
    "mexico-city": CityPreset(
        key             = "mexico-city",
        label           = "Mexico City — Centro Histórico",
        bbox            = (19.441, 19.427, -99.124, -99.143),
        building_height = 14.0,
        notes           = "Zócalo, Catedral Metropolitana, Templo Mayor, Palacio Nacional — dense colonial grid, 1968 Summer Olympics host",
    ),
    "rio": CityPreset(
        key             = "rio",
        label           = "Rio de Janeiro — Centro / Cinelândia",
        bbox            = (-22.897, -22.914, -43.162, -43.185),
        building_height = 20.0,
        notes           = "Arcos da Lapa, Teatro Municipal, Cinelândia, Candelária church, Avenida Rio Branco — 2016 Summer Olympics host",
    ),
    "pripyat": CityPreset(
        key             = "pripyat",
        label           = "Pripyat — Chernobyl Exclusion Zone",
        bbox            = (51.414, 51.400, 30.063, 30.046),
        building_height = 9.0,
        notes           = "Abandoned Soviet city evacuated April 1986: Palace of Culture, Ferris wheel, apartment blocks, hospital, school — all mapped in OSM",
    ),
    "dubai": CityPreset(
        key             = "dubai",
        label           = "Dubai — Downtown / Burj Khalifa",
        bbox            = (25.202, 25.190, 55.285, 55.267),
        building_height = 80.0,
        notes           = "Burj Khalifa, Dubai Mall, DIFC towers, Sheikh Zayed Road — world's densest supertall cluster",
    ),
    "casablanca": CityPreset(
        key             = "casablanca",
        label           = "Casablanca — Place Mohammed V / Hassan II",
        bbox            = (33.602, 33.590, -7.607, -7.634),
        building_height = 14.0,
        notes           = "Place Mohammed V, Hassan II Mosque vicinity, Boulevard Mohammed V — Art Deco and Moorish downtown grid",
    ),
    "marrakesh": CityPreset(
        key             = "marrakesh",
        label           = "Marrakesh — Medina / Djemaa el-Fna",
        bbox            = (31.632, 31.618, -7.979, -7.999),
        building_height = 8.0,
        notes           = "Djemaa el-Fna square, Koutoubia Mosque, Medina souks, Ben Youssef Madrasa — UNESCO World Heritage medina, dense organic street fabric",
    ),
    "mumbai": CityPreset(
        key             = "mumbai",
        label           = "Mumbai — Fort / Nariman Point",
        bbox            = (18.946, 18.920, 72.843, 72.818),
        building_height = 25.0,
        notes           = "Nariman Point, Fort district, Chhatrapati Shivaji Terminus, Gateway of India — financial capital, mix of Victorian Gothic and Art Deco high-rises",
    ),
    "delhi": CityPreset(
        key             = "delhi",
        label           = "New Delhi — Connaught Place / Rajpath",
        bbox            = (28.638, 28.622, 77.228, 77.207),
        building_height = 15.0,
        notes           = "Connaught Place, India Gate precinct, Rajpath — Lutyens' colonial grid with strict heritage height controls",
    ),
    "jaipur": CityPreset(
        key             = "jaipur",
        label           = "Jaipur — Old City / Pink City",
        bbox            = (26.934, 26.919, 75.836, 75.816),
        building_height = 10.0,
        notes           = "Hawa Mahal, City Palace, Jantar Mantar, bazaar grid — UNESCO-listed Pink City, dense 2–4 storey pink sandstone fabric",
    ),
    "kolkata": CityPreset(
        key             = "kolkata",
        label           = "Kolkata — BBD Bagh / Dalhousie",
        bbox            = (22.580, 22.566, 88.374, 88.352),
        building_height = 18.0,
        notes           = "BBD Bagh, Writers' Building, Howrah Bridge approach — former British colonial capital, Victorian warehouse district",
    ),
    "jakarta": CityPreset(
        key             = "jakarta",
        label           = "Jakarta — Kota Tua (Old Batavia)",
        bbox            = (-6.128, -6.143, 106.821, 106.806),
        building_height = 12.0,
        notes           = "Fatahillah Square, Dutch colonial VOC warehouses, Kali Besar canal, Bank Museum — 17th-century trading post, dense colonial grid",
    ),
    "bangkok": CityPreset(
        key             = "bangkok",
        label           = "Bangkok — Rattanakosin Island / Grand Palace",
        bbox            = (13.758, 13.742, 100.503, 100.484),
        building_height = 12.0,
        notes           = "Grand Palace, Wat Pho, Sanam Luang, Khao San Road — historic royal island, mix of temple complexes, shophouses and colonial administrative buildings (Ong-Bak country)",
    ),
    "istanbul": CityPreset(
        key             = "istanbul",
        label           = "Istanbul — Ortaköy / Bosphorus Bridge",
        bbox            = (41.055, 41.034, 29.054, 29.012),
        building_height = 20.0,
        notes           = "Ortaköy Mosque at the foot of the 15 Temmuz Şehitler Köprüsü suspension bridge, Beşiktaş waterfront, Beylerbeyi — where Europe meets Asia; wide bbox captures the full ~1.5 km bridge span",
    ),
    "kathmandu": CityPreset(
        key             = "kathmandu",
        label           = "Kathmandu — Durbar Square / Thamel",
        bbox            = (27.718, 27.700, 85.320, 85.298),
        building_height = 9.0,
        notes           = "Kathmandu Durbar Square, Thamel district, Asan bazaar — dense Newari brick architecture, UNESCO Kathmandu Valley World Heritage site",
    ),
    "tirana": CityPreset(
        key             = "tirana",
        label           = "Tirana — Skanderbeg Square",
        bbox            = (41.336, 41.323, 19.829, 19.808),
        building_height = 18.0,
        notes           = "Skanderbeg Square, Et'hem Bey Mosque, National History Museum — mix of Ottoman, Italian Fascist-era, communist and post-1990 colourful facades",
    ),
    "berat": CityPreset(
        key             = "berat",
        label           = "Berat — Mangalem / City of a Thousand Windows",
        bbox            = (40.712, 40.700, 19.955, 19.937),
        building_height = 10.0,
        notes           = "Mangalem and Gorica Ottoman quarters, Byzantine Kalaja castle above — UNESCO 'museum city', whitewashed houses with rows of large windows reflected in the Osum river",
    ),
    "gjirokaster": CityPreset(
        key             = "gjirokaster",
        label           = "Gjirokastër — Old Town / Castle",
        bbox            = (40.081, 40.068, 20.149, 20.131),
        building_height = 8.0,
        notes           = "Ottoman stone tower-houses (kula), Old Bazaar, Gjirokastër Castle — UNESCO 'city of stone', distinctive grey-schist roofed vernacular architecture",
    ),
    "florence": CityPreset(
        key             = "florence",
        label           = "Florence — Historic Centre",
        bbox            = (43.776, 43.764, 11.263, 11.242),
        building_height = 18.0,
        notes           = "Duomo (Brunelleschi dome), Palazzo Vecchio, Uffizi, Ponte Vecchio, Oltrarno — UNESCO Renaissance historic centre",
    ),
    "venice": CityPreset(
        key             = "venice",
        label           = "Venice — San Marco / Rialto",
        bbox            = (45.441, 45.428, 12.350, 12.325),
        building_height = 14.0,
        notes           = "Piazza San Marco, Rialto Bridge, Grand Canal palazzi — UNESCO lagoon city, Gothic and Byzantine facades rising directly from the water",
    ),
    "dubrovnik": CityPreset(
        key             = "dubrovnik",
        label           = "Dubrovnik — Old City",
        bbox            = (42.645, 42.636, 18.117, 18.102),
        building_height = 10.0,
        notes           = "City walls, Stradun, Sponza Palace, Franciscan Monastery — UNESCO medieval Dalmatian city, compact limestone grid inside intact 13th-century walls",
    ),
    "kotor": CityPreset(
        key             = "kotor",
        label           = "Kotor — Walled Old Town",
        bbox            = (42.431, 42.420, 18.781, 18.764),
        building_height = 9.0,
        notes           = "Venetian-era walled old town, St Tryphon Cathedral, Sea Gate — UNESCO, most intact medieval fortified city on the Adriatic",
    ),
    "bruges": CityPreset(
        key             = "bruges",
        label           = "Bruges — Markt / Historic Centre",
        bbox            = (51.213, 51.203, 3.232, 3.215),
        building_height = 14.0,
        notes           = "Markt, Belfry, Burg Square, Begijnhof, canal network — UNESCO, best-preserved medieval Flemish city",
    ),
    "bern": CityPreset(
        key             = "bern",
        label           = "Bern — Altstadt / Zytglogge",
        bbox            = (46.952, 46.942, 7.460, 7.437),
        building_height = 14.0,
        notes           = "Zytglogge clocktower, Kramgasse arcades, Bärengraben, Münster, Bundeshaus — UNESCO, Swiss capital, 6 km of continuous sandstone arcade streets",
    ),
    "zurich": CityPreset(
        key             = "zurich",
        label           = "Zürich — Altstadt / Grossmünster",
        bbox            = (47.379, 47.369, 8.548, 8.528),
        building_height = 18.0,
        notes           = "Grossmünster, Fraumünster (Chagall windows), Limmatquai, Bahnhofstrasse, Lindenhügel — Switzerland's largest city, intact dual-bank Altstadt",
    ),
    "lucerne": CityPreset(
        key             = "lucerne",
        label           = "Lucerne — Kapellbrücke / Altstadt",
        bbox            = (47.058, 47.046, 8.319, 8.296),
        building_height = 14.0,
        notes           = "Kapellbrücke (oldest roofed wooden bridge in Europe, 1333), Wasserturm, Spreuerbrücke, Altstadt muralled facades — compact Alpine lakeside medieval city",
    ),
    "luxembourg": CityPreset(
        key             = "luxembourg",
        label           = "Luxembourg City — Ville Haute / Bock Casemates",
        bbox            = (49.614, 49.602, 6.135, 6.117),
        building_height = 16.0,
        notes           = "Place d'Armes, Bock Casemates, Adolphe Bridge, Cathedral Notre-Dame — UNESCO, dramatically sited capital atop sandstone cliffs above two river valleys",
    ),
    "valletta": CityPreset(
        key             = "valletta",
        label           = "Valletta — Republic Street / Grand Harbour",
        bbox            = (35.901, 35.893, 14.519, 14.504),
        building_height = 14.0,
        notes           = "St John's Co-Cathedral, Grand Harbour, Upper Barrakka Gardens, Republic Street — UNESCO, Europe's smallest capital, Baroque Order of Malta fortified city",
    ),
    "tallinn": CityPreset(
        key             = "tallinn",
        label           = "Tallinn — Old Town (Vanalinn)",
        bbox            = (59.444, 59.431, 24.757, 24.731),
        building_height = 12.0,
        notes           = "Toompea upper town, Town Hall Square, St Olaf's Church, medieval towers — UNESCO, best-preserved Hanseatic medieval city in Northern Europe",
    ),
    "riga": CityPreset(
        key             = "riga",
        label           = "Riga — Old Town / Art Nouveau Quarter",
        bbox            = (56.954, 56.942, 24.121, 24.098),
        building_height = 16.0,
        notes           = "Blackheads House, Town Hall Square, Dom Cathedral, Alberta iela Art Nouveau facades — UNESCO, world's finest Art Nouveau streetscapes alongside Hanseatic old town",
    ),
    "vilnius": CityPreset(
        key             = "vilnius",
        label           = "Vilnius — Old Town / Gediminas Tower",
        bbox            = (54.690, 54.677, 25.295, 25.272),
        building_height = 14.0,
        notes           = "Gediminas Tower, Cathedral Square, Užupis bohemian quarter, Pilies Street — UNESCO, largest surviving Baroque old town in Northern Europe",
    ),
    "krakow": CityPreset(
        key             = "krakow",
        label           = "Kraków — Rynek Główny / Wawel",
        bbox            = (50.066, 50.050, 19.950, 19.924),
        building_height = 16.0,
        notes           = "Rynek Główny (Europe's largest medieval market square), Wawel Royal Castle, St Mary's Basilica, Kazimierz Jewish Quarter — UNESCO",
    ),
    "salzburg": CityPreset(
        key             = "salzburg",
        label           = "Salzburg — Altstadt / Getreidegasse",
        bbox            = (47.804, 47.792, 13.057, 13.034),
        building_height = 18.0,
        notes           = "Getreidegasse, Salzburg Cathedral, Residenzplatz, Hohensalzburg Fortress approach — UNESCO Baroque Alpine city, Mozart birthplace",
    ),
    "porto": CityPreset(
        key             = "porto",
        label           = "Porto — Ribeira / Dom Luís Bridge",
        bbox            = (41.148, 41.135, -8.601, -8.624),
        building_height = 14.0,
        notes           = "Ribeira waterfront, Dom Luís I Bridge, Palácio da Bolsa — UNESCO, azulejo-tiled Baroque facades, port wine lodges across the Douro",
    ),
    "cairo": CityPreset(
        key             = "cairo",
        label           = "Cairo — Islamic Cairo / Khan el-Khalili",
        bbox            = (30.056, 30.038, 31.274, 31.253),
        building_height = 12.0,
        notes           = "Khan el-Khalili bazaar, Al-Azhar Mosque, Fatimid al-Qahira medieval core — UNESCO Historic Cairo, densest surviving medieval Islamic city fabric",
    ),
    "fez": CityPreset(
        key             = "fez",
        label           = "Fez — Fès el-Bali (Old Medina)",
        bbox            = (34.071, 34.054, -4.963, -4.989),
        building_height = 8.0,
        notes           = "World's largest car-free urban zone, Al-Qarawiyyin (oldest university), Chouara tanneries — UNESCO, most complex medina on Earth",
    ),
    "jerusalem": CityPreset(
        key             = "jerusalem",
        label           = "Jerusalem — Old City",
        bbox            = (31.785, 31.769, 35.243, 35.224),
        building_height = 10.0,
        notes           = "Jewish Quarter, Church of the Holy Sepulchre, Dome of the Rock, Western Wall — UNESCO, holy to Judaism, Christianity and Islam",
    ),
    "hoi-an": CityPreset(
        key             = "hoi-an",
        label           = "Hội An — Ancient Town",
        bbox            = (15.882, 15.874, 108.345, 108.328),
        building_height = 7.0,
        notes           = "Japanese Covered Bridge, Chinese Assembly Halls, merchant house facades — UNESCO, outstanding 15th–19th c. Southeast Asian trading architecture",
    ),
    "pingyao": CityPreset(
        key             = "pingyao",
        label           = "Pingyao — Ancient Walled City",
        bbox            = (37.203, 37.188, 112.186, 112.163),
        building_height = 8.0,
        notes           = "Ming-dynasty city walls, Rishengchang Exchange House (world's first bank), courtyard houses — UNESCO, best-preserved ancient Han Chinese city",
    ),
    "havana": CityPreset(
        key             = "havana",
        label           = "Havana — Habana Vieja",
        bbox            = (23.145, 23.129, -82.339, -82.361),
        building_height = 14.0,
        notes           = "Plaza Vieja, Plaza de la Catedral, Obispo Street — UNESCO, crumbling Spanish Baroque and Art Deco streetscapes",
    ),
    "cusco": CityPreset(
        key             = "cusco",
        label           = "Cusco — Historic Centre",
        bbox            = (-13.508, -13.524, -71.967, -71.987),
        building_height = 8.0,
        notes           = "Plaza de Armas, Qorikancha Inca temple, San Blas barrio — UNESCO, Inca precision-cut stonework foundations beneath Spanish colonial churches",
    ),
    "cartagena": CityPreset(
        key             = "cartagena",
        label           = "Cartagena — Old Walled City",
        bbox            = (10.431, 10.415, -75.535, -75.558),
        building_height = 10.0,
        notes           = "Las Murallas city walls, Plaza de Bolívar, colourful colonial facades — UNESCO, finest Spanish colonial military architecture in the Americas",
    ),

    "zanzibar": CityPreset(
        key             = "zanzibar",
        label           = "Zanzibar — Stone Town",
        bbox            = (-6.155, -6.172, 39.200, 39.181),
        building_height = 8.0,
        notes           = "House of Wonders, Old Fort, Arab-Omani merchant houses, carved wooden doors — UNESCO, Swahili-Arab-Persian-Indian architectural fusion on the Indian Ocean",
    ),
    # ── New Zealand ───────────────────────────────────────────────────────────
    "nz-taranaki": CityPreset(
        key             = "nz-taranaki",
        label           = "Mt. Taranaki — Egmont Volcano, New Zealand",
        bbox            = (-39.080, -39.510, 174.310, 173.820),
        building_height = 5.0,
        notes           = "Near-perfect symmetrical stratovolcano (2,518 m), 'Fuji of the Southern Hemisphere' — use --volcano --volcano-height 2518 --volcano-radius 25000 for the cone mesh",
    ),
    # ── Italy (Campania) ──────────────────────────────────────────────────────
    "naples": CityPreset(
        key             = "naples",
        label           = "Naples — Centro Storico / Spaccanapoli",
        bbox            = (40.852, 40.836, 14.268, 14.247),
        building_height = 14.0,
        notes           = "Piazza del Plebiscito, Royal Palace, San Carlo, Spaccanapoli axis, Castel Nuovo — UNESCO, largest historic centre in Europe, dense 4–6 storey fabric",
    ),
    # ── Terrain / Volcano presets ─────────────────────────────────────────────
    # Location bboxes for volcano scenes.  Combine with --volcano to generate a
    # parametric cone instead of fetching OSM buildings.
    "tokyo-fujihama": CityPreset(
        key             = "tokyo-fujihama",
        label           = "Fujisan — Mt. Fuji / Kawaguchiko",
        bbox            = (35.450, 35.270, 138.840, 138.620),
        building_height = 7.0,
        notes           = "Mt. Fuji stratovolcano (3,776 m) — use --volcano for the mountain cone; buildings-only fetch returns sparse 5th-station / Kawaguchiko lakeshore facilities",
    ),
    "mexico-popo": CityPreset(
        key             = "mexico-popo",
        label           = "Popocatépetl — Active Stratovolcano",
        bbox            = (19.100, 18.945, -98.540, -98.705),
        building_height = 5.0,
        notes           = "Active stratovolcano 5,426 m asl, 70 km SE of Mexico City — use --volcano for the cone mesh; sparse OSM coverage inside the exclusion zone",
    ),
    "quito-pichincha": CityPreset(
        key             = "quito-pichincha",
        label           = "Pichincha — Twin-Peaked Stratovolcano above Quito",
        bbox            = (-0.080, -0.265, -78.488, -78.700),
        building_height = 5.0,
        notes           = "Rucu (4,696 m) and Guagua Pichincha (4,784 m) directly west of Quito city centre — use --volcano for the cone mesh",
    ),
    # ── Ukraine ───────────────────────────────────────────────────────────────
    "kyiv": CityPreset(
        key             = "kyiv",
        label           = "Kyiv — Maidan Nezalezhnosti / Khreshchatyk",
        bbox            = (50.457, 50.444, 30.527, 30.509),
        building_height = 18.0,
        notes           = "Independence Square (Maidan), Khreshchatyk boulevard, Bessarabska market — Stalinist-era civic buildings alongside 19th-c. eclectic facades; the symbolic heart of modern Ukraine",
    ),
    "kyiv-lavra": CityPreset(
        key             = "kyiv-lavra",
        label           = "Kyiv — Pechersk Lavra / Cave Monastery",
        bbox            = (50.439, 50.427, 30.562, 30.546),
        building_height = 14.0,
        notes           = "Kyivo-Pecherska Lavra (UNESCO): Dormition Cathedral, Great Lavra Bell Tower (96 m), Baroque refectory church, underground cave system — hilltop monastery complex above the Dnipro river",
    ),
    "lviv": CityPreset(
        key             = "lviv",
        label           = "Lviv — Rynok Square / Old Town",
        bbox            = (49.845, 49.834, 24.038, 24.019),
        building_height = 16.0,
        notes           = "Rynok Square, Latin Cathedral, Dominican Cathedral, Black Stone House, Dormition Church — UNESCO, finest Renaissance-to-Baroque streetscape in Eastern Europe; cross-roads of Polish, Austrian and Ukrainian culture",
    ),
    "odesa": CityPreset(
        key             = "odesa",
        label           = "Odesa — Potemkin Steps / Primorsky Boulevard",
        bbox            = (46.490, 46.480, 30.739, 30.724),
        building_height = 16.0,
        notes           = "Odesa Opera House, Potemkin Steps (192 granite treads), Primorsky Boulevard, Vorontsov Palace — Neoclassical Black Sea port-city grid; Eisenstein filming location",
    ),
    "kharkiv": CityPreset(
        key             = "kharkiv",
        label           = "Kharkiv — Freedom Square / Derzhprom",
        bbox            = (49.999, 49.991, 36.236, 36.219),
        building_height = 20.0,
        notes           = "Derzhprom (1928 — world's first Soviet constructivist skyscraper complex), Freedom Square (one of Europe's largest at 12 ha), Gosprom, Assumption Cathedral — monumental avant-garde Soviet ensemble",
    ),
    "chernivtsi": CityPreset(
        key             = "chernivtsi",
        label           = "Chernivtsi — University / Theatre Square",
        bbox            = (48.301, 48.290, 25.943, 25.922),
        building_height = 12.0,
        notes           = "Chernivtsi University (UNESCO — Residence of Bukovinian Metropolitans, 1882): Moorish-Gothic-Romanesque complex by Czech architect Hlavka, with Byzantine domed chapel and ornate brickwork — Austro-Hungarian Bukovina gem",
    ),
    "kamianets": CityPreset(
        key             = "kamianets",
        label           = "Kamianets-Podilskyi — Fortress / Smotrych Canyon",
        bbox            = (48.682, 48.668, 26.581, 26.558),
        building_height = 12.0,
        notes           = "Medieval fortress on a rock island almost entirely encircled by a canyon bend of the Smotrych river — one of Europe's most dramatic castle settings; combine with --dem for the canyon topography",
    ),
    "hoverla": CityPreset(
        key             = "hoverla",
        label           = "Hoverla — Highest Peak of Ukraine / Chornohora Massif",
        bbox            = (48.220, 48.060, 24.620, 24.380),
        building_height = 5.0,
        notes           = "Hoverla (2,061 m) — highest summit of the Ukrainian Carpathians; Chornohora massif, Carpathian Biosphere Reserve. Use --volcano --volcano-height 2061 --volcano-radius 8000 for the mountain cone",
    ),
    # ── Poland ────────────────────────────────────────────────────────────────
    "warsaw": CityPreset(
        key             = "warsaw",
        label           = "Warsaw — Stare Miasto / Royal Castle",
        bbox            = (52.254, 52.244, 21.016, 20.999),
        building_height = 14.0,
        notes           = "Royal Castle, Old Town Market Square, St John's Cathedral — UNESCO; entire historic centre meticulously rebuilt from rubble after 90% destruction in 1944",
    ),
    "gdansk": CityPreset(
        key             = "gdansk",
        label           = "Gdańsk — Długi Targ / Motława Quay",
        bbox            = (54.352, 54.343, 18.655, 18.641),
        building_height = 16.0,
        notes           = "Długi Targ, Green Gate, Golden Gate, St Mary's (largest brick Gothic church in the world), Motława canal — Hanseatic merchant city; Solidarność birthplace",
    ),
    "wroclaw": CityPreset(
        key             = "wroclaw",
        label           = "Wrocław — Rynek / Cathedral Island",
        bbox            = (51.116, 51.104, 17.040, 17.018),
        building_height = 18.0,
        notes           = "Rynek market square, Gothic Town Hall, Cathedral Island (Ostrów Tumski), Wrocław University — Silesian city of 12 islands; architectural layers from Romanesque to Modernist",
    ),
    # ── Bosnia & Herzegovina ──────────────────────────────────────────────────
    "sarajevo": CityPreset(
        key             = "sarajevo",
        label           = "Sarajevo — Baščaršija / Latin Bridge",
        bbox            = (43.861, 43.851, 18.435, 18.415),
        building_height = 10.0,
        notes           = "Baščaršija Ottoman bazaar, Gazi Husrev-beg Mosque, Latin Bridge (Franz Ferdinand assassination site, 1914), Vijećnica City Hall — Ottoman, Austro-Hungarian and Yugoslav layers compressed into a mountain valley",
    ),
    "mostar": CityPreset(
        key             = "mostar",
        label           = "Mostar — Stari Most / Kujundžiluk Bazaar",
        bbox            = (43.340, 43.330, 17.816, 17.799),
        building_height = 8.0,
        notes           = "Stari Most (Old Bridge, 1557, UNESCO), Kujundžiluk Ottoman bazaar, Koski Mehmed-Pasha Mosque — single-arch Ottoman bridge over the emerald Neretva gorge; dramatic canyon setting ideal for --dem terrain",
    ),
    # ── Serbia ────────────────────────────────────────────────────────────────
    "belgrade": CityPreset(
        key             = "belgrade",
        label           = "Belgrade — Kalemegdan / Knez Mihajlova",
        bbox            = (44.825, 44.815, 20.461, 20.444),
        building_height = 18.0,
        notes           = "Kalemegdan Fortress at the Sava–Danube confluence, Knez Mihajlova pedestrian street, National Theatre — Roman, Byzantine, Ottoman and Habsburg strata at a strategic river junction",
    ),
    # ── Romania ───────────────────────────────────────────────────────────────
    "bucharest": CityPreset(
        key             = "bucharest",
        label           = "Bucharest — Palace of Parliament / Bulevardul Unirii",
        bbox            = (44.432, 44.422, 26.098, 26.081),
        building_height = 20.0,
        notes           = "Palace of the Parliament (world's 2nd largest building by volume, 12 storeys, 1984–1997), Bulevardul Unirii — Ceaușescu's megalomaniac civic axis replacing a demolished Ottoman-era neighbourhood",
    ),
    "brasov": CityPreset(
        key             = "brasov",
        label           = "Brașov — Council Square / Black Church",
        bbox            = (45.645, 45.635, 25.597, 25.577),
        building_height = 12.0,
        notes           = "Piața Sfatului, Black Church (largest Gothic church in Romania), medieval city walls, Saxon towers — Transylvanian fortified Saxon city ringed by the Carpathian Postăvar massif; combine with --dem for the mountain cirque",
    ),
    "sighisoara": CityPreset(
        key             = "sighisoara",
        label           = "Sighișoara — Citadel / Clock Tower",
        bbox            = (46.222, 46.214, 24.799, 24.781),
        building_height = 10.0,
        notes           = "Cetățuie citadel, Clock Tower (14th c.), coloured merchant houses, covered wooden stairway to hill church — UNESCO; only continuously inhabited medieval citadel in Europe; birthplace of Vlad the Impaler",
    ),
    # ── Bulgaria ──────────────────────────────────────────────────────────────
    "sofia": CityPreset(
        key             = "sofia",
        label           = "Sofia — Alexander Nevsky / Vitosha Boulevard",
        bbox            = (42.701, 42.691, 23.336, 23.318),
        building_height = 16.0,
        notes           = "Alexander Nevsky Cathedral (largest Orthodox church on the Balkans), Vitosha Boulevard, National Palace of Culture, Ivan Vazov Theatre — Sofia above the ancient Thracian Serdica, Mt. Vitosha as backdrop",
    ),
    "plovdiv": CityPreset(
        key             = "plovdiv",
        label           = "Plovdiv — Old Town / Kapana",
        bbox            = (42.153, 42.143, 24.754, 24.736),
        building_height = 10.0,
        notes           = "National Revival merchant houses on the Three Hills, Dzhumaya Mosque, Roman amphitheatre (intact), Kapana creative district — European Capital of Culture 2019; one of Europe's oldest continuously inhabited cities (8,000 years)",
    ),
    # ── North Macedonia ───────────────────────────────────────────────────────
    "ohrid": CityPreset(
        key             = "ohrid",
        label           = "Ohrid — Old Town / Lake Ohrid",
        bbox            = (41.121, 41.111, 20.805, 20.790),
        building_height = 9.0,
        notes           = "Plaošnik church complex, Samuel's Fortress, St John at Kaneo (cliff-perched Byzantine church above the lake) — UNESCO natural and cultural WHS; Lake Ohrid is one of Europe's oldest and deepest lakes",
    ),
    # ── Belarus ───────────────────────────────────────────────────────────────
    "minsk": CityPreset(
        key             = "minsk",
        label           = "Minsk — Independence Avenue / Stalin Baroque",
        bbox            = (53.908, 53.898, 27.563, 27.545),
        building_height = 22.0,
        notes           = "Independence Avenue (one of Europe's most intact Stalinist ensembles, 15 km), Government House, Red Church, Gates of Minsk triumphal towers — entirely rebuilt post-WWII as a Soviet showcase capital; extraordinary architectural time-capsule",
    ),
    # ── Alps / Dolomites ──────────────────────────────────────────────────────
    "dolomiti-tre-cime": CityPreset(
        key             = "dolomiti-tre-cime",
        label           = "Tre Cime di Lavaredo — Dolomites, Italy",
        bbox            = (46.640, 46.590, 12.340, 12.260),
        building_height = 5.0,
        notes           = "Tre Cime (2,999 m) — the iconic triple limestone pinnacle; UNESCO Dolomites WHS. OSM gives the Auronzo hut and rifugi. Use --dem for the scree-and-cliff terrain or --volcano for a simplified cone backdrop",
    ),
    "dolomiti-marmolada": CityPreset(
        key             = "dolomiti-marmolada",
        label           = "Marmolada — Queen of the Dolomites",
        bbox            = (46.470, 46.400, 11.900, 11.810),
        building_height = 5.0,
        notes           = "Marmolada (3,343 m) — highest peak of the Dolomites; shrinking glacier on the north face, WWI Città di Ghiaccio ice tunnels. Use --dem for the glacier/ridge topography",
    ),
    "dolomiti-seceda": CityPreset(
        key             = "dolomiti-seceda",
        label           = "Seceda — Val Gardena / Odle Group, Dolomites",
        bbox            = (46.630, 46.530, 11.770, 11.650),
        building_height = 6.0,
        notes           = "Seceda plateau (2,518 m) above Ortisei; Odle/Geisler pinnacles behind, Val Gardena Ladin villages below — photogenic layered ridge with cable-car infrastructure well-mapped in OSM",
    ),
    "matterhorn": CityPreset(
        key             = "matterhorn",
        label           = "Matterhorn — Zermatt, Swiss/Italian Alps",
        bbox            = (46.020, 45.930, 7.720, 7.580),
        building_height = 8.0,
        notes           = "Matterhorn (4,478 m) — near-perfect pyramid; Zermatt car-free resort village at its foot. OSM: dense chalet and hotel fabric. Use --volcano --volcano-height 4478 --volcano-radius 6000 for the iconic pyramid cone",
    ),
    "mont-blanc": CityPreset(
        key             = "mont-blanc",
        label           = "Mont Blanc — Chamonix / Highest Alpine Summit",
        bbox            = (45.950, 45.760, 7.010, 6.750),
        building_height = 8.0,
        notes           = "Mont Blanc (4,808 m) — highest peak in the Alps and Western Europe; Chamonix resort town below, Aiguilles Rouges opposite, Mer de Glace glacier. Use --dem for glacier/moraine topography; OSM gives Chamonix town fabric",
    ),
    "eiger": CityPreset(
        key             = "eiger",
        label           = "Eiger — Grindelwald / Bernese Oberland",
        bbox            = (46.650, 46.510, 8.080, 7.930),
        building_height = 8.0,
        notes           = "Eiger (3,967 m) north face — 1,800 m of vertical limestone; Grindelwald village at the base, Jungfrau railway station at 3,454 m. OSM has the village and rack-railway infrastructure",
    ),
    "zugspitze": CityPreset(
        key             = "zugspitze",
        label           = "Zugspitze — Garmisch-Partenkirchen, Highest German Peak",
        bbox            = (47.460, 47.380, 11.030, 10.920),
        building_height = 8.0,
        notes           = "Zugspitze (2,962 m) — highest peak in Germany; Garmisch-Partenkirchen resort and 1936 Winter Olympics venue below. Use --dem for the Zugspitzplatt glacial plateau; OSM gives the town and cable-car termini",
    ),
    "grossglockner": CityPreset(
        key             = "grossglockner",
        label           = "Grossglockner — Highest Peak in Austria",
        bbox            = (47.120, 47.020, 12.740, 12.620),
        building_height = 5.0,
        notes           = "Grossglockner (3,798 m) — Austria's highest summit; Pasterze glacier (longest in the Eastern Alps), Franz-Josefs-Höhe viewpoint, Hochalpenstraße alpine road. Use --dem for the glacier cirque topography",
    ),
    # ── Patagonia ─────────────────────────────────────────────────────────────
    "torres-del-paine": CityPreset(
        key             = "torres-del-paine",
        label           = "Torres del Paine — Patagonian Granite Towers, Chile",
        bbox            = (-50.840, -51.100, -72.700, -73.050),
        building_height = 4.0,
        notes           = "Torres del Paine (2,850 m) and Cuernos del Paine — UNESCO Biosphere Reserve; vertical granite monoliths rising 2,000 m directly from the Patagonian steppe. Use --dem for the massif terrain; OSM maps park infrastructure and trekking huts",
    ),
    "fitz-roy": CityPreset(
        key             = "fitz-roy",
        label           = "Fitz Roy — El Chaltén / Patagonian Andes, Argentina",
        bbox            = (-49.150, -49.380, -72.900, -73.200),
        building_height = 4.0,
        notes           = "Monte Fitz Roy / Cerro Chaltén (3,405 m) and Cerro Torre (3,128 m) — jagged granite spires perpetually shrouded in cloud; El Chaltén trekking village below. Use --dem; OSM has village and trail hut network",
    ),
    # ── Caucasus / Near East ──────────────────────────────────────────────────
    "elbrus": CityPreset(
        key             = "elbrus",
        label           = "Mt. Elbrus — Highest Peak in Europe / Greater Caucasus",
        bbox            = (43.500, 43.180, 42.650, 42.200),
        building_height = 5.0,
        notes           = "Elbrus (5,642 m west summit) — highest peak in Europe and the Caucasus; twin-summit dormant volcano with 22 glaciers. Use --volcano --volcano-height 5642 --volcano-radius 20000 for the broad dome cone",
    ),
    "kazbek": CityPreset(
        key             = "kazbek",
        label           = "Mt. Kazbek — Georgian Military Highway / Gergeti Trinity Church",
        bbox            = (42.780, 42.580, 44.620, 44.380),
        building_height = 5.0,
        notes           = "Kazbek (5,047 m) — dormant stratovolcano above the Daryal Gorge; Gergeti Trinity Church (14th c.) perched at 2,170 m on a spur below the glacier — one of the world's most photogenic mountain churches",
    ),
    "ararat": CityPreset(
        key             = "ararat",
        label           = "Mt. Ararat — Ağrı Dağı, Eastern Turkey",
        bbox            = (39.900, 39.450, 44.600, 43.950),
        building_height = 5.0,
        notes           = "Great Ararat (5,137 m) — sacred dormant stratovolcano; near-perfect cone visible from Yerevan, Noah's Ark tradition, Armenian national symbol (despite being in Turkey). Use --volcano --volcano-height 5137 --volcano-radius 20000",
    ),
    # ── Iceland ───────────────────────────────────────────────────────────────
    "snaefellsjokull": CityPreset(
        key             = "snaefellsjokull",
        label           = "Snæfellsjökull — Glacier Volcano / Snæfellsnes, Iceland",
        bbox            = (64.930, 64.650, -23.550, -23.980),
        building_height = 4.0,
        notes           = "Snæfellsjökull (1,446 m) — Jules Verne's 'Journey to the Centre of the Earth' entrance; glacier-capped stratovolcano at the tip of Snæfellsnes peninsula. Use --volcano --volcano-height 1446 --volcano-radius 8000",
    ),
    "hekla": CityPreset(
        key             = "hekla",
        label           = "Hekla — Most Active Volcano in Iceland",
        bbox            = (64.100, 63.850, -19.450, -19.900),
        building_height = 4.0,
        notes           = "Hekla (1,491 m) — elongated ridge fissure volcano, erupted 20+ times since 874 AD; historically called 'Gateway to Hell' in medieval Europe. Use --volcano --volcano-height 1491 --volcano-radius 12000",
    ),
    "eyjafjallajokull": CityPreset(
        key             = "eyjafjallajokull",
        label           = "Eyjafjallajökull — Glacier Volcano, Iceland",
        bbox            = (63.720, 63.520, -19.400, -19.900),
        building_height = 4.0,
        notes           = "Eyjafjallajökull (1,651 m) — glacier-topped stratovolcano whose 2010 eruption shut European airspace for 6 days. Use --volcano --volcano-height 1651 --volcano-radius 10000",
    ),
    # ── South America volcanoes ───────────────────────────────────────────────
    "cotopaxi": CityPreset(
        key             = "cotopaxi",
        label           = "Cotopaxi — Active Stratovolcano, Ecuador",
        bbox            = (-0.500, -0.900, -78.200, -78.700),
        building_height = 5.0,
        notes           = "Cotopaxi (5,897 m) — one of the world's highest active volcanoes; near-perfect snow-capped cone rising above the Andean páramo. Use --volcano --volcano-height 5897 --volcano-radius 15000",
    ),
    "chimborazo": CityPreset(
        key             = "chimborazo",
        label           = "Chimborazo — Farthest Point from Earth's Centre, Ecuador",
        bbox            = (-1.320, -1.620, -78.650, -78.980),
        building_height = 5.0,
        notes           = "Chimborazo (6,268 m) — Ecuador's highest peak; due to equatorial bulge its summit is the farthest point from Earth's centre; dormant stratovolcano with multiple glacier-covered summits. Use --volcano --volcano-height 6268 --volcano-radius 18000",
    ),
    "aconcagua": CityPreset(
        key             = "aconcagua",
        label           = "Aconcagua — Highest Peak in the Americas, Argentina",
        bbox            = (-32.440, -32.860, -69.750, -70.200),
        building_height = 4.0,
        notes           = "Aconcagua (6,961 m) — highest mountain in both the Western and Southern Hemispheres; non-volcanic but formed by tectonic collision of Nazca and South American plates. Use --volcano --volcano-height 6961 --volcano-radius 15000 for a simplified cone",
    ),
    "villarrica": CityPreset(
        key             = "villarrica",
        label           = "Villarrica — Active Cone Volcano / Lake District, Chile",
        bbox            = (-39.250, -39.600, -71.750, -72.150),
        building_height = 5.0,
        notes           = "Villarrica (2,847 m) — one of Chile's most active volcanoes; near-perfect snow-capped cone above Lago Villarrica and the resort town of Pucón. Use --volcano --volcano-height 2847 --volcano-radius 10000",
    ),
    "osorno": CityPreset(
        key             = "osorno",
        label           = "Osorno — Perfect Cone Volcano / Chilean Lake District",
        bbox            = (-40.900, -41.300, -72.250, -72.750),
        building_height = 5.0,
        notes           = "Osorno (2,652 m) — perfectly symmetrical glaciated stratovolcano reflected in Lago Llanquihue; often called the 'Fuji of South America'. Use --volcano --volcano-height 2652 --volcano-radius 10000",
    ),
    # ── Africa ────────────────────────────────────────────────────────────────
    "kilimanjaro": CityPreset(
        key             = "kilimanjaro",
        label           = "Kilimanjaro — Highest Peak in Africa, Tanzania",
        bbox            = (-2.900, -3.250, 37.550, 37.150),
        building_height = 5.0,
        notes           = "Kibo summit (5,895 m) — Africa's highest peak; free-standing stratovolcano rising 4,877 m above the Tanzanian savanna; three volcanic cones (Kibo, Mawenzi, Shira). Use --volcano --volcano-height 5895 --volcano-radius 25000 for the mountain cone",
    ),
    # ── Hawaii ────────────────────────────────────────────────────────────────
    "mauna-kea": CityPreset(
        key             = "mauna-kea",
        label           = "Mauna Kea — Tallest Mountain from Base, Hawai'i",
        bbox            = (20.000, 19.600, -155.200, -155.750),
        building_height = 5.0,
        notes           = "Mauna Kea (4,205 m above sea level; 10,210 m from ocean floor — tallest from base) — dormant shield volcano; 13 astronomical observatories on summit in arid cold desert. Use --volcano --volcano-height 4205 --volcano-radius 35000 for the broad shield dome",
    ),
    "mauna-loa": CityPreset(
        key             = "mauna-loa",
        label           = "Mauna Loa — Largest Volcano on Earth, Hawai'i",
        bbox            = (19.700, 19.200, -155.300, -155.900),
        building_height = 5.0,
        notes           = "Mauna Loa (4,169 m) — largest volcano on Earth by volume (75,000 km³); broad shield profile, active (erupted 2022). Use --volcano --volcano-height 4169 --volcano-radius 50000 for the shield shape",
    ),
    # ── Pacific Ring of Fire ──────────────────────────────────────────────────
    "mayon": CityPreset(
        key             = "mayon",
        label           = "Mayon — World's Most Perfect Volcanic Cone, Philippines",
        bbox            = (13.380, 13.100, 123.820, 123.540),
        building_height = 6.0,
        notes           = "Mayon (2,463 m) — most active volcano in the Philippines, extraordinary 47.5° slope symmetry; Legazpi city and Daraga church in the foreground. Use --volcano --volcano-height 2463 --volcano-radius 8000",
    ),
    "merapi": CityPreset(
        key             = "merapi",
        label           = "Merapi — Most Active Volcano in Java / Indonesia",
        bbox            = (-7.380, -7.700, 110.580, 110.300),
        building_height = 6.0,
        notes           = "Merapi (2,930 m) — most active volcano in Indonesia; pyroclastic flow scars on the slopes, Prambanan temple complex 28 km south. Use --volcano --volcano-height 2930 --volcano-radius 12000",
    ),
    "bromo": CityPreset(
        key             = "bromo",
        label           = "Mt. Bromo — Caldera / Tengger Massif, Java",
        bbox            = (-7.840, -8.060, 113.070, 112.820),
        building_height = 4.0,
        notes           = "Bromo (2,329 m) — active volcano inside the enormous Tengger caldera (10 km wide sea-of-sand); Semeru (3,676 m — Java's highest) on the horizon. Use --volcano --volcano-height 2329 --volcano-radius 5000 for the inner cone",
    ),
    # ── North America (mountains) ─────────────────────────────────────────────
    "rainier": CityPreset(
        key             = "rainier",
        label           = "Mt. Rainier — Most Glaciated Peak in Contiguous USA",
        bbox            = (47.000, 46.680, -121.520, -121.960),
        building_height = 5.0,
        notes           = "Rainier (4,392 m) — active stratovolcano; 26 glaciers covering 91 km²; most glaciated peak in the contiguous US, a Decade Volcano. Use --volcano --volcano-height 4392 --volcano-radius 20000",
    ),
    "st-helens": CityPreset(
        key             = "st-helens",
        label           = "Mt. St. Helens — 1980 Eruption Crater, Washington",
        bbox            = (46.330, 46.050, -121.990, -122.420),
        building_height = 5.0,
        notes           = "Mt. St. Helens (2,549 m after the 1980 eruption removed 400 m of summit) — lateral blast removed the north flank; crater open to the north. Use --volcano --volcano-height 2549 --volcano-radius 10000",
    ),
    "denali": CityPreset(
        key             = "denali",
        label           = "Denali — Highest Peak in North America, Alaska",
        bbox            = (63.200, 62.900, -150.750, -151.300),
        building_height = 4.0,
        notes           = "Denali (6,190 m) — highest peak in North America; McKinley massif rising 5,500 m from the Kahiltna glacier base — greatest base-to-summit rise of any land peak. Use --volcano --volcano-height 6190 --volcano-radius 30000",
    ),
    # ── New Zealand (additional) ──────────────────────────────────────────────
    "aoraki": CityPreset(
        key             = "aoraki",
        label           = "Aoraki / Mt. Cook — Highest Peak in New Zealand",
        bbox            = (-43.420, -43.750, 170.320, 169.940),
        building_height = 4.0,
        notes           = "Aoraki/Mt. Cook (3,724 m) — highest peak in New Zealand; Tasman Glacier (longest in Australasia) on its eastern flank, Hooker Valley below. Use --dem or --volcano --volcano-height 3724 --volcano-radius 12000",
    ),
    "ngauruhoe": CityPreset(
        key             = "ngauruhoe",
        label           = "Ngauruhoe — Mt. Doom / Tongariro National Park, NZ",
        bbox            = (-39.020, -39.280, 175.780, 175.470),
        building_height = 4.0,
        notes           = "Ngauruhoe (2,291 m) — near-perfect active andesite cone, filmed as Mt. Doom in Lord of the Rings; adjacent Tongariro Red Crater and Blue Lake. Use --volcano --volcano-height 2291 --volcano-radius 5000",
    ),
    # ── Norway (terrain) ──────────────────────────────────────────────────────
    "preikestolen": CityPreset(
        key             = "preikestolen",
        label           = "Preikestolen — Pulpit Rock / Lysefjord, Norway",
        bbox            = (59.050, 58.910, 6.270, 6.100),
        building_height = 5.0,
        notes           = "Preikestolen (604 m) — flat-topped cliff rising vertically above Lysefjord; Forsand village and ferry dock below. Use --dem for the plateau and fjord topography; OSM maps the ferry terminal and trailhead",
    ),
    "lofoten": CityPreset(
        key             = "lofoten",
        label           = "Lofoten Islands — Dramatic Arctic Peaks, Norway",
        bbox            = (68.230, 68.020, 14.650, 13.950),
        building_height = 6.0,
        notes           = "Svolværgeita and Vagakallen peaks (942 m) rising directly from the sea above the fishing village of Svolvær — UNESCO tentative site; red and yellow rorbu (fishermen's cabins) in OSM, knife-edge ridges for --dem",
    ),
}

# Canonical display order for `sdlos mesh presets` — grouped by region
_PRESET_ORDER = [
    # Germany
    "mainz", "mainz-core", "cologne", "hamburg", "berlin", "munich", "nurnberg", "frankfurt",
    "freiburg", "lübeck", "regensburg",
    # Italy
    "rome", "milan", "florence", "venice", "naples",
    # France
    "paris",
    # Netherlands
    "amsterdam",
    # Switzerland
    "bern", "zurich", "lucerne",
    # Belgium
    "bruges",
    # Luxembourg
    "luxembourg",
    # Malta
    "valletta",
    # United Kingdom
    "london",
    # Norway
    "oslo",
    # Sweden
    "stockholm", "malmo",
    # Denmark
    "copenhagen",
    # Baltic
    "tallinn", "riga", "vilnius",
    # Greece
    "athens",
    # Central Europe
    "vienna", "salzburg", "prague", "budapest", "krakow",
    # Iberia
    "lisbon", "porto", "madrid", "barcelona",
    # Adriatic
    "dubrovnik", "kotor",
    # Albania
    "tirana", "berat", "gjirokaster",
    # Russia
    "moscow",
    # Canada
    "toronto", "vancouver", "montreal", "calgary",
    # USA
    "nyc", "chicago", "detroit", "philadelphia", "austin",
    # Mexico
    "mexico-city", "mexico-popo",
    # Caribbean
    "havana",
    # South America
    "quito", "rio", "cusco", "cartagena", "quito-pichincha",
    # New Zealand
    "nz-taranaki",
    # Japan
    "tokyo", "osaka", "kyoto", "kyoto-hiei", "tokyo-fujihama",
    # East Asia
    "seoul", "beijing", "pingyao",
    # Turkey
    "istanbul", "cappadocia", "erciyes", "mt-hasan",
    # Middle East
    "dubai", "jerusalem",
    # North Africa
    "casablanca", "marrakesh", "fez",
    # Egypt
    "cairo",
    # India
    "mumbai", "delhi", "jaipur", "kolkata",
    # Southeast Asia
    "jakarta", "bangkok", "hoi-an",
    # Nepal
    "kathmandu",
    # Australia
    "sydney",
    # East Africa
    "zanzibar",
    # Ukraine
    "kyiv", "kyiv-lavra", "lviv", "odesa", "kharkiv", "chernivtsi", "kamianets",
    "pripyat", "hoverla",
    # Poland
    "warsaw", "gdansk", "wroclaw",
    # Bosnia & Herzegovina
    "sarajevo", "mostar",
    # Serbia
    "belgrade",
    # Romania
    "bucharest", "brasov", "sighisoara",
    # Bulgaria
    "sofia", "plovdiv",
    # North Macedonia
    "ohrid",
    # Belarus
    "minsk",
    # Alps / Dolomites
    "dolomiti-tre-cime", "dolomiti-marmolada", "dolomiti-seceda",
    "matterhorn", "mont-blanc", "eiger", "zugspitze", "grossglockner",
    # Patagonia
    "torres-del-paine", "fitz-roy",
    # Caucasus / Near East
    "elbrus", "kazbek", "ararat",
    # Iceland
    "snaefellsjokull", "hekla", "eyjafjallajokull",
    # South America (volcanoes)
    "cotopaxi", "chimborazo", "aconcagua", "villarrica", "osorno",
    # Africa (mountains)
    "kilimanjaro",
    # Hawaii
    "mauna-kea", "mauna-loa",
    # Pacific Ring of Fire
    "mayon", "merapi", "bromo",
    # North America (mountains)
    "rainier", "st-helens", "denali",
    # New Zealand (additional)
    "aoraki", "ngauruhoe",
    # Norway (terrain)
    "preikestolen", "lofoten",
]

# Region grouping for the presets table display
_PRESET_REGIONS: list[tuple[str, list[str]]] = [
    ("Germany",         ["mainz", "mainz-core", "cologne", "hamburg", "berlin", "munich", "nurnberg", "frankfurt", "freiburg", "lübeck", "regensburg"]),
    ("Italy",           ["rome", "milan", "florence", "venice", "naples"]),
    ("France",          ["paris"]),
    ("Netherlands",     ["amsterdam"]),
    ("Switzerland",     ["bern", "zurich", "lucerne"]),
    ("Belgium",         ["bruges"]),
    ("Luxembourg",      ["luxembourg"]),
    ("Malta",           ["valletta"]),
    ("United Kingdom",  ["london"]),
    ("Norway",          ["oslo"]),
    ("Sweden",          ["stockholm", "malmo"]),
    ("Denmark",         ["copenhagen"]),
    ("Baltic",          ["tallinn", "riga", "vilnius"]),
    ("Greece",          ["athens"]),
    ("Central Europe",  ["vienna", "salzburg", "prague", "budapest", "krakow"]),
    ("Iberia",          ["lisbon", "porto", "madrid", "barcelona"]),
    ("Adriatic",        ["dubrovnik", "kotor"]),
    ("Albania",         ["tirana", "berat", "gjirokaster"]),
    ("Russia",          ["moscow"]),
    ("Canada",          ["toronto", "vancouver", "montreal", "calgary"]),
    ("USA",             ["nyc", "chicago", "detroit", "philadelphia", "austin"]),
    ("Mexico",          ["mexico-city", "mexico-popo"]),
    ("Caribbean",       ["havana"]),
    ("South America",   ["quito", "rio", "cusco", "cartagena", "quito-pichincha"]),
    ("Japan",           ["tokyo", "osaka", "kyoto", "kyoto-hiei", "tokyo-fujihama"]),
    ("East Asia",       ["seoul", "beijing", "pingyao"]),
    ("Turkey",          ["istanbul", "cappadocia", "erciyes", "mt-hasan"]),
    ("Middle East",     ["dubai", "jerusalem"]),
    ("North Africa",    ["casablanca", "marrakesh", "fez"]),
    ("Egypt",           ["cairo"]),
    ("India",           ["mumbai", "delhi", "jaipur", "kolkata"]),
    ("Southeast Asia",  ["jakarta", "bangkok", "hoi-an"]),
    ("Nepal",           ["kathmandu"]),
    ("Australia",       ["sydney"]),
    ("New Zealand",     ["nz-taranaki", "aoraki", "ngauruhoe"]),
    ("East Africa",     ["zanzibar"]),
    ("Ukraine",              ["kyiv", "kyiv-lavra", "lviv", "odesa", "kharkiv", "chernivtsi", "kamianets", "pripyat", "hoverla"]),
    ("Poland",               ["warsaw", "gdansk", "wroclaw"]),
    ("Bosnia & Herzegovina", ["sarajevo", "mostar"]),
    ("Serbia",               ["belgrade"]),
    ("Romania",              ["bucharest", "brasov", "sighisoara"]),
    ("Bulgaria",             ["sofia", "plovdiv"]),
    ("North Macedonia",      ["ohrid"]),
    ("Belarus",              ["minsk"]),
    ("Alps / Dolomites",     ["dolomiti-tre-cime", "dolomiti-marmolada", "dolomiti-seceda", "matterhorn", "mont-blanc", "eiger", "zugspitze", "grossglockner"]),
    ("Patagonia",            ["torres-del-paine", "fitz-roy"]),
    ("Caucasus / Near East", ["elbrus", "kazbek", "ararat"]),
    ("Iceland",              ["snaefellsjokull", "hekla", "eyjafjallajokull"]),
    ("S. America volcanoes", ["cotopaxi", "chimborazo", "aconcagua", "villarrica", "osorno"]),
    ("Africa (mountains)",   ["kilimanjaro"]),
    ("Hawaii",               ["mauna-kea", "mauna-loa"]),
    ("Pacific Ring of Fire", ["mayon", "merapi", "bromo"]),
    ("N. America mountains", ["rainier", "st-helens", "denali"]),
    ("Norway (terrain)",     ["preikestolen", "lofoten"]),
]


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
    volcano: bool = False,
    volcano_height: float = 2000.0,
    volcano_radius: float = 5000.0,
    volcano_segments: int = 64,
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

    # ── Volcano cone shortcut (steps 2–5 replaced) ───────────────────────────
    # When --volcano is set, generate a parametric cone centred on the bbox
    # mid-point and skip the Overpass fetch entirely.
    if volcano:
        _log(
            f"mode      volcano cone  h={volcano_height:.0f} m  "
            f"r={volcano_radius:.0f} m  segments={volcano_segments}"
        )
        scene: trimesh.Trimesh = trimesh.creation.cone(
            radius=float(volcano_radius),
            height=float(volcano_height),
            sections=int(volcano_segments),
        )
        # Z-up → Y-up (GLTF-compliant, same rotation as OSM extrusion path)
        scene.apply_transform(
            np.array(
                [[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]],
                dtype=np.float64,
            )
        )
        _log(f"  {len(scene.faces):,} faces  {len(scene.vertices):,} vertices")

    if not volcano:
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

        # 5a. Centre at origin
        # GIS pipelines (UTM / WGS-84) produce global coordinates in the millions.
        # 3-D renderers expect local coordinates near the world origin.
        centroid = scene.vertices.mean(axis=0)
        scene.vertices -= centroid

        # 5b. Z-up → Y-up conversion (GLTF standard)
        # trimesh.creation.extrude_polygon() extrudes along the Z axis, so the
        # input mesh has:
        #   X = UTM easting   (horizontal)
        #   Y = UTM northing  (horizontal)
        #   Z = building height (vertical / "up")
        # GLTF requires a Y-up right-hand coordinate system:
        #   X = right (easting)
        #   Y = up    (height)
        #   Z = depth (southing, negated for RH)
        # The 4×4 homogeneous rotation below applies this swap in-place.
        # det = +1  → proper rotation, face winding and normal handedness preserved.
        rot_zup_to_yup = np.array(
            [[1,  0,  0,  0],
             [0,  0,  1,  0],
             [0, -1,  0,  0],
             [0,  0,  0,  1]],
            dtype=np.float64,
        )
        scene.apply_transform(rot_zup_to_yup)
        _log("  applied Z-up → Y-up rotation (GLTF-compliant export)")

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
    "--volcano",
    is_flag=True, default=False,
    help=(
        "Generate a parametric volcano cone mesh instead of fetching OSM buildings.  "
        "The cone is centred on the bbox mid-point.  "
        "Pair with a terrain preset for the correct location automatically.  "
        "e.g. --preset mexico-popo --volcano --volcano-height 5426"
    ),
)
@click.option(
    "--volcano-height",
    type=float, default=2000.0, show_default=True, metavar="M",
    help="Peak height of the volcano cone above the base plane in metres.",
)
@click.option(
    "--volcano-radius",
    type=float, default=5000.0, show_default=True, metavar="M",
    help="Base radius of the volcano cone in metres.",
)
@click.option(
    "--volcano-segments",
    type=int, default=64, show_default=True, metavar="N",
    help="Number of radial segments around the cone circumference.",
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
    out_dir: OptionalPath,
    app: Optional[str],
    no_cache: bool,
    volcano: bool,
    volcano_height: float,
    volcano_radius: float,
    volcano_segments: int,
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
            # Each city gets its own sub-folder so buffer files don't collide:
            #   examples/apps/<app>/data/models/<name>/<name>.gltf
            out_dir = root / "examples" / "apps" / app / "data" / "models" / resolved_name
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
        if volcano:
            click.echo(
                f"  mode:     volcano cone  "
                f"h={volcano_height:.0f} m  r={volcano_radius:.0f} m  "
                f"segments={volcano_segments}"
            )
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
            volcano=volcano,
            volcano_height=volcano_height,
            volcano_radius=volcano_radius,
            volcano_segments=volcano_segments,
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


# ── sdlos mesh presets ───────────────────────────────────────────────────────

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
