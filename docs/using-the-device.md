# Using the Device

FlightDial has three inputs: the **rotary encoder** (twist and press), and the **touchscreen**. Here's what each does on the main radar screen.

## The radar display

- The **red crosshair** at the centre is your home location.
- **Concentric rings** mark 25%, 50%, and 100% of the current zoom radius, with an **N** tick at the top and range labels on the right (toggle these off under Settings → Range rings).
- Aircraft are drawn as dots with a short heading line, coloured by state:
    - White (or your theme's aircraft colour) — normal, airborne
    - Grey — on the ground
    - Orange — currently selected (tapped)
    - **Red, with an extra ring** — squawking an emergency code (7500/7600/7700); see [Emergency Alerts](emergency-alerts.md)
- A small **poll icon** sits near the bottom of the screen: a shrinking ring counts down to the next automatic refresh, a solid ring means a request is in flight, and a **solid red ring** means the last request failed.

## Rotary encoder: zoom

Rotate the dial to change the radar range. There are five steps: **10, 25, 50, 100, and 200&nbsp;km**. Each click moves exactly one step — the firmware deliberately waits for the reading to settle before committing to a step (a fraction of a second), which trades a small amount of responsiveness for not registering phantom double-steps from this hardware's rotary encoder.

Changing zoom triggers an immediate re-fetch at the new radius (and composes/loads the map for that view).

## Touch: select an aircraft

**Tap an aircraft's dot** to open a detail panel showing its callsign, ICAO24 address, altitude, speed, heading, and country of registration. Tap the same aircraft again (or tap elsewhere) to dismiss it.

## Touch: check API status

**Tap the poll icon** (bottom of the screen) to open the **API status panel** — this shows whether the last request succeeded, the HTTP status code and response size, a short reason on failure, and how long ago it happened. Tap anywhere to dismiss it. This is the fastest way to check *why* aircraft aren't showing — see [Troubleshooting](troubleshooting.md).

## Encoder button: settings & reset

- **Short press** (under 3 seconds): opens the **Settings menu** as a floating panel over the live radar. See [Settings Reference](settings-reference.md) for every option. Rotate to move the cursor, press to select/cycle a value, and **tap anywhere on the screen** to close the menu at any time (it also auto-closes after 30 seconds of inactivity).
- **Hold for 3 seconds**: triggers a **factory reset** — this wipes WiFi credentials, OpenSky login, home location, and saved favourites, then reboots into first-boot setup. There's no confirmation for this physical-button combo (it's designed as a recovery mechanism when the device is otherwise unusable), so use it deliberately. The same action is also available as a hold-to-confirm menu item under **Settings → Factory Reset**, which is the safer route for everyday use.
