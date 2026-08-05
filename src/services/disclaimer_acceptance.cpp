#include "services/disclaimer_acceptance.h"

#include <Preferences.h>
#include <cstring>

#include "config.h"
#include "services/settings_state.h"

namespace services::disclaimer {
namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kVersionKey[] = "disc_ver";
constexpr char kFirmwareKey[] = "disc_fw";
constexpr size_t kFirmwareValueCap = 48;

uint16_t s_stored_version = 0;
char s_stored_firmware[kFirmwareValueCap] = {};

void persist(uint16_t version) {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, false)) {
    return;
  }
  if (version == 0) {
    prefs.remove(kVersionKey);
    prefs.remove(kFirmwareKey);
  } else {
    prefs.putUShort(kVersionKey, version);
    prefs.putString(kFirmwareKey, config::kFirmwareVersion);
  }
  prefs.end();
  s_stored_version = version;
  if (version == 0) {
    s_stored_firmware[0] = '\0';
  } else {
    strncpy(s_stored_firmware, config::kFirmwareVersion,
            sizeof(s_stored_firmware) - 1);
    s_stored_firmware[sizeof(s_stored_firmware) - 1] = '\0';
  }
  settingsStateBump();
}

}  // namespace

void bootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    s_stored_version = 0;
    s_stored_firmware[0] = '\0';
    return;
  }
  s_stored_version = prefs.getUShort(kVersionKey, 0);
  s_stored_firmware[0] = '\0';
  prefs.getString(kFirmwareKey, s_stored_firmware,
                  sizeof(s_stored_firmware));
  prefs.end();
}

bool isRemembered() {
  return s_stored_version == kCurrentVersion &&
         strcmp(s_stored_firmware, config::kFirmwareVersion) == 0;
}

void rememberCurrent() { persist(kCurrentVersion); }

void clear() { persist(0); }

uint16_t storedVersion() { return s_stored_version; }

}  // namespace services::disclaimer
