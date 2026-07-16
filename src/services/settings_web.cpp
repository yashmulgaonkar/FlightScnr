#include "services/settings_web.h"

#include <WebServer.h>
#include <WiFi.h>

#include <esp_heap_caps.h>

#include <cmath>
#include <cstdio>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "hardware/buzzer.h"
#include "hardware/display_brightness.h"
#include "services/adsb_client.h"
#include "services/api_keys.h"
#include "services/clock_time.h"
#include "services/map_center.h"
#include "services/route_cache_store.h"
#include "services/settings_apply.h"
#include "services/tz_lookup.h"
#include "services/weather.h"
#include "services/aircraft_alert.h"
#include "services/off_hours.h"
#include "services/wifi_setup.h"
#include "ui/display_prefs.h"
#include "ui/radar_accent.h"
#include "ui/radar_scale.h"

namespace {

WebServer* s_server = nullptr;
bool s_active = false;

/** Page compose buffer — PSRAM, allocated on first request.
 *  Must not be a static internal-DRAM array (24 KB of DRAM gone at link time)
 *  and must never pass through WebServer::send(code, type, const char*): that
 *  overload copies the whole page into an internal-heap String, which under a
 *  ~26 KB post-detail heap starved lwIP (min free 5 KB), stalled loop() for
 *  ~12 s, and left max_blk permanently fragmented below the panel/TLS gates. */
constexpr size_t kSettingsPageCap = 28672;
char* s_settings_page = nullptr;

char* settingsPageBuffer() {
  if (s_settings_page == nullptr) {
    s_settings_page = static_cast<char*>(
        heap_caps_malloc(kSettingsPageCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  return s_settings_page;
}

const char kPageHead[] = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlightScnr Settings</title>
<style>
:root{--bg:#0a0a0a;--card:#141414;--card2:#1c1c1c;--line:#333;--text:#ececec;
--muted:#9a9a9a;--accent:#1a9c3c;--accent2:#22c24c;--field:#1e1e1e;--fline:#3a3a3a;--link:#cfcfcf;}
*{box-sizing:border-box;}
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:var(--bg);
color:var(--text);line-height:1.45;padding-bottom:5.5rem;}
.wrap{max-width:34rem;margin:0 auto;padding:1.25rem 1rem;}
.app-head{display:flex;align-items:center;gap:.65rem;margin-bottom:.25rem;}
.logo{width:2.1rem;height:2.1rem;border-radius:50%;flex:none;border:1px solid #0a5;
background:radial-gradient(circle at 50% 50%,#0c2,#063 70%,#021 100%);}
h1{font-size:1.3rem;margin:0;}
.subtitle{color:var(--muted);font-size:.85rem;margin:.15rem 0 1.1rem;}
.banner{background:#143d22;border:1px solid #1a9c3c;color:#d8ffe4;border-radius:10px;
padding:.7rem .85rem;margin:0 0 1rem;font-size:.9rem;}
.banner b{color:#fff;}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;margin:0 0 .9rem;overflow:hidden;}
.card>summary{list-style:none;cursor:pointer;display:flex;align-items:center;gap:.6rem;
padding:.85rem 1rem;font-weight:600;font-size:.98rem;user-select:none;}
.card>summary::-webkit-details-marker{display:none;}
.card>summary .ico{width:1.6rem;height:1.6rem;border-radius:7px;flex:none;display:grid;
place-items:center;font-size:.95rem;background:var(--card2);border:1px solid var(--line);}
.card>summary .chev{margin-left:auto;color:var(--muted);transition:transform .18s;font-size:.8rem;}
.card[open]>summary .chev{transform:rotate(90deg);}
.card>summary .sum{color:var(--muted);font-weight:400;font-size:.78rem;margin-left:.1rem;}
.card .body{padding:.25rem 1rem 1rem;border-top:1px solid var(--line);}
label{display:block;margin:.85rem 0 .3rem;font-size:.84rem;color:#d2d2d2;}
input,select{width:100%;padding:.6rem .65rem;border-radius:9px;border:1px solid var(--fline);
background:var(--field);color:#fff;font-size:1rem;outline:none;}
input:focus,select:focus{border-color:var(--accent2);box-shadow:0 0 0 2px rgba(34,194,76,.2);}
input::placeholder{color:#777;}
.hint,.note{color:var(--muted);font-size:.78rem;margin:.4rem 0 0;}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:.6rem;}
@media(max-width:24rem){.row2{grid-template-columns:1fr;}}
.chk{display:flex;align-items:center;gap:.6rem;padding:.7rem 0;border-bottom:1px solid rgba(120,120,120,.22);}
.chk:last-of-type{border-bottom:none;}
.chk label{margin:0;font-size:.9rem;color:var(--text);flex:1;}
.switch{position:relative;width:2.6rem;height:1.5rem;flex:none;}
.switch input{position:absolute;opacity:0;width:100%;height:100%;margin:0;cursor:pointer;}
.switch .track{position:absolute;inset:0;border-radius:999px;background:#333;
border:1px solid var(--fline);transition:.18s;pointer-events:none;}
.switch .track::after{content:"";position:absolute;top:50%;left:.18rem;transform:translateY(-50%);
width:1.05rem;height:1.05rem;border-radius:50%;background:#9a9a9a;transition:.18s;}
.switch input:checked + .track{background:var(--accent);border-color:var(--accent2);}
.switch input:checked + .track::after{left:1.32rem;background:#fff;}
.api{border:1px solid var(--line);border-radius:11px;padding:.2rem .8rem .8rem;margin:.8rem 0;background:var(--card2);}
.api .api-head{display:flex;align-items:center;gap:.6rem;padding:.65rem 0 0;}
.api .api-head .name{font-weight:600;font-size:.92rem;}
.api .api-head .badge{margin-left:auto;font-size:.7rem;padding:.12rem .5rem;border-radius:999px;
background:#262626;border:1px solid #444;color:#bbb;}
.usage{font-size:.76rem;color:var(--muted);margin:.5rem 0 0;background:#161616;
border:1px solid var(--line);border-radius:8px;padding:.45rem .6rem;}
.usage b{color:#ddd;}
a{color:var(--link);}
.dl{display:inline-flex;align-items:center;gap:.4rem;text-decoration:none;background:var(--card2);
border:1px solid var(--line);border-radius:9px;padding:.55rem .8rem;font-size:.88rem;margin-top:.5rem;}
.savebar{position:fixed;left:0;right:0;bottom:0;z-index:20;padding:1rem;
background:linear-gradient(180deg,rgba(10,10,10,0),rgba(10,10,10,.92) 30%,#0a0a0a);}
.savebar .inner{max-width:34rem;margin:0 auto;}
button.save{width:100%;padding:.85rem;font-size:1.02rem;font-weight:700;border:none;
border-radius:11px;background:var(--accent);color:#fff;cursor:pointer;box-shadow:0 6px 20px rgba(26,156,60,.35);}
button.save:hover{background:var(--accent2);}
.wifi-note{font-size:.78rem;color:#8a8a8a;margin-top:.55rem;}
.net-row{border:1px solid var(--line);border-radius:10px;padding:.65rem .75rem;margin:.55rem 0;
background:var(--card2);}
.net-row .top{display:flex;align-items:baseline;gap:.5rem;flex-wrap:wrap;}
.net-row .ord{font-weight:700;color:#fff;}
.net-row .ssid{font-weight:600;}
.net-row .skip{font-size:.72rem;color:#c9a227;margin-left:.15rem;}
.net-actions{display:flex;flex-wrap:wrap;gap:.4rem;margin-top:.5rem;}
button.sm,a.sm{font-size:.78rem;padding:.4rem .55rem;border-radius:8px;border:1px solid var(--fline);
background:#262626;color:#eee;cursor:pointer;text-decoration:none;display:inline-block;}
button.sm.danger{border-color:#633;background:#2a1515;color:#fcc;}
.wifi-add{margin-top:.75rem;padding-top:.65rem;border-top:1px solid var(--line);}
.foot{text-align:center;font-size:.8rem;color:var(--muted);margin:1.2rem 0 .2rem;}
.foot a{color:var(--link);}
</style></head><body>
<form id="fs-save" method="POST" action="/save">
<div class="wrap">
<div class="app-head"><div class="logo"></div><h1>FlightScnr</h1></div>
<p class="subtitle">Changes save to flash. Radar refreshes when you tap <strong>Save</strong>.</p>
)HTML";

void formatUsdMicro(uint32_t micro, char* out, size_t len, int decimals) {
  snprintf(out, len, "%.*f", decimals, static_cast<double>(micro) / 1000000.0);
}

void appendRaw(char* page, size_t len, size_t* used, const char* html) {
  if (page == nullptr || used == nullptr || html == nullptr || *used >= len) {
    return;
  }
  const int n = snprintf(page + *used, len - *used, "%s", html);
  if (n > 0) {
    const size_t space = len - *used;
    *used += static_cast<size_t>(n) < space ? static_cast<size_t>(n) : space - 1;
  }
}

void appendClamped(char* page, size_t len, size_t* used, int n) {
  if (used == nullptr || n <= 0 || *used >= len) {
    return;
  }
  const size_t space = len - *used;
  *used += static_cast<size_t>(n) < space ? static_cast<size_t>(n) : space - 1;
}

// Emits a labelled on/off toggle row (label left, switch right). id is reused as name.
void appendToggle(char* page, size_t len, size_t* used, const char* id, const char* label_html,
                  bool checked) {
  if (page == nullptr || used == nullptr || *used >= len) {
    return;
  }
  const int n = snprintf(
      page + *used, len - *used,
      "<div class=\"chk\"><label for=\"%s\">%s</label>"
      "<span class=\"switch\"><input id=\"%s\" name=\"%s\" type=\"checkbox\" value=\"T\"%s>"
      "<span class=\"track\"></span></span></div>",
      id, label_html, id, id, checked ? " checked" : "");
  appendClamped(page, len, used, n);
}

// Emits an inline toggle switch (no row chrome) for use inside an API header.
void appendInlineToggle(char* page, size_t len, size_t* used, const char* id, bool checked) {
  if (page == nullptr || used == nullptr || *used >= len) {
    return;
  }
  const int n = snprintf(
      page + *used, len - *used,
      "<span class=\"switch\"><input id=\"%s\" name=\"%s\" type=\"checkbox\" value=\"T\"%s>"
      "<span class=\"track\"></span></span>",
      id, id, checked ? " checked" : "");
  appendClamped(page, len, used, n);
}

void redirectToSettings(const char* query) {
  char loc[32];
  if (query != nullptr && query[0] != '\0') {
    snprintf(loc, sizeof(loc), "/?%s", query);
  } else {
    snprintf(loc, sizeof(loc), "/");
  }
  s_server->sendHeader("Location", loc, true);
  s_server->send(303, "text/plain", "");
}

void sendLocationErrorPage() {
  char page[960];
  snprintf(page, sizeof(page),
           "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Radar center not saved</title></head>"
           "<body style=\"font-family:system-ui,sans-serif;max-width:28rem;margin:1.5rem auto;"
           "padding:0 1rem;background:#000;color:#ececec\">"
           "<h1 style=\"font-size:1.25rem;color:#f66\">Radar center not saved</h1>"
           "<p>Other settings were saved, but the <strong>Radar Center</strong> value could "
           "not be parsed. Use decimal degrees with a comma between latitude and longitude, "
           "for example:</p>"
           "<p style=\"font-family:monospace;background:#222;padding:.75rem;border-radius:6px\">"
           "51.507400, -0.127800</p>"
           "<p style=\"color:#9a9a9a;font-size:.9rem\">Spaces around the comma are fine. "
           "Latitude must be between &minus;90 and 90; longitude between &minus;180 and 180.</p>"
           "<p><a href=\"/\" style=\"color:#cfcfcf\">Back to settings</a></p>"
           "</body></html>");
  s_server->send(400, "text/html; charset=utf-8", page);
}

void appendAccentOptions(char* buf, size_t len, size_t* used) {
  const uint8_t current = ui::radar::accentColorIndex();
  for (uint8_t i = 0; i < ui::radar::kRadarAccentCount; ++i) {
    const int n = snprintf(buf + *used, len - *used, "<option value=\"%u\"%s>%s</option>",
                           static_cast<unsigned>(i), (i == current) ? " selected" : "",
                           ui::radar::accentColorNameAt(i));
    appendClamped(buf, len, used, n);
  }
}

void appendRangeMileHint(char* buf, size_t len, size_t* used) {
  const int n = snprintf(buf + *used, len - *used,
                         "<p class=\"hint\">Number with optional unit (mi, km, nm; default mi). "
                         "Snaps to nearest preset: 2, 3, 6, 8, 10, 20, or 30 mi.</p>");
  if (n > 0) {
    appendClamped(buf, len, used, n);
  }
}

void handleSettingsPage() {
  services::apikeys::load();
  char* const page = settingsPageBuffer();
  if (page == nullptr) {
    s_server->send(503, "text/plain", "Out of memory");
    return;
  }
  size_t used = 0;
  char masked[24];
  char watch_buf[160];
  services::alert::watchCallsignsFormatted(watch_buf, sizeof(watch_buf));
  const size_t watch_count = services::alert::watchCallsignCount();

  const int head_n = snprintf(page, kSettingsPageCap, "%s", kPageHead);
  if (head_n > 0) {
    used = static_cast<size_t>(head_n) < kSettingsPageCap
               ? static_cast<size_t>(head_n)
               : kSettingsPageCap - 1;
  }

  if (s_server->hasArg("saved")) {
    appendRaw(page, kSettingsPageCap, &used,
              "<div class=\"banner\"><b>Saved.</b> Settings applied — radar will refresh."
              "</div>");
  }
  if (s_server->hasArg("wifi_ok")) {
    appendRaw(page, kSettingsPageCap, &used,
              "<div class=\"banner\"><b>Wi&#8209;Fi updated.</b></div>");
  }
  if (s_server->hasArg("wifi_err")) {
    appendRaw(page, kSettingsPageCap, &used,
              "<div class=\"banner\" style=\"background:#3d1414;border-color:#c33\">"
              "<b>Wi&#8209;Fi change failed.</b> Check SSID/password and free slots (max 3)."
              "</div>");
  }

  // ---------- Radar card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\" open><summary><span class=\"ico\">&#9678;</span>Radar"
            "<span class=\"sum\">center, range, units</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">");

  char center_value[48];
  snprintf(center_value, sizeof(center_value), "%.6f, %.6f",
           services::map_center::latitude(), services::map_center::longitude());
  const int center_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"radar_center\">Radar center (lat, lon)</label>"
      "<input id=\"radar_center\" name=\"radar_center\" type=\"text\" required "
      "autocomplete=\"off\" placeholder=\"37.636422, -122.365968\" value=\"%s\">",
      center_value);
  appendClamped(page, kSettingsPageCap, &used, center_n);

  const ui::radar::DistanceUnit unit = ui::radar::distanceUnit();
  const int range_units_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"row2\"><div>"
      "<label for=\"range_mi\">Range</label>"
      "<input id=\"range_mi\" name=\"range_mi\" type=\"text\" required autocomplete=\"off\" "
      "placeholder=\"e.g. 30mi, 48km, 26nm\" value=\"%umi\">"
      "</div><div>"
      "<label for=\"dist_unit\">Distance units</label>"
      "<select id=\"dist_unit\" name=\"dist_unit\">"
      "<option value=\"km\"%s>kilometers</option>"
      "<option value=\"mi\"%s>statute miles</option>"
      "<option value=\"nm\"%s>nautical miles</option>"
      "</select></div></div>",
      static_cast<unsigned>(ui::radar::scaleActiveMiles()),
      unit == ui::radar::DistanceUnit::Km ? " selected" : "",
      unit == ui::radar::DistanceUnit::StatuteMile ? " selected" : "",
      unit == ui::radar::DistanceUnit::NauticalMile ? " selected" : "");
  appendClamped(page, kSettingsPageCap, &used, range_units_n);
  appendRangeMileHint(page, kSettingsPageCap, &used);

  const int min_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"min_height\">Min altitude floor (ft, 0 = off)</label>"
      "<input id=\"min_height\" name=\"min_height\" type=\"number\" min=\"0\" step=\"100\" "
      "value=\"%d\">",
      services::adsb::altitudeFloorFt());
  appendClamped(page, kSettingsPageCap, &used, min_n);

  appendRaw(page, kSettingsPageCap, &used,
            "<label for=\"radar_accent\">Color theme</label>"
            "<select id=\"radar_accent\" name=\"radar_accent\">");
  appendAccentOptions(page, kSettingsPageCap, &used);
  appendRaw(page, kSettingsPageCap, &used, "</select>");

  appendToggle(page, kSettingsPageCap, &used, "show_cardinals", "Show compass rose",
               ui::radar::showCompassRose());
  {
    const int facing_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<label for=\"facing_deg\">Facing direction (degrees at top of radar, 0=N)</label>"
        "<input id=\"facing_deg\" name=\"facing_deg\" type=\"number\" min=\"0\" max=\"359\" "
        "step=\"5\" value=\"%u\">"
        "<p class=\"hint\">0=North, 90=East, 180=South, 270=West. Snaps to 5&deg; steps. "
        "On the device: Settings &rarr; Display &rarr; Facing, then turn the dial.</p>",
        static_cast<unsigned>(ui::radar::facingDeg()));
    if (facing_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, facing_n);
    }
  }
  appendToggle(page, kSettingsPageCap, &used, "show_sweep", "Show radar sweep line",
               ui::displayPrefsSweepLineEnabled());

  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Display & screens card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9788;</span>"
            "Display &amp; screens<span class=\"sum\">brightness, timeouts, clock</span>"
            "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">");

  const uint8_t bright = hardware::displayBrightnessPercent();
  const int bright_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"bright_pct\">Screen brightness</label>"
      "<select id=\"bright_pct\" name=\"bright_pct\">"
      "<option value=\"20\"%s>20%%</option>"
      "<option value=\"40\"%s>40%%</option>"
      "<option value=\"60\"%s>60%%</option>"
      "<option value=\"80\"%s>80%%</option>"
      "<option value=\"100\"%s>100%%</option>"
      "</select>",
      bright == 20 ? " selected" : "", bright == 40 ? " selected" : "",
      bright == 60 ? " selected" : "", bright == 80 ? " selected" : "",
      bright == 100 ? " selected" : "");
  appendClamped(page, kSettingsPageCap, &used, bright_n);

  const unsigned long detail_sec = ui::displayPrefsFlightDetailTimeoutMs() / 1000UL;
  const unsigned long clock_sec = ui::displayPrefsClockWeatherTimeoutMs() / 1000UL;
  const int timeouts_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"row2\"><div>"
      "<label for=\"detail_timeout\">Flight detail screen</label>"
      "<select id=\"detail_timeout\" name=\"detail_timeout\">"
      "<option value=\"0\"%s>Manual (swipe)</option>"
      "<option value=\"10\"%s>10 seconds</option>"
      "<option value=\"20\"%s>20 seconds</option>"
      "<option value=\"30\"%s>30 seconds</option>"
      "</select></div><div>"
      "<label for=\"clock_timeout\">Clock / forecast</label>"
      "<select id=\"clock_timeout\" name=\"clock_timeout\">"
      "<option value=\"0\"%s>Manual (swipe)</option>"
      "<option value=\"5\"%s>5 seconds</option>"
      "<option value=\"10\"%s>10 seconds</option>"
      "<option value=\"15\"%s>15 seconds</option>"
      "</select></div></div>",
      detail_sec == 0 ? " selected" : "", detail_sec == 10 ? " selected" : "",
      detail_sec == 20 ? " selected" : "", detail_sec == 30 ? " selected" : "",
      clock_sec == 0 ? " selected" : "", clock_sec == 5 ? " selected" : "",
      clock_sec == 10 ? " selected" : "", clock_sec == 15 ? " selected" : "");
  appendClamped(page, kSettingsPageCap, &used, timeouts_n);

  appendToggle(page, kSettingsPageCap, &used, "idle_clock",
               "Return to clock when no aircraft visible",
               ui::displayPrefsAutoIdleClockEnabled());
  appendToggle(page, kSettingsPageCap, &used, "auto_timezone",
               "Auto timezone from radar center (DST-aware)",
               services::clock::useAutoTimezone());
  appendRaw(page, kSettingsPageCap, &used,
            "<p class=\"note\">Auto timezone resolves the local zone from your radar lat/lon over "
            "Wi&#8209;Fi. Turn the clock-settings knob on the device to override manually.</p>");

  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Off-Hours card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9790;</span>"
            "Off-Hours<span class=\"sum\">night mode schedule</span>"
            "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">");
  appendToggle(page, kSettingsPageCap, &used, "night_en", "Enable off-hours (night mode)",
               services::offhours::enabled());
  {
    const auto night_mode = services::offhours::mode();
    const int nm = snprintf(
        page + used, kSettingsPageCap - used,
        "<label for=\"night_mode\">During off-hours</label>"
        "<select id=\"night_mode\" name=\"night_mode\">"
        "<option value=\"0\"%s>Dim clock (20%%)</option>"
        "<option value=\"1\"%s>Turn off display</option>"
        "</select>",
        night_mode == services::offhours::Mode::Dim ? " selected" : "",
        night_mode == services::offhours::Mode::DisplayOff ? " selected" : "");
    if (nm > 0) appendClamped(page, kSettingsPageCap, &used, nm);
  }
  {
    const uint16_t start = services::offhours::startMinute();
    const uint16_t end = services::offhours::endMinute();
    const int nt = snprintf(
        page + used, kSettingsPageCap - used,
        "<div class=\"row2\">"
        "<div><label for=\"night_start\">Start</label>"
        "<input type=\"time\" id=\"night_start\" name=\"night_start\" value=\"%02u:%02u\"></div>"
        "<div><label for=\"night_end\">End</label>"
        "<input type=\"time\" id=\"night_end\" name=\"night_end\" value=\"%02u:%02u\"></div>"
        "</div>",
        start / 60, start % 60, end / 60, end % 60);
    if (nt > 0) appendClamped(page, kSettingsPageCap, &used, nt);
  }
  appendRaw(page, kSettingsPageCap, &used,
            "<p class=\"note\">During off-hours the device shows a dim clock or turns off the "
            "display. All API calls are paused. Knob press wakes the device.</p>");
  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Sound card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9835;</span>Sound"
            "<span class=\"sum\">UI beep</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">");
  appendToggle(page, kSettingsPageCap, &used, "ui_beep", "UI beep on touch and knob",
               hardware::buzzerEnabled());
  const char beep_tone = hardware::buzzerToneLetter();
  const int beep_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"beep_tone\">Beep tone</label>"
      "<select id=\"beep_tone\" name=\"beep_tone\">"
      "<option value=\"A\"%s>A</option>"
      "<option value=\"B\"%s>B</option>"
      "<option value=\"C\"%s>C</option>"
      "<option value=\"D\"%s>D</option>"
      "<option value=\"E\"%s>E</option>"
      "</select>",
      beep_tone == 'A' ? " selected" : "", beep_tone == 'B' ? " selected" : "",
      beep_tone == 'C' ? " selected" : "", beep_tone == 'D' ? " selected" : "",
      beep_tone == 'E' ? " selected" : "");
  appendClamped(page, kSettingsPageCap, &used, beep_n);
  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Alerts card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\" open><summary><span class=\"ico\">&#9888;</span>Alerts"
            "<span class=\"sum\">aircraft alerts</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">");
  appendToggle(page, kSettingsPageCap, &used, "alert_mil", "Alert on military aircraft",
               services::alert::militaryAlertEnabled());
  appendToggle(page, kSettingsPageCap, &used, "alert_emrg",
               "Alert on emergency squawk (7700/7600/7500)",
               services::alert::emergencyAlertEnabled());
  appendToggle(page, kSettingsPageCap, &used, "alert_hide",
               "Hide non-alerted aircraft on radar",
               services::alert::hideNonAlertedEnabled());
  const int watch_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"alert_watch\">Alert on ICAO flight numbers</label>"
      "<input id=\"alert_watch\" type=\"text\" "
      "autocomplete=\"off\" placeholder=\"ACA739, UAL123\" value=\"%s\">",
      watch_buf);

  appendClamped(page, kSettingsPageCap, &used, watch_n);
  if (watch_count == 0) {
    appendRaw(page, kSettingsPageCap, &used,
              "<p class=\"usage\">No flight numbers tracked.</p>");
  } else {
    const int tracked_n = snprintf(page + used, kSettingsPageCap - used,
                                   "<p class=\"usage\"><b>Tracking %u:</b> %s</p>",
                                   static_cast<unsigned>(watch_count), watch_buf);
    if (tracked_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, tracked_n);
    }
  }
  appendRaw(page, kSettingsPageCap, &used,
            "<p class=\"hint\">Comma-separated callsigns (3-letter airline + flight number). "
            "Buzzes and highlights when a watched flight appears on radar. "
            "To clear all, delete the text in the field above and tap <b>Save</b>.</p>");
  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Route APIs card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9992;</span>Route APIs"
            "<span class=\"sum\">airline &amp; route lookup</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">"
            "<p class=\"note\">Enabled providers run in order &mdash; <b>FlightAware &rarr; AirLabs "
            "&rarr; FR24</b> &mdash; first hit per callsign wins. Paste multiple keys "
            "comma-separated (key1, key2, key3); when one hits its monthly cap the next is used "
            "before moving to the next provider. Leave blank to keep the saved value. Caps reset "
            "on the 1st once NTP syncs.</p>");

  // FlightAware (1st priority)
  appendRaw(page, kSettingsPageCap, &used, "<div class=\"api\"><div class=\"api-head\">");
  appendInlineToggle(page, kSettingsPageCap, &used, "use_flightaware",
                     services::apikeys::useFlightAware());
  appendRaw(page, kSettingsPageCap, &used,
            "<span class=\"name\">FlightAware</span><span class=\"badge\">1st</span></div>");

  services::apikeys::maskedFlightAware(masked, sizeof(masked));
  char fa_budget[16];
  char fa_cost[16];
  char fa_spent[16];
  formatUsdMicro(services::apikeys::flightAwareBudgetUsdMicro(), fa_budget, sizeof(fa_budget), 2);
  formatUsdMicro(services::apikeys::flightAwareCostUsdMicro(), fa_cost, sizeof(fa_cost), 4);
  formatUsdMicro(services::apikeys::flightAwareSpentUsdMicro(), fa_spent, sizeof(fa_spent), 4);
  char fa_used_note[72];
  if (services::apikeys::flightAwareBudgetUsdMicro() == 0) {
    snprintf(fa_used_note, sizeof(fa_used_note), "Spent this month: $%s (unlimited budget)",
             fa_spent);
  } else {
    snprintf(fa_used_note, sizeof(fa_used_note), "Spent this month: $%s of $%s", fa_spent,
             fa_budget);
  }
  const int fa_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"flightaware_key\">AeroAPI keys (%s)</label>"
      "<input id=\"flightaware_key\" name=\"flightaware_key\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"key1, key2, key3\">"
      "<div class=\"row2\"><div>"
      "<label for=\"flightaware_max_usd\">Max budget / key ($, 0 = unlimited)</label>"
      "<input id=\"flightaware_max_usd\" name=\"flightaware_max_usd\" type=\"number\" min=\"0\" "
      "step=\"0.01\" value=\"%s\">"
      "</div><div>"
      "<label for=\"flightaware_cost_usd\">Cost per call ($)</label>"
      "<input id=\"flightaware_cost_usd\" name=\"flightaware_cost_usd\" type=\"number\" min=\"0\" "
      "step=\"0.0001\" value=\"%s\">"
      "</div></div>"
      "<p class=\"hint\">AeroAPI GET /flights/{ident}; default $0.005 per result set.</p>"
      "<p class=\"usage\">%s</p></div>",
      masked, fa_budget, fa_cost, fa_used_note);
  appendClamped(page, kSettingsPageCap, &used, fa_n);

  // AirLabs (2nd priority)
  appendRaw(page, kSettingsPageCap, &used, "<div class=\"api\"><div class=\"api-head\">");
  appendInlineToggle(page, kSettingsPageCap, &used, "use_airlabs",
                     services::apikeys::useAirLabs());
  appendRaw(page, kSettingsPageCap, &used,
            "<span class=\"name\">AirLabs</span><span class=\"badge\">2nd</span></div>");

  services::apikeys::maskedAirLabs(masked, sizeof(masked));
  char al_used_note[64];
  if (services::apikeys::airLabsMaxCalls() == 0) {
    snprintf(al_used_note, sizeof(al_used_note), "Used this month: %u (unlimited cap)",
             static_cast<unsigned>(services::apikeys::airLabsCallsUsed()));
  } else {
    snprintf(al_used_note, sizeof(al_used_note), "Used this month: %u / %u",
             static_cast<unsigned>(services::apikeys::airLabsCallsUsed()),
             static_cast<unsigned>(services::apikeys::airLabsMaxCalls()));
  }
  const int al_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"airlabs_key\">API keys (%s)</label>"
      "<input id=\"airlabs_key\" name=\"airlabs_key\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"key1, key2, key3\">"
      "<label for=\"airlabs_max_calls\">Max calls / month per key (0 = unlimited; "
      "free tier 1,000/mo)</label>"
      "<input id=\"airlabs_max_calls\" name=\"airlabs_max_calls\" type=\"number\" min=\"0\" "
      "step=\"1\" value=\"%u\">"
      "<p class=\"usage\">%s</p></div>",
      masked, static_cast<unsigned>(services::apikeys::airLabsMaxCalls()), al_used_note);
  appendClamped(page, kSettingsPageCap, &used, al_n);

  // FlightRadar24 (3rd priority)
  appendRaw(page, kSettingsPageCap, &used, "<div class=\"api\"><div class=\"api-head\">");
  appendInlineToggle(page, kSettingsPageCap, &used, "use_fr24", services::apikeys::useFr24());
  appendRaw(page, kSettingsPageCap, &used,
            "<span class=\"name\">FlightRadar24</span><span class=\"badge\">3rd</span></div>");

  services::apikeys::maskedFr24(masked, sizeof(masked));
  char fr_budget[16];
  char fr_cost[16];
  char fr_spent[16];
  formatUsdMicro(services::apikeys::fr24BudgetUsdMicro(), fr_budget, sizeof(fr_budget), 2);
  formatUsdMicro(services::apikeys::fr24CostUsdMicro(), fr_cost, sizeof(fr_cost), 4);
  formatUsdMicro(services::apikeys::fr24SpentUsdMicro(), fr_spent, sizeof(fr_spent), 4);
  char fr_used_note[72];
  if (services::apikeys::fr24BudgetUsdMicro() == 0) {
    snprintf(fr_used_note, sizeof(fr_used_note), "Spent this month: $%s (unlimited budget)",
             fr_spent);
  } else {
    snprintf(fr_used_note, sizeof(fr_used_note), "Spent this month: $%s of $%s", fr_spent,
             fr_budget);
  }
  const int fr_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"fr24_key\">API tokens (%s)</label>"
      "<input id=\"fr24_key\" name=\"fr24_key\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"token1, token2, token3\">"
      "<div class=\"row2\"><div>"
      "<label for=\"fr24_max_usd\">Max budget / key ($, 0 = unlimited)</label>"
      "<input id=\"fr24_max_usd\" name=\"fr24_max_usd\" type=\"number\" min=\"0\" "
      "step=\"0.01\" value=\"%s\">"
      "</div><div>"
      "<label for=\"fr24_cost_usd\">Cost per call ($)</label>"
      "<input id=\"fr24_cost_usd\" name=\"fr24_cost_usd\" type=\"number\" min=\"0\" "
      "step=\"0.0001\" value=\"%s\">"
      "</div></div>"
      "<p class=\"usage\">%s</p></div>",
      masked, fr_budget, fr_cost, fr_used_note);
  appendClamped(page, kSettingsPageCap, &used, fr_n);

  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Weather card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9729;</span>Weather"
            "<span class=\"sum\">Tomorrow.io</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">");
  appendToggle(page, kSettingsPageCap, &used, "use_weather", "Use Tomorrow.io",
               services::apikeys::useWeather());
  services::apikeys::maskedWeather(masked, sizeof(masked));
  const int wx_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"weather_key\">API key (%s)</label>"
      "<input id=\"weather_key\" name=\"weather_key\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"paste key\">"
      "<label for=\"weather_units\">Units</label>"
      "<select id=\"weather_units\" name=\"weather_units\">"
      "<option value=\"metric\"%s>Metric (&deg;C)</option>"
      "<option value=\"imperial\"%s>Imperial (&deg;F)</option>"
      "</select>"
      "<p class=\"note\">Current conditions show on the clock screen; swipe right for the 3-day "
      "forecast.</p></div></details>",
      masked, services::weather::useImperial() ? "" : " selected",
      services::weather::useImperial() ? " selected" : "");
  appendClamped(page, kSettingsPageCap, &used, wx_n);

  // Close the settings form but keep .wrap open so Wi-Fi / route-cache cards
  // share the same column spacing (nested forms cannot live inside /save).
  const int form_close_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<input type=\"hidden\" name=\"alert_watch\" id=\"alert_watch_post\" value=\"%s\">"
      "<script>"
      "document.getElementById('fs-save').addEventListener('submit',function(){"
      "var v=document.getElementById('alert_watch'),h=document.getElementById('alert_watch_post');"
      "if(v&&h){h.value=v.value;}"
      "});"
      "</script>"
      "</form>",
      watch_buf);
  appendClamped(page, kSettingsPageCap, &used, form_close_n);

  // ---------- Wi-Fi networks card (above route cache) ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">W</span>"
            "Wi&#8209;Fi networks<span class=\"sum\">up to 3</span>"
            "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">"
            "<p class=\"note\">Tried in preference order (#1 first). After repeated failures a "
            "network is temporarily skipped this session until you remove or edit it.</p>");

  const uint8_t net_n = wifiNetsCount();
  if (net_n == 0) {
    appendRaw(page, kSettingsPageCap, &used, "<p class=\"note\">No saved networks.</p>");
  }
  for (uint8_t i = 0; i < net_n; ++i) {
    char ssid[33] = "";
    wifiNetsGetSsid(i, ssid, sizeof(ssid));
    char esc[96];
    size_t eo = 0;
    for (size_t c = 0; ssid[c] != '\0' && eo + 6 < sizeof(esc); ++c) {
      if (ssid[c] == '&') {
        memcpy(esc + eo, "&amp;", 5);
        eo += 5;
      } else if (ssid[c] == '<') {
        memcpy(esc + eo, "&lt;", 4);
        eo += 4;
      } else if (ssid[c] == '"') {
        memcpy(esc + eo, "&quot;", 6);
        eo += 6;
      } else {
        esc[eo++] = ssid[c];
      }
    }
    esc[eo] = '\0';

    const int row_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<div class=\"net-row\"><div class=\"top\"><span class=\"ord\">#%u</span>"
        "<span class=\"ssid\">%s</span>%s</div>"
        "<div class=\"net-actions\">"
        "<form method=\"POST\" action=\"/wifi/up\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"i\" value=\"%u\">"
        "<button class=\"sm\" type=\"submit\"%s>Move up</button></form>"
        "<form method=\"POST\" action=\"/wifi/down\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"i\" value=\"%u\">"
        "<button class=\"sm\" type=\"submit\"%s>Move down</button></form>"
        "<form method=\"POST\" action=\"/wifi/remove\" style=\"display:inline\" "
        "onsubmit=\"return confirm('Remove this network?');\">"
        "<input type=\"hidden\" name=\"i\" value=\"%u\">"
        "<button class=\"sm danger\" type=\"submit\">Remove</button></form>"
        "</div>"
        "<form method=\"POST\" action=\"/wifi/pass\" style=\"margin-top:.55rem\">"
        "<input type=\"hidden\" name=\"i\" value=\"%u\">"
        "<label for=\"pass%u\">Update password</label>"
        "<div class=\"row2\"><input id=\"pass%u\" name=\"p\" type=\"password\" "
        "autocomplete=\"new-password\" placeholder=\"new password\">"
        "<button class=\"sm\" type=\"submit\">Update</button></div></form></div>",
        static_cast<unsigned>(i + 1), esc,
        wifiNetsIsDemoted(i) ? " <span class=\"skip\">temporarily skipped</span>" : "",
        static_cast<unsigned>(i), i == 0 ? " disabled" : "",
        static_cast<unsigned>(i), (i + 1 >= net_n) ? " disabled" : "",
        static_cast<unsigned>(i), static_cast<unsigned>(i), static_cast<unsigned>(i),
        static_cast<unsigned>(i));
    if (row_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, row_n);
    }
  }

  if (net_n < config::kWifiMaxNetworks) {
    const int add_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<div class=\"wifi-add\"><form method=\"POST\" action=\"/wifi/add\">"
        "<label for=\"wifi_ssid\">Add network (%u/%u)</label>"
        "<input id=\"wifi_ssid\" name=\"s\" maxlength=\"32\" required "
        "placeholder=\"SSID\">"
        "<label for=\"wifi_pass\">Password</label>"
        "<input id=\"wifi_pass\" name=\"p\" type=\"password\" maxlength=\"63\" "
        "autocomplete=\"new-password\" placeholder=\"password\">"
        "<p style=\"margin-top:.6rem\"><button class=\"sm\" type=\"submit\">"
        "Add network</button></p></form></div>",
        static_cast<unsigned>(net_n), static_cast<unsigned>(config::kWifiMaxNetworks));
    if (add_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, add_n);
    }
  } else {
    appendRaw(page, kSettingsPageCap, &used,
              "<p class=\"note\">Store full (3/3). Remove one before adding another.</p>");
  }

  appendRaw(page, kSettingsPageCap, &used,
            "<p class=\"wifi-note\">Hold knob 5&nbsp;s clears all networks and opens the setup "
            "portal.</p></div></details>");

  // ---------- Route cache card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#8681;</span>Route cache"
            "<span class=\"sum\">export</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">"
            "<p class=\"note\">Airline/route lookups are cached on flash (written about every "
            "10&nbsp;min) so repeat callsigns don't re-bill an API.</p>"
            "<a class=\"dl\" href=\"/route_cache.csv\" download=\"route_cache.csv\">"
            "&#8681;&nbsp; Download route_cache.csv</a></div></details>");

  const int tail_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<p class=\"foot\"><a href=\"%s\" target=\"_blank\" rel=\"noopener\">"
      "github.com/yashmulgaonkar/FlightScnr</a></p>"
      "</div>"
      "<div class=\"savebar\"><div class=\"inner\">"
      "<button class=\"save\" type=\"submit\" form=\"fs-save\">Save</button></div></div>"
      "</body></html>",
      config::kGithubRepoUrl);
  if (tail_n > 0) {
    const size_t space = kSettingsPageCap - used;
    used += static_cast<size_t>(tail_n) < space ? static_cast<size_t>(tail_n) : space - 1;
  }
  if (used >= kSettingsPageCap) {
    used = kSettingsPageCap - 1;
  }
  page[used] = '\0';
  if (used >= kSettingsPageCap - 512) {
    Serial.printf("[settings] page warn used=%u cap=%u\n", static_cast<unsigned>(used),
                  static_cast<unsigned>(kSettingsPageCap));
  }

  // Zero-copy send: set the length up front, emit headers with an empty body,
  // then stream the PSRAM buffer directly. Never pass `page` to send() — the
  // const char* overload duplicates the whole page into an internal-heap String.
  s_server->setContentLength(used);
  s_server->send(200, "text/html; charset=utf-8", "");
  s_server->sendContent(page, used);
}

