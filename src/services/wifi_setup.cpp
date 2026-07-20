#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>
#include <cstring>

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "hardware/input.h"
#include "services/adsb_client.h"
#include "services/reboot.h"
#include "services/settings_web.h"
#include "ui/boot_screens.h"
#include "ui/flight_detail_screen.h"
#include "ui/radar_display.h"

namespace {

constexpr char kWifiPrefsNamespace[] = "fs_wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

constexpr char kNetsPrefsNamespace[] = "fs_wifi_nets";
constexpr char kNetsCountKey[] = "n";

/** Injected into every WiFiManager page via setCustomHeadElement (keep small — heap). */
char s_portal_custom_head[384];
char s_portal_custom_menu[160];
char s_portal_flash[128];

WiFiManager* s_active_wm = nullptr;

struct WifiNetSlot {
  char ssid[33];
  char pass[65];
  uint8_t fail_streak;
  bool demoted;
};

WifiNetSlot s_nets[config::kWifiMaxNetworks];
uint8_t s_nets_count = 0;
bool s_nets_loaded = false;
uint8_t s_reconnect_fail_rounds = 0;

void setPortalFlash(const char* msg) {
  if (msg == nullptr || msg[0] == '\0') {
    s_portal_flash[0] = '\0';
    return;
  }
  strncpy(s_portal_flash, msg, sizeof(s_portal_flash) - 1);
  s_portal_flash[sizeof(s_portal_flash) - 1] = '\0';
}

void slotKey(char* out, size_t len, char prefix, uint8_t i) {
  snprintf(out, len, "%c%u", prefix, static_cast<unsigned>(i));
}

void clearSlot(WifiNetSlot* slot) {
  slot->ssid[0] = '\0';
  slot->pass[0] = '\0';
  slot->fail_streak = 0;
  slot->demoted = false;
}

void persistNets() {
  Preferences prefs;
  if (!prefs.begin(kNetsPrefsNamespace, false)) {
    return;
  }
  prefs.clear();
  prefs.putUChar(kNetsCountKey, s_nets_count);
  for (uint8_t i = 0; i < s_nets_count; ++i) {
    char sk[4];
    char pk[4];
    char fk[4];
    slotKey(sk, sizeof(sk), 's', i);
    slotKey(pk, sizeof(pk), 'p', i);
    slotKey(fk, sizeof(fk), 'f', i);
    prefs.putString(sk, s_nets[i].ssid);
    prefs.putString(pk, s_nets[i].pass);
    prefs.putUChar(fk, s_nets[i].fail_streak);
  }
  prefs.end();
}

void loadNetsFromPrefs() {
  for (uint8_t i = 0; i < config::kWifiMaxNetworks; ++i) {
    clearSlot(&s_nets[i]);
  }
  s_nets_count = 0;

  Preferences prefs;
  if (!prefs.begin(kNetsPrefsNamespace, false)) {
    s_nets_loaded = true;
    return;
  }
  const uint8_t n = prefs.getUChar(kNetsCountKey, 0);
  for (uint8_t i = 0; i < n && i < config::kWifiMaxNetworks; ++i) {
    char sk[4];
    char pk[4];
    char fk[4];
    slotKey(sk, sizeof(sk), 's', i);
    slotKey(pk, sizeof(pk), 'p', i);
    slotKey(fk, sizeof(fk), 'f', i);
    String ssid = prefs.getString(sk, "");
    if (ssid.length() == 0) {
      continue;
    }
    String pass = prefs.getString(pk, "");
    strncpy(s_nets[s_nets_count].ssid, ssid.c_str(), sizeof(s_nets[0].ssid) - 1);
    strncpy(s_nets[s_nets_count].pass, pass.c_str(), sizeof(s_nets[0].pass) - 1);
    s_nets[s_nets_count].fail_streak = prefs.getUChar(fk, 0);
    s_nets[s_nets_count].demoted = false;
    ++s_nets_count;
  }
  prefs.end();
  s_nets_loaded = true;
}

void ensureNetsLoaded() {
  if (!s_nets_loaded) {
    loadNetsFromPrefs();
  }
}

void clearAllNetsStore() {
  for (uint8_t i = 0; i < config::kWifiMaxNetworks; ++i) {
    clearSlot(&s_nets[i]);
  }
  s_nets_count = 0;
  s_nets_loaded = true;
  Preferences prefs;
  if (prefs.begin(kNetsPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
}

bool readStaCredentials(String* ssid, String* pass) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  if (conf.sta.ssid[0] == '\0') {
    return false;
  }

  if (ssid != nullptr) {
    *ssid = reinterpret_cast<const char*>(conf.sta.ssid);
  }
  if (pass != nullptr) {
    *pass = reinterpret_cast<const char*>(conf.sta.password);
  }
  return true;
}

void migrateStaIntoNetsIfNeeded() {
  ensureNetsLoaded();
  if (s_nets_count > 0) {
    return;
  }
  String ssid;
  String pass;
  if (!readStaCredentials(&ssid, &pass)) {
    return;
  }
  strncpy(s_nets[0].ssid, ssid.c_str(), sizeof(s_nets[0].ssid) - 1);
  strncpy(s_nets[0].pass, pass.c_str(), sizeof(s_nets[0].pass) - 1);
  s_nets[0].fail_streak = 0;
  s_nets[0].demoted = false;
  s_nets_count = 1;
  persistNets();
  Serial.printf("[wifi] migrated STA SSID into slot 0: %s\n", s_nets[0].ssid);
}

void mirrorActiveCredsToSta(const char* ssid, const char* pass) {
  WiFi.persistent(true);
  wifi_config_t conf = {};
  strncpy(reinterpret_cast<char*>(conf.sta.ssid), ssid, sizeof(conf.sta.ssid));
  strncpy(reinterpret_cast<char*>(conf.sta.password), pass != nullptr ? pass : "",
          sizeof(conf.sta.password));
  esp_wifi_set_config(WIFI_IF_STA, &conf);
  WiFi.persistent(false);
}

int findNetIndexBySsid(const char* ssid) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return -1;
  }
  for (uint8_t i = 0; i < s_nets_count; ++i) {
    if (strcmp(s_nets[i].ssid, ssid) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void noteConnectSuccess(uint8_t index) {
  if (index >= s_nets_count) {
    return;
  }
  s_nets[index].fail_streak = 0;
  s_nets[index].demoted = false;
  persistNets();
  mirrorActiveCredsToSta(s_nets[index].ssid, s_nets[index].pass);
}

void noteConnectFailure(uint8_t index) {
  if (index >= s_nets_count) {
    return;
  }
  if (s_nets[index].fail_streak < 255) {
    ++s_nets[index].fail_streak;
  }
  if (s_nets[index].fail_streak >= config::kWifiDemoteAfterFails) {
    s_nets[index].demoted = true;
    Serial.printf("[wifi] demoting SSID for this session: %s (fails=%u)\n",
                  s_nets[index].ssid, static_cast<unsigned>(s_nets[index].fail_streak));
  }
  persistNets();
}

void buildTryOrder(uint8_t* out_indices, uint8_t* out_count) {
  ensureNetsLoaded();
  *out_count = 0;
  for (uint8_t i = 0; i < s_nets_count; ++i) {
    if (!s_nets[i].demoted) {
      out_indices[(*out_count)++] = i;
    }
  }
  for (uint8_t i = 0; i < s_nets_count; ++i) {
    if (s_nets[i].demoted) {
      out_indices[(*out_count)++] = i;
    }
  }
}

bool ssidSeenInScan(const char* ssid) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return false;
  }
  const int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (n <= 0) {
    WiFi.scanDelete();
    return true;  // scan failed/empty — do not soft-skip; try begin()
  }
  bool found = false;
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) == ssid) {
      found = true;
      break;
    }
  }
  WiFi.scanDelete();
  return found;
}

