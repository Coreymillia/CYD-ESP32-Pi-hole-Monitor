# CYDPiHole — Pi-hole Monitor for the Cheap Yellow Display

A real-time Pi-hole DNS dashboard and watcher running on the **ESP32 CYD (Cheap Yellow Display)**. Displays live DNS queries, today's stats, top blocked domains, top clients, a 24-hour activity graph, watched hits, watched target summaries, and a lightweight DNS sniffer view — all from your local Pi-hole v6 instance.

This version can also be **tuned to watch for specific DNS queries**. You can choose the domains you care about, optionally limit them to selected devices, import one extra blocklist, and quickly see whether Pi-hole **blocked** those lookups or whether they **slipped through**.

![CYDPiHole - Live Feed](IMG_20260222_134532.jpg)

---

## Features

- **9 display modes** — swipe left/right on the touchscreen to cycle between modes
- **Mode 1 — Live Feed:** last 10 DNS queries with allowed/blocked status, client IP, and domain
- **Mode 2 — Stats Dashboard:** queries today, blocked today, % blocked, blocklist size, active clients
- **Mode 3 — Top Blocked:** the 10 most-blocked domains today with hit counts
- **Mode 4 — Top Clients:** the 10 most active devices on your network by query count
- **Mode 5 — 24h Activity Graph:** stacked bar chart of DNS traffic over the last 24 hours (green = allowed, red = blocked)
- **Mode 6 — Watched Hits:** only watched-domain hits for watched devices, color-coded by blocked vs slipped-through
- **Mode 7 — Target Devices:** per-device watched hit summary with slipped/blocked counts and DoH-provider hit counts
- **Mode 8 — DNS Sniffer:** compact alert list for repeated allowed watched hits and DoH-provider activity
- **Mode 9 — Seen Devices:** on-device picker for recently discovered Pi-hole devices with touch confirm add/remove
- **Countdown bar** — 1px progress bar at the bottom edge shows time until next refresh
- **Stale data preserved on error** — brief network hiccups show an error in the header only; last good data stays visible
- Color-coded results: **OK** rows cycle through cyan → yellow → blue; **BLK** rows are orange
- Watched-mode colors: **blue = blocked / protected**, **red = allowed / slipped through**
- First-boot **captive portal** for WiFi, Pi-hole, watched devices, watched domains, and one optional blocklist URL — no code editing required
- **Hold BOOT 3 seconds** at any time to re-enter setup
- Supports both **passwordless** and **password-protected** Pi-hole v6 installs

---

## Why use the watcher modes?

The dashboard modes are useful for general Pi-hole visibility, but the watcher features are aimed at **targeted monitoring**:

- watch for domains you care about such as streaming, social, gaming, or DoH-related services
- narrow the watch down to specific phones, tablets, or other clients by **IP**, **client name**, or **MAC**
- see whether those queries were **protected** by Pi-hole or **allowed** through
- surface repeated allowed hits in the sniffer view so suspicious or unwanted DNS behavior stands out faster
- build the watched-device list from **recently seen devices** instead of typing everything manually

This makes the CYD better suited for keeping an eye on the DNS activity you actually care about, instead of only acting as a general Pi-hole stats screen.

---

## Screenshots

| Stats Dashboard | Top Clients | 24h Activity Graph |
|---|---|---|
| ![Stats](IMG_20260301_094008.jpg) | ![Top Clients](IMG_20260301_094038.jpg) | ![24h Activity](IMG_20260301_094051.jpg) |

---

## Requirements

### Hardware
- **ESP32 CYD** board (ESP32-2432S028R or compatible "Cheap Yellow Display")

