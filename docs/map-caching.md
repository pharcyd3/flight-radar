# Map & Caching

## Where the map comes from

FlightDial renders a real street map underneath the radar rings and aircraft, built from [OpenStreetMap](https://www.openstreetmap.org/) raster tiles (`tile.openstreetmap.org`). There's no map for the *whole world* stored on the device — tiles are fetched for the current home location and zoom radius, stitched together, and composited into a 240×240 sprite that becomes the radar's background.

## Caching

Every composed map is written to the device's flash filesystem (LittleFS, on an ~1.5&nbsp;MB partition) and is **content-addressed** by the exact latitude, longitude, and radius it was built for. This means:

- Switching between a zoom level or location you've visited before **loads instantly from flash** — no network fetch needed.
- A genuinely new combination (new zoom level, new location) fetches fresh tiles over WiFi, which takes a few seconds (a "loading map..." message is shown while this happens).
- The cache never needs to be manually cleared for correctness — old entries for locations/zooms you no longer use just sit unused. If the cache partition ever fills up, the **least-recently-written** entry is automatically evicted to make room for a new one.

Saved [favourite locations](favourite-locations.md) are pre-cached automatically as soon as you save them, so switching to one is always instant.

## Why tiles are decoded one at a time

The M5Dial's ESP32-S3 has no PSRAM — the map sprite alone (115&nbsp;KB) plus PNG decode buffers plus the WiFi/TLS stack all have to share a ~320&nbsp;KB RAM budget. FlightDial streams each tile straight to a small flash-backed scratch file and decodes it directly from there, rather than holding a fully-buffered image in RAM, and the PNG decoder's own ~44&nbsp;KB working buffer is deliberately released again right after a map composes, so it doesn't sit around competing with the more frequent OpenSky network requests for the same scarce memory.

## Zoom levels and OpenSky credit cost

FlightDial has five zoom levels: **10, 25, 50, 100, and 200&nbsp;km** radius. Each corresponds to a bounding box sent to OpenSky's API, and — per [OpenSky's credit model](opensky-setup.md) — the cost of a request depends on that box's geographic area:

| Area | Credit cost |
|---|---|
| ≤ 25&nbsp;sq° | 1 |
| 25–100&nbsp;sq° | 2 |
| 100–400&nbsp;sq° | 3 |
| \>400&nbsp;sq° (global) | 4 |

At most latitudes, even FlightDial's largest 200&nbsp;km zoom stays comfortably under the 25&nbsp;sq° threshold (1 credit per request). Longitude spans widen in degrees as you go further from the equator, though — at roughly **58.7°N (or S)** and beyond, a 200&nbsp;km-radius box crosses into the 2-credit tier, effectively halving your daily request budget if you use max zoom heavily from a high-latitude home location. If you've relocated your home coordinates somewhere far north or south, it's worth checking your device's serial log for the actual bounding box being requested to confirm which tier you're in.