void buildPortalCustomHead() {
  // Keep this tiny: WebServer::send() duplicates the page String and SoftAP is
  // already heap-starved after failed reconnects (~25 KB max block).
  snprintf(s_portal_custom_head, sizeof(s_portal_custom_head),
           "<style>"
           "body{background:#0a0a0a!important;color:#eee!important}"
           "input,select{background:#333!important;color:#fff!important;"
           "border:1px solid #555!important}"
           "button{background:#1a9c3c!important;color:#fff!important}"
           "a{color:#9cf!important}"
           ".msg{background:#222!important;color:#eee!important}"
           ".fs-flash{background:#143d22;border:1px solid #1a9c3c;color:#d8ffe4;"
           "border-radius:8px;padding:.55rem .7rem;margin:0 0 .8rem}"
           ".fs-net{border:1px solid #444;border-radius:8px;padding:.55rem .7rem;"
           "margin:.45rem 0}.fs-net .row{display:flex;flex-wrap:wrap;gap:.35rem;"
           "margin-top:.4rem}"
           "</style>");

  snprintf(s_portal_custom_menu, sizeof(s_portal_custom_menu),
           "<form action='/networks' method='get'><button type='submit'>"
           "Saved networks</button></form><br/>");
}

bool s_force_config_portal = false;

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

void onStaConnected() { settingsWebStart(); }

void onStaLinkReady() {
  WiFi.setAutoReconnect(true);
  onStaConnected();
}

void prepareWifiForPortal() {
  settingsWebStop();
#ifdef WM_MDNS
  MDNS.end();
#endif
  services::adsb::cancelPendingFetch();
  // Free large UI allocs so portal HTML / SoftAP stack can allocate.
  ui::flightDetailReleaseSprite();
  ui::radarDisplayReleasePressureSprites();

  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(500);
}

void eraseWifiCredentials() {
  prepareWifiForPortal();
  clearAllNetsStore();

  WiFi.persistent(true);
  WiFiManager wm;
  wm.resetSettings();
  wm.erase();
  WiFi.disconnect(true, false);
  WiFi.persistent(false);

#ifdef ESP32
  esp_wifi_restore();
#endif

  WiFi.mode(WIFI_OFF);
  delay(200);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  Serial.println("WiFi credentials cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  // HTTP server is not up yet here — only refresh the on-device hint.
  // mDNS starts after startConfigPortal() returns (server listening).
  bootScreenShowPortalHint();
  Serial.printf("SoftAP starting (HTTP next): %s\n", config::kPortalApName);
}

void startPortalMdns() {
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n",
                  WiFi.softAPIP().toString().c_str());
  }
