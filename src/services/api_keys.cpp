#include "services/api_keys.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"

namespace services::apikeys {

namespace {

constexpr char kNs[] = "flightscnr";
constexpr char kAirLabs[] = "al_key";
constexpr char kFlightAware[] = "fa_key";
constexpr char kFr24[] = "fr24_key";
constexpr char kWeather[] = "wx_key";
constexpr char kUseWeather[] = "wx_use";
constexpr char kUseAirLabs[] = "al_use";
constexpr char kUseFlightAware[] = "fa_use";
constexpr char kUseFr24[] = "fr24_use";
constexpr char kUseAdsbDb[] = "adb_use";
constexpr char kUseOpenMeteo[] = "om_use";
constexpr char kAlMaxCalls[] = "al_max";
constexpr char kAlUsedKeys[] = "al_used_k";
constexpr char kFaBudgetMicro[] = "fa_bgt_u";
constexpr char kFaCostMicro[] = "fa_cst_u";
constexpr char kFaSpentKeys[] = "fa_sp_k";
constexpr char kFr24BudgetMicro[] = "fr_bgt_u";
constexpr char kFr24CostMicro[] = "fr_cst_u";
constexpr char kFr24SpentKeys[] = "fr_sp_k";
constexpr char kUsageResetYm[] = "use_ym";
/** Legacy single-counter prefs (migrated to per-key on load). */
constexpr char kAlUsedCalls[] = "al_used";
constexpr char kFaSpentMicro[] = "fa_sp_u";
constexpr char kFr24SpentMicro[] = "fr_sp_u";

constexpr size_t kMaxKeysBlobLen = 512;
constexpr time_t kMinValidEpoch = 1600000000;
constexpr uint32_t kUsdMicroPerDollar = 1000000UL;

struct ProviderState {
  char blob[kMaxKeysBlobLen + 1];
  char keys[kMaxKeysPerProvider][kMaxSingleKeyLen + 1];
  size_t key_count;
  uint32_t used[kMaxKeysPerProvider];
};

ProviderState s_airlabs;
ProviderState s_flightaware;
ProviderState s_fr24;

bool s_use_airlabs = false;
bool s_use_flightaware = false;
bool s_use_fr24 = false;

char s_weather_key[kMaxSingleKeyLen + 1] = {0};
bool s_use_weather = true;
bool s_use_adsbdb = true;
bool s_use_openmeteo = true;

uint32_t s_al_max_calls = config::kDefaultAirLabsMaxCalls;
uint32_t s_fa_budget_micro = config::kDefaultFlightAwareBudgetUsdMicro;
uint32_t s_fa_cost_micro = config::kDefaultFlightAwareCostUsdMicro;
uint32_t s_fr24_budget_micro = config::kDefaultFr24BudgetUsdMicro;
uint32_t s_fr24_cost_micro = config::kDefaultFr24CostUsdMicro;
uint32_t s_usage_reset_ym = 0;

bool s_warned_al_limit = false;
bool s_warned_fa_limit = false;
bool s_warned_fr24_limit = false;

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  if ((value[0] == 'F' || value[0] == 'f') && value[1] == '\0') {
    return false;
  }
  if ((value[0] == 'T' || value[0] == 't') && value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

void copyBlob(char* dest, const char* src, size_t cap) {
  if (src == nullptr) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, cap - 1);
  dest[cap - 1] = '\0';
}

void trimToken(char* token) {
  if (token == nullptr) {
    return;
  }
  size_t start = 0;
  while (token[start] != '\0' && isspace(static_cast<unsigned char>(token[start]))) {
    ++start;
  }
  if (start > 0) {
    memmove(token, token + start, strlen(token + start) + 1);
  }
  size_t n = strlen(token);
  while (n > 0 && isspace(static_cast<unsigned char>(token[n - 1]))) {
    token[n - 1] = '\0';
    --n;
  }
}

void parseKeyList(const char* blob, ProviderState* state) {
  state->key_count = 0;
  memset(state->used, 0, sizeof(state->used));
  for (size_t i = 0; i < kMaxKeysPerProvider; ++i) {
    state->keys[i][0] = '\0';
  }
  if (blob == nullptr || blob[0] == '\0') {
    return;
  }

  char scratch[kMaxKeysBlobLen + 1];
  strncpy(scratch, blob, sizeof(scratch) - 1);
  scratch[sizeof(scratch) - 1] = '\0';

  char* save = nullptr;
  char* token = strtok_r(scratch, ",", &save);
  while (token != nullptr && state->key_count < kMaxKeysPerProvider) {
    trimToken(token);
    if (token[0] != '\0') {
      strncpy(state->keys[state->key_count], token, kMaxSingleKeyLen);
      state->keys[state->key_count][kMaxSingleKeyLen] = '\0';
      ++state->key_count;
    }
    token = strtok_r(nullptr, ",", &save);
  }
}

bool parseU32(const char* text, uint32_t* out) {
  if (text == nullptr || text[0] == '\0' || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  const unsigned long v = strtoul(text, &end, 10);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

void serializeUsageCsv(const uint32_t* used, size_t count, char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  size_t pos = 0;
  for (size_t i = 0; i < count; ++i) {
    const int n = snprintf(out + pos, len - pos, i == 0 ? "%lu" : ",%lu",
                           static_cast<unsigned long>(used[i]));
    if (n <= 0 || static_cast<size_t>(n) >= len - pos) {
      break;
    }
    pos += static_cast<size_t>(n);
  }
  if (pos == 0) {
    out[0] = '\0';
  }
}

bool parseUsageCsv(const char* csv, uint32_t* used, size_t max_count, size_t* parsed_count) {
  if (parsed_count != nullptr) {
    *parsed_count = 0;
  }
  if (csv == nullptr || csv[0] == '\0' || used == nullptr) {
    return false;
  }

  char scratch[128];
  strncpy(scratch, csv, sizeof(scratch) - 1);
  scratch[sizeof(scratch) - 1] = '\0';

  size_t count = 0;
  char* save = nullptr;
  char* token = strtok_r(scratch, ",", &save);
  while (token != nullptr && count < max_count) {
    trimToken(token);
    uint32_t v = 0;
    if (parseU32(token, &v)) {
      used[count++] = v;
    }
    token = strtok_r(nullptr, ",", &save);
  }
  if (parsed_count != nullptr) {
    *parsed_count = count;
  }
  return count > 0;
}

void maskedPreviewSingle(const char* key, char* out, size_t len) {
  if (len == 0) {
    return;
  }
  out[0] = '\0';
  if (key == nullptr || key[0] == '\0') {
    strncpy(out, "(not set)", len - 1);
    out[len - 1] = '\0';
    return;
  }
  const size_t n = strlen(key);
  if (n <= 4) {
    strncpy(out, "••••", len - 1);
  } else {
    snprintf(out, len, "••••%s", key + n - 4);
  }
  out[len - 1] = '\0';
}

void maskedPreviewProvider(const ProviderState& state, char* out, size_t len) {
  if (state.key_count == 0) {
    strncpy(out, "(not set)", len - 1);
    out[len - 1] = '\0';
    return;
  }
  if (state.key_count == 1) {
    maskedPreviewSingle(state.keys[0], out, len);
    return;
  }
  char tail[8];
  maskedPreviewSingle(state.keys[0], tail, sizeof(tail));
  snprintf(out, len, "%u keys (%s…)", static_cast<unsigned>(state.key_count), tail);
  out[len - 1] = '\0';
}

bool saveIfNonEmpty(const char* value, const char* pref_key) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return false;
  }
  prefs.putString(pref_key, value);
  prefs.end();
  return true;
}

uint32_t currentYearMonth() {
  const time_t now = time(nullptr);
  if (now < kMinValidEpoch) {
    return 0;
  }
  struct tm local {};
  localtime_r(&now, &local);
  return static_cast<uint32_t>((local.tm_year + 1900) * 100 + (local.tm_mon + 1));
}

void persistUsageCounters() {
  char buf[96];
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  serializeUsageCsv(s_airlabs.used, s_airlabs.key_count, buf, sizeof(buf));
  prefs.putString(kAlUsedKeys, buf);
  serializeUsageCsv(s_flightaware.used, s_flightaware.key_count, buf, sizeof(buf));
  prefs.putString(kFaSpentKeys, buf);
  serializeUsageCsv(s_fr24.used, s_fr24.key_count, buf, sizeof(buf));
  prefs.putString(kFr24SpentKeys, buf);
  prefs.putUInt(kUsageResetYm, s_usage_reset_ym);
  prefs.end();
}

void persistLimitConfig() {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  prefs.putUInt(kAlMaxCalls, s_al_max_calls);
  prefs.putUInt(kFaBudgetMicro, s_fa_budget_micro);
  prefs.putUInt(kFaCostMicro, s_fa_cost_micro);
  prefs.putUInt(kFr24BudgetMicro, s_fr24_budget_micro);
  prefs.putUInt(kFr24CostMicro, s_fr24_cost_micro);
  prefs.end();
}

void resetProviderUsage(ProviderState* state) {
  memset(state->used, 0, sizeof(state->used));
}

void resetMonthlyUsage(uint32_t ym) {
  resetProviderUsage(&s_airlabs);
  resetProviderUsage(&s_flightaware);
  resetProviderUsage(&s_fr24);
  s_usage_reset_ym = ym;
  s_warned_al_limit = false;
  s_warned_fa_limit = false;
  s_warned_fr24_limit = false;
  persistUsageCounters();
  if (ym != 0) {
    Serial.printf("API usage reset for %04u-%02u\n", ym / 100, ym % 100);
  }
}

void maybeResetMonthlyUsage() {
  const uint32_t ym = currentYearMonth();
  if (ym == 0) {
    return;
  }
  if (s_usage_reset_ym != ym) {
    resetMonthlyUsage(ym);
  }
}

bool parseUsdToMicro(const char* text, uint32_t* out_micro) {
  if (text == nullptr || text[0] == '\0' || out_micro == nullptr) {
    return false;
  }
  char* end = nullptr;
  const double usd = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  if (usd < 0.0) {
    return false;
  }
  const double micro = usd * static_cast<double>(kUsdMicroPerDollar);
  if (micro > static_cast<double>(UINT32_MAX)) {
    *out_micro = UINT32_MAX;
  } else {
    *out_micro = static_cast<uint32_t>(micro + 0.5);
  }
  return true;
}

bool budgetAllowsCall(uint32_t spent_micro, uint32_t cost_micro, uint32_t budget_micro) {
  if (budget_micro == 0) {
    return true;
  }
  if (cost_micro == 0) {
    return spent_micro < budget_micro;
  }
  return spent_micro + cost_micro <= budget_micro;
}

void warnLimitOnce(bool* warned, const char* provider, const char* detail) {
  if (warned == nullptr || *warned) {
    return;
  }
  *warned = true;
  Serial.printf("Route API limit: %s — %s\n", provider, detail);
}

void loadProviderKeys(Preferences& prefs, const char* pref_key, ProviderState* state) {
  if (prefs.isKey(pref_key)) {
    copyBlob(state->blob, prefs.getString(pref_key).c_str(), sizeof(state->blob));
  } else {
    state->blob[0] = '\0';
  }
  parseKeyList(state->blob, state);
}

void loadProviderUsage(Preferences& prefs, const char* per_key_pref,
                       const char* legacy_pref, ProviderState* state) {
  if (prefs.isKey(per_key_pref)) {
    size_t parsed = 0;
    parseUsageCsv(prefs.getString(per_key_pref).c_str(), state->used, kMaxKeysPerProvider,
                  &parsed);
    (void)parsed;
    return;
  }
  if (prefs.isKey(legacy_pref) && state->key_count > 0) {
    state->used[0] = prefs.getUInt(legacy_pref, 0);
  }
}

void applyProviderBlob(ProviderState* state, const char* new_blob) {
  const bool changed = strcmp(state->blob, new_blob) != 0;
  copyBlob(state->blob, new_blob, sizeof(state->blob));
  parseKeyList(state->blob, state);
  if (changed) {
    resetProviderUsage(state);
  }
}

uint32_t sumUsage(const uint32_t* used, size_t count) {
  uint32_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    total += used[i];
  }
  return total;
}

bool canUseAirLabsAtImpl(size_t index) {
  if (index >= s_airlabs.key_count || s_airlabs.keys[index][0] == '\0') {
    return false;
  }
  maybeResetMonthlyUsage();
  if (s_airlabs.used[index] == UINT32_MAX) {
    return false; // marked exhausted (e.g. month_limit_exceeded from API)
  }
  if (s_al_max_calls == 0) {
    return true;
  }
  return s_airlabs.used[index] < s_al_max_calls;
}

bool canUseFlightAwareAtImpl(size_t index) {
  if (index >= s_flightaware.key_count || s_flightaware.keys[index][0] == '\0') {
    return false;
  }
  maybeResetMonthlyUsage();
  return budgetAllowsCall(s_flightaware.used[index], s_fa_cost_micro, s_fa_budget_micro);
}

bool canUseFr24AtImpl(size_t index) {
  if (index >= s_fr24.key_count || s_fr24.keys[index][0] == '\0') {
    return false;
  }
  maybeResetMonthlyUsage();
  return budgetAllowsCall(s_fr24.used[index], s_fr24_cost_micro, s_fr24_budget_micro);
}

}  // namespace

