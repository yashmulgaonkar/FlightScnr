# 0001 — adsbdb.com: Nutzungsbedingungen & Quellenangabe

Status: accepted
Datum: 2026-07-20
Kontext: Feature „Freie Flugdetail-Quelle adsbdb.com" (Spec `.scratch/free-route-enrichment/`).

## Kontext

Die Firmware nutzt adsbdb.com als freie, key-lose Routenquelle (`GET /v0/callsign/{callsign}`),
abgefragt nur beim Antippen eines Flugs. Die Spec markiert die ToS-Prüfung als harten
Release-Punkt: Falls Attribution verlangt wird, ist eine dezente Quellenangabe zu ergänzen.

## Prüfergebnis (2026-07-20)

- **adsbdb-Code:** MIT-Lizenz.
- **Zugrundeliegende Routendaten** (David J Taylor, Edinburgh / Jim Mason): tragen eine
  **Redistributions-/Datenbank-Beschränkung** — die Daten „may not be copied, published, or
  incorporated into other databases without the explicit permission of David J Taylor,
  Edinburgh." Das ist **keine reine Attributionspflicht**, sondern eine Beschränkung des
  Weiterverbreitens/der Aufnahme in andere Datenbanken.
- **Aircraft-Daten** (PlaneBase) und **Fotos** (airport-data.com) verlangen Attribution, wenn
  angezeigt — im aktuellen Feature-Scope werden diese Assets nicht genutzt.

## Entscheidung

- Die transiente On-Tap-Anzeige der Route/Airline auf dem Gerät ist **normaler API-Konsum**
  (kein Re-Publishing, keine Aufnahme in eine fremde/weitergegebene Datenbank). Der lokale
  RAM-/Flash-Cache dient nur der Geräte-Performance und wird nicht veröffentlicht.
- Eine **dezente Quellennennung** erfolgt im **Info-/Status-Screen** (`adsbdb.com: on/off`).
  Das nennt die Quelle sichtbar, ohne Layoutarbeit am Flugdetail-Screen (der `photo_credit`
  dort ist ein eigenes Layout-Element mit reserviertem Platz — Detail-Screen-Redesign ist
  laut Spec out of scope).
- **Kein** eigener Attributions-Slot auf dem Flugdetail-Screen in dieser Runde. Falls später
  gewünscht, ist das eine separate Layout-Entscheidung.

## Konsequenzen

- Regelkonform für den aktuellen Nutzungsfall (Live-Anzeige, kein Redistribute).
- Wird künftig ein Export/Sharing der Routendaten (z.B. öffentlicher Cache-Download als
  Datenquelle für Dritte) eingeführt, ist die Redistributions-Beschränkung neu zu bewerten.
  Der bestehende Cache-CSV-Download ist ein lokales Betriebs-/Debug-Feature, kein Publishing.