### Software / Services
- [PlatformIO](https://platformio.org/) (VSCode extension recommended)
- **Pi-hole v6** running on your local network
  - Pi-hole v5 is **not supported** — the v6 REST API is required

---

## Setup

### 1. Flash the firmware

1. Clone this repository and open it in VSCode with the PlatformIO extension
2. Connect your CYD board via USB
3. Click **Upload** in PlatformIO (or run `pio run --target upload`)

### 2. Configure WiFi, Pi-hole, and watch lists (first boot)

On first boot the CYD will open a setup access point:

1. On your phone or PC, connect to the WiFi network: **`CYDPiHole_Setup`** (no password)
2. Open a browser and go to **`192.168.4.1`**
3. Fill in the main settings form:
    - **WiFi Network Name (SSID)** — your 2.4 GHz WiFi name
    - **WiFi Password** — leave blank for open networks
    - **Pi-hole IP / Hostname** — just the IP, e.g. `192.168.0.103` *(no `http://`, no `/admin/`)*
    - **Pi-hole Password** — leave blank if your Pi-hole has no password set
    - **Watched Devices** — one device per line in the format `IP | Client Name | MAC | Label`
4. Tap **Save & Connect**
5. Use the separate **Domains and Blocklist** section any time you want to change:
    - **Watched Domains** — one domain per line, e.g. `youtube.com`
    - **Blocklist URL** — one `http://` or `https://` list at a time
    - **Allowed-Hit Warning Threshold** — how many allowed watched hits should escalate the sniffer view
6. Tap **Save Domains / Blocklist** when you only want to update domain watching without re-saving WiFi details

The display will connect to your WiFi and start showing queries within a few seconds.

After the CYD has connected to Pi-hole at least once, the setup portal will also show a **Recently Seen Devices** section populated from Pi-hole's `network/devices` data so you can add real devices directly into the watched list.

> ⚠️ The ESP32 supports **2.4 GHz WiFi only**.

### 3. Changing modes

**Touch the screen** to cycle between modes:

| Touch zone | Action |
|------------|--------|
| Right half of screen | Next mode → |
| Left half of screen | ← Previous mode |

You can also press the **BOOT button** briefly to advance to the next mode.

```
Live Feed  ←→  Stats  ←→  Top Blocked  ←→  Top Clients  ←→  24h Activity
   ←→  Watched Hits  ←→  Target Devices  ←→  DNS Sniffer  ←→  Seen Devices
```

### 4. Re-entering setup

**Hold the BOOT button for 3 seconds** from any mode. The device will restart directly into the setup portal.

---

## Display Modes

### Mode 1 — Live Feed

```
[ Pi-Hole Monitor         192.168.0.103 ]
[  ST  | .CLT | DOMAIN                  ]
[────────────────────────────────────── ]
[  OK  | .5   | example.com             ]
[ BLK  | .12  | ads.tracker.net         ]
  ...
```

| Column | Description |
|--------|-------------|
| **ST** | `OK ` (cyan/yellow/blue, cycling) = allowed &nbsp;/&nbsp; `BLK` (orange) = blocked |
| **.CLT** | Last octet of the client IP (e.g. `.5` = `192.168.0.5`) |
| **DOMAIN** | Queried domain, truncated with `..` if too long |

### Mode 2 — Stats Dashboard

```
[ Pi-Hole Stats           192.168.0.103 ]
[────────────────────────────────────── ]
  QUERIES TODAY        BLOCKED TODAY
    44,541               14,937

           33.5% BLOCKED

  BLOCKLIST DOMAINS        CLIENTS
    1,033,564                 21
```

### Mode 3 — Top Blocked

```
[ Top Blocked             192.168.0.103 ]
[  #  | COUNT | DOMAIN                  ]
[────────────────────────────────────── ]
[  1    2847   doubleclick.net          ]
[  2    1203   googleadservices.com     ]
  ...
```

### Mode 4 — Top Clients

```
[ Top Clients             192.168.0.103 ]
[  #  | COUNT | CLIENT                  ]
[────────────────────────────────────── ]
[  1    7124   192.168.0.112            ]
[  2    6601   192.168.0.101            ]
  ...
```

Shows the 10 most active devices on your network by total DNS query count. Displays hostname if available, falls back to IP address.

### Mode 5 — 24h Activity Graph

A stacked bar chart of all DNS traffic over the last 24 hours in 10-minute buckets.
- 🟢 Lime = allowed queries
- 🟠 Orange = blocked queries
- Left edge = 24 hours ago, right edge = now

### Mode 6 — Watched Hits

Shows only watched-domain hits. These are filtered by your manual watched domains, the optional imported blocklist, and, when configured, your watched devices.

- 🔵 **BLK** = Pi-hole blocked the watched request
- 🔴 **SLP** = the watched request was allowed / slipped through

### Mode 7 — Target Devices

Summarizes each watched device with:

- **S/B** = slipped-through count / blocked count for watched domains
- **DOH** = unblocked DoH-provider hit count
- watched devices can now match by **IP**, **client name**, or **MAC** when Pi-hole provides it

### Mode 8 — DNS Sniffer

Shows compact alerts derived from Pi-hole data only:

- repeated allowed watched-domain hits above your configured threshold
- DoH-provider hits from watched devices
- protected-only summaries when watched hits were blocked cleanly

### Mode 9 — Seen Devices

Lists recently discovered devices from Pi-hole's `network/devices` cache directly on the CYD.

- top-left and top-right header corners still switch to the previous/next mode
- tap a row to choose a device
- use the footer left/right areas to move between pages when more devices exist than fit on one screen
- tap the footer to **confirm add** or **confirm remove**
- uses the same watch-device format stored in Preferences, so the AP and on-device picker stay in sync

---

## Hardware Pinout (CYD)

| Function | GPIO |
|----------|------|
| TFT DC | 2 |
| TFT CS | 15 |
| TFT SCK | 14 |
| TFT MOSI | 13 |
| TFT MISO | 12 |
| Backlight | 21 |
| BOOT button | 0 |
| Touch IRQ | 36 |
| Touch MOSI | 32 |
| Touch MISO | 39 |
| Touch CLK | 25 |
| Touch CS | 33 |

---

## Libraries Used

All managed automatically by PlatformIO:

- [`moononournation/GFX Library for Arduino @ 1.4.7`](https://github.com/moononournation/Arduino_GFX) — ILI9341 display driver
- [`bblanchon/ArduinoJson @ ^7`](https://arduinojson.org/) — Pi-hole API JSON parsing
- [`PaulStoffregen/XPT2046_Touchscreen`](https://github.com/PaulStoffregen/XPT2046_Touchscreen) — touchscreen driver
- `WebServer`, `DNSServer`, `Preferences` — built into the ESP32 Arduino core (captive portal + NVS storage)
- `HTTPClient`, `WiFiClient`, `WiFiClientSecure` — built into the ESP32 Arduino core (HTTP/HTTPS requests)

---

## Troubleshooting

| Error on screen | Cause | Fix |
|-----------------|-------|-----|
| `Fetch failed: HTTP 404` | Wrong Pi-hole host entered | Hold BOOT 3s to re-enter setup. Enter **only** the IP, e.g. `192.168.0.103` |
| `Fetch failed: HTTP 401` | Wrong Pi-hole password | Hold BOOT 3s to re-enter setup and correct the password |
| `Fetch failed: Auth HTTP 4xx` | Pi-hole unreachable | Check the IP address and that Pi-hole is running |
| `ERR: begin() failed` | Brief network hiccup (common during heavy streaming) | Clears automatically on next poll — last data stays on screen |
| `Fetch failed: No Pi-hole host set` | Setup was skipped | Hold BOOT 3s to complete setup |
| `WiFi failed: "YourSSID"` | Wrong WiFi credentials or out of range | Hold BOOT 3s to re-enter setup |

---

## CYDPiAlert Integration

This firmware includes [`CYDIdentity.h`](include/CYDIdentity.h) and serves a `GET /identify` endpoint on port 80. Any device running this firmware will be automatically discovered by **[CYDPiAlert-ESPID](https://github.com/coreymillia/CYDPiAlert-ESPID)'s Mode 9 — ESP Devices**, which scans your network for microcontrollers and displays their name, firmware version, RSSI, uptime, and IP.

The identity response looks like:

```json
{
  "name": "CYDPiHole",
  "mac": "xx:xx:xx:xx:xx:xx",
  "version": "1.0.0",
  "uptime_s": 3600,
  "rssi": -45,
  "last_fetch": 0,
  "errors": 0
}
```

No configuration needed — it activates automatically once the device is connected to WiFi. The `INVERTEDPihole` variant identifies itself as `"INVERTEDPihole"`.

---

## License

MIT
