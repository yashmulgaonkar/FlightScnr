#include "services/ota_github.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "services/adsb_client.h"
#include "services/firmware_image.h"
#include "services/https_heap.h"
#include "services/https_lock.h"

namespace services::ota_github {

namespace {

constexpr char kStoreNs[] = "fs_ota_gh";
constexpr char kTagKey[] = "tag";
constexpr char kUrlKey[] = "url";
constexpr char kAvailKey[] = "av";

constexpr char kApiHost[] = "api.github.com";
constexpr char kApiPath[] =
    "/repos/yashmulgaonkar/FlightScnr/releases/latest";
constexpr char kAppAssetName[] = "FlightScnr-" FLIGHTSCNR_BOARD_NAME "-app.bin";
constexpr char kUserAgent[] = "FlightScnr-OTA";

constexpr unsigned long kCheckIntervalMs = 24UL * 60UL * 60UL * 1000UL;
/** After a failed GitHub check (e.g. HTTP 403), wait before retrying. */
constexpr unsigned long kCheckRetryOnFailMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kCheckTimeoutMs = 20000;
constexpr uint32_t kInstallTimeoutMs = 10UL * 60UL * 1000UL;
constexpr size_t kStreamChunk = 4096;
constexpr size_t kOtaBreatherBytes = 16u * 1024u;

char s_latest_tag[32] = {};
char s_asset_url[256] = {};
bool s_available = false;
unsigned long s_last_check_ms = 0;
unsigned long s_check_cooldown_ms = kCheckIntervalMs;

volatile InstallState s_install_state = InstallState::Idle;
volatile uint8_t s_install_pct = 0;
volatile uint32_t s_install_bytes = 0;
volatile uint32_t s_install_total = 0;
char s_install_error[96] = {};
TaskHandle_t s_install_task = nullptr;

const char* skipVersionPrefix(const char* s) {
  if (s == nullptr) {
    return "";
  }
  if ((s[0] == 'v' || s[0] == 'V') && s[1] != '\0') {
    return s + 1;
  }
  return s;
}

/** True when latest is strictly newer than current (dotted numeric). "dev" is always older. */
bool isNewerThanRunning(const char* latest_tag) {
  const char* latest = skipVersionPrefix(latest_tag);
  const char* current = skipVersionPrefix(config::kFirmwareVersion);
  if (latest[0] == '\0') {
    return false;
  }
  if (strcmp(current, "dev") == 0) {
    return true;
  }
  if (strcmp(latest, current) == 0) {
    return false;
  }

  const char* a = latest;
  const char* b = current;
  while (true) {
    char* a_end = nullptr;
    char* b_end = nullptr;
    const long av = strtol(a, &a_end, 10);
    const long bv = strtol(b, &b_end, 10);
    if (a_end == a && b_end == b) {
      return strcmp(a, b) > 0;
    }
    if (a_end == a) {
      return false;
    }
    if (b_end == b) {
      return true;
    }
    if (av != bv) {
      return av > bv;
    }
    a = (*a_end == '.') ? a_end + 1 : a_end;
    b = (*b_end == '.') ? b_end + 1 : b_end;
    if (*a == '\0' && *b == '\0') {
      return false;
    }
    if (*a == '\0') {
      return false;
    }
    if (*b == '\0') {
      return true;
    }
  }
}

void persistCache() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, false)) {
    return;
  }
  prefs.putString(kTagKey, s_latest_tag);
  prefs.putString(kUrlKey, s_asset_url);
  prefs.putBool(kAvailKey, s_available);
  prefs.end();
}

void loadCache() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  String tag = prefs.getString(kTagKey, "");
  String url = prefs.getString(kUrlKey, "");
  s_available = prefs.getBool(kAvailKey, false);
  prefs.end();

  strncpy(s_latest_tag, tag.c_str(), sizeof(s_latest_tag) - 1);
  s_latest_tag[sizeof(s_latest_tag) - 1] = '\0';
  strncpy(s_asset_url, url.c_str(), sizeof(s_asset_url) - 1);
  s_asset_url[sizeof(s_asset_url) - 1] = '\0';
  // Recompute availability from tags; do not restore millis-based freshness —
  // that incorrectly skipped checks after reboot.
  if (s_latest_tag[0] != '\0') {
    s_available = isNewerThanRunning(s_latest_tag);
  }
}

/** True when a check already ran this boot and is still within the cooldown. */
bool cacheFresh() {
  if (s_last_check_ms == 0) {
    return false;
  }
  const unsigned long now = millis();
  if (now < s_last_check_ms) {
    return false;
  }
  return (now - s_last_check_ms) < s_check_cooldown_ms;
}

void markCheckAttempt(bool success) {
  s_last_check_ms = millis();
  s_check_cooldown_ms = success ? kCheckIntervalMs : kCheckRetryOnFailMs;
}

size_t otaAppPartitionSize() {
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  return next != nullptr ? static_cast<size_t>(next->size) : 0;
}