void load() {
  Preferences prefs;
  if (!prefs.begin(kNs, true)) {
    s_airlabs.blob[0] = s_flightaware.blob[0] = s_fr24.blob[0] = '\0';
    parseKeyList(nullptr, &s_airlabs);
    parseKeyList(nullptr, &s_flightaware);
    parseKeyList(nullptr, &s_fr24);
    s_use_airlabs = s_use_flightaware = s_use_fr24 = false;
    return;
  }

  loadProviderKeys(prefs, kAirLabs, &s_airlabs);
  loadProviderKeys(prefs, kFlightAware, &s_flightaware);
  loadProviderKeys(prefs, kFr24, &s_fr24);

  loadProviderUsage(prefs, kAlUsedKeys, kAlUsedCalls, &s_airlabs);
  loadProviderUsage(prefs, kFaSpentKeys, kFaSpentMicro, &s_flightaware);
  loadProviderUsage(prefs, kFr24SpentKeys, kFr24SpentMicro, &s_fr24);

  s_use_airlabs = prefs.getBool(kUseAirLabs, false);
  s_use_flightaware = prefs.getBool(kUseFlightAware, false);
  s_use_fr24 = prefs.getBool(kUseFr24, false);

  if (prefs.isKey(kWeather)) {
    copyBlob(s_weather_key, prefs.getString(kWeather).c_str(), sizeof(s_weather_key));
  } else {
    s_weather_key[0] = '\0';
  }
  s_use_weather = prefs.getBool(kUseWeather, true);
  s_use_adsbdb = prefs.getBool(kUseAdsbDb, true);
  s_use_openmeteo = prefs.getBool(kUseOpenMeteo, true);

  s_al_max_calls = prefs.getUInt(kAlMaxCalls, config::kDefaultAirLabsMaxCalls);
  s_fa_budget_micro = prefs.getUInt(kFaBudgetMicro, config::kDefaultFlightAwareBudgetUsdMicro);
  s_fa_cost_micro = prefs.getUInt(kFaCostMicro, config::kDefaultFlightAwareCostUsdMicro);
  s_fr24_budget_micro = prefs.getUInt(kFr24BudgetMicro, config::kDefaultFr24BudgetUsdMicro);
  s_fr24_cost_micro = prefs.getUInt(kFr24CostMicro, config::kDefaultFr24CostUsdMicro);
  s_usage_reset_ym = prefs.getUInt(kUsageResetYm, 0);
  prefs.end();

  maybeResetMonthlyUsage();
}

