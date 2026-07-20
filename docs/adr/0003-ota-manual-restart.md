# 0003 — Web-OTA: manueller Neustart statt programmatischem Reboot

Status: accepted
Datum: 2026-07-20
Kontext: Feature „Firmware-Update per Datei-Upload aus dem Settings-Web-Interface"
(OTA über die `Update.h`-Bibliothek des ESP32-Arduino-Cores).

## Kontext

Das Web-OTA schreibt das hochgeladene App-Image (`firmware.bin`) in die inaktive
OTA-App-Partition und schaltet `otadata` erst nach vollständigem, validiertem
`Update.end(true)` um. Danach muss das Gerät neu starten, damit der Bootloader in
die neue App wechselt.

Der ursprüngliche Plan war ein **automatischer Reboot** nach dem Upload
(verzögert, damit die HTTP-Antwort noch rausgeht).

## Prüfergebnis (Gerätetests)

Auf dieser Hardware (LilyGO T-Encoder Pro: ESP32-S3, USB-CDC-on-boot
`ARDUINO_USB_CDC_ON_BOOT=1`, OPI-PSRAM `qio_opi`, AMOLED-QSPI) **hängt ein
programmatischer Reset in der frühen Boot-Init, wenn kein USB-Host am CDC-Port
angeschlossen ist**. Belegt per A/B-Test:

- Serial-Monitor (USB-Host) angeschlossen → das Gerät bootet nach dem Reset sauber.
- Kein Host angeschlossen → das Gerät hängt; nur ein Hardware-/Power-Reset recovert.

Beide programmatischen Reset-Varianten wurden getestet und zeigen denselben Hang:

1. `esp_restart()` (CPU/System-Soft-Reset, `RTC_SW_CPU_RST`).
2. Voller Chip-Reset über den RTC-Watchdog (`WDT_STAGE_ACTION_RESET_RTC`, RWDT).

Vermutete Ursache: Der programmatische Reset setzt die USB-Peripherie/PHY nicht so
zurück, dass die USB-CDC-Init ohne Host durchläuft. Nur der Hardware-/Power-Reset
räumt das vollständig auf.

## Entscheidung

- **Kein automatischer Reboot nach Web-OTA.** Das Update wird geschrieben und
  validiert; danach zeigt die Weboberfläche die klare Anweisung, das Gerät
  **manuell** neu zu starten (Reset-Taste oder Strom trennen/wieder verbinden).
- Erfolgsantwort bleibt HTTP 200 (damit die Client-JS-Erfolgs-Branch greift),
  Text und Warnbanner wurden auf die manuelle Anweisung umgestellt; kein
  Auto-Reload der Seite mehr.
- Die Deferred-Reboot-Maschinerie (Pending-Flag + Zeitstempel in `settingsWebPoll`)
  wurde entfernt.

## Konsequenzen

- Web-OTA ist zuverlässig auch am Feldgerät ohne angeschlossenen Rechner — der
  Nutzer drückt nach dem Upload einmal Reset.
- **Nebenbefund:** Dieselbe Einschränkung trifft die bestehenden programmatischen
  Feld-Reboots, die weiterhin `esp_restart()` nutzen:
  - Heap-Recovery-Reboot (`src/main.cpp`),
  - WiFi „configured" / Credentials-Reset (`src/services/wifi_setup.cpp`, 2 Stellen).
  Diese können im Feld (ohne USB-Host) denselben Boot-Hang zeigen. Das ist in
  diesem OTA-PR **nicht** gefixt — eigenes Follow-up-Ticket, damit der PR sauber
  auf das OTA-Feature scoped bleibt.
- **Bekannte Einschränkung:** Der `/update`-Endpunkt ist **unauthentifiziert**
  (erreichbar, solange das Gerät im Heim-WLAN als STA verbunden ist). Das ist
  eine bewusste Design-Entscheidung — Absicherung über Confirm-Dialog + deutliche
  Warnung statt Login, konsistent zum offenen Settings-Modell (und zum
  WebFlasher). Ein optionales PIN/Token vor dem Upload ist ein möglicher
  Follow-up, falls stärkerer Schutz gewünscht wird.
