# Aircraft radar icons (Pi-style)

Source silhouettes match FlightScnr_Pi `flightscnr/assets/aircraft/icons/`.

- `aircraft-icons.json` — category list + ICAO type → category map (committed)
- `*.png` — silhouettes with alpha (gitignored; copy from Pi or keep local)

Build converts PNGs to 32×32 alpha masks in `include/data/aircraft_icons_lookup.h`
(`tools/aircraft_icons_to_header.py`): crop to ink, thicken thin strokes at a
fixed working size, then zoom-to-fit into the square (aspect preserved). PNGs
that store gray RGB under `alpha=0` use the alpha channel only.

Firmware tints and rotates them on the radar (vector V-wing fallback if missing).

```bash
python tools/aircraft_icons_to_header.py
```