bool fetchLatestFromGitHub(bool wait_for_link) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (wait_for_link) {
    const unsigned long deadline = millis() + 12000UL;
    while (millis() < deadline) {
      if (!services::adsb::fetchInProgress() && !services::https::busy() &&
          services::https::heapReadyForRouteApi()) {
        break;
      }
      delay(50);
    }
  } else if (services::adsb::fetchInProgress() || services::https::busy() ||
             !services::https::heapReadyForRouteApi()) {
    return false;
  }

  if (services::adsb::fetchInProgress() || services::https::busy() ||
      !services::https::heapReadyForRouteApi()) {
    Serial.println("[ota_gh] skip: https/adsb busy or low heap");
    return false;
  }

  services::https::ScopedLock tls(kCheckTimeoutMs + 4000);
  if (!tls.held()) {
    Serial.println("[ota_gh] skip: could not take https lock");
    return false;
  }
  services::https::drainTlsHeapAfterSession(400);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(kCheckTimeoutMs / 1000);

  HTTPClient http;
  http.setTimeout(kCheckTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.addHeader("Accept", "application/vnd.github+json");

  char url[128];
  snprintf(url, sizeof(url), "https://%s%s", kApiHost, kApiPath);
  if (!http.begin(client, url)) {
    markCheckAttempt(false);
    services::https::drainTlsHeapAfterSession();
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota_gh] releases HTTP %d (retry in %luh)\n", code,
                  static_cast<unsigned long>(kCheckRetryOnFailMs / 3600000UL));
    markCheckAttempt(false);
    http.end();
    services::https::drainTlsHeapAfterSession();
    return false;
  }

  // Filter keeps tag + asset name/url only (release body can be large).
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  services::https::drainTlsHeapAfterSession();

  if (err) {
    Serial.printf("[ota_gh] json: %s\n", err.c_str());
    markCheckAttempt(false);
    return false;
  }

  const char* tag = doc["tag_name"] | "";
  if (tag[0] == '\0') {
    Serial.println("[ota_gh] missing tag_name");
    markCheckAttempt(false);
    return false;
  }

  const char* asset_url = nullptr;
  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject asset : assets) {
    const char* name = asset["name"] | "";
    if (strcmp(name, kAppAssetName) == 0) {
      asset_url = asset["browser_download_url"] | "";
      break;
    }
  }
  if (asset_url == nullptr || asset_url[0] == '\0') {
    Serial.println("[ota_gh] app.bin asset not found");
    markCheckAttempt(false);
    return false;
  }

  strncpy(s_latest_tag, tag, sizeof(s_latest_tag) - 1);
  s_latest_tag[sizeof(s_latest_tag) - 1] = '\0';
  strncpy(s_asset_url, asset_url, sizeof(s_asset_url) - 1);
  s_asset_url[sizeof(s_asset_url) - 1] = '\0';
  s_available = isNewerThanRunning(s_latest_tag);
  markCheckAttempt(true);
  persistCache();
  Serial.printf("[ota_gh] latest=%s available=%d\n", s_latest_tag,
                s_available ? 1 : 0);
  return true;
}

void setInstallFailed(const char* msg) {
  strncpy(s_install_error, msg != nullptr ? msg : "failed",
          sizeof(s_install_error) - 1);
  s_install_error[sizeof(s_install_error) - 1] = '\0';
  s_install_pct = 0;
  s_install_bytes = 0;
  s_install_total = 0;
  s_install_state = InstallState::Failed;
}

void updateInstallProgress(size_t written, int content_len, size_t max_part) {
  s_install_bytes = static_cast<uint32_t>(written);
  size_t total = 0;
  if (content_len > 0) {
    total = static_cast<size_t>(content_len);
  } else if (max_part > 0) {
    // No Content-Length (common after GitHub redirects). Use a soft estimate so
    // the bar moves; clamp at 99 until Update.end succeeds.
    total = max_part > (6u * 1024u * 1024u) ? (5u * 1024u * 1024u + 512u * 1024u)
                                            : (max_part * 3u) / 4u;
  }
  s_install_total = static_cast<uint32_t>(total);
  if (total == 0) {
    s_install_pct = 0;
    return;
  }
  unsigned pct = static_cast<unsigned>((written * 100UL) / total);
  if (pct > 99) {
    pct = 99;
  }
  s_install_pct = static_cast<uint8_t>(pct);
}

