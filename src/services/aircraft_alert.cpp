#include "services/aircraft_alert.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cctype>
#include <cstring>

#include "hardware/buzzer.h"
#include "ui/radar_display.h"

namespace services::alert {
namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kMilKey[] = "alert_mil";
constexpr char kEmrgKey[] = "alert_emrg";
constexpr char kHideKey[] = "alert_hide";
constexpr char kWatchKey[] = "alert_watch";

constexpr size_t kSeenCapacity = 32;
constexpr size_t kWatchMax = 16;
constexpr size_t kWatchCallsignLen = 9;
constexpr size_t kWatchBlobLen = 160;
constexpr unsigned long kPulseCycleMs = 250;

bool s_mil_enabled = false;
bool s_emrg_enabled = false;
bool s_hide_non_alerted = false;

char s_watch_blob[kWatchBlobLen] = "";
char s_watch_callsigns[kWatchMax][kWatchCallsignLen];
size_t s_watch_count = 0;

struct SeenEntry {
  uint32_t hash = 0;
  bool occupied = false;
};

SeenEntry s_seen[kSeenCapacity];
size_t s_seen_write = 0;

uint32_t hashCallsign(const char* cs) {
  uint32_t h = 2166136261u;
  for (const char* p = cs; *p != '\0'; ++p) {
    h ^= static_cast<uint8_t>(*p);
    h *= 16777619u;
  }
  return h;
}

bool alreadySeen(uint32_t h) {
  for (size_t i = 0; i < kSeenCapacity; ++i) {
    if (s_seen[i].occupied && s_seen[i].hash == h) {
      return true;
    }
  }
  return false;
}

void markSeen(uint32_t h) {
  s_seen[s_seen_write].hash = h;
  s_seen[s_seen_write].occupied = true;
  s_seen_write = (s_seen_write + 1) % kSeenCapacity;
}

bool isValidWatchCallsign(const char* cs) {
  if (cs == nullptr || cs[0] == '\0') {
    return false;
  }
  size_t len = strlen(cs);
  if (len < 4 || len >= kWatchCallsignLen) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!isalpha(static_cast<unsigned char>(cs[i]))) {
      return false;
    }
  }
  for (size_t i = 3; cs[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(cs[i]);
    if (!isupper(c) && !isdigit(c)) {
      return false;
    }
  }
  return true;
}

void normalizeWatchCallsign(const char* in, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  char scratch[kWatchCallsignLen];
  const char* src = in;
  if (in == nullptr || in[0] == '\0') {
    out[0] = '\0';
    return;
  }
  if (in == out) {
    strncpy(scratch, in, sizeof(scratch) - 1);
    scratch[sizeof(scratch) - 1] = '\0';
    src = scratch;
  }
  out[0] = '\0';
  if (out_len < 2) {
    return;
  }
  size_t n = 0;
  for (size_t i = 0; src[i] != '\0' && n + 1 < out_len && n < kWatchCallsignLen - 1; ++i) {
    const unsigned char c = static_cast<unsigned char>(src[i]);
    if (isspace(c)) {
      continue;
    }
    out[n++] = static_cast<char>(toupper(c));
  }
  out[n] = '\0';
}

bool watchlistContains(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0' || s_watch_count == 0) {
    return false;
  }
  char normalized[kWatchCallsignLen];
  normalizeWatchCallsign(callsign, normalized, sizeof(normalized));
  if (!isValidWatchCallsign(normalized)) {
    return false;
  }
  for (size_t i = 0; i < s_watch_count; ++i) {
    if (strcmp(s_watch_callsigns[i], normalized) == 0) {
      return true;
    }
  }
  return false;
}

void parseWatchBlob(const char* blob) {
  s_watch_count = 0;
  if (blob == nullptr || blob[0] == '\0') {
    return;
  }
  char token[kWatchCallsignLen];
  const char* start = blob;
  while (*start != '\0' && s_watch_count < kWatchMax) {
    while (*start == ',' || isspace(static_cast<unsigned char>(*start))) {
      ++start;
    }
    if (*start == '\0') {
      break;
    }
    const char* end = start;
    while (*end != '\0' && *end != ',') {
      ++end;
    }
    const size_t len = static_cast<size_t>(end - start);
    if (len == 0) {
      start = end;
      continue;
    }
    size_t copy_len = len;
    if (copy_len >= sizeof(token)) {
      copy_len = sizeof(token) - 1;
    }
    memcpy(token, start, copy_len);
    token[copy_len] = '\0';
    char normalized[kWatchCallsignLen];
    normalizeWatchCallsign(token, normalized, sizeof(normalized));
    if (!isValidWatchCallsign(normalized)) {
      Serial.printf("[alert] watch reject '%s'\n", normalized[0] != '\0' ? normalized : token);
      start = (*end == ',') ? end + 1 : end;
      continue;
    }
    bool dup = false;
    for (size_t i = 0; i < s_watch_count; ++i) {
      if (strcmp(s_watch_callsigns[i], normalized) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      strncpy(s_watch_callsigns[s_watch_count], normalized, kWatchCallsignLen - 1);
      s_watch_callsigns[s_watch_count][kWatchCallsignLen - 1] = '\0';
      ++s_watch_count;
    }
    start = (*end == ',') ? end + 1 : end;
  }
}

void rebuildWatchBlob() {
  s_watch_blob[0] = '\0';
  size_t used = 0;
  for (size_t i = 0; i < s_watch_count; ++i) {
    if (i > 0) {
      if (used + 1 < sizeof(s_watch_blob)) {
        s_watch_blob[used++] = ',';
        s_watch_blob[used] = '\0';
      }
    }
    const size_t n = strlen(s_watch_callsigns[i]);
    if (used + n >= sizeof(s_watch_blob)) {
      break;
    }
    memcpy(s_watch_blob + used, s_watch_callsigns[i], n);
    used += n;
    s_watch_blob[used] = '\0';
  }
}

bool shouldAlert(const services::adsb::Aircraft& ac) {
  if (s_mil_enabled && ac.isMilitary()) {
    return true;
  }
  if (s_emrg_enabled && ac.isEmergencySquawk()) {
    return true;
  }
  if (watchlistContains(ac.callsign)) {
    return true;
  }
  return false;
}

bool alertsActive() {
  return s_mil_enabled || s_emrg_enabled || s_watch_count > 0;
}

}  // namespace

void bootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    Serial.println("[alert] NVS open failed");
    return;
  }
  s_mil_enabled = prefs.getBool(kMilKey, false);
  s_emrg_enabled = prefs.getBool(kEmrgKey, false);
  s_hide_non_alerted = prefs.getBool(kHideKey, false);
  const String watch = prefs.getString(kWatchKey, "");
  strncpy(s_watch_blob, watch.c_str(), sizeof(s_watch_blob) - 1);
  s_watch_blob[sizeof(s_watch_blob) - 1] = '\0';
  prefs.end();
  parseWatchBlob(s_watch_blob);
  Serial.printf("[alert] boot mil=%d emrg=%d hide=%d watch=%u\n",
                s_mil_enabled ? 1 : 0, s_emrg_enabled ? 1 : 0, s_hide_non_alerted ? 1 : 0,
                static_cast<unsigned>(s_watch_count));
}

void checkNewAircraft(const services::adsb::Aircraft* list, size_t count) {
  if (!alertsActive()) {
    return;
  }
  bool fired = false;
  for (size_t i = 0; i < count; ++i) {
    if (!shouldAlert(list[i])) {
      continue;
    }
    const uint32_t h = hashCallsign(list[i].callsign);
    if (alreadySeen(h)) {
      continue;
    }
    markSeen(h);
    fired = true;
    Serial.printf("[alert] %s mil=%d emrg=%d watch=%d squawk=%s\n",
                  list[i].callsign, list[i].isMilitary() ? 1 : 0,
                  list[i].isEmergencySquawk() ? 1 : 0,
                  watchlistContains(list[i].callsign) ? 1 : 0, list[i].squawk);
  }
  if (fired) {
    hardware::buzzerAlert();
  }
}

bool isHighlighted(const services::adsb::Aircraft& ac) {
  if (s_mil_enabled && ac.isMilitary()) {
    return true;
  }
  if (s_emrg_enabled && ac.isEmergencySquawk()) {
    return true;
  }
  if (watchlistContains(ac.callsign)) {
    return true;
  }
  return false;
}

bool pulsePhase() {
  return (millis() / kPulseCycleMs) % 2 == 0;
}

bool militaryAlertEnabled() { return s_mil_enabled; }
bool emergencyAlertEnabled() { return s_emrg_enabled; }
bool hideNonAlertedEnabled() { return s_hide_non_alerted; }
bool watchCallsignsEnabled() { return s_watch_count > 0; }
size_t watchCallsignCount() { return s_watch_count; }

void watchCallsignsFormatted(char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  strncpy(out, s_watch_blob, out_len - 1);
  out[out_len - 1] = '\0';
}

void saveFromForm(const char* mil_checkbox, const char* emrg_checkbox,
                  const char* hide_checkbox, const char* watch_callsigns,
                  bool update_watch_callsigns) {
  const bool prev_mil = s_mil_enabled;
  const bool prev_emrg = s_emrg_enabled;
  const bool prev_hide = s_hide_non_alerted;
  const size_t prev_watch = s_watch_count;
  char prev_blob[sizeof(s_watch_blob)];
  strncpy(prev_blob, s_watch_blob, sizeof(prev_blob));
  prev_blob[sizeof(prev_blob) - 1] = '\0';

  s_mil_enabled = (mil_checkbox != nullptr && mil_checkbox[0] == 'T');
  s_emrg_enabled = (emrg_checkbox != nullptr && emrg_checkbox[0] == 'T');
  s_hide_non_alerted = (hide_checkbox != nullptr && hide_checkbox[0] == 'T');

  if (update_watch_callsigns) {
    char scratch[kWatchBlobLen];
    scratch[0] = '\0';
    if (watch_callsigns != nullptr) {
      strncpy(scratch, watch_callsigns, sizeof(scratch) - 1);
      scratch[sizeof(scratch) - 1] = '\0';
    }
    parseWatchBlob(scratch);
    rebuildWatchBlob();
  }

  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kMilKey, s_mil_enabled);
    prefs.putBool(kEmrgKey, s_emrg_enabled);
    prefs.putBool(kHideKey, s_hide_non_alerted);
    prefs.putString(kWatchKey, s_watch_blob);
    prefs.end();
  }
  const bool watch_changed =
      s_watch_count != prev_watch || strcmp(s_watch_blob, prev_blob) != 0;
  if (s_hide_non_alerted != prev_hide || s_mil_enabled != prev_mil ||
      s_emrg_enabled != prev_emrg || watch_changed) {
    ui::radarDisplayInvalidateAircraft();
  }
  Serial.printf("[alert] saved mil=%d emrg=%d hide=%d watch=%u (%s)\n",
                s_mil_enabled ? 1 : 0, s_emrg_enabled ? 1 : 0, s_hide_non_alerted ? 1 : 0,
                static_cast<unsigned>(s_watch_count), s_watch_blob);
}

}  // namespace services::alert
