# Settings Reference

Open the settings menu with a short press of the encoder button. Rotate to move the highlighted row, press to select it. For cycling options, pressing immediately advances to the next value and saves it — you stay in the menu, so you can adjust several settings in one visit. Tap anywhere to close.

Settings are saved to flash immediately as you change them (no separate "save" step), and persist across reboots and firmware updates.

## Flight labels

Controls when an aircraft's callsign is shown next to its dot.

| Value | Behaviour |
|---|---|
| Off | Never show callsigns |
| **Selected** *(default)* | Only the currently-tapped aircraft shows its callsign |
| All | Every aircraft shows its callsign at all times |

Emergency-squawking aircraft always show their callsign regardless of this setting.

## Colour theme

Four full colour palettes for the radar display — background, rings, aircraft, selection highlight, and text all change together.

- **Radar** *(default)* — classic green-on-black
- **Amber** — amber/orange monochrome, reminiscent of older radar displays
- **Ocean** — blues and cyans
- **Neon** — high-contrast magenta/cyan

The settings menu itself always uses a fixed look regardless of this choice, so it stays legible no matter which radar theme is active.

## Units

| Value | Altitude | Speed |
|---|---|---|
| **ft/kts** *(default)* | Feet | Knots |
| m/km/h | Metres | Kilometres per hour |

Applies to the aircraft detail panel (tap an aircraft to see it).

## Traffic

Which aircraft are drawn on the radar.

| Value | Behaviour |
|---|---|
| **Airborne** *(default)* | Aircraft on the ground are hidden |
| All | On-ground aircraft are shown too (in grey, no heading line) |

## Min altitude

Hides aircraft below a given altitude (those with a known, positive altitude reading only — aircraft with no altitude data, e.g. many on the ground, are unaffected by this filter and governed by **Traffic** instead).

Options: **Off** *(default)*, 1,000&nbsp;ft, 5,000&nbsp;ft, 10,000&nbsp;ft, 20,000&nbsp;ft.

## Heading trails

| Value | Behaviour |
|---|---|
| **On** *(default)* | Airborne aircraft (over 5&nbsp;m/s ground speed) show a short line indicating their direction of travel |
| Off | No heading lines |

## Range rings

| Value | Behaviour |
|---|---|
| **On** *(default)* | The three range rings, N tick, and range labels are drawn |
| Off | Radar screen shows only the map, home crosshair, and aircraft |

## Refresh rate

How often FlightDial polls OpenSky for updated positions: **10&nbsp;s**, **20&nbsp;s**, or **30&nbsp;s** *(default)*.

See [OpenSky API Setup](opensky-setup.md#choosing-a-refresh-rate) for why 30&nbsp;s is the only option that reliably lasts a full day under a standard authenticated account's quota.

## Buzz on Emergency

| Value | Behaviour |
|---|---|
| **On** *(default)* | An emergency squawk (7500/7600/7700) triggers a flashing red ring around the screen bezel and three audible tones |
| Off | Emergency aircraft are still highlighted visually on the radar, but silently |

See [Emergency Alerts](emergency-alerts.md) for details.

## Location & API Keys

Opens the WiFi captive portal (`FlightDial-Setup` / `192.168.4.1`) to change your home coordinates, saved favourite locations, or OpenSky API credentials — **without** erasing your saved WiFi connection. See [Getting Started](getting-started.md) and [OpenSky API Setup](opensky-setup.md).

## Saved Locations

Opens a picker for your three saved favourite locations (set via **Location & API Keys**). Rotate to highlight one, press to make it your active home location. See [Favourite Locations](favourite-locations.md).

## Factory Reset

Hold-to-confirm (3 seconds) reset that wipes WiFi credentials, OpenSky login, home location, and saved favourites, then reboots into first-boot setup. Releasing early, tapping the screen, or 15 seconds of inactivity cancels it.
