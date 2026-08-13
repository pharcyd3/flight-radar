# Flash Firmware

Install the latest firmware directly from this page — no PlatformIO, no command line, no cloning the repo. Works for a brand new, never-flashed M5Dial or for updating one already running.

**You need:**

- A USB cable connecting the M5Dial to this computer
- **Chrome or Edge** — this uses the [Web Serial API](https://developer.chrome.com/docs/capabilities/serial), which Firefox and Safari don't support

<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js"></script>

<esp-web-install-button manifest="../firmware/manifest.json">
</esp-web-install-button>

1. Plug the M5Dial into this computer with USB.
2. Click **Connect** above.
3. A browser popup lists nearby serial devices — pick the M5Dial's entry and confirm. (Nothing listed? See [Troubleshooting](#troubleshooting) below.)
4. Click **Install**.
5. Wait — it takes under a minute. The device reboots on its own when done; you don't need to touch it.

See the [Changelog](changelog.md) for what's in the latest version.

## Troubleshooting

- **No devices in the popup list** — try a different USB cable (some are charge-only, with no data lines) or a different USB port. On Windows, the M5Dial should also appear in Device Manager under "Ports (COM & LPT)" if the driver's working.
- **Button says the browser isn't supported** — you're not in Chrome or Edge. Copy this page's URL into one of those.
- **Install fails partway through** — unplug and replug the M5Dial, click Connect again, and retry. If it keeps failing, try a shorter/better-quality USB cable — flashing needs a stable data connection.

## Why USB instead of wireless

This board (an ESP32-S3 with no PSRAM) doesn't have enough free contiguous memory to hold both a TLS download connection and the firmware-write buffer at the same time — a wireless self-update reliably failed in testing for exactly that reason. Flashing over USB sidesteps it entirely: the browser talks straight to the bootloader, and the device's own app — and its memory constraints — are never involved.