void handleSave() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }

  const bool loc_ok = settingsApplyFromForm(
      s_server->arg("radar_center").c_str(), nullptr, nullptr,
      s_server->arg("dist_unit").c_str(), s_server->arg("use_miles").c_str(),
      s_server->arg("show_cardinals").c_str(),
      s_server->arg("min_height").c_str(),
      s_server->arg("range_mi").c_str(), s_server->arg("airlabs_key").c_str(),
      s_server->arg("flightaware_key").c_str(), s_server->arg("fr24_key").c_str(),
      s_server->arg("use_airlabs").c_str(), s_server->arg("use_flightaware").c_str(),
      s_server->arg("use_fr24").c_str(), s_server->arg("airlabs_max_calls").c_str(),
      s_server->arg("flightaware_max_usd").c_str(),
      s_server->arg("flightaware_cost_usd").c_str(), s_server->arg("fr24_max_usd").c_str(),
      s_server->arg("fr24_cost_usd").c_str(), s_server->arg("ui_beep").c_str(),
      s_server->arg("beep_tone").c_str(), s_server->arg("bright_pct").c_str(),
      s_server->arg("show_sweep").c_str(), s_server->arg("detail_timeout").c_str());

  const bool use_weather_before = services::apikeys::useWeather();
  const bool weather_key_saved =
      services::apikeys::saveWeatherKeyFromForm(s_server->arg("weather_key").c_str());
  services::apikeys::saveWeatherEnabledFromForm(s_server->arg("use_weather").c_str());
  services::weather::saveUnitsFromForm(s_server->arg("weather_units").c_str());
  ui::displayPrefsSaveClockWeatherTimeoutFromForm(s_server->arg("clock_timeout").c_str());
  ui::displayPrefsSaveAutoIdleClockFromForm(s_server->arg("idle_clock").c_str());
  ui::radar::saveFacingDegFromForm(s_server->arg("facing_deg").c_str());
  const bool auto_tz_before = services::clock::useAutoTimezone();
  services::clock::saveAutoTimezoneFromForm(s_server->arg("auto_timezone").c_str());
  ui::radar::accentSaveFromForm(s_server->arg("radar_accent").c_str());
  services::offhours::saveFromForm(s_server->arg("night_en").c_str(),
                                   s_server->arg("night_mode").c_str(),
                                   s_server->arg("night_start").c_str(),
                                   s_server->arg("night_end").c_str());
  const bool watch_arg_present = s_server->hasArg("alert_watch");
  char watch_form[160] = "";
  if (watch_arg_present) {
    strncpy(watch_form, s_server->arg("alert_watch").c_str(), sizeof(watch_form) - 1);
    watch_form[sizeof(watch_form) - 1] = '\0';
    Serial.printf("[alert] form watch='%s'\n", watch_form);
  } else {
    Serial.println("[alert] form watch arg missing (keeping saved list)");
  }
  services::alert::saveFromForm(s_server->arg("alert_mil").c_str(),
                                s_server->arg("alert_emrg").c_str(),
                                s_server->arg("alert_hide").c_str(),
                                watch_arg_present ? watch_form : nullptr, watch_arg_present);

  Serial.printf("Settings web save (lat/lon %s)\n", loc_ok ? "ok" : "invalid");

  if (!loc_ok) {
    sendLocationErrorPage();
    return;
  }

  if (weather_key_saved || use_weather_before != services::apikeys::useWeather()) {
    services::weather::notifyEnabledChanged();
  }

  if (auto_tz_before != services::clock::useAutoTimezone()) {
    if (services::clock::useAutoTimezone()) {
      services::tzlookup::notifyLocationChanged();
    }
  }

  redirectToSettings("saved=1");
  s_server->client().flush();
  settingsNotifySaved();
}

