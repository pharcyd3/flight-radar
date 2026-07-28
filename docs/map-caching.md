# Map & Caching

The **Map** setting (Settings → Map) chooses the background style: **Full** (the OpenStreetMap street map described below), **Lo-fi** (an offline vector map), or **Off** (a plain scope). This page is mostly about the Full raster map and its caching; the [lo-fi map](#the-lo-fi-vector-map) is covered at the end.

## Where the map comes from

Frank's Flight Radar renders a real street map underneath the radar rings and aircraft, built from [OpenStreetMap](https://www.openstreetmap.org/) raster tiles (`tile.openstreetmap.org`). There's no map for the *whole world* stored on the device — tiles are fetched for the current home location and zoom radius, stitched together, and composited into a 240×240 sprite that becomes the radar's background.

## Caching

Every composed map is written to the device's flash filesystem (LittleFS, on an ~1.5&nbsp;MB partition) and is **content-addressed** by the exact latitude, longitude, and radius it was built for. This means:

- Switching between a zoom level or location you've visited before **loads instantly from flash** — no network fetch needed.
- A genuinely new combination (new zoom level, new location) fetches fresh tiles over WiFi, which takes a few seconds (a "loading map..." message is shown while this happens).
- The cache never needs to be manually cleared for correctness — old entries for locations/zooms you no longer use just sit unused. If the cache partition ever fills up, the **least-recently-written** entry is automatically evicted to make room for a new one.

Saved [favourite locations](favourite-locations.md) are pre-cached automatically as soon as you save them, so switching to one is always instant.

## Why tiles are decoded one at a time

The M5Dial's ESP32-S3 has no PSRAM — the map sprite alone (115&nbsp;KB) plus PNG decode buffers plus the WiFi/TLS stack all have to share a ~320&nbsp;KB RAM budget. Frank's Flight Radar streams each tile straight to a small flash-backed scratch file and decodes it directly from there, rather than holding a fully-buffered image in RAM, and the PNG decoder's own ~44&nbsp;KB working buffer is deliberately released again right after a map composes, so it doesn't sit around competing with the more frequent flight-data network requests for the same scarce memory.

## The lo-fi vector map

Set **Map → Lo-fi** for a lightweight, offline alternative to the street map: **coastlines, national borders, rivers, and lakes** drawn as themed lines, with labels for **major cities** and **airports** (by IATA code). Unlike the raster map, it has nothing to fetch or cache — the entire world (simplified [Natural Earth](https://www.naturalearthdata.com/) data, public domain) is **baked into the firmware** and rendered directly.

![The offline lo-fi vector map with coastlines, aircraft, and airports](images/radar-lofi.png)

That makes it a good fit when you want geographic context without the weight of the street map:

- **Works anywhere, offline** — no tiles to download, so it's instant at every location and zoom, including places you've never visited.
- **No "loading map…" pauses** and no credit or bandwidth cost.
- **Smooth while following** — because there's nothing to recompose, [following an aircraft](using-the-device.md#following-an-aircraft) re-centres instantly, with none of the periodic map reloads the Full map does.
- **Themed** — the lines recolour with your chosen [colour theme](settings-reference.md#colour-theme).

It's deliberately **low-detail** — a scope-style geographic reference, not a substitute for the street map's roads and place detail. Coastlines are coarse and only larger towns are labelled.
