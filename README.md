# Frank's Flight Radar

A live aircraft radar for the [M5Stack M5Dial](https://docs.m5stack.com/en/core/M5Dial), built on real [OpenSky Network](https://opensky-network.org/) ADS-B data with an [OpenStreetMap](https://www.openstreetmap.org/) tile underlay.

**[Read the full user guide →](https://pharcyd3.github.io/flight-radar/)**

## Quick start

```bash
git clone https://github.com/pharcyd3/flight-radar.git
cd flight-radar
pio run -t upload
```

Then follow the on-device WiFi setup portal — see [Getting Started](https://pharcyd3.github.io/flight-radar/getting-started/) for the full walkthrough, and [OpenSky API Setup](https://pharcyd3.github.io/flight-radar/opensky-setup/) for setting up reliable, authenticated flight data access.

## Project layout

- `src/` — ESP32-S3 firmware (PlatformIO/Arduino)
- `emulator/` — a desktop Python/pygame emulator mirroring the on-device UI
- `docs/` — the user guide (built with [MkDocs Material](https://squidfunk.github.io/mkdocs-material/), auto-deployed to GitHub Pages)

See [Building From Source](https://pharcyd3.github.io/flight-radar/development/) for details.
