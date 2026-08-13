# Flash Firmware

Install the latest firmware directly from this page — no PlatformIO, no command line. Plug the M5Dial in over USB, then use the button below.

**Requirements:**

- A USB cable connecting the M5Dial to this computer
- **Chrome or Edge** — this uses the [Web Serial API](https://developer.chrome.com/docs/capabilities/serial), which Firefox and Safari don't support

<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js"></script>

<esp-web-install-button manifest="../firmware/manifest.json">
</esp-web-install-button>

Click **Connect**, pick the M5Dial's serial port from the browser's list, then **Install**. It takes under a minute; the device reboots on its own when done.

See the [Changelog](changelog.md) for what's in the latest version.

## Why USB instead of wireless

This board (an ESP32-S3 with no PSRAM) doesn't have enough free contiguous memory to hold both a TLS download connection and the firmware-write buffer at the same time — a wireless self-update reliably failed in testing for exactly that reason. Flashing over USB sidesteps it entirely: the browser talks straight to the bootloader, and the device's own app — and its memory constraints — are never involved.
