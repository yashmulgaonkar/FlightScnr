#pragma once

namespace services::tzlookup {

/** Load cached timezone from NVS. Call once at boot before Wi-Fi. */
void bootLoad();

/** Ensure the shared HTTPS worker exists (idempotent). */
void init();

/** Queue a timezone lookup for the current map center when auto mode is on.
 *  No-op when manual timezone is selected or a fetch is already pending. */
void requestForMapCenter();

/** Call after radar center coordinates change. */
void notifyLocationChanged();

/** Retry/backoff handling; call from the main loop while Wi-Fi is up. */
void tick();

bool lookupInProgress();
/** True after a successful lookup for the current map center. */
bool hasResolvedTimezone();

}  // namespace services::tzlookup