bool saveFromForm(const char* airlabs, const char* flightaware, const char* fr24) {
  bool any = false;
  if (saveIfNonEmpty(airlabs, kAirLabs)) {
    applyProviderBlob(&s_airlabs, airlabs);
    any = true;
  }
  if (saveIfNonEmpty(flightaware, kFlightAware)) {
    applyProviderBlob(&s_flightaware, flightaware);
    any = true;
  }
  if (saveIfNonEmpty(fr24, kFr24)) {
    applyProviderBlob(&s_fr24, fr24);
    any = true;
  }
  if (any) {
    persistUsageCounters();
  }
  return any;
}

void saveEnabledFromForm(const char* use_airlabs, const char* use_flightaware,
                         const char* use_fr24) {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  if (use_airlabs != nullptr) {
    s_use_airlabs = portalCheckboxChecked(use_airlabs);
    prefs.putBool(kUseAirLabs, s_use_airlabs);
  }
  if (use_flightaware != nullptr) {
    s_use_flightaware = portalCheckboxChecked(use_flightaware);
    prefs.putBool(kUseFlightAware, s_use_flightaware);
  }
  if (use_fr24 != nullptr) {
    s_use_fr24 = portalCheckboxChecked(use_fr24);
    prefs.putBool(kUseFr24, s_use_fr24);
  }
  prefs.end();
}

