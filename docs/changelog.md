# Changelog

Every entry here corresponds to a `FIRMWARE_VERSION` in `src/config.h` — CI
checks that a version bump always comes with a matching `## X.Y` section here
and fails the build otherwise (see [Building From
Source](development.md#cutting-a-release)). See [Flash
Firmware](flash.md) to install a release.

## 1.1

Performance and stability pass, driven by on-device profiling.

**Smoother radar**

- The display now repaints at 5&nbsp;fps while traffic is moving, up from 1&nbsp;fps, so aircraft glide instead of stepping once a second. It also no longer freezes during a poll, which at the 5&nbsp;s refresh setting had been stopping the animation for a third of every cycle.
- Faster projection maths and per-vertex culling of the offline map, so the wider zoom steps cost less to draw. Rivers and lakes are dropped past the 100&nbsp;km step, where they read as clutter.

**Responsive wide zooms**

- Traffic is now requested out to about 185&nbsp;km whatever the zoom. At 400&nbsp;km the API had been returning ~150&nbsp;KB per poll — roughly five seconds to transfer and parse, longer than the refresh interval — and almost all of it was discarded. The widest views now show the nearer traffic on a wider canvas.
- When more aircraft are in range than the device can hold, it keeps the **nearest** ones. Previously it sampled them at random and re-rolled every poll, so traffic visibly popped in and out between refreshes.
- A stalled response is abandoned after 3.5&nbsp;s instead of 9&nbsp;s, so a bad transfer no longer pins the feed busy for longer than a whole refresh cycle.

**Poll sweep replaces the countdown**

- The ring at the bottom of the screen is now a continuously rotating sweep driven by the clock, not a countdown to the next poll. A countdown implied a predictable schedule the device doesn't have — a fetch takes a large and variable share of each interval — so it spent much of its time apparently frozen. Colour still reports feed health.
- New **Settings → Poll sweep** to hide it, and **Settings → API status** so the status panel stays reachable when it's hidden.

**Emergency alerts**

- An emergency squawk now makes the device follow that aircraft automatically, draws its reticle in red, and shows the squawk code beneath it.
- The alert is ten flashes and tones rather than three, and each tone lasts as long as the flash it accompanies.

**Fixes**

- Settings screens no longer reboot the device if you linger. Every menu screen runs its own loop and none of them fed the watchdog, so spending more than a minute in one — panning around to pick a home location, say — triggered a reset and discarded the change.
- The setup screen no longer reboots every 60&nbsp;s while waiting for WiFi details, for the same reason.
- The aircraft icon now defaults to the plane shape.

## 1.0

- Initial release.
