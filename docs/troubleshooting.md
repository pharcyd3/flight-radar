# Troubleshooting

Start by **tapping the poll icon** at the bottom of the radar screen — the API status panel it opens tells you exactly what the last request did (HTTP code, response size, a short reason, and how long ago it happened). Most of the issues below are diagnosable from that panel alone.

## No aircraft showing

There are three distinct causes, and the status panel tells you which one you're looking at:

**Status shows "OFFLINE" or "HTTP ERROR"** — a real connectivity problem. Check:

- WiFi is connected (device reboots and re-shows the setup portal if it can't connect at boot)
- The status panel's HTTP code — a `429` means you've momentarily outrun [airplanes.live](https://airplanes.live/)'s fair-use rate limit; it's keyless with no account or daily quota, so this clears on its own within a poll or two. If it happens often, switch to a slower **Refresh rate** (Settings menu).

**Status shows "NO DATA"** — the request succeeded (HTTP 200) but airplanes.live returned no aircraft for your exact search radius. This is **not necessarily a bug**. airplanes.live is a free community-run ADS-B aggregator, not a commercial one — its coverage can be genuinely sparse in some areas, especially at small zoom radii. This is normal and expected occasionally; if it's the *only* thing you ever see even at maximum zoom (200&nbsp;km) over several minutes, it's worth double-checking there's actually traffic nearby via an independent source (e.g. [adsb.lol](https://adsb.lol/)) before assuming something's wrong on the device.

**Status shows "ONLINE" but the radar still looks empty** — check your **Traffic** and **Min altitude** filters (Settings menu) aren't hiding everything currently in range, and confirm you're not zoomed in tighter than where the traffic actually is.

## Map not showing, or looks broken/blank

- The very first time you visit a given location/zoom, a **"loading map..."** message is expected while tiles fetch over WiFi (a few seconds).
- If a specific location/zoom combination shows a broken or blank map indefinitely, it may be a **stale cache entry** from an earlier firmware version. There's no on-device "clear cache" menu item currently, but the cache is content-addressed, so visiting a genuinely new zoom level or location will always compose fresh.

## Device stuck in a boot loop

Watch the serial console (115200 baud) for a `Guru Meditation Error` panic dump — this is a firmware crash, not a normal hang. Common historical causes (already fixed in the current firmware, mentioned here in case a future change reintroduces something similar):

- **Heap exhaustion during TLS handshakes** — the ESP32-S3 has no PSRAM, and the persistent map sprite plus PNG decoder buffers can leave too little contiguous memory for a TLS connection. Look for `SSL - Memory allocation failed` in the log.
- **Dangling pointers in network retry logic** — if `WiFiClientSecure`/`HTTPClient` objects go out of scope while still referenced elsewhere, expect an `IllegalInstruction` panic with a corrupted backtrace.

If you hit a genuine crash loop, a full [factory reset](getting-started.md#resetting-later) (hold the encoder button 3 seconds) is a reasonable first recovery step, followed by re-flashing if that doesn't help.

## Zoom or menu behaves oddly ("it jumps," "it flickers")

The M5Dial's rotary encoder on this hardware exhibits occasional electrical noise — a genuine click and a spurious bounce can look identical at the raw signal level and are only distinguishable by timing. The firmware deliberately waits for a reading to settle before acting on it (longer for zoom, shorter for menu navigation), which should make one physical click reliably equal one step. If you still see inconsistent behaviour, it's worth reporting with a description of the exact sequence (which direction, how many clicks, what happened) since this is an active area of tuning.

## Getting more detail

Everything above can be cross-checked against the serial console. Connect over USB and open a serial monitor at **115200 baud**. The firmware logs each airplanes.live request/response, map tile fetches, WiFi/provisioning events, and any crash backtraces there.
