#!/usr/bin/env python3
"""Build the FlightDial lo-fi vector map blob from Natural Earth 1:50m data.

Reads five GeoJSON layers, simplifies the geometry (Douglas-Peucker) to a lo-fi
resolution, quantises coordinates to int16, and packs a compact little-endian
binary the firmware memory-maps and renders as themed lines + city labels.

Binary format (all little-endian):
  header:  'L','F','M','1', u8 version=1, u8 reserved,
           i16 fullscale (=32767 -> 180 deg), u32 nLines, u32 nCities
  lines:   per record: u8 layer, i16 minLon,minLat,maxLon,maxLat,
                        u16 nPts, (i16 lon, i16 lat)*nPts
  cities:  per record: i16 lon, i16 lat, u8 rank, u8 nameLen, char name[nameLen]

Coordinate quantisation: q = round(deg * 32767 / 180); deg = q * 180/32767
(~611 m). Layers: 0 = coastline, 1 = border, 2 = water (rivers + lakes).

Usage: build_lofimap.py [--src DIR] [--out FILE]
"""
import argparse, json, math, os, struct, sys, unicodedata

LAYER_COAST, LAYER_BORDER, LAYER_WATER = 0, 1, 2
Q = 32767.0 / 180.0            # degrees -> int16

# Simplification tolerance per layer, in degrees (~111 km/deg). Bigger = coarser.
TOL = {LAYER_COAST: 0.02, LAYER_BORDER: 0.02, LAYER_WATER: 0.03}

FILES = {
    "ne_50m_coastline.geojson":                    LAYER_COAST,
    "ne_50m_admin_0_boundary_lines_land.geojson":  LAYER_BORDER,
    "ne_50m_rivers_lake_centerlines.geojson":      LAYER_WATER,
    "ne_50m_lakes.geojson":                        LAYER_WATER,
}
PLACES   = "ne_50m_populated_places_simple.geojson"
AIRPORTS = "ne_10m_airports.geojson"   # only exists at 1:10m; ~890 major airports


def linestrings(geom):
    """Yield lists of (lon,lat) for any geometry type (lines and polygon rings)."""
    t = geom.get("type")
    c = geom.get("coordinates")
    if t == "LineString":
        yield c
    elif t == "MultiLineString":
        yield from c
    elif t == "Polygon":
        yield from c                       # each ring, closed
    elif t == "MultiPolygon":
        for poly in c:
            yield from poly


def perp2(p, a, b):
    """Squared perpendicular distance from p to segment a-b (planar degrees)."""
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    if dx == 0 and dy == 0:
        return (px - ax) ** 2 + (py - ay) ** 2
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    cx, cy = ax + t * dx, ay + t * dy
    return (px - cx) ** 2 + (py - cy) ** 2


def douglas_peucker(pts, tol):
    if len(pts) < 3:
        return pts
    tol2 = tol * tol
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        s, e = stack.pop()
        dmax, idx = 0.0, -1
        for i in range(s + 1, e):
            d = perp2(pts[i], pts[s], pts[e])
            if d > dmax:
                dmax, idx = d, i
        if idx != -1 and dmax > tol2:
            keep[idx] = True
            stack.append((s, idx))
            stack.append((idx, e))
    return [p for p, k in zip(pts, keep) if k]


def quantise(pts):
    """Degrees -> unique consecutive int16 (lon,lat) pairs."""
    out = []
    for lon, lat in pts:
        qx = max(-32767, min(32767, round(lon * Q)))
        qy = max(-32767, min(32767, round(lat * Q)))
        if not out or out[-1] != (qx, qy):
            out.append((qx, qy))
    return out


def transliterate(name):
    """Best-effort ASCII (the device font has no accented glyphs)."""
    s = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode("ascii")
    return s.strip()[:24]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="src_geojson", help="dir with the NE GeoJSON files")
    ap.add_argument("--out", default="lofimap.bin")
    args = ap.parse_args()

    lines = []   # (layer, [(qx,qy),...])
    for fname, layer in FILES.items():
        path = os.path.join(args.src, fname)
        with open(path) as f:
            gj = json.load(f)
        n0 = len(lines)
        for feat in gj["features"]:
            geom = feat.get("geometry")
            if not geom:
                continue
            for ring in linestrings(geom):
                simplified = douglas_peucker([(p[0], p[1]) for p in ring], TOL[layer])
                q = quantise(simplified)
                if len(q) >= 2:
                    lines.append((layer, q))
        print(f"  {fname}: +{len(lines)-n0} linestrings")

    # Cities: keep all named places, ranked so the device can pick the most
    # prominent when several fall in view. scalerank 0 = world city, higher = minor.
    cities = []
    with open(os.path.join(args.src, PLACES)) as f:
        gj = json.load(f)
    for feat in gj["features"]:
        pr = feat.get("properties", {})
        name = transliterate(pr.get("name") or "")
        if not name:
            continue
        lon, lat = feat["geometry"]["coordinates"][:2]
        rank = int(pr.get("scalerank", 10))
        pop = int(pr.get("pop_max", 0) or 0)
        cities.append((rank, -pop, name, lon, lat))
    cities.sort()   # most prominent first
    print(f"  {PLACES}: {len(cities)} cities")

    # Airports: labelled by IATA code (e.g. LHR), ranked by scalerank so the
    # device can prefer the biggest hubs when several fall in view.
    airports = []
    with open(os.path.join(args.src, AIRPORTS)) as f:
        gj = json.load(f)
    for feat in gj["features"]:
        pr = feat.get("properties", {})
        code = transliterate(pr.get("iata_code") or pr.get("abbrev") or "")
        if not code or len(code) > 4:
            continue
        lon, lat = feat["geometry"]["coordinates"][:2]
        rank = int(pr.get("scalerank", 9) or 9)
        airports.append((rank, code, lon, lat))
    airports.sort()
    print(f"  {AIRPORTS}: {len(airports)} airports")

    # A city/airport record: i16 lon, i16 lat, u8 rank, u8 nameLen, name.
    def point_record(lon, lat, rank, name):
        qx = max(-32767, min(32767, round(lon * Q)))
        qy = max(-32767, min(32767, round(lat * Q)))
        nb = name.encode("ascii")
        return struct.pack("<hhBB", qx, qy, min(rank, 255), len(nb)) + nb

    # ── pack ──
    buf = bytearray()
    buf += struct.pack("<4sBBhIII", b"LFM2", 2, 0, 32767,
                       len(lines), len(cities), len(airports))
    npts = 0
    for layer, q in lines:
        xs = [p[0] for p in q]; ys = [p[1] for p in q]
        buf += struct.pack("<Bhhhh H", layer, min(xs), min(ys), max(xs), max(ys), len(q))
        for qx, qy in q:
            buf += struct.pack("<hh", qx, qy)
        npts += len(q)
    for rank, negpop, name, lon, lat in cities:
        buf += point_record(lon, lat, rank, name)
    for rank, code, lon, lat in airports:
        buf += point_record(lon, lat, rank, code)

    with open(args.out, "wb") as f:
        f.write(buf)

    print(f"\nlines={len(lines)}  points={npts}  cities={len(cities)}  airports={len(airports)}")
    print(f"OUTPUT {args.out}: {len(buf)} bytes ({len(buf)/1024:.0f} KB)")


if __name__ == "__main__":
    main()
