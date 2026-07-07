#!/usr/bin/env python3
"""FlightDial desktop emulator — mirrors the M5Dial radar app.

Controls:
  scroll wheel      = zoom in / out  (encoder rotation)
  left click        = select aircraft / dismiss detail
  right click       = open / close settings menu
  scroll in menu    = navigate rows
  left click in menu= cycle that setting's value
  Q / Escape        = quit (or close menu if open)

Credentials (optional — anonymous works but is rate-limited):
  set OPENSKY_USER and OPENSKY_PASS environment variables before running.
"""

import io
import json
import math
import os
import threading
import time
from dataclasses import dataclass
from typing import List

import pygame
import requests

# ── Location & zoom ───────────────────────────────────────────────────────────
# True first-run defaults — mirrors DEFAULT_HOME_LAT/LON in src/config.h. Never
# mutated at runtime (unlike HOME_LAT/HOME_LON below) so a factory reset always
# has the real compiled-in fallback to return to, not the last-used value.
DEFAULT_HOME_LAT = 51.5
DEFAULT_HOME_LON = -0.1

# Live/current home location — starts at the default, overwritten at startup
# from location_state.json (the emulator's equivalent of the firmware's NVS
# flash storage) if it exists, and mutated at runtime as the location changes.
HOME_LAT     = DEFAULT_HOME_LAT
HOME_LON     = DEFAULT_HOME_LON
ZOOM_STEPS   = [10, 25, 50, 100, 200]
ZOOM_DEFAULT = 1

# ── Saved favourite locations (persisted alongside home lat/lon) ────────────
FAV_COUNT  = 3
STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "location_state.json")


def load_location_state():
    state = {"home_lat": DEFAULT_HOME_LAT, "home_lon": DEFAULT_HOME_LON,
             "favourites": [{"name": "", "lat": 0.0, "lon": 0.0} for _ in range(FAV_COUNT)]}
    if os.path.exists(STATE_FILE):
        try:
            with open(STATE_FILE) as f:
                saved = json.load(f)
            state["home_lat"] = saved.get("home_lat", state["home_lat"])
            state["home_lon"] = saved.get("home_lon", state["home_lon"])
            for i, fav in enumerate(saved.get("favourites", [])[:FAV_COUNT]):
                state["favourites"][i] = {
                    "name": fav.get("name", ""),
                    "lat":  fav.get("lat", 0.0),
                    "lon":  fav.get("lon", 0.0),
                }
        except Exception as exc:
            print(f"[Location] failed to load {STATE_FILE}: {exc}")
    return state


def save_location_state(state):
    try:
        with open(STATE_FILE, "w") as f:
            json.dump(state, f, indent=2)
    except Exception as exc:
        print(f"[Location] failed to save {STATE_FILE}: {exc}")

# ── OpenSky credentials ───────────────────────────────────────────────────────
# Loaded from environment variables if set, otherwise from a local .env file
# (KEY=VALUE per line, gitignored) so you don't need to set them every launch
# or paste them into a coding-assistant chat. See .env.example for the format.
def _load_env_file(path):
    if not os.path.exists(path):
        return
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            os.environ.setdefault(key.strip(), val.strip())


