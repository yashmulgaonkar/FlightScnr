#pragma once

#include <cstddef>
#include <cstdint>

namespace services::clock {

void bootLoad();

/** Apply persisted timezone and start NTP (call when Wi-Fi is up). */
void startNtp();

/** Local-time minute bucket for display refresh; UINT32_MAX while unsynced. */
uint32_t localMinuteStamp();

/** Current offset from UTC in seconds (DST-aware when auto timezone is active). */
int32_t timezoneOffsetSec();
/** Days since the Unix epoch in local civil time, or -1 when unsynced. */
int32_t localDayIndex();

bool useAutoTimezone();
void setAutoTimezone(bool enabled);
/** Web-form checkbox ("T" = auto from radar center). */
void saveAutoTimezoneFromForm(const char* auto_timezone);

/** Manual UTC offset adjustment; disables auto timezone. */
void stepTimezoneHours(int8_t delta);

bool use24Hour();
void toggleHourFormat();

/** Apply a resolved IANA/POSIX timezone for the given coordinates. */
bool applyAutoTimezone(const char* iana, const char* posix, double lat, double lon);
/** True when auto mode has a cached zone for the current map center. */
bool autoTimezoneMatchesCoords(double lat, double lon);
/** IANA zone name for display; empty when unknown. */
const char* timezoneIanaName();

void formatTimezoneLabel(char* out, size_t len);
void formatTimeOfDay(char* out, size_t len);
/** Format a UTC epoch (seconds) as local "H:MM[ AM/PM]" using the active timezone
 *  and 12/24-hour preference. Writes "--:--" when the epoch is invalid. */
void formatClockFromEpoch(int64_t utc_epoch_sec, char* out, size_t len);
void formatDateLine(char* out, size_t len);
/** "AM"/"PM" when 12-hour mode; empty string in 24-hour mode. */
void formatAmPm(char* out, size_t len);

}  // namespace services::clock