void saveLimitsFromForm(const char* airlabs_max_calls, const char* flightaware_max_usd,
                        const char* flightaware_cost_usd, const char* fr24_max_usd,
                        const char* fr24_cost_usd) {
  bool changed = false;

  if (airlabs_max_calls != nullptr && airlabs_max_calls[0] != '\0') {
    uint32_t max_calls = 0;
    if (parseU32(airlabs_max_calls, &max_calls)) {
      s_al_max_calls = max_calls;
      changed = true;
    }
  }

  if (flightaware_max_usd != nullptr && flightaware_max_usd[0] != '\0') {
    uint32_t micro = 0;
    if (parseUsdToMicro(flightaware_max_usd, &micro)) {
      s_fa_budget_micro = micro;
      changed = true;
    }
  }
  if (flightaware_cost_usd != nullptr && flightaware_cost_usd[0] != '\0') {
    uint32_t micro = 0;
    if (parseUsdToMicro(flightaware_cost_usd, &micro)) {
      s_fa_cost_micro = micro;
      changed = true;
    }
  }

  if (fr24_max_usd != nullptr && fr24_max_usd[0] != '\0') {
    uint32_t micro = 0;
    if (parseUsdToMicro(fr24_max_usd, &micro)) {
      s_fr24_budget_micro = micro;
      changed = true;
    }
  }
  if (fr24_cost_usd != nullptr && fr24_cost_usd[0] != '\0') {
    uint32_t micro = 0;
    if (parseUsdToMicro(fr24_cost_usd, &micro)) {
      s_fr24_cost_micro = micro;
      changed = true;
    }
  }

  if (changed) {
    persistLimitConfig();
    s_warned_al_limit = false;
    s_warned_fa_limit = false;
    s_warned_fr24_limit = false;
  }
}