#else
  Serial.printf("Setup portal: http://%s\n", WiFi.softAPIP().toString().c_str());
#endif
}

// Lightweight PROGMEM home page — registered on `/` before WiFiManager so it
// wins the handler list and avoids WM's large heap-built HTML (which goes blank
// when maxAllocHeap is ~25 KB after reconnect failures).
static const char kPortalRootHtml[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>FlightScnr</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#0a0a0a;color:#eee;"
    "margin:1.25rem;max-width:28rem;line-height:1.4}"
    "h1{font-size:1.35rem;margin:0 0 .5rem}"
    "p{color:#aaa;font-size:.92rem}"
    "a{display:block;width:100%;box-sizing:border-box;margin:.55rem 0;padding:.85rem;"
    "border-radius:10px;background:#1a9c3c;color:#fff;text-align:center;"
    "text-decoration:none;font-size:1rem;font-weight:600}"
    "a.sec{background:#2a2a2a}"
    "</style></head><body>"
    "<h1>FlightScnr setup</h1>"
    "<p>Connect this device to Wi&#8209;Fi. Up to 3 networks can be saved.</p>"
    "<a href=\"/wifi\">Configure Wi&#8209;Fi</a>"
    "<a href=\"/networks\">Saved networks</a>"
    "<a class=\"sec\" href=\"/info\">Info</a>"
    "</body></html>";

void handlePortalRoot() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  Serial.printf("[wifi] portal GET %s stations=%u heap=%u\n",
                s_active_wm->server->uri().c_str(),
                static_cast<unsigned>(WiFi.softAPgetStationNum()), ESP.getFreeHeap());
  s_active_wm->server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  s_active_wm->server->sendHeader("Pragma", "no-cache");
  s_active_wm->server->send_P(200, PSTR("text/html"), kPortalRootHtml);
}

void handlePortalCaptiveProbe() {
  // Serve the landing page directly — 302 leaves many OS captive sheets blank.
  handlePortalRoot();
}

void htmlEscape(const char* in, char* out, size_t out_len) {
  size_t o = 0;
  for (size_t i = 0; in != nullptr && in[i] != '\0' && o + 1 < out_len; ++i) {
    const char c = in[i];
    const char* rep = nullptr;
    if (c == '&') {
      rep = "&amp;";
    } else if (c == '<') {
      rep = "&lt;";
    } else if (c == '>') {
      rep = "&gt;";
    } else if (c == '"') {
      rep = "&quot;";
    }
    if (rep != nullptr) {
      const size_t rl = strlen(rep);
      if (o + rl >= out_len) {
        break;
      }
      memcpy(out + o, rep, rl);
      o += rl;
    } else {
      out[o++] = c;
    }
  }
  out[o] = '\0';
}

/** SoftAP portal HTML buffer — prefer PSRAM; WebServer::send(String) OOM blanks pages. */
constexpr size_t kPortalPageCap = 6144;
char* s_portal_page = nullptr;

