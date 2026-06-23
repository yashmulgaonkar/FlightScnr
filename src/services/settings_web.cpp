#include "services/settings_web.h"

#include <WebServer.h>
#include <WiFi.h>

#include <cmath>
#include <cstdio>

#include <esp_system.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "hardware/buzzer.h"
#include "hardware/display_brightness.h"
#include "services/adsb_client.h"
#include "services/api_keys.h"
#include "services/map_center.h"
#include "services/route_cache_store.h"
#include "services/settings_apply.h"
#include "ui/display_prefs.h"
#include "ui/radar_scale.h"

namespace {

WebServer* s_server = nullptr;
bool s_active = false;

/** Static storage — must not live on loopTask stack (~8 KB). */
constexpr size_t kSettingsPageCap = 9216;
char s_settings_page[kSettingsPageCap];

const char kPageHead[] = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlightScnr Settings</title>
<style>
body{font-family:system-ui,sans-serif;max-width:28rem;margin:1.5rem auto;padding:0 1rem;
background:#000;color:#e8f0ff;}
h1{font-size:1.25rem;margin:0 0 .5rem;}
p{color:#9ab;line-height:1.4;font-size:.9rem;}
label{display:block;margin:.75rem 0 .25rem;font-size:.85rem;}
input,select{width:100%;box-sizing:border-box;padding:.5rem;border-radius:6px;
border:1px solid #345;background:#555;color:#fff;font-size:1rem;}
.chk{margin:.75rem 0;display:flex;align-items:center;gap:.5rem;}
.chk input{width:auto;}
button{margin-top:1.25rem;width:100%;padding:.75rem;font-size:1rem;font-weight:600;
border:none;border-radius:8px;background:#1a9c3c;color:#fff;cursor:pointer;}
.note{margin-top:1rem;font-size:.8rem;color:#7a9;}
.gh{margin:.35rem 0 1rem;font-size:.85rem;text-align:center;}
.gh a{color:#6cf;}
</style></head><body>
<h1>FlightScnr</h1>
<p>Changes are saved to flash. The device reboots after you tap <strong>Save &amp; reboot</strong>.</p>
<form method="POST" action="/save">
)HTML";

const char kPageTail[] = R"HTML(
<button type="submit">Save &amp; reboot</button>
</form>
<p class="note">Wi‑Fi credentials are configured in the setup portal (hold knob 3&nbsp;s to reset).</p>
</body></html>
)HTML";

void formatUsdMicro(uint32_t micro, char* out, size_t len, int decimals) {
  snprintf(out, len, "%.*f", decimals, static_cast<double>(micro) / 1000000.0);
}

void appendGithubLink(char* page, size_t len, size_t* used) {
  const int n = snprintf(
      page + *used, len - *used,
      "<p class=\"gh\"><a href=\"%s\" target=\"_blank\" rel=\"noopener\">"
      "github.com/yashmulgaonkar/FlightScnr</a></p>",
      config::kGithubRepoUrl);
  if (n > 0) {
    *used += static_cast<size_t>(n);
  }
}

void sendRebootPage() {
  char page[640];
  snprintf(page, sizeof(page),
           "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Rebooting</title></head>"
           "<body style=\"font-family:system-ui,sans-serif;text-align:center;padding:2rem;"
           "background:#000;color:#e8f0ff\">"
           "<h1>Saved</h1><p>Rebooting&hellip;</p>"
           "<p style=\"margin-top:1.5rem;font-size:.9rem\">"
           "<a href=\"%s\" style=\"color:#6cf\" target=\"_blank\" rel=\"noopener\">"
           "github.com/yashmulgaonkar/FlightScnr</a></p>"
           "</body></html>",
           config::kGithubRepoUrl);
  s_server->send(200, "text/html; charset=utf-8", page);
}

void sendLocationErrorPage() {
  char page[960];
  snprintf(page, sizeof(page),
           "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Radar center not saved</title></head>"
           "<body style=\"font-family:system-ui,sans-serif;max-width:28rem;margin:1.5rem auto;"
           "padding:0 1rem;background:#000;color:#e8f0ff\">"
           "<h1 style=\"font-size:1.25rem;color:#f66\">Radar center not saved</h1>"
           "<p>Other settings were saved, but the <strong>Radar Center</strong> value could "
           "not be parsed. Use decimal degrees with a comma between latitude and longitude, "
           "for example:</p>"
           "<p style=\"font-family:monospace;background:#222;padding:.75rem;border-radius:6px\">"
           "51.507400, -0.127800</p>"
           "<p style=\"color:#9ab;font-size:.9rem\">Spaces around the comma are fine. "
           "Latitude must be between &minus;90 and 90; longitude between &minus;180 and 180.</p>"
           "<p><a href=\"/\" style=\"color:#6cf\">Back to settings</a></p>"
           "</body></html>");
  s_server->send(400, "text/html; charset=utf-8", page);
}

void appendRangeOptions(char* buf, size_t len, size_t* used) {
  for (uint8_t i = 0; i < ui::radar::kScaleBandCount; ++i) {
    const ui::radar::ScaleBand& p = ui::radar::kScaleBands[i];
    const int mi = static_cast<int>(lroundf(p.label_km / 1.609344f));
    const int n = snprintf(
        buf + *used, len - *used,
        "<option value=\"%u\"%s>%d km / %d mi</option>",
        static_cast<unsigned>(i),
        (i == ui::radar::scaleActiveIndex()) ? " selected" : "",
        static_cast<int>(lroundf(p.label_km)), mi);
    if (n > 0) {
      *used += static_cast<size_t>(n);
    }
  }
}

void handleSettingsPage() {
  services::apikeys::load();
  char* const page = s_settings_page;
  size_t used = 0;
  char masked[24];

  const int head_n = snprintf(page, kSettingsPageCap, "%s", kPageHead);
  if (head_n > 0) {
    used = static_cast<size_t>(head_n);
  }

  char center_value[48];
  snprintf(center_value, sizeof(center_value), "%.6f, %.6f",
           services::map_center::latitude(), services::map_center::longitude());
  const int center_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"radar_center\">Radar Center</label>"
      "<input id=\"radar_center\" name=\"radar_center\" type=\"text\" required "
      "autocomplete=\"off\" placeholder=\"37.636422, -122.365968\" value=\"%s\">",
      center_value);
  if (center_n > 0) {
    used += static_cast<size_t>(center_n);
  }

  const ui::radar::DistanceUnit unit = ui::radar::distanceUnit();
  const int units_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"dist_unit\">Distance units</label>"
      "<select id=\"dist_unit\" name=\"dist_unit\">"
      "<option value=\"km\"%s>kilometers</option>"
      "<option value=\"mi\"%s>statute miles</option>"
      "<option value=\"nm\"%s>nautical miles</option>"
      "</select>",
      unit == ui::radar::DistanceUnit::Km ? " selected" : "",
      unit == ui::radar::DistanceUnit::StatuteMile ? " selected" : "",
      unit == ui::radar::DistanceUnit::NauticalMile ? " selected" : "");
  if (units_n > 0) {
    used += static_cast<size_t>(units_n);
  }

  const int card_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"chk\"><input id=\"show_cardinals\" name=\"show_cardinals\" type=\"checkbox\" "
      "value=\"T\"%s><label for=\"show_cardinals\">Show Compass Rose</label></div>",
      ui::radar::showCompassRose() ? " checked" : "");
  if (card_n > 0) {
    used += static_cast<size_t>(card_n);
  }

  const int sweep_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"chk\"><input id=\"show_sweep\" name=\"show_sweep\" type=\"checkbox\" "
      "value=\"T\"%s><label for=\"show_sweep\">Show radar sweep line</label></div>",
      ui::displayPrefsSweepLineEnabled() ? " checked" : "");
  if (sweep_n > 0) {
    used += static_cast<size_t>(sweep_n);
  }

  const unsigned long detail_ms = ui::displayPrefsFlightDetailTimeoutMs();
  const unsigned long detail_sec = detail_ms / 1000UL;
  const int detail_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"detail_timeout\">Flight detail screen</label>"
      "<select id=\"detail_timeout\" name=\"detail_timeout\">"
      "<option value=\"0\"%s>Manual (swipe away)</option>"
      "<option value=\"10\"%s>10 seconds</option>"
      "<option value=\"20\"%s>20 seconds</option>"
      "<option value=\"30\"%s>30 seconds</option>"
      "</select>",
      detail_sec == 0 ? " selected" : "",
      detail_sec == 10 ? " selected" : "",
      detail_sec == 20 ? " selected" : "",
      detail_sec == 30 ? " selected" : "");
  if (detail_n > 0) {
    used += static_cast<size_t>(detail_n);
  }

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
  if (bright_n > 0) {
    used += static_cast<size_t>(bright_n);
  }

