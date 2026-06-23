/**
 * FlightScnr — WiFi setup, then radar UI on the T-Encoder Pro AMOLED display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/task.h>

#include "config.h"
#include "services/route_cache_store.h"
#include "hardware/buzzer.h"
#include "hardware/display.h"
#include "hardware/plane_gfx.h"
#include "hardware/display_brightness.h"
#include "hardware/input.h"
#include "hardware/panel.h"
#include "services/adsb_client.h"
#include "services/https_lock.h"
#include "services/https_heap.h"
#include "services/clock_time.h"
#include "services/map_center.h"
#include "services/route_lookup.h"
#include "services/settings_web.h"
#include "services/settings_apply.h"
#include "services/wifi_setup.h"
#include "ui/clock_screen.h"
#include "ui/clock_settings_screen.h"
#include "ui/details_screen.h"
#include "ui/display_prefs.h"
#include "ui/flight_detail_screen.h"
#include "ui/info_screen.h"
#include "ui/radar_accent.h"
#include "ui/radar_display.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"

namespace {

enum class AppScreen : uint8_t {
  Radar,
  FlightDetail,
  Settings,
  Details,
  Clock,
  ClockSettings,
};

AppScreen g_screen = AppScreen::Radar;
bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_adsb_ssl_recover_ms = 0;
unsigned long g_last_tls_proactive_refresh_ms = 0;
unsigned long g_last_radar_frame_ms = 0;
unsigned long g_secondary_activity_ms = 0;
unsigned long g_boot_details_until_ms = 0;
uint32_t g_last_clock_minute_stamp = UINT32_MAX;
unsigned long g_last_heap_log_ms = 0;
unsigned long g_loop_max_ms = 0;
bool g_radar_full_draw_pending = false;
bool g_flight_detail_draw_pending = false;

const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "poweron";
    case ESP_RST_SW:
      return "sw";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "int_wdt";
    case ESP_RST_TASK_WDT:
      return "task_wdt";
    case ESP_RST_WDT:
      return "wdt";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_DEEPSLEEP:
      return "deepsleep";
    default:
      return "other";
  }
}

const char* screenName(AppScreen screen) {
  switch (screen) {
    case AppScreen::Radar:
      return "radar";
    case AppScreen::FlightDetail:
      return "detail";
    case AppScreen::Settings:
      return "settings";
    case AppScreen::Details:
      return "details";
    case AppScreen::Clock:
      return "clock";
    case AppScreen::ClockSettings:
      return "clock_set";
    default:
      return "?";
  }
}

void logDiagLine(const char* tag) {
  const unsigned long uptime_sec = millis() / 1000UL;
  const unsigned hours = static_cast<unsigned>(uptime_sec / 3600UL);
  const unsigned mins = static_cast<unsigned>((uptime_sec % 3600UL) / 60UL);

  const bool wifi_up = WiFi.status() == WL_CONNECTED;
  const int rssi = wifi_up ? WiFi.RSSI() : 0;

  char adsb_ok[16];
  const uint32_t adsb_age_ms = services::adsb::lastFetchOkAgeMs();
  if (adsb_age_ms == UINT32_MAX) {
    strncpy(adsb_ok, "never", sizeof(adsb_ok) - 1);
  } else {
    snprintf(adsb_ok, sizeof(adsb_ok), "%lus", adsb_age_ms / 1000UL);
  }
  adsb_ok[sizeof(adsb_ok) - 1] = '\0';

  size_t lfs_used = 0;
  size_t lfs_total = 0;
  const bool lfs_ok = services::route_cache::flashUsage(&lfs_used, &lfs_total);
  char lfs_buf[24];
  if (lfs_ok && lfs_total > 0) {
    snprintf(lfs_buf, sizeof(lfs_buf), "%u/%uKB",
             static_cast<unsigned>(lfs_used / 1024U),
             static_cast<unsigned>(lfs_total / 1024U));
  } else {
    strncpy(lfs_buf, "n/a", sizeof(lfs_buf) - 1);
    lfs_buf[sizeof(lfs_buf) - 1] = '\0';
  }

  const uint32_t loop_stack =
      static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));

  Serial.printf(
      "[diag] %s uptime=%uh%02um free=%u min=%u max_blk=%u psram=%u wifi=%s rssi=%d "
      "screen=%s ac=%u adsb_ok=%s adsb_busy=%u adsb_fail=%u stk_loop=%u stk_adsb=%u "
      "loop_max=%lums lfs=%s\n",
      tag, hours, mins, ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM), wifi_up ? "up" : "down", rssi,
      screenName(g_screen), static_cast<unsigned>(services::adsb::aircraftCount()), adsb_ok,
      services::adsb::fetchInProgress() ? 1U : 0U, services::adsb::fetchFailStreak(),
      loop_stack, services::adsb::fetchTaskStackFreeBytes(), g_loop_max_ms, lfs_buf);

  g_loop_max_ms = 0;
}

void tickDiagLog() {
  const unsigned long now = millis();
  if (g_last_heap_log_ms != 0 && now - g_last_heap_log_ms < config::kDiagLogIntervalMs) {
    return;
  }
  g_last_heap_log_ms = now;
  logDiagLine("tick");
}

bool bootDetailsActive() { return g_boot_details_until_ms != 0; }

bool adsbFetchScreenActive() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (g_screen == AppScreen::Radar && g_radar_visible) {
    return true;
  }
  if (g_screen == AppScreen::FlightDetail) {
    return true;
  }
  return bootDetailsActive();
}

bool adsbDnsReady(unsigned long now) {
  static unsigned long s_next_probe_ms = 0;
  static bool s_resolved = false;

  if (WiFi.status() != WL_CONNECTED) {
    s_resolved = false;
    s_next_probe_ms = 0;
    return false;
  }
  if (s_resolved) {
    return true;
  }
  if (s_next_probe_ms != 0 && now < s_next_probe_ms) {
    return false;
  }

  IPAddress ip;
  s_resolved = WiFi.hostByName("opendata.adsb.fi", ip);
  s_next_probe_ms = now + (s_resolved ? 60000UL : 1500UL);
  if (!s_resolved && config::kSerialTraceDebug) {
    Serial.println("[fetch] defer: DNS not ready for opendata.adsb.fi");
  }
  return s_resolved;
}

void noteSecondaryActivity() {
  if (g_screen != AppScreen::Radar) {
    g_secondary_activity_ms = millis();
  }
}

void releaseHttpsPressureMemory() {
  ui::flightDetailReleaseSprite();
  services::route::cancelDetailEnrichment();
}

bool tlsBlocksPanel() {
  return services::adsb::fetchInProgress() || services::https::busy();
}

bool deferAdsbForRouteWorker() {
  return services::route::detailWorkerBusy();
}

void completeDeferredRadarDraw() {
  if (!g_radar_full_draw_pending || g_screen != AppScreen::Radar || !g_radar_visible) {
    return;
  }
  if (tlsBlocksPanel()) {
    return;
  }
  PanelSession panel(tft);
  ui::radarDisplayDraw();
  g_radar_full_draw_pending = false;
  if (config::kSerialTraceDebug) {
    Serial.println("[radar] deferred full draw complete");
  }
}

void showRadar() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    g_radar_full_draw_pending = false;
    return;
  }
  ui::flightDetailReleaseSprite();
  g_radar_visible = true;
  g_last_radar_frame_ms = millis();
  g_last_adsb_fetch_ms = 0;

  if (tlsBlocksPanel()) {
    g_radar_full_draw_pending = true;
    if (config::kSerialTraceDebug) {
      Serial.println("[radar] defer full draw (tls busy)");
    }
    return;
  }

  PanelSession panel(tft);
  ui::radarDisplayDraw();
  g_radar_full_draw_pending = false;
}

void showFlightDetail() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (tlsBlocksPanel()) {
    g_flight_detail_draw_pending = true;
    if (config::kSerialTraceDebug) {
      Serial.println("[detail] defer draw (tls busy)");
    }
    g_radar_visible = false;
    return;
  }
  g_flight_detail_draw_pending = false;
  PanelSession panel(tft);
  ui::flightDetailDraw();
  g_radar_visible = false;
}

void requestFlightDetailRouteEnrich(const bool immediate) {
  const char* callsign = ui::flightDetailSelectedCallsign();
  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] enrich request %s immediate=%d\n",
                  callsign != nullptr ? callsign : "(none)",
                  immediate ? 1 : 0);
  }
  if (callsign != nullptr) {
    services::route::onFlightDetailSelected(callsign, immediate);
  }
}

void tickFlightDetailRouteEnrich() {
  static bool s_detail_enrich_in_flight = false;
  if (g_screen != AppScreen::FlightDetail) {
    s_detail_enrich_in_flight = false;
    return;
  }
  const unsigned long now = millis();
  services::route::tickDetailEnrichDebounce(now);
  services::route::tickDetailWorkerWatchdog(now);

  if (services::route::detailEnrichmentReady() && services::route::detailEnrichmentConsume()) {
    s_detail_enrich_in_flight = false;
    if (config::kSerialTraceDebug) {
      const char* cs = ui::flightDetailSelectedCallsign();
      Serial.printf("[detail] enrich ready -> redraw %s\n",
                    cs != nullptr ? cs : "(none)");
    }
    showFlightDetail();
    return;
  }

  const char* callsign = ui::flightDetailSelectedCallsign();
  const bool in_flight =
      callsign != nullptr && services::route::detailEnrichmentInFlight(callsign);
  if (in_flight != s_detail_enrich_in_flight) {
    s_detail_enrich_in_flight = in_flight;
    // Opening flight detail already drew the screen; completion redraws via consume().
  }

  ui::flightDetailTick(now);
}

void showSettings() {
  ui::flightDetailReleaseSprite();
  ui::infoScreenDraw();
  g_radar_visible = false;
}

void showClock() {
  ui::flightDetailReleaseSprite();
  ui::clockScreenDraw();
  g_radar_visible = false;
  g_last_clock_minute_stamp = services::clock::localMinuteStamp();
}

void showClockSettings() {
  ui::flightDetailReleaseSprite();
  ui::clockSettingsScreenDraw();
  g_radar_visible = false;
}

void showDetails(bool boot_splash = false) {
  ui::flightDetailReleaseSprite();
  ui::detailsScreenDraw(boot_splash);
  g_radar_visible = false;
}

void applySettingsLive() {
  services::route::cancelDetailEnrichment();
  ui::flightDetailReleaseSprite();
  hardware::displayApplyBrightness();
  g_last_adsb_fetch_ms = 0;

  switch (g_screen) {
    case AppScreen::Radar:
      if (WiFi.status() == WL_CONNECTED) {
        showRadar();
      }
      break;
    case AppScreen::FlightDetail:
      if (WiFi.status() == WL_CONNECTED) {
        showFlightDetail();
        requestFlightDetailRouteEnrich(true);
      }
      break;
    case AppScreen::Settings:
      showSettings();
      break;
    case AppScreen::Details:
      showDetails();
      break;
    case AppScreen::Clock:
      showClock();
      break;
    case AppScreen::ClockSettings:
      showClockSettings();
      break;
  }
  Serial.println("[settings] applied live");
}

void returnToRadar(bool from_idle_timeout = false) {
  if (g_screen == AppScreen::FlightDetail) {
    releaseHttpsPressureMemory();
  } else {
    services::route::cancelDetailEnrichment();
  }
  if (from_idle_timeout) {
    ui::infoScreenResetToMain();
    ui::clockSettingsResetFocus();
    inputDiscardPendingInteractions();
  }
  g_screen = AppScreen::Radar;
  g_radar_visible = false;
  if (WiFi.status() == WL_CONNECTED) {
    showRadar();
  }
  Serial.println(from_idle_timeout ? "Screen: radar (timeout)"
                                   : "Screen: radar");
}

void returnToClockFromIdleTimeout() {
  ui::clockSettingsResetFocus();
  inputDiscardPendingInteractions();
  g_screen = AppScreen::Clock;
  showClock();
  Serial.println("Screen: clock (timeout)");
}

void openSettingsFromRadar() {
  ui::infoScreenResetToMain();
  g_screen = AppScreen::Settings;
  noteSecondaryActivity();
  showSettings();
  Serial.println("Screen: settings (1/3)");
}

void openClockFromRadar() {
  g_screen = AppScreen::Clock;
  showClock();
  Serial.println("Screen: clock");
}

void openDetailsFromRadar() {
  g_screen = AppScreen::Details;
  noteSecondaryActivity();
  showDetails();
  Serial.println("Screen: details");
}

void openClockSettingsFromClock() {
  ui::clockSettingsResetFocus();
  g_screen = AppScreen::ClockSettings;
  noteSecondaryActivity();
  showClockSettings();
  Serial.println("Screen: clock settings");
}

void openFlightDetailFromRadar(int16_t tap_x, int16_t tap_y, bool from_screen_tap) {
  if (from_screen_tap) {
    ui::flightDetailSelectAtScreen(tap_x, tap_y);
  } else {
    ui::flightDetailSelectClosest();
  }
  g_screen = AppScreen::FlightDetail;
  noteSecondaryActivity();
  inputDiscardPendingInteractions();
  requestFlightDetailRouteEnrich(true);
  showFlightDetail();
  Serial.println("Screen: flight detail");
}

void onFlightDetailStep(int8_t delta) {
  if (g_screen != AppScreen::FlightDetail || delta == 0) {
    return;
  }
  noteSecondaryActivity();
  if (config::kSerialTraceDebug) {
    Serial.printf("[detail] encoder step %+d\n", static_cast<int>(delta));
  }
  if (ui::flightDetailCycle(delta)) {
    requestFlightDetailRouteEnrich(false);
    showFlightDetail();
  }
}

void onRangeStep(int8_t delta) {
  if (g_screen != AppScreen::Radar || delta == 0) {
    return;
  }

  if (delta > 0) {
    ui::radar::scaleIncrease();
  } else {
    ui::radar::scaleDecrease();
  }
  char range_label[12];
  ui::radar::formatActiveScaleTag(range_label, sizeof(range_label));
  Serial.printf("Scale: %s (coverage ~%.0f km)\n", range_label,
                ui::radar::scaleActive().coverage_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleNavigation() {
  if (bootDetailsActive()) {
    inputConsumeSwipe();
    return;
  }

  const SwipeGesture swipe = inputConsumeSwipe();
  if (swipe != SwipeNone && g_screen != AppScreen::Radar &&
      g_screen != AppScreen::Clock) {
    noteSecondaryActivity();
  }
  if (swipe == SwipeDown && g_screen == AppScreen::Radar) {
    openClockFromRadar();
  } else if (swipe == SwipeUp && g_screen == AppScreen::Radar) {
    openDetailsFromRadar();
  } else if (swipe == SwipeDown && g_screen == AppScreen::Details) {
    returnToRadar(false);
  } else if (swipe == SwipeUp && g_screen == AppScreen::Clock) {
    returnToRadar(false);
  } else if (swipe == SwipeLeft && g_screen == AppScreen::Clock) {
    openClockSettingsFromClock();
  } else if (swipe == SwipeRight && g_screen == AppScreen::ClockSettings) {
    g_screen = AppScreen::Clock;
    showClock();
    Serial.println("Screen: clock");
  } else if (swipe == SwipeLeft && g_screen == AppScreen::Radar) {
    openSettingsFromRadar();
  } else if (swipe == SwipeLeft && g_screen == AppScreen::Settings &&
             ui::infoScreenPage() == ui::InfoSettingsPage::Main) {
    ui::infoScreenSetPage(ui::InfoSettingsPage::Display);
    ui::infoScreenResetDisplayFocus();
    showSettings();
    Serial.println("Screen: settings (2/3)");
  } else if (swipe == SwipeLeft && g_screen == AppScreen::Settings &&
             ui::infoScreenPage() == ui::InfoSettingsPage::Display) {
    ui::infoScreenSetPage(ui::InfoSettingsPage::Colors);
    showSettings();
    Serial.println("Screen: settings (3/3)");
  } else if (swipe == SwipeRight && g_screen == AppScreen::FlightDetail) {
    returnToRadar(false);
  } else if (swipe == SwipeRight && g_screen == AppScreen::Settings &&
             ui::infoScreenPage() == ui::InfoSettingsPage::Colors) {
    ui::infoScreenSetPage(ui::InfoSettingsPage::Display);
    ui::infoScreenResetDisplayFocus();
    showSettings();
    Serial.println("Screen: settings (2/3)");
  } else if (swipe == SwipeRight && g_screen == AppScreen::Settings &&
             ui::infoScreenPage() == ui::InfoSettingsPage::Display) {
    ui::infoScreenSetPage(ui::InfoSettingsPage::Main);
    showSettings();
    Serial.println("Screen: settings (1/3)");
  } else if (swipe == SwipeRight && g_screen == AppScreen::Settings) {
    returnToRadar(false);
  }
}

void tickBootDetailsSplash() {
  if (g_boot_details_until_ms == 0) {
    return;
  }
  if (millis() < g_boot_details_until_ms) {
    return;
  }

  g_boot_details_until_ms = 0;
  inputDiscardPendingInteractions();
  returnToRadar(false);
  Serial.println("Screen: radar (boot splash done)");
}

void tickSecondaryScreenTimeout() {
  if (bootDetailsActive()) {
    return;
  }
  if (g_screen == AppScreen::Radar || g_screen == AppScreen::Clock) {
    return;
  }
  if (g_screen == AppScreen::FlightDetail) {
    const unsigned long timeout_ms = ui::displayPrefsFlightDetailTimeoutMs();
    if (timeout_ms == 0) {
      return;
    }
    if (millis() - g_secondary_activity_ms >= timeout_ms) {
      returnToRadar(true);
    }
    return;
  }
  if (millis() - g_secondary_activity_ms >= config::kSecondaryScreenTimeoutMs) {
    if (g_screen == AppScreen::ClockSettings) {
      returnToClockFromIdleTimeout();
    } else {
      returnToRadar(true);
    }
  }
}

void tickClockDisplay() {
  if (g_screen != AppScreen::Clock) {
    return;
  }
  const uint32_t stamp = services::clock::localMinuteStamp();
  if (stamp == g_last_clock_minute_stamp) {
    return;
  }
  g_last_clock_minute_stamp = stamp;
  ui::clockScreenDraw();
}

void handleInput() {
  inputPoll();
  inputPollLongPress();
  handleNavigation();

  if (g_screen == AppScreen::Radar) {
    int16_t tx = 0;
    int16_t ty = 0;
    if (inputConsumeScreenTap(&tx, &ty)) {
      openFlightDetailFromRadar(tx, ty, true);
      return;
    }
    if (inputConsumeKnobTap()) {
      openFlightDetailFromRadar(0, 0, false);
      return;
    }
    const int8_t enc = inputConsumeEncoderDelta();
    if (enc != 0) {
      onRangeStep(enc);
      hardware::buzzerClick();
    }
    return;
  }

  if (g_screen == AppScreen::FlightDetail) {
    const int8_t enc = inputConsumeEncoderDelta();
    if (enc != 0) {
      noteSecondaryActivity();
      onFlightDetailStep(enc);
      hardware::buzzerClick();
    }
    return;
  }

  if (g_screen == AppScreen::Settings) {
    if (ui::infoScreenPage() == ui::InfoSettingsPage::Display) {
      if (inputConsumeKnobPress()) {
        noteSecondaryActivity();
        hardware::buzzerClick();
      }
      if (inputConsumeKnobTap()) {
        noteSecondaryActivity();
        ui::infoScreenCycleDisplayFocus();
        showSettings();
        return;
      }
    } else if (ui::infoScreenPage() == ui::InfoSettingsPage::Colors) {
      if (inputConsumeKnobPress()) {
        noteSecondaryActivity();
        hardware::buzzerClick();
      }
    }
    const int8_t enc = inputConsumeEncoderDelta();
    if (enc != 0) {
      noteSecondaryActivity();
      ui::infoScreenHandleKnob(enc);
      hardware::buzzerClick();
    }
    return;
  }

  if (g_screen == AppScreen::ClockSettings) {
    if (inputConsumeKnobPress()) {
      noteSecondaryActivity();
      hardware::buzzerClick();
    }
    if (inputConsumeKnobTap()) {
      noteSecondaryActivity();
      ui::clockSettingsCycleFocus();
      showClockSettings();
      return;
    }
    const int8_t enc = inputConsumeEncoderDelta();
    if (enc != 0) {
      noteSecondaryActivity();
      ui::clockSettingsHandleKnob(enc);
      hardware::buzzerClick();
    }
    return;
  }
}

void tickRadarAnimation() {
  if (g_radar_full_draw_pending) {
    return;
  }
  if (g_screen != AppScreen::Radar || !g_radar_visible) {
    return;
  }

  const unsigned long now = millis();
  if (now - g_last_radar_frame_ms < ui::radar::kSweepFrameMs) {
    return;
  }
  g_last_radar_frame_ms = now;
  PanelSession panel(tft);
  ui::radarDisplayRefreshSweep();
}

bool canRecycleWifiForTls(unsigned long now) {
  return WiFi.status() == WL_CONNECTED && !services::adsb::fetchInProgress() &&
         !services::https::busy() &&
         now - g_last_adsb_ssl_recover_ms >= config::kTlsRecoverCooldownMs;
}

void recycleWifiForTls(unsigned long now, const char* reason) {
  releaseHttpsPressureMemory();
  g_last_adsb_fetch_ms = 0;
  g_last_adsb_ssl_recover_ms = now;
  g_last_tls_proactive_refresh_ms = now;
  g_last_adsb_fetch_ms = now;
  services::adsb::fetchResetFailStreak();
  Serial.printf("[adsb] %s — recycling WiFi\n", reason);
  wifiReconnect();
}

void tryTlsWifiRefresh(unsigned long now) {
  if (!canRecycleWifiForTls(now)) {
    return;
  }

  if (config::kTlsProactiveRefreshMs > 0 &&
      now - g_last_tls_proactive_refresh_ms >= config::kTlsProactiveRefreshMs) {
    recycleWifiForTls(now, "proactive TLS refresh");
    return;
  }

  if (services::adsb::fetchFailStreak() >= config::kTlsRecoverFailStreak &&
      services::adsb::lastFetchOkAgeMs() > config::kTlsRecoverStaleMs) {
    recycleWifiForTls(now, "repeated TLS failures");
  }
}

unsigned long adsbFetchPollIntervalMs() {
  if (services::adsb::fetchFailStreak() >= 2) {
    return config::kAdsbFetchBackoffMs;
  }
  return config::kTrafficPollIntervalMs;
}

void tickAdsbFetch() {
  const unsigned long now = millis();
  services::adsb::fetchWatchdog(now);

  const bool on_radar = g_screen == AppScreen::Radar && g_radar_visible;
  const bool on_detail = g_screen == AppScreen::FlightDetail;
  const bool prefetch = bootDetailsActive();
  if (!adsbFetchScreenActive()) {
    return;
  }

  if (services::adsb::fetchReady()) {
    // Route cache on ADS-B poll only while flight detail is open (radar tags
    // do not use route fields; skipping enrich avoids LittleFS work on the loop).
    const bool enrich_routes = g_screen == AppScreen::FlightDetail;
    services::adsb::fetchProcessReady(enrich_routes);
    if (g_radar_full_draw_pending && g_screen == AppScreen::Radar && g_radar_visible) {
      completeDeferredRadarDraw();
    } else if (on_radar) {
      PanelSession panel(tft);
      ui::radarDisplayRefreshAircraft();
    } else if (on_detail) {
      if (!tlsBlocksPanel()) {
        PanelSession panel(tft);
        ui::flightDetailRefresh();
      }
    }
  }

  completeDeferredRadarDraw();

  if (g_flight_detail_draw_pending && on_detail && !tlsBlocksPanel()) {
    showFlightDetail();
  }

  tryTlsWifiRefresh(now);

  if (now - g_last_adsb_fetch_ms < adsbFetchPollIntervalMs()) {
    return;
  }
  if (services::adsb::fetchInProgress()) {
    return;
  }
  if (services::https::busy()) {
    if (config::kSerialTraceDebug) {
      static unsigned long s_last_https_defer_log_ms = 0;
      if (now - s_last_https_defer_log_ms >= 3000UL) {
        Serial.println("[fetch] defer: https busy");
        s_last_https_defer_log_ms = now;
      }
    }
    return;
  }
  if (on_detail && deferAdsbForRouteWorker()) {
    if (config::kSerialTraceDebug) {
      static unsigned long s_last_detail_defer_log_ms = 0;
      if (now - s_last_detail_defer_log_ms >= 3000UL) {
        Serial.println("[fetch] defer: route detail worker busy");
        s_last_detail_defer_log_ms = now;
      }
    }
    return;
  }
  if (!adsbDnsReady(now)) {
    return;
  }
  if (!services::https::heapReadyForAdsb()) {
    if (on_detail) {
      ui::flightDetailReleaseSprite();
    }
    static unsigned long s_last_heap_recover_ms = 0;
    if ((on_radar || prefetch) && now - s_last_heap_recover_ms >= 10000UL) {
      s_last_heap_recover_ms = now;
      releaseHttpsPressureMemory();
      if (config::kSerialTraceDebug) {
        Serial.printf("[fetch] heap recover free=%u max_blk=%u\n", ESP.getFreeHeap(),
                      ESP.getMaxAllocHeap());
      }
    }
    if (!services::https::heapReadyForAdsb()) {
      if (config::kSerialTraceDebug) {
        static unsigned long s_last_heap_defer_log_ms = 0;
        if (now - s_last_heap_defer_log_ms >= 3000UL) {
          Serial.printf("[fetch] defer: heap free=%u max_blk=%u\n", ESP.getFreeHeap(),
                        ESP.getMaxAllocHeap());
          s_last_heap_defer_log_ms = now;
        }
      }
      return;
    }
  }

  const float fetch_km = ui::radar::adsbQueryRadiusKm();
  if (services::adsb::fetchRequest(services::map_center::latitude(),
                                   services::map_center::longitude(), fetch_km)) {
    g_last_adsb_fetch_ms = now;
    if (config::kSerialTraceDebug) {
      const char* where = prefetch ? "prefetch" : (on_detail ? "detail" : "radar");
      Serial.printf("[fetch] queued (screen=%s)\n", where);
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.printf("[diag] boot reset=%s fw=%s\n", resetReasonName(), config::kFirmwareVersion);
  Serial.println("FlightScnr (T-Encoder Pro)");

  hardware::panelBootResolve();
  displayInit();
  inputInit();
  hardware::buzzerInit();
  hardware::buzzerBootLoad();
  services::map_center::bootLoad();
  ui::radar::scaleBootLoad();
  ui::radar::accentBootLoad();
  ui::displayPrefsBootLoad();
  services::adsb::trafficFilterBootLoad();
  services::clock::bootLoad();

  if (wifiSetupConnect()) {
    services::clock::startNtp();
    services::route::init();
    services::adsb::fetchInit();
    settingsSetSavedCallback(applySettingsLive);
    g_last_adsb_fetch_ms = 0;
    g_last_tls_proactive_refresh_ms = millis();
    g_screen = AppScreen::Details;
    g_boot_details_until_ms = millis() + config::kBootDetailsDurationMs;
    showDetails(true);
    Serial.println("Screen: details (boot splash)");
    logDiagLine("boot");
  }
}

void loop() {
  const unsigned long loop_start = millis();
  hardware::buzzerPoll();
  tickBootDetailsSplash();
  tickSecondaryScreenTimeout();
  tickClockDisplay();
  handleInput();
  settingsWebPoll();
  services::route::tickCacheFlush(millis());
  tickFlightDetailRouteEnrich();
  tickDiagLog();

  if (WiFi.status() != WL_CONNECTED) {
    settingsWebStop();
    if (g_radar_visible) {
      Serial.println("[wifi] lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        services::clock::startNtp();
        if (g_screen == AppScreen::Radar) {
          showRadar();
        } else if (g_screen == AppScreen::FlightDetail) {
          showFlightDetail();
        } else if (g_screen == AppScreen::Clock) {
          showClock();
        } else if (g_screen == AppScreen::ClockSettings) {
          showClockSettings();
        } else if (g_screen == AppScreen::Details) {
          showDetails();
        } else {
          showSettings();
        }
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (g_screen == AppScreen::Radar) {
      if (!g_radar_visible) {
        showRadar();
      } else {
        tickAdsbFetch();
        tickRadarAnimation();
      }
    } else if (g_screen == AppScreen::FlightDetail) {
      tickAdsbFetch();
    }
  }

  const unsigned long loop_ms = millis() - loop_start;
  if (loop_ms > g_loop_max_ms) {
    g_loop_max_ms = loop_ms;
  }

  delay(1);
}
