# OpenSky API Setup

Frank's Flight Radar can run without any account at all, but you'll want to set up a free OpenSky **API client** for reliable use. Here's why, and how.

## Why you need this

OpenSky's REST API used to accept a plain username/password (HTTP Basic Auth). That's been **retired** — the API now requires **OAuth2 client-credentials** authentication. Anonymous access still works, but it shares a very small daily quota across every anonymous request from your IP address:

| Tier | Daily credit budget |
|---|---|
| Anonymous (no credentials) | 400 |
| Standard authenticated client | 4,000 |
| Active ADS-B feeder (≥30% uptime/month) | 8,000 |

Each request to the live-state endpoint costs credits based on the geographic area of its bounding box (≤25&nbsp;sq° = 1 credit, up to 4 credits for a global query). At Frank's Flight Radar's zoom levels (10–200&nbsp;km radius), a request almost always costs **1 credit**, regardless of zoom — see [Map & Caching](map-caching.md) for the exact numbers at your latitude.

At the default 30-second refresh rate, that's about **2,880 requests/day** — comfortably inside the standard authenticated budget, with no margin at all anonymously (400/day runs out in under two hours at that rate).

## Creating an API client

1. Go to [opensky-network.org](https://opensky-network.org/) and log in (create a free account if you don't have one).
2. Open your **Account** page and find the **API Client** section.
3. Create a new API client. You'll be given a **`client_id`** and a **`client_secret`** — copy the secret immediately, as it's typically only shown once.

## Entering credentials on the device

You have two options, both equally valid:

### Option A — Settings menu (recommended)

1. Short-press the encoder button to open **Settings**.
2. Rotate to **Location & API Keys** and press.
3. This opens the same WiFi captive portal as first-boot setup, on the **`Franks-Flight-Radar-Setup`** network at `192.168.4.1` — but it does **not** erase your saved WiFi or location.
4. Fill in **OpenSky client_id** and **OpenSky client_secret**, and save.

The client_secret field is intentionally left blank when re-opening this form — leave it blank to keep the previously saved secret, or type a new one to replace it.

### Option B — Serial debug command

If the device is connected to your computer over USB, you can set credentials directly without touching the WiFi portal:

```
SETCREDS:<client_id>:<client_secret>
```

Send this as a single line (newline-terminated) over the serial connection at 115200 baud. This is a debug convenience built into the firmware for exactly this purpose — it writes straight to the same persisted storage as the portal form, and only works over a physical USB connection.

## Confirming it worked

Watch the serial console (`pio device monitor` or any serial terminal at 115200 baud) after entering credentials. A successful setup looks like:

```
[Provision] OpenSky client_id set via serial: your-client-id
[OpenSky] Got OAuth2 token (expires in 1800s)
[OpenSky] GET https://opensky-network.org/api/states/all?...  (authed, heap=...)
[OpenSky] HTTP 200  (Content-Length -1)
```

The `(authed)` tag on the GET line confirms the device is using your credentials rather than falling back to anonymous access. Tokens are cached and automatically refreshed roughly a minute before they expire (they last 30 minutes).

## Choosing a refresh rate

Under **Settings → Refresh rate** you can pick 10, 20, or 30 seconds. Only **30 seconds** comfortably fits a full day within the standard authenticated 4,000/day budget:

| Refresh | Requests/day | Fits standard budget all day? |
|---|---|---|
| 10 s | 8,640 | No — exhausts budget in ~11 hours |
| 20 s | 4,320 | Barely over — risks throttling late in the day |
| 30 s | 2,880 | Yes, with margin |

If you only run Frank's Flight Radar for a few hours at a time rather than continuously, faster refresh rates are fine.