char* portalPageBuffer() {
  if (s_portal_page == nullptr) {
    s_portal_page = static_cast<char*>(
        heap_caps_malloc(kPortalPageCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (s_portal_page == nullptr) {
    s_portal_page = static_cast<char*>(malloc(kPortalPageCap));
  }
  return s_portal_page;
}

void portalAppend(char* page, size_t* used, const char* html) {
  if (page == nullptr || used == nullptr || html == nullptr || *used >= kPortalPageCap) {
    return;
  }
  const int n = snprintf(page + *used, kPortalPageCap - *used, "%s", html);
  if (n > 0) {
    const size_t space = kPortalPageCap - *used;
    *used += static_cast<size_t>(n) < space ? static_cast<size_t>(n) : space - 1;
  }
}

void portalSendBuffer(size_t used) {
  if (s_active_wm == nullptr || !s_active_wm->server || s_portal_page == nullptr) {
    return;
  }
  if (used >= kPortalPageCap) {
    used = kPortalPageCap - 1;
  }
  s_portal_page[used] = '\0';
  s_active_wm->server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  s_active_wm->server->sendHeader("Pragma", "no-cache");
  s_active_wm->server->setContentLength(used);
  s_active_wm->server->send(200, "text/html; charset=utf-8", "");
  s_active_wm->server->sendContent(s_portal_page, used);
}

void portalBeginPage(char* page, size_t* used, const char* title) {
  *used = 0;
  portalAppend(page, used,
               "<!DOCTYPE html><html><head><meta charset=utf-8>"
               "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
               "<title>");
  portalAppend(page, used, title);
  portalAppend(page, used,
               "</title><style>"
               "body{font-family:system-ui,sans-serif;background:#0a0a0a;color:#eee;"
               "margin:1.1rem;max-width:28rem;line-height:1.35}"
               "h1{font-size:1.2rem;margin:0 0 .6rem}"
               "p,.hint{color:#aaa;font-size:.88rem}"
               "a,button{font-size:.95rem}"
               "a.btn,button{display:inline-block;margin:.25rem .25rem .25rem 0;"
               "padding:.55rem .75rem;border-radius:8px;border:0;background:#1a9c3c;"
               "color:#fff;text-decoration:none;cursor:pointer}"
               "a.sec,button.sec{background:#333}"
               "a.danger,button.danger{background:#5a2020}"
               "input{width:100%;box-sizing:border-box;padding:.55rem;margin:.25rem 0 .55rem;"
               "border-radius:8px;border:1px solid #555;background:#222;color:#fff}"
               ".net{border:1px solid #444;border-radius:8px;padding:.55rem .7rem;margin:.45rem 0}"
               ".row{display:flex;flex-wrap:wrap;gap:.3rem;margin-top:.4rem}"
               ".flash{background:#143d22;border:1px solid #1a9c3c;color:#d8ffe4;"
               "border-radius:8px;padding:.5rem .65rem;margin:0 0 .7rem}"
               "label{display:block;font-size:.85rem;color:#ccc;margin-top:.35rem}"
               "</style></head><body><h1>");
  portalAppend(page, used, title);
  portalAppend(page, used, "</h1>");
  if (s_portal_flash[0] != '\0') {
    portalAppend(page, used, "<p class=flash>");
    portalAppend(page, used, s_portal_flash);
    portalAppend(page, used, "</p>");
    s_portal_flash[0] = '\0';
  }
}

void portalEndPage(char* page, size_t* used) {
  portalAppend(page, used, "<p style=\"margin-top:1.1rem\"><a class=\"btn sec\" href=\"/\">"
                           "Back to portal</a></p></body></html>");
}

void handlePortalNetworks() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  char* const page = portalPageBuffer();
  if (page == nullptr) {
    s_active_wm->server->send(503, "text/plain", "Out of memory");
    return;
  }
  ensureNetsLoaded();
  Serial.printf("[wifi] portal GET /networks heap=%u\n", ESP.getFreeHeap());

  size_t used = 0;
  portalBeginPage(page, &used, "Saved networks");
  portalAppend(page, &used,
               "<p class=hint>#1 is tried first. Up to 3 networks. Failing ones are "
               "skipped this session, not deleted.</p>");

  if (s_nets_count == 0) {
    portalAppend(page, &used, "<p>No saved networks yet.</p>");
  }
  for (uint8_t i = 0; i < s_nets_count; ++i) {
    char esc[96];
    char chunk[384];
    htmlEscape(s_nets[i].ssid, esc, sizeof(esc));
    snprintf(chunk, sizeof(chunk),
             "<div class=net><strong>#%u</strong> %s%s"
             "<div class=row>",
             static_cast<unsigned>(i + 1), esc,
             s_nets[i].demoted ? " <em>(skipped)</em>" : "");
    portalAppend(page, &used, chunk);
    if (i > 0) {
      snprintf(chunk, sizeof(chunk),
               "<form method=POST action=/networks/up style=display:inline>"
               "<input type=hidden name=i value=%u>"
               "<button type=submit class=sec>Up</button></form>",
               static_cast<unsigned>(i));
      portalAppend(page, &used, chunk);
    }
    if (i + 1 < s_nets_count) {
      snprintf(chunk, sizeof(chunk),
               "<form method=POST action=/networks/down style=display:inline>"
               "<input type=hidden name=i value=%u>"
               "<button type=submit class=sec>Down</button></form>",
               static_cast<unsigned>(i));
      portalAppend(page, &used, chunk);
    }
    snprintf(chunk, sizeof(chunk),
             "<form method=POST action=/networks/del style=display:inline "
             "onsubmit=\"return confirm('Remove?');\">"
             "<input type=hidden name=i value=%u>"
             "<button type=submit class=danger>Delete</button></form></div></div>",
             static_cast<unsigned>(i));
    portalAppend(page, &used, chunk);
  }

  if (s_nets_count < config::kWifiMaxNetworks) {
    portalAppend(page, &used,
                 "<p><a class=btn href=/wifi>Add via scan</a></p>"
                 "<form method=POST action=/networks/add>"
                 "<label>SSID</label><input name=s maxlength=32 required>"
                 "<label>Password</label><input name=p type=password maxlength=63>"
                 "<button type=submit>Add network</button></form>");
  } else {
    portalAppend(page, &used, "<p class=hint>Store full (3/3). Delete one first.</p>");
  }
  portalEndPage(page, &used);
  portalSendBuffer(used);
}

void handlePortalWifi() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  char* const page = portalPageBuffer();
  if (page == nullptr) {
    s_active_wm->server->send(503, "text/plain", "Out of memory");
    return;
  }
  Serial.printf("[wifi] portal GET /wifi heap=%u\n", ESP.getFreeHeap());

  // SoftAP-only scan is unreliable; briefly enable STA for the scan.
  WiFi.mode(WIFI_AP_STA);
  delay(50);
  const int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);

  char picked[33] = "";
  if (s_active_wm->server->hasArg("i")) {
    const int idx = s_active_wm->server->arg("i").toInt();
    if (idx >= 0 && idx < n) {
      strncpy(picked, WiFi.SSID(idx).c_str(), sizeof(picked) - 1);
    }
  }

  size_t used = 0;
  portalBeginPage(page, &used, "Configure Wi-Fi");
  portalAppend(page, &used,
               "<p class=hint>Pick a network or type an SSID. Credentials are saved "
               "(up to 3) and the device will try to connect.</p>");

  if (n <= 0) {
    portalAppend(page, &used, "<p>No networks found. Enter SSID manually.</p>");
  } else {
    portalAppend(page, &used, "<p class=hint>Tap an SSID to fill the form:</p>");
    const int show = n > 12 ? 12 : n;
    for (int i = 0; i < show; ++i) {
      char esc[96];
      char chunk[220];
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) {
        continue;
      }
      htmlEscape(ssid.c_str(), esc, sizeof(esc));
      snprintf(chunk, sizeof(chunk),
               "<form method=GET action=/wifi style=\"display:inline;margin:0\">"
               "<input type=hidden name=i value=%d>"
               "<button type=submit class=sec>%s (%d)</button></form> ",
               i, esc, WiFi.RSSI(i));
      portalAppend(page, &used, chunk);
    }
  }
  WiFi.scanDelete();

  char esc_pick[96];
  htmlEscape(picked, esc_pick, sizeof(esc_pick));
  char form[512];
  snprintf(form, sizeof(form),
           "<form method=POST action=/wifi/save style=\"margin-top:1rem\">"
           "<label>SSID</label><input name=s maxlength=32 required value=\"%s\">"
           "<label>Password</label><input name=p type=password maxlength=63>"
           "<button type=submit>Save &amp; connect</button></form>",
           esc_pick);
  portalAppend(page, &used, form);
  portalEndPage(page, &used);
  portalSendBuffer(used);
}