void handleRouteCacheDownload() {
  services::route_cache::sendDownload(s_server);
}

void handleWifiAdd() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  char err[96];
  const bool ok =
      wifiNetsAddOrUpdate(s_server->arg("s").c_str(), s_server->arg("p").c_str(), err,
                          sizeof(err));
  redirectToSettings(ok ? "wifi_ok=1" : "wifi_err=1");
}

void handleWifiRemove() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  const bool ok = wifiNetsRemove(static_cast<uint8_t>(s_server->arg("i").toInt()));
  redirectToSettings(ok ? "wifi_ok=1" : "wifi_err=1");
}

void handleWifiUp() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  wifiNetsMoveUp(static_cast<uint8_t>(s_server->arg("i").toInt()));
  redirectToSettings("wifi_ok=1");
}

void handleWifiDown() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  wifiNetsMoveDown(static_cast<uint8_t>(s_server->arg("i").toInt()));
  redirectToSettings("wifi_ok=1");
}

void handleWifiPass() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  const bool ok = wifiNetsUpdatePassword(static_cast<uint8_t>(s_server->arg("i").toInt()),
                                         s_server->arg("p").c_str());
  redirectToSettings(ok ? "wifi_ok=1" : "wifi_err=1");
}

void handleNotFound() {
  s_server->sendHeader("Location", "/", true);
  s_server->send(302, "text/plain", "");
}

