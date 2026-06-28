#pragma once

#include <cstddef>
#include <cstdint>

namespace services::apikeys {

constexpr size_t kMaxKeysPerProvider = 8;
constexpr size_t kMaxSingleKeyLen = 80;

void load();
bool saveFromForm(const char* airlabs, const char* flightaware, const char* fr24);
/** Portal/web checkboxes ("T" = enabled). Pass nullptr to leave flags unchanged. */
void saveEnabledFromForm(const char* use_airlabs, const char* use_flightaware,
                         const char* use_fr24);

/**
 * Monthly API limits from web form. Pass nullptr to leave a field unchanged.
 * Limits apply per key (each comma-separated key gets its own monthly cap).
 */
void saveLimitsFromForm(const char* airlabs_max_calls, const char* flightaware_max_usd,
                        const char* flightaware_cost_usd, const char* fr24_max_usd,
                        const char* fr24_cost_usd);

bool hasAirLabs();
bool hasFlightAware();
bool hasFr24();
bool hasWeather();

/** Tomorrow.io weather key (single key). Returns "" when unset. */
const char* weatherKey();
/** Persist a new weather key from the web form; ignores null/empty. */
bool saveWeatherKeyFromForm(const char* weather);
/** Portal/web checkbox ("T" = enabled). */
void saveWeatherEnabledFromForm(const char* use_weather);

bool useWeather();
/** True when Tomorrow.io is enabled and a key is configured. */
bool canUseWeather();
void maskedWeather(char* out, size_t len);

bool useAirLabs();
bool useFlightAware();
bool useFr24();

/** True when at least one key for the provider can still be used this month. */
bool canUseAirLabs();
bool canUseFlightAware();
bool canUseFr24();

size_t airLabsKeyCount();
size_t flightAwareKeyCount();
size_t fr24KeyCount();

const char* airLabsKeyAt(size_t index);
const char* flightAwareKeyAt(size_t index);
const char* fr24KeyAt(size_t index);

bool canUseAirLabsAt(size_t index);
bool canUseFlightAwareAt(size_t index);
bool canUseFr24At(size_t index);

void recordAirLabsCallAt(size_t index);
void recordFlightAwareCallAt(size_t index);
void recordFr24CallAt(size_t index);

/** AirLabs can return `month_limit_exceeded` even if the local cap is 0 (unlimited).
 *  Mark that specific key as exhausted so we stop trying it until monthly reset. */
void exhaustAirLabsKeyAt(size_t index);

uint32_t airLabsMaxCalls();
uint32_t airLabsCallsUsed();
uint32_t flightAwareBudgetUsdMicro();
uint32_t flightAwareCostUsdMicro();
uint32_t flightAwareSpentUsdMicro();
uint32_t fr24BudgetUsdMicro();
uint32_t fr24CostUsdMicro();
uint32_t fr24SpentUsdMicro();

/** Masked preview for settings page (e.g. "3 keys" or "••••abcd"). */
void maskedAirLabs(char* out, size_t len);
void maskedFlightAware(char* out, size_t len);
void maskedFr24(char* out, size_t len);

}  // namespace services::apikeys