void portalRedirectNetworks() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  s_active_wm->server->sendHeader("Location", "/networks", true);
  s_active_wm->server->send(303, "text/plain", "");
}

void handlePortalWifiSave() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  if (s_active_wm->server->method() != HTTP_POST) {
    s_active_wm->server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  const String ssid = s_active_wm->server->arg("s");
  const String pass = s_active_wm->server->arg("p");
  char err[96];
  if (!wifiNetsAddOrUpdate(ssid.c_str(), pass.c_str(), err, sizeof(err))) {
    setPortalFlash(err[0] ? err : "Could not save network");
    s_active_wm->server->sendHeader("Location", "/wifi", true);
    s_active_wm->server->send(303, "text/plain", "");
    return;
  }
  Serial.printf("[wifi] portal save+connect SSID=%s\n", ssid.c_str());
  mirrorActiveCredsToSta(ssid.c_str(), pass.c_str());
  // Exit SoftAP loop so waitForStaAfterPortal / preferred connect can run.
  s_active_wm->server->send(200, "text/html; charset=utf-8",
                            "<!DOCTYPE html><html><body style=\"background:#0a0a0a;color:#eee;"
                            "font-family:system-ui;padding:1.5rem\">"
                            "<p>Saved. Connecting&hellip;</p></body></html>");
  s_active_wm->server->client().flush();
  delay(200);
  s_active_wm->stopConfigPortal();
}

void handlePortalNetworksAdd() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  if (s_active_wm->server->method() != HTTP_POST) {
    s_active_wm->server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  char err[96];
  if (!wifiNetsAddOrUpdate(s_active_wm->server->arg("s").c_str(),
                           s_active_wm->server->arg("p").c_str(), err, sizeof(err))) {
    setPortalFlash(err[0] ? err : "Could not add network");
  } else {
    setPortalFlash("Network saved.");
  }
  portalRedirectNetworks();
}

void handlePortalNetworksDel() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  if (s_active_wm->server->method() != HTTP_POST) {
    s_active_wm->server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  const int i = s_active_wm->server->arg("i").toInt();
  if (wifiNetsRemove(static_cast<uint8_t>(i))) {
    setPortalFlash("Network removed.");
  } else {
    setPortalFlash("Could not remove network.");
  }
  portalRedirectNetworks();
}

void handlePortalNetworksUp() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  if (s_active_wm->server->method() != HTTP_POST) {
    s_active_wm->server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  wifiNetsMoveUp(static_cast<uint8_t>(s_active_wm->server->arg("i").toInt()));
  portalRedirectNetworks();
}

void handlePortalNetworksDown() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  if (s_active_wm->server->method() != HTTP_POST) {
    s_active_wm->server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  wifiNetsMoveDown(static_cast<uint8_t>(s_active_wm->server->arg("i").toInt()));
  portalRedirectNetworks();
}

void onPortalWebServerReady() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  auto& srv = *s_active_wm->server;

  // Register before WiFiManager routes so our lightweight handlers win.
  srv.on("/", HTTP_GET, handlePortalRoot);
  srv.on("/wifi", HTTP_GET, handlePortalWifi);
  srv.on("/wifi/save", HTTP_POST, handlePortalWifiSave);

  srv.on("/generate_204", HTTP_GET, handlePortalCaptiveProbe);
  srv.on("/gen_204", HTTP_GET, handlePortalCaptiveProbe);
  srv.on("/hotspot-detect.html", HTTP_GET, handlePortalCaptiveProbe);
  srv.on("/library/test/success.html", HTTP_GET, handlePortalCaptiveProbe);
  srv.on("/connecttest.txt", HTTP_GET, handlePortalCaptiveProbe);
  srv.on("/ncsi.txt", HTTP_GET, handlePortalCaptiveProbe);
  srv.on("/fwlink", HTTP_GET, handlePortalCaptiveProbe);
  srv.on("/canonical.html", HTTP_GET, handlePortalCaptiveProbe);
  srv.on("/success.txt", HTTP_GET, handlePortalCaptiveProbe);

  srv.on("/networks", HTTP_GET, handlePortalNetworks);
  srv.on("/networks/add", HTTP_POST, handlePortalNetworksAdd);
  srv.on("/networks/del", HTTP_POST, handlePortalNetworksDel);
  srv.on("/networks/up", HTTP_POST, handlePortalNetworksUp);
  srv.on("/networks/down", HTTP_POST, handlePortalNetworksDown);
}

