# FlightScnr: Mini ADS-B Radar Style Flight Scanner

The best part? There is absolutely no coding or soldering required!



[Youtube Video](https://youtube.com/shorts/vinE6DK6SSY?si=bhuOrcAyPHRql8ar)



Open-source firmware that shows **live ADS-B traffic** on a sweeping radar around your preset position. Built for the **[LilyGO T-Encoder Pro](https://www.lilygo.cc/zo4apl)**, inspired by **[ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar)** and **[deskradar](https://github.com/arvis91/deskradar)**.

Firmware is **[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)** ([LICENSE](LICENSE)) - shareable for hobbyists, not for closed commercial forks.

**Enclosure:** [MakerWorld](https://makerworld.com/en/models/2902669-flightscnr-live-ads-b-traffic-sweeping-radar#profileId-3245055) (separate license - see [below](#enclosure-license)).

## Features

- **Radar** - sweeping display with range rings (2/4/6/8 mi, default 4), compass rose, optional sweep line, themed colors (Green default), km/mi/nm units, rim dots for out-of-range traffic. Live ADS-B via [adsb.fi](https://adsb.fi), ~2 s refresh, up to 64 aircraft.
- **Flight detail** - tap a blip or short-press the knob: callsign, airline logo/name, route, ICAO type, altitude, speed. Knob cycles visible aircraft. Optional route lookup (see [APIs](#optional-apis)).
- **Clock & forecast** - swipe down from radar: NTP time, date, current weather, sunrise/sunset. Swipe right for a 3-day forecast (hi/lo, icons, rain %).
- **Auto timezone** - Fetch Timezone + DST from your radar center ([timeapi.io](https://timeapi.io), no API key needed). Manual UTC offset on-device disables auto until re-enabled on the web portal.
- **Tomorrow.io weather** - optional key + enable checkbox on the web portal; metric or imperial.
- **Auto-idle clock** (default on) - empty radar (no in-range aircraft) switches to the clock; returns when traffic appears.
- **Settings** - three on-device pages (network/API status, display/timeouts, color/beep) plus full config at [http://flightscnr.local/](http://flightscnr.local/). Web **Save** applies live - no reboot.

Screen timeouts (configurable on web or device page 2): flight detail 10/20/30 s or manual; clock/forecast 5/10/15 s or manual. Settings and about auto-return to radar after 10 s.

## Navigation

**From radar:** knob = range preset · tap / short press = flight detail · swipe ↓ clock · ↑ about · ← settings

**From clock:** ← clock settings · → forecast · ↑ radar

**From forecast:** ← clock · ↑ radar

**From flight detail / settings / about:** swipe right (or timeout) → back

**Everywhere:** hold knob **3 s** = Wi‑Fi reset (setup portal). Do **not** hold the screen at power-on - that is BOOT/download mode.

On-device settings: page 2 = brightness, units, compass, sweep, timeouts, idle clock. Page 3 = radar color, beep on/off, tone A–E.

## Setup

1. Power on → join **FlightScnr-AP** if prompted.
2. Open [http://4.3.2.1](http://4.3.2.1) or [http://flightscnr.local](http://flightscnr.local) → enter Wi‑Fi credentials. Reboot the unit.
3. After connect: boot splash (~5 s) → radar.
4. Set radar center, weather key, and optional route APIs at [http://flightscnr.local/](http://flightscnr.local/) (or device IP shown on settings page 1).

To change settings later: same URL, **Save**. To reset Wi‑Fi only: hold knob 3 s.

## Screenshots

**Flight detail** - route, airline logo, altitude, speed



**Settings** - network, display, color & audio (3 pages via swipe left)



**Clock** - time, weather, sunrise/sunset (→ forecast on swipe right)



## Hardware


| Item          | Details                                                                                                                             |
| ------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| **Board**     | [LilyGO T-Encoder Pro](https://www.lilygo.cc/zo4apl) - ESP32-S3, 16 MB flash, 8 MB PSRAM                                            |
| **Display**   | 1.2″ 390×390 AMOLED; auto-detects DXQ120 or TFD12 panel at boot                                                                     |
| **Enclosure** | [MakerWorld](https://makerworld.com/en/models/2902669-flightscnr-live-ads-b-traffic-sweeping-radar#profileId-3245055) (not in repo) |


## Build & flash

Requires [PlatformIO](https://platformio.org/).

```bash
python -m platformio run -e tencoder-pro -t upload   # Windows if pio not on PATH
pio run -e tencoder-pro -t upload                      # otherwise
```

**WebFlasher (no PlatformIO):** [yashmulgaonkar.github.io/FlightScnr](https://yashmulgaonkar.github.io/FlightScnr) - Connect → Install (factory image at 0x0). Hold screen (**BOOT**) if the port doesn’t appear.

**Merged binary:** `pio run -t merge -e tencoder-pro` → `.pio/build/tencoder-pro/firmware-merged.bin`

Builds auto-download [tar1090-db](https://github.com/wiedehopf/tar1090-db) and [Airports](https://github.com/mwgg/Airports) lookups. Wrong panel saved? Erase flash and re-detect, or override with `-D FLIGHTSCNR_PANEL_DXQ` / `-D FLIGHTSCNR_PANEL_TFD12` in `platformio.ini`.

## Optional APIs

### Weather (Tomorrow.io)

Sign up at [Tomorrow.io](https://app.tomorrow.io/signup?planid=60d46beae90c3b3549a59ff3), enable **Use Tomorrow.io** on the web portal, paste key, **Save**. Fetches only while clock or forecast is open; refreshes after 30 min.

### Route / airline (AirLabs, FlightAware, FR24)

Enable providers and keys on the web portal. Multiple comma-separated keys per provider; per-key monthly limits; counters reset each calendar month (NTP).

**Order:** AirLabs → FlightAware → FR24 (first enabled provider with quota wins per callsign).

One live API call per uncached callsign on first flight-detail open; results cached in RAM + flash (`/route_cache.csv`, up to 1500 rows, downloadable from the portal). Cached callsigns don’t count toward limits.


| Service     | Sign up                                                                |
| ----------- | ---------------------------------------------------------------------- |
| AirLabs     | [airlabs.co/signup](https://airlabs.co/signup)                         |
| FlightAware | [aeroapi signup](https://www.flightaware.com/aeroapi/signup/personal)  |
| FR24        | [fr24api docs](https://fr24api.flightradar24.com/docs/getting-started) |


## License

### Firmware

Original application code, tools, and documentation in this repository are licensed under **[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International](https://creativecommons.org/licenses/by-nc-sa/4.0/)** ([LICENSE](LICENSE)).

- **Attribution:** credit the author and link to the license when you share or adapt this work.
- **NonCommercial:** you may not use this material for commercial purposes without separate permission.
- **ShareAlike:** adaptations must be released under the same license.

Vendored libraries (`lib/Arduino_GFX`, `lib/SensorLib`, and PlatformIO registry dependencies) remain under **their own licenses** (GPL, MIT, etc.). Combining them into a binary does not re-license those components. Comply with each upstream license when you distribute builds.

### Enclosure license

The optional 3D-printed enclosure is **not** part of this firmware repository. Its digital files and physical prints are governed by the license shown on the linked **MakerWorld** model page. That content is published under a **Standard Digital File License**, which includes terms such as:

> This user content is licensed under a Standard Digital File License.  
> You shall not share, sub-license, sell, rent, host, transfer, or distribute in any way the digital or 3D printed versions of this object, nor any other derivative work of this object in its digital or physical format (including - but not limited to - remixes of this object, and hosting on other digital platforms). The objects may not be used without permission in any way whatsoever in which you charge money, or collect fees.

Always read the full license on MakerWorld before downloading, printing, or sharing the enclosure design.