bool hasAirLabs() { return s_airlabs.key_count > 0; }
bool hasFlightAware() { return s_flightaware.key_count > 0; }
bool hasFr24() { return s_fr24.key_count > 0; }

bool useAirLabs() { return s_use_airlabs; }
bool useFlightAware() { return s_use_flightaware; }
bool useFr24() { return s_use_fr24; }

size_t airLabsKeyCount() { return s_airlabs.key_count; }
size_t flightAwareKeyCount() { return s_flightaware.key_count; }
size_t fr24KeyCount() { return s_fr24.key_count; }

const char* airLabsKeyAt(size_t index) {
  if (index >= s_airlabs.key_count) {
    return "";
  }
  return s_airlabs.keys[index];
}

const char* flightAwareKeyAt(size_t index) {
  if (index >= s_flightaware.key_count) {
    return "";
  }
  return s_flightaware.keys[index];
}

const char* fr24KeyAt(size_t index) {
  if (index >= s_fr24.key_count) {
    return "";
  }
  return s_fr24.keys[index];
}

bool canUseAirLabsAt(size_t index) { return canUseAirLabsAtImpl(index); }

bool canUseFlightAwareAt(size_t index) { return canUseFlightAwareAtImpl(index); }

bool canUseFr24At(size_t index) { return canUseFr24AtImpl(index); }

bool canUseAirLabs() {
  for (size_t i = 0; i < s_airlabs.key_count; ++i) {
    if (canUseAirLabsAtImpl(i)) {
      return true;
    }
  }
  if (s_airlabs.key_count > 0) {
    warnLimitOnce(&s_warned_al_limit, "AirLabs",
                  "all keys at monthly call cap — skipping until next month");
  }
  return false;
}

bool canUseFlightAware() {
  for (size_t i = 0; i < s_flightaware.key_count; ++i) {
    if (canUseFlightAwareAtImpl(i)) {
      return true;
    }
  }
  if (s_flightaware.key_count > 0) {
    warnLimitOnce(&s_warned_fa_limit, "FlightAware",
                  "all keys at monthly budget cap — skipping until next month");
  }
  return false;
}

bool canUseFr24() {
  for (size_t i = 0; i < s_fr24.key_count; ++i) {
    if (canUseFr24AtImpl(i)) {
      return true;
    }
  }
  if (s_fr24.key_count > 0) {
    warnLimitOnce(&s_warned_fr24_limit, "FlightRadar24",
                  "all keys at monthly budget cap — skipping until next month");
  }
  return false;
}