void onPortalWifiPreSave() {
  if (s_active_wm == nullptr || !s_active_wm->server) {
    return;
  }
  String ssid = s_active_wm->server->arg("s");
  String pass = s_active_wm->server->arg("p");
  if (ssid.length() == 0 && pass.length() > 0) {
    ssid = s_active_wm->getWiFiSSID(true);
  }
  if (ssid.length() == 0) {
    return;
  }
  char err[96];
  if (!wifiNetsAddOrUpdate(ssid.c_str(), pass.c_str(), err, sizeof(err))) {
    setPortalFlash(err[0] ? err : "Network store full (3/3)");
    Serial.printf("[wifi] portal save not stored: %s\n", err);
  } else {
    setPortalFlash("Network saved to preference list.");
    Serial.printf("[wifi] portal saved SSID into multi-slot store: %s\n", ssid.c_str());
  }
}

void onPortalWifiSaved() {
  ensureNetsLoaded();
  const String ssid = WiFi.SSID();
  const String pass = WiFi.psk();
  if (ssid.length() == 0) {
    return;
  }
  char err[96];
  if (findNetIndexBySsid(ssid.c_str()) < 0) {
    if (!wifiNetsAddOrUpdate(ssid.c_str(), pass.c_str(), err, sizeof(err))) {
      // Slots full: replace lowest-preference so the connected network is retained.
      if (s_nets_count == config::kWifiMaxNetworks) {
        wifiNetsRemove(config::kWifiMaxNetworks - 1);
        wifiNetsAddOrUpdate(ssid.c_str(), pass.c_str(), err, sizeof(err));
        setPortalFlash("Replaced lowest-preference network with the one just connected.");
      }
    }
  }
  const int idx = findNetIndexBySsid(ssid.c_str());
  if (idx >= 0) {
    noteConnectSuccess(static_cast<uint8_t>(idx));
  }
}

void configureWifiManager(WiFiManager& wm) {
  s_active_wm = &wm;
  wm.setTitle("FlightScnr");
  // Avoid WM "invert" dark class + large script-laden custom head; SoftAP heap is tight.
  wm.setDarkMode(false);
  buildPortalCustomHead();
  wm.setCustomHeadElement(s_portal_custom_head);
  wm.setCustomMenuHTML(s_portal_custom_menu);
  const char* menu[] = {"wifi", "info", "custom", "exit"};
  wm.setMenu(menu, 4);
  wm.setShowInfoUpdate(false);
  wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  wm.setConfigPortalBlocking(false);
  wm.setCaptivePortalEnable(true);
  wm.setWiFiAPChannel(1);
  wm.setAPStaticIPConfig(IPAddress(4, 3, 2, 1), IPAddress(4, 3, 2, 1),
                         IPAddress(255, 255, 255, 0));
  wm.setHostname(config::kPortalHostname);
  wm.setAPCallback(onConfigPortalApStarted);
  wm.setWebServerCallback(onPortalWebServerReady);
  wm.setPreSaveConfigCallback(onPortalWifiPreSave);
  wm.setSaveConfigCallback(onPortalWifiSaved);
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void prepareSta() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const char* ssid, const char* pass) {
  prepareSta();
  if (ssid != nullptr && ssid[0] != '\0') {
    WiFi.begin(ssid, pass != nullptr ? pass : "");
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    inputPollLongPress();
    if (inputConsumeWifiResetUiCancelled()) {
      bootScreenConnectingStart(ssid_for_ui != nullptr ? ssid_for_ui : "network");
    } else if (!bootScreenWifiResetCountdownActive()) {
      bootScreenConnectingPulse();
    }
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectOneSlot(uint8_t index, bool show_ui) {
  if (index >= s_nets_count) {
    return false;
  }
  const char* ssid = s_nets[index].ssid;
  const char* pass = s_nets[index].pass;

  if (show_ui) {
    bootScreenConnectingStart(ssid);
  }

  // Soft skip when a scan finds no matching BSSID (e.g. office SSID while at home).
  prepareSta();
  if (!ssidSeenInScan(ssid)) {
    Serial.printf("[wifi] SSID not in scan, skipping for now: %s\n", ssid);
    if (show_ui) {
      bootScreenConnectingStart(ssid);
      delay(400);
    }
    return false;
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u (%s)\n", attempt,
                    config::kWifiConnectAttempts, ssid);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
      if (show_ui) {
        bootScreenConnectingStart(ssid);
      }
    }

    startStaConnect(ssid, pass);

    bool ok = false;
    if (show_ui) {
      ok = waitForLinkWithUi(ssid, config::kWifiConnectAttemptMs);
    } else {
      const unsigned long deadline = millis() + config::kWifiConnectAttemptMs;
      while (millis() < deadline) {
        if (wifiLinkUp()) {
          ok = true;
          break;
        }
        delay(50);
      }
      if (!ok) {
        ok = wifiLinkUp();
      }
    }

    if (ok) {
      noteConnectSuccess(index);
      onStaLinkReady();
      return true;
    }
  }

  noteConnectFailure(index);
  return false;
}

