# Getting Started

## What you need

- An **M5Stack M5Dial** (ESP32-S3)
- A USB-C cable
- A computer with [PlatformIO](https://platformio.org/) (via the [PlatformIO IDE extension](https://platformio.org/platformio-ide) for VS Code, or the standalone `pio` CLI)
- A 2.4&nbsp;GHz WiFi network (the M5Dial's ESP32-S3 radio does not support 5&nbsp;GHz)
- A phone or laptop to complete the on-device setup wizard

## 1. Flash the firmware

Clone the repository and build/upload with PlatformIO:

```bash
git clone https://github.com/pharcyd3/flight-radar.git
cd flight-radar
pio run -t upload
```

PlatformIO will download the ESP32 toolchain and libraries on the first run, which can take a few minutes. Once flashed, the device reboots automatically.

If `pio` can't find the device, check the upload port explicitly:

```bash
pio run -t upload --upload-port /dev/cu.usbmodem101   # macOS example
```

## 2. First boot — WiFi & location setup

On its very first boot (or after a factory reset), Frank's Flight Radar has no saved WiFi credentials, so it starts its own setup access point and shows:

```
SETUP MODE
Connect your phone to:
Franks-Flight-Radar-Setup
then open:
192.168.4.1
```

1. On your phone or laptop, join the **`Franks-Flight-Radar-Setup`** WiFi network.
2. Open **`192.168.4.1`** in a browser — this is [WiFiManager](https://github.com/tzapu/WiFiManager)'s captive portal.
3. Choose **Configure WiFi**, select your home network, and enter its password.
4. On the same form, you'll also see:
    - **OpenSky client_id** / **OpenSky client_secret** *(optional at this stage — see [OpenSky API Setup](opensky-setup.md))*
    - **Home location** — the centre point of your radar. You have three options, easiest first:
        - **Leave everything blank** to have the device auto-detect your approximate location from your network once it's online (city-level; a VPN will throw it off).
        - Type a **place name** in the *Place name* field (e.g. `Berlin`, `Paris, France`) — it's geocoded to coordinates after the device reconnects. A place name always wins over the coordinate fields.
        - Enter **Home latitude** / **Home longitude** directly in decimal degrees (e.g. `51.5007`, `-0.1246`) for a precise fix. See **[Setting Your Location](setting-location.md)** for how to read these off Google Maps, and every other way to set or change your location later.
5. Save. The device connects to your WiFi, resolves your location, and reboots into the radar view.

If the portal times out (5 minutes) without a successful connection, the device restarts and re-opens the portal automatically.

## 3. You should now see the radar

Once connected, Frank's Flight Radar fetches its first batch of map tiles (a "loading map..." message appears briefly) and starts polling OpenSky. If you haven't set up API credentials yet, it runs **anonymously** — this works, but is quota-limited (see [OpenSky API Setup](opensky-setup.md) for why you'll want real credentials for reliable use).

## Resetting later

- **Change WiFi/location only**: open the on-device Settings menu (short-press the encoder button) → **Location & API Keys**. This re-opens the same captive portal without erasing anything else.
- **Full factory reset**: hold the encoder button down for 3 seconds. This wipes WiFi credentials, OpenSky login, home location, and saved favourites, then reboots into first-boot setup. The same reset is also available as a menu item under **Settings → Factory Reset** (hold-to-confirm, so it can't be triggered by accident).
