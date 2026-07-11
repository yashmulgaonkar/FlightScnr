# Aircraft radar icons (Pi-style)

Source silhouettes match FlightScnr_Pi `flightscnr/assets/aircraft/icons/`.

- `aircraft-icons.json` — category list + ICAO type → category map (committed)
- `*.png` — black alpha silhouettes (gitignored; copy from Pi or regenerate cache)

Build converts PNGs to 32×32 alpha masks in `include/data/aircraft_icons_lookup.h`.
Firmware tints and rotates them on the radar (vector V-wing fallback if missing).

```bash
# From a FlightScnr_Pi checkout:
python -c "..."  # or copy PNGs into this folder
python tools/aircraft_icons_to_header.py
```