bool wifiTryConnectPreferred(bool show_ui) {
  migrateStaIntoNetsIfNeeded();
  ensureNetsLoaded();

  if (wifiLinkUp()) {
    onStaLinkReady();
    return true;
  }

  if (s_nets_count == 0) {
    return false;
  }

  uint8_t order[config::kWifiMaxNetworks];
  uint8_t order_n = 0;
  buildTryOrder(order, &order_n);

  for (uint8_t t = 0; t < order_n; ++t) {
    const uint8_t idx = order[t];
    Serial.printf("[wifi] trying #%u %s%s\n", static_cast<unsigned>(idx + 1),
                  s_nets[idx].ssid, s_nets[idx].demoted ? " (demoted)" : "");
    if (tryConnectOneSlot(idx, show_ui)) {
      return true;
    }
  }
  return false;
}

bool waitForStaAfterPortal() {
  if (wifiLinkUp()) {
    onPortalWifiSaved();
    return true;
  }
  return wifiTryConnectPreferred(true);
}

bool openConfigPortal(WiFiManager& wm) {
  prepareWifiForPortal();
  bootScreenShowPortalHint();

  wm.startConfigPortal(config::kPortalApName);
  if (!wm.getConfigPortalActive()) {
    Serial.println("Config portal failed to start");
    return false;
  }

  startPortalMdns();
  Serial.printf("[wifi] portal AP IP %s  free_heap=%u max_blk=%u\n",
                WiFi.softAPIP().toString().c_str(), ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());

  unsigned long last_sta_log_ms = 0;
  while (wm.getConfigPortalActive()) {
    inputPollLongPress();
    if (inputConsumeWifiResetUiCancelled()) {
      bootScreenShowPortalHint();
    }
    wm.process();
    const unsigned long now = millis();
    if (now - last_sta_log_ms >= 5000UL) {
      last_sta_log_ms = now;
      Serial.printf("[wifi] portal stations=%u heap=%u\n",
                    static_cast<unsigned>(WiFi.softAPgetStationNum()), ESP.getFreeHeap());
    }
    delay(5);
  }

  s_active_wm = nullptr;
  if (s_portal_page != nullptr) {
    heap_caps_free(s_portal_page);
    s_portal_page = nullptr;
  }
  return waitForStaAfterPortal() && wifiLinkUp();
}

bool runConfigPortalFlow() {
  WiFiManager wm;
  configureWifiManager(wm);
  if (!openConfigPortal(wm) || !wifiLinkUp()) {
    s_active_wm = nullptr;
    return false;
  }

  Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());
  onStaLinkReady();
  Serial.println("WiFi configured — rebooting");
  delay(400);
  services::hardReboot();  // full chip reset (soft reset hangs USB-CDC boot with no host)
  return false;
}

}  // namespace

uint8_t wifiNetsCount() {
  ensureNetsLoaded();
  return s_nets_count;
}

bool wifiNetsGetSsid(uint8_t index, char* out, size_t out_len) {
  ensureNetsLoaded();
  if (out == nullptr || out_len == 0 || index >= s_nets_count) {
    return false;
  }
  strncpy(out, s_nets[index].ssid, out_len - 1);
  out[out_len - 1] = '\0';
  return out[0] != '\0';
}

bool wifiNetsIsDemoted(uint8_t index) {
  ensureNetsLoaded();
  return index < s_nets_count && s_nets[index].demoted;
}

bool wifiNetsAddOrUpdate(const char* ssid, const char* pass, char* err, size_t err_len) {
  ensureNetsLoaded();
  auto set_err = [&](const char* m) {
    if (err != nullptr && err_len > 0) {
      strncpy(err, m, err_len - 1);
      err[err_len - 1] = '\0';
    }
  };

  if (ssid == nullptr || ssid[0] == '\0') {
    set_err("SSID required");
    return false;
  }
  if (strlen(ssid) > 32) {
    set_err("SSID too long");
    return false;
  }

  const int existing = findNetIndexBySsid(ssid);
  if (existing >= 0) {
    if (pass != nullptr) {
      strncpy(s_nets[existing].pass, pass, sizeof(s_nets[0].pass) - 1);
      s_nets[existing].pass[sizeof(s_nets[0].pass) - 1] = '\0';
    }
    s_nets[existing].fail_streak = 0;
    s_nets[existing].demoted = false;
    persistNets();
    if (err != nullptr && err_len > 0) {
      err[0] = '\0';
    }
    return true;
  }

  if (s_nets_count >= config::kWifiMaxNetworks) {
    set_err("Already 3 networks saved — delete one first");
    return false;
  }

  strncpy(s_nets[s_nets_count].ssid, ssid, sizeof(s_nets[0].ssid) - 1);
  s_nets[s_nets_count].ssid[sizeof(s_nets[0].ssid) - 1] = '\0';
  if (pass != nullptr) {
    strncpy(s_nets[s_nets_count].pass, pass, sizeof(s_nets[0].pass) - 1);
    s_nets[s_nets_count].pass[sizeof(s_nets[0].pass) - 1] = '\0';
  } else {
    s_nets[s_nets_count].pass[0] = '\0';
  }
  s_nets[s_nets_count].fail_streak = 0;
  s_nets[s_nets_count].demoted = false;
  ++s_nets_count;
  persistNets();
  if (err != nullptr && err_len > 0) {
    err[0] = '\0';
  }
  return true;
}

