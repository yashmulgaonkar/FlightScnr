#pragma once

#include "services/adsb_client.h"

namespace services::alert {

void bootLoad();

/** Check aircraft list for new alert-worthy entries; fires buzzer if matched. */
void checkNewAircraft(const services::adsb::Aircraft* list, size_t count);

/** True if this aircraft should be drawn with alert highlight on radar. */
bool isHighlighted(const services::adsb::Aircraft& ac);

/** Pulse phase for flashing alert icons (true = bright, false = dim). Cycles ~500ms. */
bool pulsePhase();

bool militaryAlertEnabled();
bool emergencyAlertEnabled();
bool hideNonAlertedEnabled();
bool watchCallsignsEnabled();
size_t watchCallsignCount();

/** Comma-separated watch list for settings form (e.g. ACA739,UAL123). */
void watchCallsignsFormatted(char* out, size_t out_len);

void saveFromForm(const char* mil_checkbox, const char* emrg_checkbox,
                  const char* hide_checkbox, const char* watch_callsigns,
                  bool update_watch_callsigns);

}  // namespace services::alert
