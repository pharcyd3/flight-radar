# FlightDial

**FlightDial** turns an [M5Dial](https://docs.m5stack.com/en/core/M5Dial) (an ESP32-S3 round-screen dev board with a rotary encoder and touchscreen) into a live, standalone aircraft radar for your desk — no phone, no app, no browser tab.

It polls [OpenSky Network](https://opensky-network.org/)'s public flight-tracking API for real ADS-B traffic around a home location you choose, and plots it on a radar-style display with a real [OpenStreetMap](https://www.openstreetmap.org/) tile underlay.

## What it does

- **Live radar display** — aircraft plotted by position, heading, altitude, and speed, refreshed on a timer you control
- **Real map underlay** — OpenStreetMap tiles fetched once per location/zoom and cached to flash, so it doesn't need to be redownloaded every time
- **Rotary zoom** — five range rings from 10&nbsp;km to 200&nbsp;km, one twist per step
- **Touch to inspect** — tap any aircraft for its callsign, altitude, speed, heading, and country of registration
- **Emergency squawk detection** — 7500 (hijack), 7600 (radio failure), and 7700 (general emergency) are highlighted in red with an on-screen ring flash and an audible alert
- **Configurable everything** — colour themes, units, flight labels, altitude/traffic filters, refresh rate, and more, all from an on-device settings menu
- **Saved locations** — store up to three favourite locations and jump between them instantly (their maps are pre-cached in the background)

## Hardware

| | |
|---|---|
| Board | M5Stack **M5Dial** (ESP32-S3, 8&nbsp;MB flash, no PSRAM) |
| Display | 1.28" round, 240×240, capacitive touch |
| Input | Rotary encoder (zoom) + physical button (settings/reset) + touchscreen |
| Connectivity | WiFi (2.4&nbsp;GHz) |

## Where to start

- New device, never flashed? Start with **[Getting Started](getting-started.md)**.
- Device flashed and connected to WiFi, but no aircraft showing? You need an OpenSky API client — see **[OpenSky API Setup](opensky-setup.md)**.
- Already running and want to know what a button/setting does? See **[Using the Device](using-the-device.md)** and **[Settings Reference](settings-reference.md)**.
- Something's not working? Check **[Troubleshooting](troubleshooting.md)** first.

## A note on data sources

FlightDial's positional data comes entirely from [OpenSky Network](https://opensky-network.org/), a non-profit, community-driven ADS-B receiver network. Unlike commercial aggregators, OpenSky's coverage depends on volunteer ground receivers — so in areas with sparse coverage, you may occasionally see no traffic even when aircraft are genuinely nearby. See [Troubleshooting](troubleshooting.md#no-aircraft-showing) for what to expect.

Map tiles are © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors.