bool wifiNetsUpdatePassword(uint8_t index, const char* pass, char* err, size_t err_len) {
  ensureNetsLoaded();
  if (index >= s_nets_count) {
    if (err != nullptr && err_len > 0) {
      strncpy(err, "Invalid network", err_len - 1);
      err[err_len - 1] = '\0';
    }
    return false;
  }
  strncpy(s_nets[index].pass, pass != nullptr ? pass : "", sizeof(s_nets[0].pass) - 1);
  s_nets[index].pass[sizeof(s_nets[0].pass) - 1] = '\0';
  s_nets[index].fail_streak = 0;
  s_nets[index].demoted = false;
  persistNets();
  if (err != nullptr && err_len > 0) {
    err[0] = '\0';
  }
  return true;
}

bool wifiNetsRemove(uint8_t index) {
  ensureNetsLoaded();
  if (index >= s_nets_count) {
    return false;
  }
  for (uint8_t i = index; i + 1 < s_nets_count; ++i) {
    s_nets[i] = s_nets[i + 1];
  }
  clearSlot(&s_nets[s_nets_count - 1]);
  --s_nets_count;
  persistNets();
  return true;
}

bool wifiNetsSwap(uint8_t index_a, uint8_t index_b) {
  ensureNetsLoaded();
  if (index_a >= s_nets_count || index_b >= s_nets_count || index_a == index_b) {
    return false;
  }
  const WifiNetSlot tmp = s_nets[index_a];
  s_nets[index_a] = s_nets[index_b];
  s_nets[index_b] = tmp;
  persistNets();
  return true;
}

bool wifiNetsMoveUp(uint8_t index) {
  if (index == 0) {
    return false;
  }
  return wifiNetsSwap(index, static_cast<uint8_t>(index - 1));
}

bool wifiNetsMoveDown(uint8_t index) {
  ensureNetsLoaded();
  if (index + 1 >= s_nets_count) {
    return false;
  }
  return wifiNetsSwap(index, static_cast<uint8_t>(index + 1));
}

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  bootScreenShowWifiCleared();
  delay(800);
  services::hardReboot();  // full chip reset (soft reset hangs USB-CDC boot with no host)
}

bool wifiReconnect() {
  Serial.println("WiFi reconnecting...");
  migrateStaIntoNetsIfNeeded();
  ensureNetsLoaded();
  if (s_nets_count == 0 && !readStaCredentials(nullptr, nullptr)) {
    Serial.println("No saved WiFi — opening setup portal");
    s_reconnect_fail_rounds = 0;
    return runConfigPortalFlow();
  }

  if (wifiTryConnectPreferred(false)) {
    s_reconnect_fail_rounds = 0;
    return true;
  }

  if (s_reconnect_fail_rounds < 255) {
    ++s_reconnect_fail_rounds;
  }
  Serial.printf("[wifi] reconnect round failed (%u/%u)\n",
                static_cast<unsigned>(s_reconnect_fail_rounds),
                static_cast<unsigned>(config::kWifiPortalAfterReconnectFails));

  if (s_reconnect_fail_rounds >= config::kWifiPortalAfterReconnectFails) {
    Serial.println("Opening WiFi setup portal (reconnect failed repeatedly; "
                   "saved networks kept)");
    s_reconnect_fail_rounds = 0;
    bootScreenShowPortalHint();
    return runConfigPortalFlow();
  }
  return false;
}

bool wifiSoftRecycle() {
  settingsWebStop();
#ifdef WM_MDNS
  MDNS.end();
#endif
  WiFi.disconnect(false, false);
  delay(200);
  return wifiReconnect();
}

bool wifiSetupConnect() {
  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);
  migrateStaIntoNetsIfNeeded();

  if (force_portal) {
    eraseWifiCredentials();
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (runConfigPortalFlow()) {
      return true;
    }
    Serial.println("WiFi connection failed");
    bootScreenShowConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    onStaLinkReady();
    return true;
  }

  if (wifiTryConnectPreferred(true)) {
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    s_reconnect_fail_rounds = 0;
    return true;
  }

  ensureNetsLoaded();
  if (s_nets_count > 0) {
    Serial.println("Saved WiFi could not connect, opening setup portal");
  } else {
    Serial.println("No saved WiFi, opening setup portal");
  }
  bootScreenShowPortalHint();

  if (runConfigPortalFlow()) {
    return true;
  }

  Serial.println("WiFi connection failed");
  bootScreenShowConnectFailed();
  return false;
}
