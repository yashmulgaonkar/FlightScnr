#include "services/aircraft_alert.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "hardware/buzzer.h"
#include "ui/radar_display.h"

namespace services::alert {
namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kMilKey[] = "alert_mil";
constexpr char kEmrgKey[] = "alert_emrg";
constexpr char kHideKey[] = "alert_hide";

constexpr size_t kSeenCapacity = 32;
constexpr unsigned long kPulseCycleMs = 250;

bool s_mil_enabled = false;
bool s_emrg_enabled = false;
bool s_hide_non_alerted = false;

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

bool shouldAlert(const services::adsb::Aircraft& ac) {
  if (s_mil_enabled && ac.isMilitary()) {
    return true;
  }
  if (s_emrg_enabled && ac.isEmergencySquawk()) {
    return true;
  }
  return false;
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
  prefs.end();
  Serial.printf("[alert] boot mil=%d emrg=%d hide=%d\n",
                s_mil_enabled ? 1 : 0, s_emrg_enabled ? 1 : 0, s_hide_non_alerted ? 1 : 0);
}

void checkNewAircraft(const services::adsb::Aircraft* list, size_t count) {
  if (!s_mil_enabled && !s_emrg_enabled) {
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
    Serial.printf("[alert] %s mil=%d emrg=%d squawk=%s\n",
                  list[i].callsign,
                  list[i].isMilitary() ? 1 : 0,
                  list[i].isEmergencySquawk() ? 1 : 0,
                  list[i].squawk);
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
  return false;
}

bool pulsePhase() {
  return (millis() / kPulseCycleMs) % 2 == 0;
}

bool militaryAlertEnabled() { return s_mil_enabled; }
bool emergencyAlertEnabled() { return s_emrg_enabled; }
bool hideNonAlertedEnabled() { return s_hide_non_alerted; }

void saveFromForm(const char* mil_checkbox, const char* emrg_checkbox,
                  const char* hide_checkbox) {
  const bool prev_mil = s_mil_enabled;
  const bool prev_emrg = s_emrg_enabled;
  const bool prev_hide = s_hide_non_alerted;
  s_mil_enabled = (mil_checkbox != nullptr && mil_checkbox[0] == 'T');
  s_emrg_enabled = (emrg_checkbox != nullptr && emrg_checkbox[0] == 'T');
  s_hide_non_alerted = (hide_checkbox != nullptr && hide_checkbox[0] == 'T');
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kMilKey, s_mil_enabled);
    prefs.putBool(kEmrgKey, s_emrg_enabled);
    prefs.putBool(kHideKey, s_hide_non_alerted);
    prefs.end();
  }
  if (s_hide_non_alerted != prev_hide || s_mil_enabled != prev_mil ||
      s_emrg_enabled != prev_emrg) {
    ui::radarDisplayInvalidateAircraft();
  }
  Serial.printf("[alert] saved mil=%d emrg=%d hide=%d\n",
                s_mil_enabled ? 1 : 0, s_emrg_enabled ? 1 : 0, s_hide_non_alerted ? 1 : 0);
}

}  // namespace services::alert