void recordAirLabsCallAt(size_t index) {
  if (index >= s_airlabs.key_count) {
    return;
  }
  maybeResetMonthlyUsage();
  if (s_airlabs.used[index] != UINT32_MAX) {
    ++s_airlabs.used[index];
  }
  persistUsageCounters();
}

void exhaustAirLabsKeyAt(size_t index) {
  if (index >= s_airlabs.key_count) {
    return;
  }
  maybeResetMonthlyUsage();
  if (s_al_max_calls == 0) {
    // Local cap is unlimited, but the provider still said we've hit the monthly cap.
    s_airlabs.used[index] = UINT32_MAX;
  } else {
    s_airlabs.used[index] = s_al_max_calls;
  }
  persistUsageCounters();
  Serial.printf("AirLabs key %u: month_limit_exceeded (exhausted key)\n",
                static_cast<unsigned>(index + 1));
}

void recordFlightAwareCallAt(size_t index) {
  if (index >= s_flightaware.key_count) {
    return;
  }
  maybeResetMonthlyUsage();
  s_flightaware.used[index] += s_fa_cost_micro;
  persistUsageCounters();
}

void recordFr24CallAt(size_t index) {
  if (index >= s_fr24.key_count) {
    return;
  }
  maybeResetMonthlyUsage();
  s_fr24.used[index] += s_fr24_cost_micro;
  persistUsageCounters();
}

uint32_t airLabsMaxCalls() { return s_al_max_calls; }
uint32_t airLabsCallsUsed() {
  maybeResetMonthlyUsage();
  return sumUsage(s_airlabs.used, s_airlabs.key_count);
}
uint32_t flightAwareBudgetUsdMicro() { return s_fa_budget_micro; }
uint32_t flightAwareCostUsdMicro() { return s_fa_cost_micro; }
uint32_t flightAwareSpentUsdMicro() {
  maybeResetMonthlyUsage();
  return sumUsage(s_flightaware.used, s_flightaware.key_count);
}
uint32_t fr24BudgetUsdMicro() { return s_fr24_budget_micro; }
uint32_t fr24CostUsdMicro() { return s_fr24_cost_micro; }
uint32_t fr24SpentUsdMicro() {
  maybeResetMonthlyUsage();
  return sumUsage(s_fr24.used, s_fr24.key_count);
}

bool hasWeather() { return s_weather_key[0] != '\0'; }

const char* weatherKey() { return s_weather_key; }

bool saveWeatherKeyFromForm(const char* weather) {
  if (!saveIfNonEmpty(weather, kWeather)) {
    return false;
  }
  copyBlob(s_weather_key, weather, sizeof(s_weather_key));
  return true;
}

void saveWeatherEnabledFromForm(const char* use_weather) {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  s_use_weather = portalCheckboxChecked(use_weather);
  prefs.putBool(kUseWeather, s_use_weather);
  prefs.end();
}

bool useWeather() { return s_use_weather; }

bool canUseWeather() { return s_use_weather && s_weather_key[0] != '\0'; }

void saveAdsbDbEnabledFromForm(const char* use_adsbdb) {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  s_use_adsbdb = portalCheckboxChecked(use_adsbdb);
  prefs.putBool(kUseAdsbDb, s_use_adsbdb);
  prefs.end();
}

bool useAdsbDb() { return s_use_adsbdb; }

void saveOpenMeteoEnabledFromForm(const char* use_openmeteo) {
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return;
  }
  s_use_openmeteo = portalCheckboxChecked(use_openmeteo);
  prefs.putBool(kUseOpenMeteo, s_use_openmeteo);
  prefs.end();
}

bool useOpenMeteo() { return s_use_openmeteo; }

void maskedWeather(char* out, size_t len) { maskedPreviewSingle(s_weather_key, out, len); }

void maskedAirLabs(char* out, size_t len) { maskedPreviewProvider(s_airlabs, out, len); }
void maskedFlightAware(char* out, size_t len) { maskedPreviewProvider(s_flightaware, out, len); }
void maskedFr24(char* out, size_t len) { maskedPreviewProvider(s_fr24, out, len); }

}  // namespace services::apikeys
