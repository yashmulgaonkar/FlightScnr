#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "hardware/input.h"
#include "services/settings_web.h"
#include "ui/boot_screens.h"

namespace {

constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";
constexpr uint8_t kReconnectFailuresBeforePortal = 2;

/** Injected into every WiFiManager page via setCustomHeadElement. */
char s_portal_custom_head[896];

void buildPortalCustomHead() {
  snprintf(
      s_portal_custom_head, sizeof(s_portal_custom_head),
      "<style>body.invert{background:#000!important}"
      "body.invert input,body.invert select{background:#555!important;color:#fff!important;"
      "border:1px solid #444!important}"
      "body.invert button{background:#1a9c3c!important;color:#fff!important}"
      "body.invert a{color:#e8f0ff!important}"
      "body.invert a:hover{color:#6cf!important}"
      ".fs-ft{text-align:center;font-size:.85rem;margin:2rem 0 1rem;padding-top:"
      "1rem;border-top:1px solid #444}.fs-ft a{color:#6cf;text-decoration:none}"
      "</style>"
      "<script>document.addEventListener('DOMContentLoaded',function(){var "
      "w=document.querySelector('.wrap');if(!w)return;var p=document.createElement('p');"
      "p.className='fs-ft';p.innerHTML='<a href=\"%s\" target=\"_blank\" "
      "rel=\"noopener\">FlightScnr by Yash Mulgaonkar</a>';w.appendChild(p);});"
      "</script>",
      config::kPortalAuthorUrl);
}

bool s_force_config_portal = false;
uint8_t s_reconnect_failures = 0;

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

  // Read-write open: missing "wifi" namespace on first boot would log NOT_FOUND in read-only mode.
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

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

bool readSavedWifiCredentials(String* ssid, String* pass) {
  if (!storedWifiCredentials()) {
    return false;
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

void onStaConnected() { settingsWebStart(); }

void prepareWifiForPortal() {
  settingsWebStop();
#ifdef WM_MDNS
  MDNS.end();
#endif
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(200);
}

void eraseWifiCredentials() {
  prepareWifiForPortal();

  WiFi.persistent(true);
  WiFiManager wm;
  wm.resetSettings();
  wm.erase();
  WiFi.disconnect(true, true);
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
  bootScreenShowPortalHint();
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

void configureWifiManager(WiFiManager& wm) {
  wm.setTitle("FlightScnr");
  wm.setDarkMode(true);
  buildPortalCustomHead();
  wm.setCustomHeadElement(s_portal_custom_head);
  const char* menu[] = {"wifi", "info", "exit"};
  wm.setMenu(menu, 3);
  wm.setShowInfoUpdate(false);
  wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  wm.setConfigPortalBlocking(false);
  wm.setCaptivePortalEnable(true);
  wm.setWiFiAPChannel(1);
  wm.setAPStaticIPConfig(IPAddress(4, 3, 2, 1), IPAddress(4, 3, 2, 1),
                         IPAddress(255, 255, 255, 0));
  wm.setHostname(config::kPortalHostname);
  wm.setAPCallback(onConfigPortalApStarted);
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

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
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
    bootScreenConnectingPulse();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    onStaConnected();
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    bootScreenConnectingStart(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      onStaConnected();
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  String ssid;
  String pass;
  if (!readSavedWifiCredentials(&ssid, &pass)) {
    return false;
  }
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal(WiFiManager& wm) {
  prepareWifiForPortal();
  bootScreenShowPortalHint();

  // Non-blocking startConfigPortal() returns false even when the portal started;
  // wait on getConfigPortalActive() instead.
  wm.startConfigPortal(config::kPortalApName);
  if (!wm.getConfigPortalActive()) {
    Serial.println("Config portal failed to start");
    return false;
  }

  while (wm.getConfigPortalActive()) {
    inputPollLongPress();
    wm.process();
    delay(5);
  }

  return wifiLinkUp();
}

bool runConfigPortalFlow() {
  WiFiManager wm;
  configureWifiManager(wm);
  if (openConfigPortal(wm) && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    onStaConnected();
    return true;
  }
  return false;
}

}  // namespace

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
  esp_restart();
}

bool wifiReconnect() {
  Serial.println("WiFi reconnecting...");
  if (connectSavedNetwork(true)) {
    s_reconnect_failures = 0;
    return true;
  }

  if (!storedWifiCredentials()) {
    return false;
  }

  s_reconnect_failures++;
  if (s_reconnect_failures < kReconnectFailuresBeforePortal) {
    return false;
  }

  s_reconnect_failures = 0;
  Serial.println("Saved WiFi still failing, opening setup portal");
  return runConfigPortalFlow();
}

bool wifiSetupConnect() {
  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);
  s_reconnect_failures = 0;

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
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    onStaConnected();
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    onStaConnected();
    return true;
  }

  if (storedWifiCredentials()) {
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