void registerRoutes() {
  s_server->on("/", HTTP_GET, handleSettingsPage);
  s_server->on("/settings", HTTP_GET, handleSettingsPage);
  s_server->on("/save", HTTP_POST, handleSave);
  s_server->on("/route_cache.csv", HTTP_GET, handleRouteCacheDownload);
  s_server->on("/wifi/add", HTTP_POST, handleWifiAdd);
  s_server->on("/wifi/remove", HTTP_POST, handleWifiRemove);
  s_server->on("/wifi/up", HTTP_POST, handleWifiUp);
  s_server->on("/wifi/down", HTTP_POST, handleWifiDown);
  s_server->on("/wifi/pass", HTTP_POST, handleWifiPass);
  s_server->onNotFound(handleNotFound);
}

}  // namespace

void settingsWebStart() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (s_active && s_server != nullptr) {
    return;
  }

  services::apikeys::load();

  settingsWebStop();

  s_server = new WebServer(80);
  registerRoutes();
  s_server->begin();
  s_active = true;

  WiFi.setHostname(config::kPortalHostname);

#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Settings web: http://%s.local/  http://%s/\n",
                  config::kPortalHostname, WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("Settings web: http://%s/  (mDNS unavailable)\n",
                  WiFi.localIP().toString().c_str());
  }
#else
  Serial.printf("Settings web: http://%s/\n", WiFi.localIP().toString().c_str());
#endif
}

void settingsWebStop() {
  if (s_server != nullptr) {
    s_server->stop();
    delete s_server;
    s_server = nullptr;
  }
  if (s_settings_page != nullptr) {
    heap_caps_free(s_settings_page);
    s_settings_page = nullptr;
  }
  s_active = false;
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void settingsWebPoll() {
  if (!s_active || s_server == nullptr) {
    return;
  }
  s_server->handleClient();
}

bool settingsWebActive() { return s_active; }