  const int beep_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"chk\"><input id=\"ui_beep\" name=\"ui_beep\" type=\"checkbox\" "
      "value=\"T\"%s><label for=\"ui_beep\">UI beep on touch and knob</label></div>"
      "<label for=\"beep_tone\">Beep tone</label>"
      "<select id=\"beep_tone\" name=\"beep_tone\">"
      "<option value=\"A\"%s>A</option>"
      "<option value=\"B\"%s>B</option>"
      "<option value=\"C\"%s>C</option>"
      "<option value=\"D\"%s>D</option>"
      "<option value=\"E\"%s>E</option>"
      "</select>",
      hardware::buzzerEnabled() ? " checked" : "",
      hardware::buzzerToneLetter() == 'A' ? " selected" : "",
      hardware::buzzerToneLetter() == 'B' ? " selected" : "",
      hardware::buzzerToneLetter() == 'C' ? " selected" : "",
      hardware::buzzerToneLetter() == 'D' ? " selected" : "",
      hardware::buzzerToneLetter() == 'E' ? " selected" : "");
  if (beep_n > 0) {
    used += static_cast<size_t>(beep_n);
  }

  const int min_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"min_height\">Min altitude (ft, 0 = off)</label>"
      "<input id=\"min_height\" name=\"min_height\" type=\"number\" min=\"0\" step=\"100\" "
      "value=\"%d\">",
      services::adsb::altitudeFloorFt());
  if (min_n > 0) {
    used += static_cast<size_t>(min_n);
  }

  const int range_lbl = snprintf(page + used, kSettingsPageCap - used,
                                 "<label for=\"range_idx\">Range preset</label>"
                                 "<select id=\"range_idx\" name=\"range_idx\">");
  if (range_lbl > 0) {
    used += static_cast<size_t>(range_lbl);
  }
  appendRangeOptions(page, kSettingsPageCap, &used);

  const int range_end = snprintf(page + used, kSettingsPageCap - used, "</select>");
  if (range_end > 0) {
    used += static_cast<size_t>(range_end);
  }

  const int api_hdr = snprintf(
      page + used, kSettingsPageCap - used,
      "<h2 style=\"font-size:1rem;margin:1.25rem 0 .35rem\">Route APIs</h2>"
      "<p>Enabled APIs run in order: FlightAware, then AirLabs, then FR24 (only the first hit "
      "is used per callsign). Paste multiple keys "
      "comma-separated (key1, key2, key3). When one key hits its monthly cap, the next key is "
      "used before moving to the next provider. Leave blank to keep saved value. Caps reset on "
      "the 1st when NTP time is synced.</p>");
  if (api_hdr > 0) {
    used += static_cast<size_t>(api_hdr);
  }

  const int al_chk = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"chk\"><input id=\"use_airlabs\" name=\"use_airlabs\" type=\"checkbox\" "
      "value=\"T\"%s><label for=\"use_airlabs\">Use AirLabs</label></div>",
      services::apikeys::useAirLabs() ? " checked" : "");
  if (al_chk > 0) {
    used += static_cast<size_t>(al_chk);
  }

  services::apikeys::maskedAirLabs(masked, sizeof(masked));
  const int al_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"airlabs_key\">AirLabs API keys (%s)</label>"
      "<input id=\"airlabs_key\" name=\"airlabs_key\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"key1, key2, key3\">",
      masked);
  if (al_n > 0) {
    used += static_cast<size_t>(al_n);
  }

  char al_used_note[64];
  if (services::apikeys::airLabsMaxCalls() == 0) {
    snprintf(al_used_note, sizeof(al_used_note),
             "Used this month: %u (unlimited cap)",
             static_cast<unsigned>(services::apikeys::airLabsCallsUsed()));
  } else {
    snprintf(al_used_note, sizeof(al_used_note), "Used this month: %u / %u",
             static_cast<unsigned>(services::apikeys::airLabsCallsUsed()),
             static_cast<unsigned>(services::apikeys::airLabsMaxCalls()));
  }
  const int al_lim = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"airlabs_max_calls\">AirLabs max calls / month per key (0 = unlimited; "
      "free tier: 1,000 queries/month)</label>"
      "<input id=\"airlabs_max_calls\" name=\"airlabs_max_calls\" type=\"number\" min=\"0\" "
      "step=\"1\" value=\"%u\">"
      "<p class=\"note\">%s</p>",
      static_cast<unsigned>(services::apikeys::airLabsMaxCalls()), al_used_note);
  if (al_lim > 0) {
    used += static_cast<size_t>(al_lim);
  }

  const int fa_chk = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"chk\"><input id=\"use_flightaware\" name=\"use_flightaware\" type=\"checkbox\" "
      "value=\"T\"%s><label for=\"use_flightaware\">Use FlightAware</label></div>",
      services::apikeys::useFlightAware() ? " checked" : "");
  if (fa_chk > 0) {
    used += static_cast<size_t>(fa_chk);
  }

  services::apikeys::maskedFlightAware(masked, sizeof(masked));
  const int fa_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"flightaware_key\">FlightAware AeroAPI keys (%s)</label>"
      "<input id=\"flightaware_key\" name=\"flightaware_key\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"key1, key2, key3\">",
      masked);
  if (fa_n > 0) {
    used += static_cast<size_t>(fa_n);
  }

  char fa_budget[16];
  char fa_cost[16];
  char fa_spent[16];
  formatUsdMicro(services::apikeys::flightAwareBudgetUsdMicro(), fa_budget, sizeof(fa_budget),
                 2);
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
  const int fa_lim = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"flightaware_max_usd\">FlightAware max budget per key ($, 0 = unlimited)</label>"
      "<input id=\"flightaware_max_usd\" name=\"flightaware_max_usd\" type=\"number\" min=\"0\" "
      "step=\"0.01\" value=\"%s\">"
      "<label for=\"flightaware_cost_usd\">FlightAware cost per call ($) "
      "(AeroAPI GET /flights/{ident}, default $0.005/result set)</label>"
      "<input id=\"flightaware_cost_usd\" name=\"flightaware_cost_usd\" type=\"number\" min=\"0\" "
      "step=\"0.0001\" value=\"%s\">"
      "<p class=\"note\">%s</p>",
      fa_budget, fa_cost, fa_used_note);
  if (fa_lim > 0) {
    used += static_cast<size_t>(fa_lim);
  }

  const int fr_chk = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"chk\"><input id=\"use_fr24\" name=\"use_fr24\" type=\"checkbox\" "
      "value=\"T\"%s><label for=\"use_fr24\">Use FlightRadar24</label></div>",
      services::apikeys::useFr24() ? " checked" : "");
  if (fr_chk > 0) {
    used += static_cast<size_t>(fr_chk);
  }

  services::apikeys::maskedFr24(masked, sizeof(masked));
  const int fr_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"fr24_key\">FlightRadar24 API tokens (%s)</label>"
      "<input id=\"fr24_key\" name=\"fr24_key\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"token1, token2, token3\">",
      masked);
  if (fr_n > 0) {
    used += static_cast<size_t>(fr_n);
  }

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
  const int fr_lim = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"fr24_max_usd\">FlightRadar24 max budget per key ($, 0 = unlimited)</label>"
      "<input id=\"fr24_max_usd\" name=\"fr24_max_usd\" type=\"number\" min=\"0\" "
      "step=\"0.01\" value=\"%s\">"
      "<label for=\"fr24_cost_usd\">FlightRadar24 cost per call ($)</label>"
      "<input id=\"fr24_cost_usd\" name=\"fr24_cost_usd\" type=\"number\" min=\"0\" "
      "step=\"0.0001\" value=\"%s\">"
      "<p class=\"note\">%s</p>",
      fr_budget, fr_cost, fr_used_note);
  if (fr_lim > 0) {
    used += static_cast<size_t>(fr_lim);
  }

  const int cache_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<h2 style=\"font-size:1rem;margin:1.25rem 0 .35rem\">Route cache</h2>"
      "<p>Airline/route lookups saved on flash (written about every 10&nbsp;min).</p>"
      "<p class=\"gh\"><a href=\"/route_cache.csv\" download=\"route_cache.csv\">"
      "Download route_cache.csv</a></p>");
  if (cache_n > 0) {
    used += static_cast<size_t>(cache_n);
  }

  const int tail_n = snprintf(page + used, kSettingsPageCap - used, "%s", kPageTail);
  if (tail_n > 0) {
    used += static_cast<size_t>(tail_n);
  }
  appendGithubLink(page, kSettingsPageCap, &used);

  s_server->send(200, "text/html; charset=utf-8", page);
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
      s_server->arg("range_idx").c_str(), s_server->arg("airlabs_key").c_str(),
      s_server->arg("flightaware_key").c_str(), s_server->arg("fr24_key").c_str(),
      s_server->arg("use_airlabs").c_str(), s_server->arg("use_flightaware").c_str(),
      s_server->arg("use_fr24").c_str(), s_server->arg("airlabs_max_calls").c_str(),
      s_server->arg("flightaware_max_usd").c_str(),
      s_server->arg("flightaware_cost_usd").c_str(), s_server->arg("fr24_max_usd").c_str(),
      s_server->arg("fr24_cost_usd").c_str(), s_server->arg("ui_beep").c_str(),
      s_server->arg("beep_tone").c_str(), s_server->arg("bright_pct").c_str(),
      s_server->arg("show_sweep").c_str(), s_server->arg("detail_timeout").c_str());

  Serial.printf("Settings web save (lat/lon %s)\n", loc_ok ? "ok" : "invalid");

  if (!loc_ok) {
    sendLocationErrorPage();
    return;
  }

  sendRebootPage();
  s_server->client().flush();
  delay(400);
  esp_restart();
}

void handleRouteCacheDownload() {
  services::route_cache::sendDownload(s_server);
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