void installTaskThunk(void*) {
  char url[256];
  strncpy(url, s_asset_url, sizeof(url) - 1);
  url[sizeof(url) - 1] = '\0';

  if (url[0] == '\0') {
    setInstallFailed("no asset url");
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  services::https::ScopedLock tls(kInstallTimeoutMs);
  if (!tls.held()) {
    setInstallFailed("https busy");
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  services::https::drainTlsHeapAfterSession(600);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(60);

  HTTPClient http;
  http.setTimeout(static_cast<int>(kInstallTimeoutMs));
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.setReuse(false);

  if (!http.begin(client, url)) {
    setInstallFailed("http begin failed");
    services::https::drainTlsHeapAfterSession();
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char msg[48];
    snprintf(msg, sizeof(msg), "download HTTP %d", code);
    setInstallFailed(msg);
    http.end();
    services::https::drainTlsHeapAfterSession();
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  const int content_len = http.getSize();
  const size_t max_part = otaAppPartitionSize();
  if (content_len > 0 &&
      !services::ota::firmwareSizeLooksValid(static_cast<size_t>(content_len),
                                             max_part)) {
    setInstallFailed("image size invalid");
    http.end();
    services::https::drainTlsHeapAfterSession();
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  if (!Update.begin(content_len > 0 ? static_cast<size_t>(content_len)
                                    : UPDATE_SIZE_UNKNOWN)) {
    setInstallFailed("Update.begin failed");
    http.end();
    services::https::drainTlsHeapAfterSession();
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  bool buf_psram = true;
  uint8_t* buf = static_cast<uint8_t*>(
      heap_caps_malloc(kStreamChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    buf_psram = false;
    buf = static_cast<uint8_t*>(malloc(kStreamChunk));
  }
  if (buf == nullptr) {
    Update.abort();
    setInstallFailed("no buffer");
    http.end();
    services::https::drainTlsHeapAfterSession();
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  bool header_ok = false;
  size_t written = 0;
  size_t since_breather = 0;
  const unsigned long deadline = millis() + kInstallTimeoutMs;
  bool failed = false;

  while (http.connected() && (content_len < 0 || written < static_cast<size_t>(content_len))) {
    if (millis() > deadline) {
      setInstallFailed("download timeout");
      failed = true;
      break;
    }
    const size_t avail = stream->available();
    if (avail == 0) {
      delay(1);
      continue;
    }
    const size_t to_read = avail > kStreamChunk ? kStreamChunk : avail;
    const int n = stream->readBytes(buf, to_read);
    if (n <= 0) {
      delay(1);
      continue;
    }
    if (!header_ok) {
      header_ok = true;
      if (!services::ota::firmwareHeaderLooksValid(
              buf, static_cast<size_t>(n),
              content_len > 0 ? static_cast<size_t>(content_len) : 0, max_part)) {
        setInstallFailed("invalid image header");
        failed = true;
        break;
      }
    }
    if (Update.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      setInstallFailed("flash write failed");
      failed = true;
      break;
    }
    written += static_cast<size_t>(n);
    since_breather += static_cast<size_t>(n);
    updateInstallProgress(written, content_len, max_part);
    if (since_breather >= kOtaBreatherBytes) {
      since_breather = 0;
      delay(1);
    }
  }

  if (buf_psram) {
    heap_caps_free(buf);
  } else {
    free(buf);
  }
  http.end();

  if (failed) {
    Update.abort();
    services::https::drainTlsHeapAfterSession();
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  if (!services::ota::firmwareSizeLooksValid(written, max_part)) {
    Update.abort();
    setInstallFailed("final size invalid");
    services::https::drainTlsHeapAfterSession();
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  if (!Update.end(true)) {
    setInstallFailed("Update.end failed");
    services::https::drainTlsHeapAfterSession();
    s_install_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  s_install_pct = 100;
  s_install_bytes = static_cast<uint32_t>(written);
  s_install_total = static_cast<uint32_t>(written);
  s_install_error[0] = '\0';
  s_install_state = InstallState::Succeeded;
  // After a successful install the running image is "current" until reboot —
  // clear the available flag so About stops nagging.
  s_available = false;
  persistCache();
  Serial.printf("[ota_gh] install ok %u bytes — reset device\n",
                static_cast<unsigned>(written));
  services::https::drainTlsHeapAfterSession();
  s_install_task = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

void init() {
  loadCache();
  // Session freshness only — always allow a check after boot.
  s_last_check_ms = 0;
}

void pollIfDue() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (cacheFresh()) {
    return;
  }
  if (s_install_state == InstallState::Running) {
    return;
  }
  (void)fetchLatestFromGitHub(false);
}

bool checkLatest(bool force) {
  if (!force && cacheFresh()) {
    return true;
  }
  return fetchLatestFromGitHub(force);
}

bool updateAvailable() { return s_available && s_latest_tag[0] != '\0'; }

const char* latestTag() { return s_latest_tag; }

const char* currentVersion() { return config::kFirmwareVersion; }

bool startInstall() {
  if (s_install_state == InstallState::Running || s_install_task != nullptr) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setInstallFailed("wifi down");
    return false;
  }
  if (s_asset_url[0] == '\0') {
    if (!checkLatest(true) || s_asset_url[0] == '\0') {
      setInstallFailed("no release asset");
      return false;
    }
  }
  s_install_error[0] = '\0';
  s_install_pct = 0;
  s_install_bytes = 0;
  s_install_total = 0;
  s_install_state = InstallState::Running;

  const BaseType_t ok =
      xTaskCreatePinnedToCore(installTaskThunk, "ota_gh", 8192, nullptr, 1,
                              &s_install_task, 0);
  if (ok != pdPASS) {
    s_install_task = nullptr;
    setInstallFailed("task create failed");
    return false;
  }
  return true;
}

InstallState installState() { return s_install_state; }

uint8_t installPercent() { return s_install_pct; }

uint32_t installBytes() { return s_install_bytes; }

uint32_t installTotal() { return s_install_total; }

const char* installError() { return s_install_error; }

}  // namespace services::ota_github
