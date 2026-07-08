# Building From Source

## Requirements

- [PlatformIO](https://platformio.org/) (CLI or the VS Code extension)
- An M5Stack M5Dial for testing on real hardware, or the bundled desktop emulator for quick iteration without hardware

## Project layout

```
flight-radar/
├── platformio.ini      # board config: esp32-s3-devkitc-1, 8MB flash, LittleFS
├── src/
│   ├── main.cpp         # Arduino setup()/loop(), input handling, fetch scheduling
│   ├── config.h         # tunable constants: zoom steps, encoder timing, refresh options
│   ├── aircraft.h       # Aircraft struct + emergency-squawk check
│   ├── opensky.h/.cpp   # OAuth2 + REST client for OpenSky's /states/all endpoint
│   ├── map.h/.cpp        # OSM tile fetch, compositing, and LittleFS caching
│   ├── radar.h/.cpp      # all on-screen rendering (rings, aircraft, overlays, themes)
│   ├── settings.h/.cpp   # settings menu UI + persisted settings state
│   ├── provisioning.h/.cpp  # WiFiManager captive portal, NVS persistence, factory reset
│   └── encoder_debounce.h  # rotary encoder debouncing (see its own extensive comments)
├── emulator/            # desktop (pygame) emulator mirroring the on-device UI
├── docs/                # this documentation site (MkDocs)
└── mkdocs.yml
```

## Build & flash

```bash
pio run                                    # build only
pio run -t upload                          # build + flash
pio run -t upload --upload-port <port>     # flash to a specific serial port
```

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

## The desktop emulator

`emulator/emulator.py` is a Python/pygame recreation of the on-device UI, useful for quickly iterating on layout and interaction logic without needing to reflash real hardware. See the comment header in that file for its controls (scroll to zoom, click to select, right-click for settings). It reads OpenSky credentials from a local `.env` file (see `emulator/.env.example`) rather than the on-device captive portal.

## Debug serial commands

The firmware accepts one debug command over the same USB serial connection used for logging:

```
SETCREDS:<client_id>:<client_secret>
```

Sets and persists OpenSky OAuth2 credentials without going through the WiFi captive portal — handy when iterating during development. See `checkSerialCommands()` in `main.cpp`.

## Hardware constraints worth knowing before changing things

- **No PSRAM.** The ESP32-S3FN8 on the M5Dial has ~320&nbsp;KB of usable RAM total. The map sprite (115&nbsp;KB, RGB565, always resident) and the PNG decoder's ~44&nbsp;KB working buffer are the two largest consumers; TLS handshakes for HTTPS requests also need a non-trivial contiguous allocation. If you're debugging a crash or an intermittent `SSL - Memory allocation failed`, start by checking `ESP.getFreeHeap()` at the point of failure.
- **The rotary encoder is noisy.** See the extensive comments in `encoder_debounce.h` — this hardware has been observed to produce spurious tick reversals well after a genuine click, which a naive "did the value change" check cannot distinguish from a real second click. Any new encoder-driven UI should reuse `EncoderDebouncer` rather than reading `M5Dial.Encoder.read()` directly.
- **OpenSky's REST API requires OAuth2**, not the Basic Auth some older examples online still reference. See `opensky.cpp`'s token-handling code and [OpenSky API Setup](opensky-setup.md).

## Contributing

Issues and pull requests are welcome on the [GitHub repository](https://github.com/pharcyd3/flight-radar).