_load_env_file(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"))

OPENSKY_USER = os.getenv("OPENSKY_USER", "")
OPENSKY_PASS = os.getenv("OPENSKY_PASS", "")

# ── Display geometry ──────────────────────────────────────────────────────────
D      = 240
CX = CY = D // 2
PLOT_R = 105
BEZEL  = 55
WIN_W  = D + BEZEL * 2
WIN_H  = WIN_W + 28
BEZ_CX = WIN_W // 2
BEZ_CY = WIN_W // 2

# ── Colour themes ─────────────────────────────────────────────────────────────
THEMES = [
    {
        "name":     "Radar",
        "swatch":   (0, 220, 0),
        "bg":       (0,   0,   8),
        "ring":     (8,   40,  8),
        "ring_lbl": (0,   220, 0),
        "home":     (255, 0,   0),
        "ac":       (255, 255, 255),
        "sel":      (255, 165, 0),
        "gnd":      (130, 130, 130),
        "status":   (160, 160, 160),
        "overlay":  (12,  12,  12),
        "bezel_o":  (55,  55,  55),
        "bezel_i":  (25,  25,  25),
    },
    {
        "name":     "Amber",
        "swatch":   (255, 185, 0),
        "bg":       (8,   4,   0),
        "ring":     (100, 50,  0),
        "ring_lbl": (255, 185, 0),
        "home":     (255, 80,  0),
        "ac":       (255, 220, 120),
        "sel":      (255, 100, 0),
        "gnd":      (160, 100, 40),
        "status":   (210, 160, 80),
        "overlay":  (22,  11,  0),
        "bezel_o":  (70,  45,  10),
        "bezel_i":  (35,  18,  0),
    },
    {
        "name":     "Ocean",
        "swatch":   (0, 190, 255),
        "bg":       (0,   5,   18),
        "ring":     (0,   50,  110),
        "ring_lbl": (0,   190, 255),
        "home":     (255, 80,  80),
        "ac":       (180, 230, 255),
        "sel":      (0,   255, 200),
        "gnd":      (80,  120, 160),
        "status":   (120, 170, 210),
        "overlay":  (0,   12,  30),
        "bezel_o":  (20,  45,  75),
        "bezel_i":  (8,   20,  40),
    },
    {
        "name":     "Neon",
        "swatch":   (220, 0, 255),
        "bg":       (5,   0,   12),
        "ring":     (70,  0,   90),
        "ring_lbl": (220, 0,   255),
        "home":     (255, 60,  60),
        "ac":       (0,   255, 180),
        "sel":      (255, 230, 0),
        "gnd":      (150, 80,  170),
        "status":   (190, 100, 220),
        "overlay":  (16,  0,   22),
        "bezel_o":  (60,  10,  80),
        "bezel_i":  (28,  0,   38),
    },
]

# ── Settings menu items ───────────────────────────────────────────────────────
# kind "cycle"     — click cycles through item["options"] (existing behaviour)
# kind "action"    — click opens the keyboard location-entry wizard
# kind "cycle_fav" — click cycles through saved favourites, activating each as home
MENU_ITEMS = [
    {"key": "labels",  "label": "Flight labels",  "kind": "cycle", "options": ["Off", "Selected", "All"]},
    {"key": "theme",   "label": "Colour theme",   "kind": "cycle", "options": ["Radar", "Amber", "Ocean", "Neon"]},
    {"key": "units",   "label": "Units",          "kind": "cycle", "options": ["ft / kts", "m / km/h"]},
    {"key": "filter",  "label": "Traffic",        "kind": "cycle", "options": ["Airborne", "All"]},
    {"key": "minalt",  "label": "Min altitude",   "kind": "cycle", "options": ["Off", "1,000 ft", "5,000 ft", "10,000 ft", "20,000 ft"]},
    {"key": "trails",  "label": "Heading trails", "kind": "cycle", "options": ["On", "Off"]},
    {"key": "rings",   "label": "Range rings",    "kind": "cycle", "options": ["On", "Off"]},
    {"key": "refresh", "label": "Refresh rate",   "kind": "cycle", "options": ["10 s", "20 s", "30 s"]},
    {"key": "change_loc", "label": "Change Location", "kind": "action"},
    {"key": "saved_loc",  "label": "Saved Locations", "kind": "cycle_fav"},
    {"key": "factory_reset", "label": "Factory Reset", "kind": "danger"},
]

DEFAULTS = {"labels": 1, "theme": 0, "units": 0, "filter": 0,
            "minalt": 0, "trails": 0, "rings": 0, "refresh": 0}

# Min altitude thresholds in metres (Off, 1k, 5k, 10k, 20k ft)
MIN_ALT_M      = [0, 305, 1524, 3048, 6096]
REFRESH_OPTIONS = [10, 20, 30]

# Menu panel geometry (display-surface coords). Narrower than the display's
# 240px diameter, and vertically centred on CY, so left/right-aligned row text
# stays clear of the round screen's curve and the panel doesn't sit high.
PAGE_SIZE  = 4   # rows shown per page — the rest is reached by paging
_ROW_H     = 20
_MW        = 180
_MH        = 150
_MX        = CX - _MW // 2
_MY        = CY - _MH // 2
_DIVIDER_Y = _MY + 22
_ROW_Y0    = _MY + 28

C_WIN_BG = (10, 10, 10)


# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class Aircraft:
    icao24:    str   = ""
    callsign:  str   = ""
    country:   str   = ""
    lat:       float = 0.0
    lon:       float = 0.0
    alt_m:     float = 0.0
    on_ground: bool  = False
    speed_ms:  float = 0.0
    heading:   float = 0.0


RATE_LIMITED = None  # sentinel for 429 responses

def fetch_aircraft(center_lat, center_lon, radius_km):
    KM_PER_DEG = 111.0
    d_lat = radius_km / KM_PER_DEG
    d_lon = radius_km / (KM_PER_DEG * math.cos(math.radians(center_lat)))
    url = (
        "https://opensky-network.org/api/states/all"
        f"?lamin={center_lat - d_lat:.4f}&lomin={center_lon - d_lon:.4f}"
        f"&lamax={center_lat + d_lat:.4f}&lomax={center_lon + d_lon:.4f}"
    )
    auth = (OPENSKY_USER, OPENSKY_PASS) if OPENSKY_USER else None
    try:
        resp = requests.get(url, auth=auth, timeout=12)
        if resp.status_code == 429:
            retry = int(resp.headers.get("Retry-After", 30))
            print(f"[OpenSky] 429 — backing off {retry}s")
            return RATE_LIMITED
        resp.raise_for_status()
        data = resp.json()
    except Exception as exc:
        print(f"[OpenSky] {exc}")
        return []

    out = []
    for s in (data.get("states") or []):
        if s[5] is None or s[6] is None:
            continue
        ac = Aircraft()
        ac.icao24    = (s[0] or "??????").strip()
        ac.callsign  = ((s[1] or "").strip()) or ac.icao24
        ac.country   = s[2] or "???"
        ac.lon       = float(s[5])
        ac.lat       = float(s[6])
        ac.alt_m     = float(s[7]) if s[7] is not None else 0.0
        ac.on_ground = bool(s[8])
        ac.speed_ms  = float(s[9]) if s[9] is not None else 0.0
        ac.heading   = float(s[10]) if s[10] is not None else 0.0
        out.append(ac)

    print(f"[OpenSky] {len(out)} aircraft at {radius_km:.0f} km")
    return out


# ── Map underlay (OpenStreetMap raster tiles, stitched + cropped) ───────────
MAP_TILE_DIR  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "map_tile_cache")
TILE_SIZE     = 256
TILE_HEADERS  = {"User-Agent": "FlightDial-prototype/0.1 (personal project, contact: n/a)"}


