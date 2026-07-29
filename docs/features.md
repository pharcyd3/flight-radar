# Features

An exhaustive tour of what Frank's Flight Radar does, grouped by area. Each item links to the page where it's explained in full where relevant.

## Live radar display

- **Real ADS-B traffic** from [airplanes.live](https://airplanes.live/)'s free community network, plotted by position around a home location you choose.
- **Five zoom ranges** — 10, 25, 50, 100, and 200&nbsp;km radius, selected with the rotary encoder. A row of dots near the top shows the current level at a glance.
- **Smooth motion (dead reckoning)** — between polls, airborne aircraft *glide* along their heading at their reported ground speed instead of jumping on each refresh. Position is extrapolated for up to two minutes, so a missed poll never flings a stale mark across the screen.
- **Heading arrows** — moving aircraft show a short arrow in their direction of travel.
- **Position trails** — the selected aircraft leaves a fading breadcrumb trail of its recent reported positions. (Shares the **Heading trails** setting.)
- **Colour-coded state** — airborne, on-ground (grey), selected (accent), and emergency (red, with an extra ring).
- **Selectable aircraft icon** — plain dots with a heading arrow, or heading-oriented plane shapes.
- **Callsign labels** — show none, only the selected aircraft, or all of them.
- **Range rings** — three concentric rings with an **N** tick and distance labels (toggleable).
- **Poll icon** — a small ring that counts down to the next refresh, goes solid while a request is in flight, and turns solid red if the last request failed.

See [Using the Device](using-the-device.md).

## Maps & background

Three selectable **Map** modes (Settings → Map):

- **Lo-fi** *(default)* — an **offline vector map**: coastlines, national borders, rivers, and lakes drawn as themed lines, plus labels for major cities and airports. The whole world is baked into the firmware, so it works **anywhere with no network**, switches instantly, and stays smooth even while following a moving aircraft.
- **Full** — a real [OpenStreetMap](https://www.openstreetmap.org/) raster-tile street map, fetched per location/zoom and cached to flash. See [Map & Caching](map-caching.md).
- **Off** — a plain themed background (classic radar-scope look).

Supporting behaviour:

- **Flash tile cache** — raster maps are content-addressed by location + zoom, so revisiting a view is instant.
- **Background precaching** — while idle, the device quietly caches the *other* zoom levels (and your saved favourites) so switching is instant.

## Aircraft interaction

- **Tap to inspect** — tap any aircraft for a detail panel: callsign, ICAO24 address, altitude, speed, heading, country of registration, and a **GND** flag when on the ground.
- **Rotate to cycle** — with an aircraft selected, the dial steps the selection through the visible aircraft instead of zooming.
- **Follow mode** — tap **FOLLOW** in the detail panel to lock onto an aircraft; verified in flight to hold a tracked aircraft within ~100&nbsp;m of centre, and to coast through multi-minute coverage gaps and re-acquire. The view stays glued to it (a reticle marks the tracked point, home becomes an offset marker, and its altitude/speed print below its mark) as it moves. Follow is a locked mode — ordinary taps don't back out of it — with its own controls: **UNFOLLOW** to stop, **HIDE OTHERS** to declutter down to just the tracked plane, and **drag anywhere to pan** the view (e.g. to see something the buttons are covering) — while panned, a reticle icon appears on the right edge and a plain tap anywhere recentres. Ends automatically if the aircraft leaves coverage. See [Using the Device](using-the-device.md#following-an-aircraft).
- **Emergency squawk detection** — 7500 (hijack), 7600 (radio failure), and 7700 (general emergency) are highlighted in red with a bezel ring-flash and an optional buzzer. See [Emergency Alerts](emergency-alerts.md).

## Location & setup

- **Captive-portal setup** — first boot broadcasts a `Franks-Flight-Radar-Setup` WiFi network with a browser-based wizard for WiFi and location. See [Getting Started](getting-started.md).
- **IP auto-detect** — leave the location fields blank and the device locates itself (approximately, from its public IP) once online. No coordinate lookup needed.
- **Place-name search** — type a place (e.g. `Berlin`) instead of coordinates; it's geocoded via OpenStreetMap after the device reconnects.
- **Manual coordinates** — enter precise decimal lat/lon if you prefer.
- **Detect location** (Settings) — re-run IP auto-detect any time, e.g. after moving.
- **Saved favourites** — store up to three named locations and jump between them instantly (their maps are pre-cached). See [Favourite Locations](favourite-locations.md).

## Data & connectivity

- **Keyless flight data** — [airplanes.live](https://airplanes.live/)'s free community ADS-B API. No account, no API key, nothing to configure.
- **Selectable refresh rate** — 5/8/15/30&nbsp;s, default 8&nbsp;s (well inside airplanes.live's fair-use guidance).
- **Off-thread fetching** — the network request runs on its own core, so the dial and touchscreen stay fully responsive even when the connection is slow or dropping.
- **Failure tolerance** — a failed poll keeps the last known traffic on screen (marks keep dead-reckoning) rather than blanking the radar, with the poll icon red; the next scheduled poll retries.
- **API status panel** — tap the poll icon for the last request's outcome, HTTP code, payload size, and age.
- **Memory-safe fetching** — large responses stream via a flash scratch file and parse one aircraft at a time, so the no-PSRAM device never runs out of RAM.
- **WiFi auto-reconnect** — reconnects automatically if the connection drops.

## Display & personalisation

- **Four colour themes** — Radar (green), Amber, Ocean, Neon. Everything recolours together, including the lo-fi map.
- **Units** — imperial (ft/kts) or metric (m, km/h).
- **Traffic filter** — airborne only, or include on-ground aircraft.
- **Min-altitude filter** — hide aircraft below 1,000 / 5,000 / 10,000 / 20,000&nbsp;ft.
- **Toggles** — heading trails, range rings, emergency buzzer.
- **Custom boot splash** — a plane icon over the "Frank's Flight Radar" name.

See [Settings Reference](settings-reference.md).

## System & reliability

- **Persistent settings** — every option saves to flash immediately and survives reboots and firmware updates.
- **Factory reset** — hold the encoder button 3&nbsp;seconds, or use the hold-to-confirm menu item, to wipe WiFi credentials, location, and favourites, then re-run setup.
- **Desktop emulator** — a Python/pygame emulator mirrors the on-device UI for development. See [Building From Source](development.md).
