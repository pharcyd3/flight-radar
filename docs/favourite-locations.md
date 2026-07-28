# Favourite Locations

Frank's Flight Radar can remember up to **three** favourite locations, so you can jump between (for example) home, work, and a family member's house without re-typing coordinates each time.

## Setting up favourites

1. Open **Settings → Location & API Keys**. This opens the WiFi captive portal on `Franks-Flight-Radar-Setup` / `192.168.4.1`.
2. Alongside the home latitude/longitude fields, you'll find three favourite slots, each with a **name**, **latitude**, and **longitude**.
3. Fill in whichever slots you want (leave a name blank to clear that slot), and save.

Coordinates are validated on save (must be within ±90° latitude / ±180° longitude); an invalid entry is rejected and the previous value kept, logged to serial as `[Provision] favourite <field> out of range, ignoring`.

## Pre-caching

As soon as you save named favourites, Frank's Flight Radar immediately fetches and caches the map tiles for each one at the default zoom level (25&nbsp;km), showing a **"Caching maps..."** message while it works. This means that when you later switch to a favourite, its map appears instantly instead of fetching tiles live over the network. Favourites that are already cached are skipped (cheap no-op), so re-saving the form doesn't re-fetch anything unnecessarily.

## Switching to a favourite

1. Open **Settings → Saved Locations**.
2. Rotate to highlight the favourite you want (empty slots are shown as "Favourite N (empty)" and do nothing if selected).
3. Press to make it your active home location. The radar immediately re-centres and re-fetches traffic around the new location.

Switching favourites does **not** change your saved WiFi credentials — only the home coordinates used for the radar.