def _deg2tile_f(lat, lon, zoom):
    lat_r = math.radians(lat)
    n = 2 ** zoom
    xtile = (lon + 180.0) / 360.0 * n
    ytile = (1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * n
    return xtile, ytile


def _choose_zoom(lat, radius_km, target_px):
    mpp_target = (radius_km * 1000.0 * 2.0) / target_px
    for z in range(1, 20):
        mpp = 156543.03392 * math.cos(math.radians(lat)) / (2 ** z)
        if mpp <= mpp_target:
            return z, mpp
    z = 19
    return z, 156543.03392 * math.cos(math.radians(lat)) / (2 ** z)


def fetch_map_tile(center_lat, center_lon, radius_km, target_px):
    """Fetch+stitch OSM tiles, crop/scale to a target_px square centred on home. Returns raw PNG bytes."""
    zoom, mpp = _choose_zoom(center_lat, radius_km, target_px)
    xtile_f, ytile_f = _deg2tile_f(center_lat, center_lon, zoom)

    px_needed   = (radius_km * 1000.0 * 2.0) / mpp
    tiles_needed = int(math.ceil(px_needed / TILE_SIZE)) + 2
    x0 = int(math.floor(xtile_f)) - tiles_needed // 2
    y0 = int(math.floor(ytile_f)) - tiles_needed // 2

    grid = pygame.Surface((tiles_needed * TILE_SIZE, tiles_needed * TILE_SIZE))
    n = 2 ** zoom
    for ix in range(tiles_needed):
        for iy in range(tiles_needed):
            tx, ty = (x0 + ix) % n, (y0 + iy) % n
            url = f"https://tile.openstreetmap.org/{zoom}/{tx}/{ty}.png"
            try:
                resp = requests.get(url, headers=TILE_HEADERS, timeout=10)
                resp.raise_for_status()
                tile_surf = pygame.image.load(io.BytesIO(resp.content))
            except Exception as exc:
                print(f"[Map] tile {zoom}/{tx}/{ty} failed: {exc}")
                tile_surf = pygame.Surface((TILE_SIZE, TILE_SIZE))
                tile_surf.fill((20, 20, 20))
            grid.blit(tile_surf, (ix * TILE_SIZE, iy * TILE_SIZE))

    home_px_x = (xtile_f - x0) * TILE_SIZE
    home_px_y = (ytile_f - y0) * TILE_SIZE
    half = px_needed / 2.0
    crop_rect = pygame.Rect(int(home_px_x - half), int(home_px_y - half),
                             int(px_needed), int(px_needed))
    cropped = pygame.Surface((crop_rect.width, crop_rect.height))
    cropped.blit(grid, (0, 0), crop_rect)
    scaled = pygame.transform.smoothscale(cropped, (target_px, target_px))

    print(f"[Map] tile z={zoom} grid={tiles_needed}x{tiles_needed} at {radius_km:.0f} km")
    return scaled


def _map_tile_cache_path(center_lat, center_lon, radius_km, target_px):
    return os.path.join(
        MAP_TILE_DIR,
        f"tile_{center_lat:.4f}_{center_lon:.4f}_{int(radius_km)}km_{target_px}px.png")


def load_cached_map_tile(center_lat, center_lon, radius_km, target_px):
    path = _map_tile_cache_path(center_lat, center_lon, radius_km, target_px)
    if os.path.exists(path):
        return pygame.image.load(path).convert()
    return None


def save_map_tile(center_lat, center_lon, radius_km, target_px, surf):
    os.makedirs(MAP_TILE_DIR, exist_ok=True)
    pygame.image.save(surf, _map_tile_cache_path(center_lat, center_lon, radius_km, target_px))


# ── Demo mode — animated aircraft for offline / rate-limited testing ──────────
_DEMO_ROUTES = [
    # callsign, country, start_lat, start_lon, heading, speed_ms, alt_m
    ("BAW221", "United Kingdom",      55.60, -4.80, 315, 245, 10973),
    ("RYR4XR", "Ireland",             55.70, -3.90, 230, 220,  9144),
    ("EZY8BF", "United Kingdom",      55.50, -5.20,  55, 235,  8534),
    ("TOM657", "United Kingdom",      56.20, -4.20, 175, 198,  7620),
    ("DLH4KP", "Germany",             55.85, -6.10,  90, 265, 11278),
    ("UAE19",  "United Arab Emirates",55.95, -5.50, 120, 275, 12192),
]

def _demo_aircraft(radius_km: float) -> List[Aircraft]:
    t, out = time.time(), []
    for i, (cs, country, slat, slon, hdg, spd, alt) in enumerate(_DEMO_ROUTES):
        elapsed  = (t + i * 47) % 600
        hdg_r    = math.radians(hdg)
        dist_km  = spd * elapsed / 1000.0
        lat = slat + (math.cos(hdg_r) * dist_km) / 111.0
        lon = slon + (math.sin(hdg_r) * dist_km) / (111.0 * math.cos(math.radians(slat)))
        flat = math.cos(math.radians(HOME_LAT))
        if math.sqrt(((lon - HOME_LON) * flat * 111) ** 2 +
                     ((lat - HOME_LAT) * 111) ** 2) <= radius_km:
            ac = Aircraft(icao24=f"demo{i:02x}", callsign=cs, country=country,
                          lat=lat, lon=lon, alt_m=alt, speed_ms=spd, heading=float(hdg))
            out.append(ac)
    return out


def world_to_screen(lat, lon, c_lat, c_lon, radius_km):
    c_lat_r = math.radians(c_lat)
    dx = (lon - c_lon) * math.cos(c_lat_r) * 6371.0 * math.pi / 180.0
    dy = (lat - c_lat)                      * 6371.0 * math.pi / 180.0
    return CX + int((dx / radius_km) * PLOT_R), CY - int((dy / radius_km) * PLOT_R)


# ─────────────────────────────────────────────────────────────────────────────

class Emulator:
    MIN_FETCH_S = 12
    BACKOFF_S   = 30

    def __init__(self):
        self.loc_state = load_location_state()
        global HOME_LAT, HOME_LON
        HOME_LAT, HOME_LON = self.loc_state["home_lat"], self.loc_state["home_lon"]

        pygame.init()
        self.screen = pygame.display.set_mode((WIN_W, WIN_H))
        pygame.display.set_caption(f"FlightDial — {HOME_LAT:.4f}, {HOME_LON:.4f}")

        self.font_sm = pygame.font.SysFont("monospace", 11)
        self.font_xs = pygame.font.SysFont("monospace", 10)

        self.disp  = pygame.Surface((D, D), pygame.SRCALPHA)
        self._clip = pygame.Surface((D, D), pygame.SRCALPHA)
        self._clip.fill((0, 0, 0, 0))
        pygame.draw.circle(self._clip, (255, 255, 255, 255), (CX, CY), CX)

        self.aircraft:  List[Aircraft] = []
        self.zoom_idx   = ZOOM_DEFAULT
        self.selected   = -1
        self.last_upd   = 0.0
        self.fetching   = False
        self._thread    = None
        self.clock      = pygame.time.Clock()

        self.settings       = dict(DEFAULTS)
        self.show_menu      = False
        self.menu_sel       = 0
        self.rate_limited   = False
        self._backoff_until = 0.0
        self.demo_mode      = False

        self.fav_cursor    = -1     # last-activated favourite index (cycle_fav)
        self.input_active  = False  # keyboard location-entry wizard open?
        self.input_specs   = []
        self.input_index   = 0
        self.input_buffer  = ""
        self.confirm_reset = False  # factory-reset Y/N confirmation open?

        self.map_tiles   = {}   # radius_km -> pygame.Surface (D x D, pre-scaled)
        self._map_thread = None

    @property
    def t(self):
        return THEMES[self.settings["theme"]]

    # ── Data ──────────────────────────────────────────────────────────────────

    def _do_fetch(self, force=False):
        if self._thread and self._thread.is_alive():
            return
        now = time.time()
        if not force and self.last_upd and (now - self.last_upd) < self.MIN_FETCH_S:
            return
        if now < self._backoff_until:
            return
        self.fetching = True
        self.rate_limited = False

        def run():
            result = fetch_aircraft(HOME_LAT, HOME_LON, ZOOM_STEPS[self.zoom_idx])
            if result is RATE_LIMITED:
                self.rate_limited   = True
                self.demo_mode      = True
                self._backoff_until = time.time() + self.BACKOFF_S
            else:
                self.aircraft  = result
                self.last_upd  = time.time()
                self.demo_mode = False
            self.fetching = False

        self._thread = threading.Thread(target=run, daemon=True)
        self._thread.start()

    def _do_fetch_map(self, radius_km):
        if radius_km in self.map_tiles:
            return
        cached = load_cached_map_tile(HOME_LAT, HOME_LON, radius_km, D)
        if cached is not None:
            self.map_tiles[radius_km] = cached
            return
        if self._map_thread and self._map_thread.is_alive():
            return

        def run():
            try:
                surf = fetch_map_tile(HOME_LAT, HOME_LON, radius_km, D)
                self.map_tiles[radius_km] = surf
                save_map_tile(HOME_LAT, HOME_LON, radius_km, D, surf)
            except Exception as exc:
                print(f"[Map] fetch failed: {exc}")

        self._map_thread = threading.Thread(target=run, daemon=True)
        self._map_thread.start()

    # ── Radar rendering ───────────────────────────────────────────────────────

    def _draw_map(self, radius_km):
        surf = self.map_tiles.get(radius_km)
        if surf is None:
            return
        self.disp.blit(surf, (0, 0))

    def _draw_rings(self, radius_km):
        if self.settings["rings"]:
            return
        d = self.disp
        for r in [PLOT_R, PLOT_R // 2, PLOT_R // 4]:
            pygame.draw.circle(d, self.t["ring"], (CX, CY), r, 1)
        pygame.draw.line(d, self.t["ring_lbl"], (CX, CY - PLOT_R), (CX, CY - PLOT_R + 8))
        n = self.font_sm.render("N", True, self.t["ring_lbl"])
        d.blit(n, (CX - n.get_width() // 2, CY - PLOT_R - 14))
        for frac, r_px in [(0.5, PLOT_R // 2), (1.0, PLOT_R)]:
            s = self.font_sm.render(f"{radius_km * frac:.0f}km", True, self.t["ring"])
            d.blit(s, (CX + r_px + 2, CY - 6))

    HALO = (0, 0, 0)

    def _draw_aircraft(self, ac, sx, sy, selected):
        col = self.t["sel"] if selected else (self.t["gnd"] if ac.on_ground else self.t["ac"])
        r = 4 if selected else 3

        if not self.settings["trails"] and not ac.on_ground and ac.speed_ms > 5:
            rad = math.radians(ac.heading)
            ex  = sx + int(math.sin(rad) * 10)
            ey  = sy - int(math.cos(rad) * 10)
            pygame.draw.line(self.disp, self.HALO, (sx, sy), (ex, ey), 3)
            pygame.draw.line(self.disp, col, (sx, sy), (ex, ey))

        pygame.draw.circle(self.disp, self.HALO, (sx, sy), r + 2)
        pygame.draw.circle(self.disp, col, (sx, sy), r)

        show_label = (self.settings["labels"] == 2 or
                      (self.settings["labels"] == 1 and selected))
        if show_label:
            lbl_col = self.t["sel"] if selected else self.t["status"]
            lbl = self.font_sm.render(ac.callsign, True, lbl_col, self.HALO)
            self.disp.blit(lbl, (sx - lbl.get_width() // 2, sy - 18))

    def _fmt_alt(self, alt_m):
        if alt_m <= 0:
            return "Alt  n/a"
        if self.settings["units"] == 0:
            return f"Alt {int(alt_m * 3.28084):,} ft"
        return f"Alt {int(alt_m):,} m"

    def _fmt_spd(self, speed_ms, heading):
        if self.settings["units"] == 0:
            return f"{int(speed_ms * 1.94384)} kts  {heading:03.0f}°"
        return f"{int(speed_ms * 3.6)} km/h  {heading:03.0f}°"

    def _draw_detail(self, ac):
        d   = self.disp
        box = pygame.Rect(18, 148, 204, 76)
        pygame.draw.rect(d, self.t["overlay"], box, border_radius=6)
        pygame.draw.rect(d, self.t["sel"],     box, 1, border_radius=6)
        ly, step = 158, 16
        rows = [
            (f"{ac.callsign or 'N/A'}  [{ac.icao24}]", self.t["sel"]),
            (self._fmt_alt(ac.alt_m),                   self.t["status"]),
            (self._fmt_spd(ac.speed_ms, ac.heading),    self.t["status"]),
            (f"{ac.country}{'  [GND]' if ac.on_ground else ''}", self.t["status"]),
        ]
        for txt, col in rows:
            s = self.font_sm.render(txt, True, col, self.t["overlay"])
            d.blit(s, (CX - s.get_width() // 2, ly))
            ly += step

    def _draw_status_bar(self, visible, radius_km):
        # Whether a fetch is in flight is shown on the poll icon instead (a
        # solid ring), not here — this line is just the last known reading.
        if self.rate_limited:
            wait = max(0, int(self._backoff_until - time.time()))
            txt, col = f"rate limited ({wait}s)", (220, 80, 80)
        elif self.last_upd:
            txt, col = f"{radius_km:.0f}km  {visible}ac", self.t["status"]
        else:
            txt, col = "waiting...", self.t["status"]
        s = self.font_sm.render(txt, True, col)
        self.disp.blit(s, (CX - s.get_width() // 2, 134))

    # Discreet poll icon — a shrinking ring counts down to the next poll,
    # a solid ring means a request is in flight. Mirrors
    # RadarDisplay::drawPollIcon in radar.cpp instead of a numeric "Xs ago".
    POLL_ICON_X, POLL_ICON_Y, POLL_ICON_R = CX, D - 8, 7

    def _draw_poll_icon(self):
        x, y, r = self.POLL_ICON_X, self.POLL_ICON_Y, self.POLL_ICON_R
        pygame.draw.circle(self.disp, self.t["ring"], (x, y), r, 2)

        if self.fetching:
            pygame.draw.circle(self.disp, self.t["sel"], (x, y), r, 2)
            return

        refresh_s = REFRESH_OPTIONS[self.settings["refresh"]]
        elapsed   = (time.time() - self.last_upd) if self.last_upd else refresh_s
        remaining = 1.0 - min(1.0, elapsed / refresh_s)
        if remaining > 0.01:
            rect  = pygame.Rect(x - r, y - r, r * 2, r * 2)
            start = math.pi / 2
            end   = start + remaining * 2 * math.pi
            pygame.draw.arc(self.disp, self.t["ring_lbl"], rect, start, end, 2)

    # ── Settings menu ─────────────────────────────────────────────────────────

    def _draw_menu(self):
        d = self.disp

        # Semi-transparent panel
        panel = pygame.Surface((_MW, _MH), pygame.SRCALPHA)
        panel.fill((0, 0, 0, 210))
        d.blit(panel, (_MX, _MY))
        pygame.draw.rect(d, self.t["ring_lbl"],
                         pygame.Rect(_MX, _MY, _MW, _MH), 1, border_radius=6)

        # Title
        title = self.font_sm.render("SETTINGS", True, self.t["ring_lbl"])
        d.blit(title, (CX - title.get_width() // 2, _MY + 6))
        pygame.draw.line(d, self.t["ring"],
                         (_MX + 6, _DIVIDER_Y), (_MX + _MW - 6, _DIVIDER_Y))

        # Rows — only the current page's slice is shown; rows too far from
        # vertical centre would otherwise have their text clipped by the
        # round screen's curve.
        num_pages = (len(MENU_ITEMS) + PAGE_SIZE - 1) // PAGE_SIZE
        page      = self.menu_sel // PAGE_SIZE
        start     = page * PAGE_SIZE
        end       = min(start + PAGE_SIZE, len(MENU_ITEMS))

        for slot, i in enumerate(range(start, end)):
            item = MENU_ITEMS[i]
            ry   = _ROW_Y0 + slot * _ROW_H
            sel  = (i == self.menu_sel)

            if sel:
                hi = pygame.Surface((_MW - 4, _ROW_H - 2), pygame.SRCALPHA)
                hi.fill((*self.t["sel"], 50))
                d.blit(hi, (_MX + 2, ry))

            name_col  = self.t["ring_lbl"] if sel else self.t["status"]
            val_col   = self.t["sel"]      if sel else self.t["ring_lbl"]

            kind = item.get("kind", "cycle")
            if kind == "cycle":
                val_txt = item["options"][self.settings[item["key"]]]
            elif kind == "action":
                val_txt = "> edit"
            elif kind == "cycle_fav":
                favs = self.loc_state["favourites"]
                if 0 <= self.fav_cursor < FAV_COUNT and favs[self.fav_cursor]["name"]:
                    val_txt = favs[self.fav_cursor]["name"]
                else:
                    val_txt = "(none)"
            else:  # danger
                val_txt = "> reset"

            name_s = self.font_xs.render(item["label"], True, name_col)
            val_s  = self.font_xs.render(val_txt,       True, val_col)

            d.blit(name_s, (_MX + 6,  ry + 4))
            d.blit(val_s,  (_MX + _MW - val_s.get_width() - 6, ry + 4))

        if num_pages > 1:
            pg_s = self.font_xs.render(f"page {page + 1}/{num_pages}", True, self.t["status"])
            d.blit(pg_s, (CX - pg_s.get_width() // 2, _ROW_Y0 + PAGE_SIZE * _ROW_H + 4))

    def _menu_row_at(self, dx, dy):
        """Return MENU_ITEMS index for display coords, or -1 if outside a row."""
        if not (_MX <= dx < _MX + _MW and _ROW_Y0 <= dy < _ROW_Y0 + PAGE_SIZE * _ROW_H):
            return -1
        slot = (dy - _ROW_Y0) // _ROW_H
        page = self.menu_sel // PAGE_SIZE
        i    = page * PAGE_SIZE + slot
        return i if i < len(MENU_ITEMS) else -1

    def _cycle_setting(self, row):
        key  = MENU_ITEMS[row]["key"]
        opts = MENU_ITEMS[row]["options"]
        self.settings[key] = (self.settings[key] + 1) % len(opts)

    # ── Location editing (Change Location / Saved Locations) ─────────────────

    def _on_home_changed(self):
        pygame.display.set_caption(f"FlightDial — {HOME_LAT:.4f}, {HOME_LON:.4f}")
        self.map_tiles.clear()  # old-location tiles are wrong for the new home
        self._do_fetch(force=True)
        self._do_fetch_map(ZOOM_STEPS[self.zoom_idx])

    def _cycle_favourite(self):
        favs = self.loc_state["favourites"]
        if not any(f["name"] for f in favs):
            return  # nothing saved yet
        for _ in range(FAV_COUNT):
            self.fav_cursor = (self.fav_cursor + 1) % FAV_COUNT
            if favs[self.fav_cursor]["name"]:
                break
        fav = favs[self.fav_cursor]
        global HOME_LAT, HOME_LON
        HOME_LAT, HOME_LON = fav["lat"], fav["lon"]
        self.loc_state["home_lat"] = HOME_LAT
        self.loc_state["home_lon"] = HOME_LON
        save_location_state(self.loc_state)
        self._on_home_changed()

    def _location_field_specs(self):
        specs = [("Home latitude", ("home_lat",)), ("Home longitude", ("home_lon",))]
        for i in range(FAV_COUNT):
            specs.append((f"Favourite {i + 1} name", ("fav", i, "name")))
            specs.append((f"Favourite {i + 1} latitude", ("fav", i, "lat")))
            specs.append((f"Favourite {i + 1} longitude", ("fav", i, "lon")))
        return specs

    def _field_current_value(self, path):
        if path[0] == "home_lat":
            return f"{HOME_LAT:.6f}"
        if path[0] == "home_lon":
            return f"{HOME_LON:.6f}"
        _, idx, sub = path
        fav = self.loc_state["favourites"][idx]
        return fav["name"] if sub == "name" else f"{fav[sub]:.6f}"

    def _commit_field(self, path, raw):
        global HOME_LAT, HOME_LON
        kind = path[0]
        if kind in ("home_lat", "home_lon"):
            try:
                val = float(raw)
            except ValueError:
                return
            limit = 90.0 if kind == "home_lat" else 180.0
            if not (-limit <= val <= limit):
                print(f"[Location] {kind} out of range, ignoring")
                return
            if kind == "home_lat":
                HOME_LAT = val
            else:
                HOME_LON = val
            self.loc_state[kind] = val
            self._on_home_changed()
        else:
            _, idx, sub = path
            fav = self.loc_state["favourites"][idx]
            if sub == "name":
                fav["name"] = raw.strip()
            else:
                try:
                    val = float(raw)
                except ValueError:
                    return
                limit = 90.0 if sub == "lat" else 180.0
                if not (-limit <= val <= limit):
                    print(f"[Location] favourite {sub} out of range, ignoring")
                    return
                fav[sub] = val
        save_location_state(self.loc_state)

    def _start_location_wizard(self):
        self.input_specs  = self._location_field_specs()
        self.input_index  = 0
        self.input_buffer = self._field_current_value(self.input_specs[0][1])
        self.input_active = True

    def _input_advance(self):
        path = self.input_specs[self.input_index][1]
        self._commit_field(path, self.input_buffer)
        self.input_index += 1
        if self.input_index >= len(self.input_specs):
            self.input_active = False
        else:
            self.input_buffer = self._field_current_value(self.input_specs[self.input_index][1])

    def _draw_input_wizard(self):
        d = self.disp
        pw, ph = _MW, 90
        px, py = _MX, CY - ph // 2
        panel = pygame.Surface((pw, ph), pygame.SRCALPHA)
        panel.fill((0, 0, 0, 220))
        d.blit(panel, (px, py))
        pygame.draw.rect(d, self.t["ring_lbl"], pygame.Rect(px, py, pw, ph), 1, border_radius=6)

        label = self.input_specs[self.input_index][0]
        title = self.font_sm.render(label, True, self.t["ring_lbl"])
        d.blit(title, (CX - title.get_width() // 2, py + 8))

        cursor = "_" if int(time.time() * 2) % 2 == 0 else " "
        val = self.font_sm.render(self.input_buffer + cursor, True, self.t["sel"])
        d.blit(val, (CX - val.get_width() // 2, py + 32))

        hint = self.font_xs.render(
            f"{self.input_index + 1}/{len(self.input_specs)}  Enter=next  Esc=cancel",
            True, self.t["status"])
        d.blit(hint, (CX - hint.get_width() // 2, py + 62))

    def _draw_confirm_reset(self):
        d = self.disp
        pw, ph = _MW, 100
        px, py = _MX, CY - ph // 2
        panel = pygame.Surface((pw, ph), pygame.SRCALPHA)
        panel.fill((0, 0, 0, 230))
        d.blit(panel, (px, py))
        pygame.draw.rect(d, (220, 80, 80), pygame.Rect(px, py, pw, ph), 1, border_radius=6)

        title = self.font_sm.render("FACTORY RESET", True, (220, 80, 80))
        d.blit(title, (CX - title.get_width() // 2, py + 8))

        ly = py + 30
        for line in ("Erases saved locations,", "favourites & settings"):
            s = self.font_xs.render(line, True, self.t["status"])
            d.blit(s, (CX - s.get_width() // 2, ly))
            ly += 14

        hint = self.font_xs.render("Y = confirm    N/Esc = cancel", True, self.t["ring_lbl"])
        d.blit(hint, (CX - hint.get_width() // 2, py + ph - 18))

    # Mirrors factoryReset() in provisioning.cpp: wipes saved location,
    # favourites, and settings back to the compiled-in defaults.
    def _do_factory_reset(self):
        self.settings = dict(DEFAULTS)

        if os.path.exists(STATE_FILE):
            try:
                os.remove(STATE_FILE)
            except Exception as exc:
                print(f"[Location] failed to remove {STATE_FILE}: {exc}")
        self.loc_state  = load_location_state()
        self.fav_cursor = -1

        global HOME_LAT, HOME_LON
        HOME_LAT, HOME_LON = DEFAULT_HOME_LAT, DEFAULT_HOME_LON
        self._on_home_changed()

        self.confirm_reset = False
        print("[Reset] Factory reset complete")

    # ── Bezel ─────────────────────────────────────────────────────────────────

    def _render_bezel(self):
        pygame.draw.circle(self.screen, self.t["bezel_o"], (BEZ_CX, BEZ_CY), CX + BEZEL - 4)
        pygame.draw.circle(self.screen, self.t["bezel_i"], (BEZ_CX, BEZ_CY), CX + 7)

    # ── Hit test (aircraft) ───────────────────────────────────────────────────

    def _hit_test(self, dx, dy, ac_list):
        radius_km = ZOOM_STEPS[self.zoom_idx]
        for i in range(len(ac_list) - 1, -1, -1):
            sx, sy = world_to_screen(ac_list[i].lat, ac_list[i].lon,
                                     HOME_LAT, HOME_LON, radius_km)
            if (dx - sx) ** 2 + (dy - sy) ** 2 <= 144:
                return i
        return -1

    # ── Main loop ─────────────────────────────────────────────────────────────

    def run(self):
        self._do_fetch(force=True)
        self._do_fetch_map(ZOOM_STEPS[self.zoom_idx])
        last_fetch = time.time()

        while True:
            for ev in pygame.event.get():
                if ev.type == pygame.QUIT:
                    pygame.quit(); return

                elif ev.type == pygame.KEYDOWN:
                    if self.input_active:
                        if ev.key == pygame.K_ESCAPE:
                            self.input_active = False
                        elif ev.key == pygame.K_RETURN:
                            self._input_advance()
                        elif ev.key == pygame.K_BACKSPACE:
                            self.input_buffer = self.input_buffer[:-1]
                        else:
                            ch = ev.unicode
                            is_name = self.input_specs[self.input_index][0].endswith("name")
                            if ch and is_name and (ch.isalnum() or ch in " '-") and len(self.input_buffer) < 23:
                                self.input_buffer += ch
                            elif ch and not is_name and (ch.isdigit() or ch in ".-") and len(self.input_buffer) < 15:
                                self.input_buffer += ch
                    elif self.confirm_reset:
                        if ev.key == pygame.K_y:
                            self._do_factory_reset()
                        elif ev.key in (pygame.K_n, pygame.K_ESCAPE):
                            self.confirm_reset = False
                    elif ev.key in (pygame.K_ESCAPE, pygame.K_q):
                        if self.show_menu:
                            self.show_menu = False
                        else:
                            pygame.quit(); return

                elif ev.type == pygame.MOUSEWHEEL:
                    if self.show_menu:
                        self.menu_sel = (self.menu_sel + (-1 if ev.y > 0 else 1)) % len(MENU_ITEMS)
                    else:
                        self.zoom_idx = max(0, min(len(ZOOM_STEPS) - 1,
                                                   self.zoom_idx + (1 if ev.y > 0 else -1)))
                        self.selected = -1
                        self._do_fetch()
                        self._do_fetch_map(ZOOM_STEPS[self.zoom_idx])
                        last_fetch = time.time()

                elif ev.type == pygame.MOUSEBUTTONDOWN:
                    mx, my = ev.pos
                    dx, dy = mx - BEZEL, my - BEZEL
                    in_circle = (dx - CX) ** 2 + (dy - CY) ** 2 <= CX ** 2

                    if ev.button == 3:  # right-click = toggle menu
                        if in_circle:
                            self.show_menu = not self.show_menu
                            self.menu_sel  = 0

                    elif ev.button == 1:  # left-click
                        if self.input_active:
                            pass  # keyboard-only while the wizard is open
                        elif self.confirm_reset:
                            pass  # keyboard-only (Y/N) while the confirm is open
                        elif self.show_menu:
                            row = self._menu_row_at(dx, dy)
                            if row >= 0:
                                item = MENU_ITEMS[row]
                                kind = item.get("kind", "cycle")
                                self.menu_sel = row
                                if kind == "cycle":
                                    self._cycle_setting(row)
                                elif kind == "action":
                                    self.show_menu = False
                                    self._start_location_wizard()
                                elif kind == "cycle_fav":
                                    self._cycle_favourite()
                                else:  # danger
                                    self.show_menu = False
                                    self.confirm_reset = True
                            else:
                                self.show_menu = False
                        elif in_circle:
                            radius_km  = ZOOM_STEPS[self.zoom_idx]
                            display_ac = _demo_aircraft(radius_km) if self.demo_mode else self.aircraft
                            hit = self._hit_test(dx, dy, display_ac)
                            self.selected = -1 if hit == self.selected else hit

            # Auto-refresh
            refresh_s = REFRESH_OPTIONS[self.settings["refresh"]]
            if time.time() - last_fetch >= refresh_s:
                self._do_fetch()
                last_fetch = time.time()

            # Draw
            self.screen.fill(C_WIN_BG)
            self._render_bezel()
            self._render_frame()
            self.screen.blit(self.disp, (BEZEL, BEZEL))

            hint_txt = ("type value, Enter=next field, Esc=cancel" if self.input_active
                        else "scroll=zoom   click=select   right-click=settings   Q=quit")
            hint = self.font_sm.render(hint_txt, True, (50, 50, 50))
            self.screen.blit(hint, (WIN_W // 2 - hint.get_width() // 2, WIN_H - 18))

            pygame.display.flip()
            self.clock.tick(30)

    def _render_frame(self):
        radius_km  = ZOOM_STEPS[self.zoom_idx]
        min_alt    = MIN_ALT_M[self.settings["minalt"]]
        airborne_only = self.settings["filter"] == 0

        self.disp.fill((*self.t["bg"], 255))
        self._draw_map(radius_km)
        self._draw_rings(radius_km)

        # Home crosshair
        pygame.draw.line(self.disp, self.t["home"], (CX - 6, CY), (CX + 6, CY))
        pygame.draw.line(self.disp, self.t["home"], (CX, CY - 6), (CX, CY + 6))
        pygame.draw.circle(self.disp, self.t["home"], (CX, CY), 3, 1)

        display_ac = _demo_aircraft(radius_km) if self.demo_mode else self.aircraft

        visible = 0
        for i, ac in enumerate(display_ac):
            if airborne_only and ac.on_ground:
                continue
            if ac.alt_m > 0 and ac.alt_m < min_alt:
                continue
            sx, sy = world_to_screen(ac.lat, ac.lon, HOME_LAT, HOME_LON, radius_km)
            if (sx - CX) ** 2 + (sy - CY) ** 2 > PLOT_R ** 2:
                continue
            self._draw_aircraft(ac, sx, sy, i == self.selected)
            visible += 1

        self._draw_status_bar(visible, radius_km)
        self._draw_poll_icon()

        if self.demo_mode:
            lbl = self.font_sm.render("DEMO", True, (255, 200, 0), (60, 40, 0))
            self.disp.blit(lbl, (CX - lbl.get_width() // 2, 118))

        if 0 <= self.selected < len(display_ac) and not self.show_menu:
            self._draw_detail(display_ac[self.selected])

        if self.show_menu:
            self._draw_menu()

        if self.input_active:
            self._draw_input_wizard()

        if self.confirm_reset:
            self._draw_confirm_reset()

        self.disp.blit(self._clip, (0, 0), special_flags=pygame.BLEND_RGBA_MIN)


if __name__ == "__main__":
    Emulator().run()
