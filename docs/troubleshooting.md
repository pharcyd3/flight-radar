# Troubleshooting

Start by **tapping the poll sweep** at the bottom of the radar screen (or **Settings → API status** if you've turned the sweep off) — the API status panel it opens tells you exactly what the last request did (HTTP code, response size, a short reason, and how long ago it happened). Most of the issues below are diagnosable from that panel alone.

## No aircraft showing

There are three distinct causes, and the status panel tells you which one you're looking at:

**Status shows "OFFLINE" or "HTTP ERROR"** — a real connectivity problem. Check:

- WiFi is connected (device reboots and re-shows the setup portal if it can't connect at boot)
- The status panel's HTTP code — a `429` means you've momentarily outrun [airplanes.live](https://airplanes.live/)'s fair-use rate limit; it's keyless with no account or daily quota, so this clears on its own within a poll or two. If it happens often, switch to a slower **Refresh rate** (Settings menu).

**Status shows "NO DATA"** — the request succeeded (HTTP 200) but airplanes.live returned no aircraft for your exact search radius. This is **not necessarily a bug**. airplanes.live is a free community-run ADS-B aggregator, not a commercial one — its coverage can be genuinely sparse in some areas, especially at small zoom radii. This is normal and expected occasionally; if it's the *only* thing you ever see even at the widest zoom over several minutes, it's worth double-checking there's actually traffic nearby via an independent source (e.g. [adsb.lol](https://adsb.lol/)) before assuming something's wrong on the device.

**Status shows "ONLINE" but the radar still looks empty** — check your **Traffic** and **Min altitude** filters (Settings menu) aren't hiding everything currently in range, and confirm you're not zoomed in tighter than where the traffic actually is.

**The device restarts on its own after a long stretch with no aircraft** — this is deliberate, not a crash. This no-PSRAM board's free memory can fragment badly enough after long uptimes that new HTTPS connections fail (look for `SSL - Memory allocation failed` on the serial console), and fragmentation doesn't clear on its own. If a fetch that was previously working goes 5 minutes without a single success, the device restarts itself for a clean heap rather than sitting silently broken. Normal operation resumes immediately after.

## Map not showing, or looks broken/blank

- The very first time you visit a given location/zoom, a **"loading map..."** message is expected while tiles fetch over WiFi (a few seconds).
- If a specific location/zoom combination shows a broken or blank map indefinitely, it may be a **stale cache entry** from an earlier firmware version. There's no on-device "clear cache" menu item currently, but the cache is content-addressed, so visiting a genuinely new zoom level or location will always compose fresh.

## Device stuck in a boot loop

Watch the serial console (115200 baud) for a `Guru Meditation Error` panic dump — this is a firmware crash, not a normal hang. Common historical causes (already fixed in the current firmware, mentioned here in case a future change reintroduces something similar):

- **Heap exhaustion during TLS handshakes** — the ESP32-S3 has no PSRAM, and the persistent map sprite plus PNG decoder buffers can leave too little contiguous memory for a TLS connection. Look for `SSL - Memory allocation failed` in the log.
- **Dangling pointers in network retry logic** — if `WiFiClientSecure`/`HTTPClient` objects go out of scope while still referenced elsewhere, expect an `IllegalInstruction` panic with a corrupted backtrace.

If you hit a genuine crash loop, a full [factory reset](getting-started.md#resetting-later) (hold the encoder button 3 seconds) is a reasonable first recovery step, followed by re-flashing if that doesn't help.

## Taps don't select aircraft, or the view drifts when you tap

A touch only counts as a drag once your finger travels a fair distance; anything shorter is a tap. If taps seem to move the view instead of selecting, or nothing selects at all, you're on firmware older than 1.2 — that threshold used to be small enough that an ordinary fingertip roll was mistaken for a drag. [Update](flash.md).

## Traffic appears and disappears between refreshes

Aircraft that are genuinely in range should stay put from one poll to the next. If they visibly pop in and out, check the firmware version:

- Before 1.1, the device kept a *random* subset when more aircraft were in range than it can hold, re-rolled every poll.
- Before 1.2, a response that arrived truncated was still published, replacing a complete picture with whatever fragment had parsed.

Both are fixed; [update](flash.md) if you're behind. On current firmware a truncated poll is discarded and the previous traffic stays on screen, with the status panel showing `Truncated`.

## The Full map won't load

**Map → Full** is suppressed while the view is in motion — following an aircraft, or mid-drag — because composing street tiles blocks the device for seconds and competes with the traffic fetch for memory. Stop moving and it will compose for wherever you've landed. The offline lo-fi map is used meanwhile.

It can also lose a race with the flight-data fetch, which has priority for memory; it retries on the following frame, so a busy feed can delay it by a few seconds.

## Zoom or menu behaves oddly ("it jumps," "it flickers")

The M5Dial's rotary encoder on this hardware emits occasional spurious single ticks. The firmware only commits a step once the raw count has moved a full detent, so sub-detent noise is rejected outright rather than waited out — one physical click should reliably be one step, and steps register immediately. If you still see inconsistent behaviour, it's worth reporting with the exact sequence (which direction, how many clicks, what happened).

## Getting more detail

Everything above can be cross-checked against the serial console. Connect over USB and open a serial monitor at **115200 baud**. The firmware logs each airplanes.live request/response, map tile fetches, WiFi/provisioning events, and any crash backtraces there.
