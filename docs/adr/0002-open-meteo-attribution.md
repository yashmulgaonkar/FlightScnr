# 0002 — Open-Meteo: Nutzungsbedingungen & Quellenangabe

Status: accepted
Datum: 2026-07-20
Kontext: Feature „Freie, key-lose Wetterquelle Open-Meteo" als Alternative/Fallback zur
bezahlten Tomorrow.io-API (analog zur freien Routenquelle adsbdb, ADR 0001).

## Kontext

Die Firmware nutzt Open-Meteo (`GET https://api.open-meteo.com/v1/forecast`) als freie,
key-lose Wetterquelle. Sie springt ein, wenn Tomorrow.io deaktiviert ist, kein Key
konfiguriert ist oder ein Tomorrow.io-Abruf fehlschlägt. Ein Opt-in-Flag `use_openmeteo`
(Default an, NVS-persistent) steuert die Quelle.

## Prüfergebnis (2026-07-20)

- **API:** key-los, JSON per GET; nicht-kommerzielle Nutzung bis 10.000 Aufrufe/Tag frei.
- **Wetterdaten:** stehen unter **CC BY 4.0** → eine **sichtbare Quellenangabe ist Pflicht**,
  sobald die Daten angezeigt werden. Das ist eine echte Attributionspflicht (anders als
  adsbdb, wo die Live-Anzeige als normaler API-Konsum gilt).

## Entscheidung

- **Sichtbare Attribution** „Weather data: Open-Meteo.com" wird dezent (klein, gedimmt, unten)
  auf dem **Wetter-/Forecast-Screen** angezeigt — **nur dann**, wenn die aktuell dargestellten
  Daten tatsächlich von Open-Meteo stammen (`WeatherData::source == Source::OpenMeteo`), nicht
  bei Tomorrow.io-Daten. Dafür trägt `WeatherData` ein Quellen-Feld (`Source`).
- **Vorrang:** bezahltes Tomorrow.io zuerst (falls Key + aktiv), Open-Meteo als Fallback —
  konsistent zur adsbdb-Logik (bezahlte Quelle bevorzugt).
- Der lokale RAM-Cache dient nur der Geräte-Performance und wird nicht veröffentlicht.

## Konsequenzen

- Regelkonform (CC BY 4.0) für den aktuellen Nutzungsfall (Live-Anzeige mit sichtbarer
  Quellennennung).
- Bleibt die Tages-Quote (10k/Tag) durch das Abruf-Throttling der Firmware deutlich
  unterschritten; kein Sonder-Throttling für Open-Meteo nötig (der 429-Backoff-Pfad bleibt
  Tomorrow.io-spezifisch).
