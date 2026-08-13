# Building From Source

## Requirements

- [PlatformIO](https://platformio.org/) (CLI or the VS Code extension)
- An M5Stack M5Dial for testing on real hardware

## Project layout

```
flight-radar/
├── platformio.ini      # board config: esp32-s3-devkitc-1, 8MB flash, LittleFS
├── src/
│   ├── main.cpp         # Arduino setup()/loop(), input handling, fetch scheduling, follow mode
│   ├── config.h         # tunable constants: zoom steps, encoder timing, refresh, interpolation, branding
│   ├── aircraft.h       # Aircraft struct + emergency-squawk check
│   ├── apistatus.h/.cpp # shared fetch-outcome status (poll icon / status panel)
│   ├── adsblive.h/.cpp  # airplanes.live REST client — the flight-data source
│   ├── aircraftfeed.h/.cpp # runs that fetch on its own FreeRTOS task, off the UI thread
│   ├── map.h/.cpp        # OSM tile fetch, compositing, and LittleFS caching
│   ├── lofimap.h/.cpp    # reader for the embedded lo-fi vector map blob
│   ├── geolocate.h/.cpp  # IP auto-detect + place-name geocoding
│   ├── radar.h/.cpp      # all on-screen rendering (rings, aircraft, trails, lo-fi map, overlays, themes)
│   ├── settings.h/.cpp   # settings menu UI + persisted settings state
│   ├── provisioning.h/.cpp  # WiFiManager captive portal, NVS persistence, factory reset
│   ├── ota.h/.cpp        # pull-based OTA: version/changelog check + HTTPUpdate flash, triggered from the web config page
│   ├── watchdog.h/.cpp   # auto-reset if the main loop ever stalls (wedged blocking call)
│   └── encoder_debounce.h  # rotary encoder debouncing (see its own extensive comments)
├── assets/lofimap.bin   # embedded lo-fi vector map data (generated; linked into flash)
├── tools/
│   ├── build_lofimap.py  # regenerates lofimap.bin from Natural Earth GeoJSON
│   └── embed_lofimap.py  # PlatformIO pre-build hook: objcopy the blob into the firmware
├── docs/                # this documentation site (MkDocs)
└── mkdocs.yml
```

## Build & flash

```bash
pio run                                    # build only
pio run -t upload                          # build + flash
pio run -t upload --upload-port <port>     # flash to a specific serial port
```

## Cutting a release

Bump `FIRMWARE_VERSION` in `src/config.h` and add a matching `## X.Y` section to [`docs/changelog.md`](changelog.md) describing what changed, then push to `main`. `.github/workflows/docs.yml` builds the firmware, extracts that version and changelog section, and publishes `firmware.bin`, `version.txt`, and `changelog.txt` to GitHub Pages alongside the docs — the same deploy, not a separate one, since `actions/deploy-pages` replaces the whole site on every run. The CI build fails if `FIRMWARE_VERSION` has no matching changelog section, so it's not possible to ship an update with no changenotes. Devices already in the field pick it up next time someone opens their web config page and taps **Check for Firmware Updates** — see [Using the Device](using-the-device.md#firmware-updates).

## Reading serial output

`pio device monitor` requires an interactive terminal and will fail in some non-TTY environments (CI, some remote/background shells) with a `termios.error`. A portable alternative is a short Python script using [pyserial](https://pyserial.readthedocs.io/):

```python
import serial, time
ser = serial.Serial('/dev/cu.usbmodem101', 115200, timeout=1)
end = time.time() + 30
while time.time() < end:
    line = ser.readline()
    if line:
        print(line.decode('utf-8', 'replace'), end='')
```

Note that after a fresh flash, the ESP32-S3's native USB CDC interface takes a moment to re-enumerate — the very first few log lines immediately after reset are often missed if you attach too quickly.

## Debug serial commands

The firmware accepts a set of debug commands over the same USB serial connection used for logging — pose hooks for driving the device (for the manual's screenshots) plus diagnostics for on-device measurement. See `checkSerialCommands()` in `main.cpp` for the full list; the most useful are:

| Command | Purpose |
|---|---|
| `INFO` | State dump: aircraft count, zoom, selection, follow state, position, and the health counters below |
| `PERF` | Times one full radar composite+push, so animation cadence can be set from a measured frame cost |
| `TRAILS` | Dumps the breadcrumb store, including each trail's span in km **and in pixels at the current zoom** — the number that decides whether a trail is visible at all rather than hidden under the aircraft mark |
| `ENC` | Streams raw encoder counts for 6&nbsp;s, so a physical click's tick count can be measured rather than assumed |
| `LIST` | Current aircraft with position, speed and altitude |
| `BATT` | Raw power-IC readings (level, voltage, current, charging) behind the battery gauge's show/hide decision |
| `SHOT` | Streams the framebuffer for screenshot capture |

`INFO` reports three health counters worth knowing:

- **`loophz`** — `loop()` iterations per second. Touch is only sampled once per iteration, so this bounds input responsiveness. Healthy is ~100&nbsp;kHz; it collapsed below 1&nbsp;Hz when the network fetch still ran inline on the UI thread.
- **`maxalloc`** — largest *contiguous* free block. This, not total free heap, is what predicts TLS handshake failures.
- **`feedstack`** — unused stack on the fetch task, for checking mbedTLS headroom against real traffic.

## Regenerating the lo-fi map data

The [lo-fi vector map](map-caching.md#the-lo-fi-vector-map) is an embedded binary (`assets/lofimap.bin`, ~292&nbsp;KB) built from public-domain [Natural Earth](https://www.naturalearthdata.com/) 1:50m layers. It's committed to the repo, so a normal build just links it in — you only need to rebuild it to change the layers, detail, or city set:

```bash
# downloads the Natural Earth GeoJSON, simplifies + packs the blob
python3 tools/build_lofimap.py --src <dir-with-geojson> --out assets/lofimap.bin
```

Tuning knobs live at the top of `tools/build_lofimap.py` (`TOL`, the per-layer Douglas–Peucker simplification tolerances). At build time, `tools/embed_lofimap.py` (a PlatformIO `pre:` script wired up in `platformio.ini`) `objcopy`s the blob into a linkable object exposing `_binary_lofimap_bin_start/_end`, which `lofimap.cpp` reads directly from flash — `board_build.embed_files` only works under the ESP-IDF framework, not the Arduino one used here.

## Hardware constraints worth knowing before changing things

- **No PSRAM.** The ESP32-S3FN8 on the M5Dial has ~320&nbsp;KB of usable RAM total. The map sprite (115&nbsp;KB, RGB565, always resident) and the PNG decoder's ~44&nbsp;KB working buffer are the two largest consumers; TLS handshakes for HTTPS requests also need a non-trivial contiguous allocation. If you're debugging a crash or an intermittent `SSL - Memory allocation failed`, start by checking `ESP.getMaxAllocHeap()` (largest contiguous block) rather than `ESP.getFreeHeap()` — the total can look healthy while no single allocation of the needed size is possible. Both are reported by the `INFO` debug command.
- **The flight-data fetch runs on its own FreeRTOS task** (`aircraftfeed.h`), pinned to core 0 alongside the WiFi stack, while `loop()` runs on core 1. Nothing on the UI thread may block on the network: a synchronous fetch there stops touch sampling and encoder polling for its whole duration, which is what made the device feel frozen. The two sides hand off a single `std::vector<Aircraft>` by swapping it under a mutex; both buffers are `reserve()`d once at startup so neither ever reallocates and fragments the contiguous block the next TLS handshake needs.
- **Three different network timeouts, easily confused.** `client.setTimeout()` is `Stream::setTimeout` (milliseconds, per body read — `NetworkClient` does *not* override it), `setConnectionTimeout()` bounds the TCP connect, and `setHandshakeTimeout()` bounds the TLS handshake and is in **seconds**, defaulting to 120. All three need setting; a `setTimeout(10)` intended as 10 seconds is actually 10 **milliseconds** and will truncate every response on a slow link.
- **The M5Dial exposes no battery voltage to the ESP32** — measured, not assumed. `M5.Power.getBatteryLevel()` returns `-2` and `getBatteryVoltage()` returns 0, because M5Unified configures no battery sensing for `board_M5Dial` (its pin-identical sibling the M5DinMeter *is* configured, reading a 1:2 divider on GPIO10). GPIO10 is also the only ADC1 pin M5Dial leaves free — GPIO4-9 drive the GC9A01 display, GPIO1-3 are taken — but it reads 4095 counts, saturated at the 3.3&nbsp;V rail; a 4.1&nbsp;V cell behind a 1:2 divider would read ~2050&nbsp;mV. ADC2 is unusable because WiFi owns it. So the battery gauge stays hidden on this board by design; it will appear unchanged if a source ever reports through `M5.Power`. Check with the `BATT` command before assuming the gauge is broken.
- **USB serial only enumerates at boot.** The build sets `ARDUINO_USB_CDC_ON_BOOT=1`, so the port appears during startup. With a battery fitted the device boots on battery power, and plugging USB in afterwards will *not* bring up the port — press the physical RST button (not the encoder, which factory-resets on a 3&nbsp;s hold) with the cable already attached.
- **The rotary encoder is noisy.** See the extensive comments in `encoder_debounce.h` — this hardware has been observed to produce spurious single ticks. `EncoderDebouncer` rejects them with hysteresis — a step is only emitted once the raw count moves a full detent — which is both instant and more selective than the settling timer it replaced. Any new encoder-driven UI should reuse it rather than reading `M5Dial.Encoder.read()` directly.

## Contributing

Issues and pull requests are welcome on the [GitHub repository](https://github.com/pharcyd3/flight-radar).
