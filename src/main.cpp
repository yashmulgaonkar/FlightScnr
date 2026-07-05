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
#include "services/tz_lookup.h"
#include "services/map_center.h"
#include "services/route_lookup.h"
#include "services/settings_web.h"
#include "services/weather.h"
#include "services/weather_icon.h"
#include "services/settings_apply.h"
#include "services/wifi_setup.h"
#include "services/off_hours.h"
#include "services/aircraft_alert.h"
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
#include "ui/weather_screen.h"

namespace {

enum class AppScreen : uint8_t {
  Radar,
  FlightDetail,
  Settings,
  Details,
  Clock,
  ClockSettings,
  Weather,
};

AppScreen g_screen = AppScreen::Radar;
bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_adsb_ssl_recover_ms = 0;
unsigned long g_last_tls_proactive_refresh_ms = 0;
unsigned long g_last_radar_frame_ms = 0;
unsigned long g_last_sweep_done_ms = 0;
unsigned long g_secondary_activity_ms = 0;
unsigned long g_clock_weather_activity_ms = 0;
bool g_auto_idle_clock = false;
bool g_hold_empty_radar = false;
bool g_manual_radar_timed_visit = false;
unsigned long g_ignore_swipes_until_ms = 0;
unsigned long g_boot_details_until_ms = 0;
uint32_t g_last_clock_minute_stamp = UINT32_MAX;
unsigned long g_last_heap_log_ms = 0;
unsigned long g_loop_max_ms = 0;
bool g_radar_full_draw_pending = false;
unsigned long g_radar_full_draw_pending_since_ms = 0;
bool g_flight_detail_draw_pending = false;

bool g_off_hours_active = false;
unsigned long g_off_hours_last_check_ms = 0;
unsigned long g_off_hours_wake_override_until_ms = 0;

uint32_t g_diag_sweep_frames = 0;
uint32_t g_diag_slow_loops = 0;
uint32_t g_diag_interval_min_heap = UINT32_MAX;
unsigned long g_diag_adsb_process_max_ms = 0;

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
    case AppScreen::Weather:
      return "weather";
    default:
      return "?";
  }
}

void logNavContext(const char* event) {
  if (!config::kOvernightPerfLog) {
    return;
  }
  Serial.printf(
      "[nav] %s screen=%s radar_vis=%d auto_idle=%d hold=%d idle_clk=%d ac=%u ac_in=%u "
      "heap=%u\n",
      event, screenName(g_screen), g_radar_visible ? 1 : 0, g_auto_idle_clock ? 1 : 0,
      g_hold_empty_radar ? 1 : 0, ui::displayPrefsAutoIdleClockEnabled() ? 1 : 0,
      static_cast<unsigned>(services::adsb::aircraftCount()),
      static_cast<unsigned>(ui::radarDisplayInRangeAircraftCount()), ESP.getFreeHeap());
}

void logRadarDebugState(const char* tag) {
  if (!config::kRadarResumeDebug) {
    return;
  }
  const unsigned long now = millis();
  const unsigned long pending_ms =
      g_radar_full_draw_pending && g_radar_full_draw_pending_since_ms != 0
          ? now - g_radar_full_draw_pending_since_ms
          : 0UL;
  const unsigned long sweep_age_ms =
      g_last_sweep_done_ms != 0 ? now - g_last_sweep_done_ms : 0UL;
  const uint32_t adsb_age_ms = services::adsb::lastFetchOkAgeMs();
  char adsb_age_buf[16];
  if (adsb_age_ms == UINT32_MAX) {
    strncpy(adsb_age_buf, "never", sizeof(adsb_age_buf) - 1);
  } else {
    snprintf(adsb_age_buf, sizeof(adsb_age_buf), "%lus", adsb_age_ms / 1000UL);
  }
  adsb_age_buf[sizeof(adsb_age_buf) - 1] = '\0';

  Serial.printf(
      "[radar] %s screen=%s full_draw=%d pending_ms=%lu radar_vis=%d sweep_age_ms=%lu "
      "bg=%d content=%d base=%d detail_sp=%d https=%d adsb_busy=%d adsb_ready=%d adsb_age=%s "
      "route_wkr=%d route_pause=%d step=%s sprite_rel=%d heap=%u max_blk=%u\n",
      tag, screenName(g_screen), g_radar_full_draw_pending ? 1 : 0, pending_ms,
      g_radar_visible ? 1 : 0, sweep_age_ms, ui::radarDisplayDebugBgReady() ? 1 : 0,
      ui::radarDisplayDebugContentReady() ? 1 : 0, ui::radarDisplayDebugContentBaseValid() ? 1 : 0,
      ui::flightDetailSpriteReady() ? 1 : 0, services::https::busy() ? 1 : 0,
      services::adsb::fetchInProgress() ? 1 : 0, services::adsb::fetchReady() ? 1 : 0, adsb_age_buf,
      services::route::detailWorkerBusy() ? 1 : 0, services::route::detailAdsbFetchPaused() ? 1 : 0,
      services::route::detailWorkerDebugStepTag(),
      services::route::detailWorkerDebugSpriteReleasePending() ? 1 : 0, ESP.getFreeHeap(),
      ESP.getMaxAllocHeap());
}

void logFetchDefer(const char* reason) {
  if (!config::kRadarResumeDebug && !config::kSerialTraceDebug) {
    return;
  }
  static unsigned long s_last_log_ms = 0;
  const unsigned long now = millis();
  if (now - s_last_log_ms < 3000UL) {
    return;
  }
  s_last_log_ms = now;
  Serial.printf("[fetch] defer: %s screen=%s radar_vis=%d https=%d adsb_busy=%d route_pause=%d "
                "heap=%u max_blk=%u\n",
                reason, screenName(g_screen), g_radar_visible ? 1 : 0, services::https::busy() ? 1 : 0,
                services::adsb::fetchInProgress() ? 1 : 0,
                services::route::detailAdsbFetchPaused() ? 1 : 0, ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
}

void logDiagLine(const char* tag, unsigned long diag_ivl_ms = config::kDiagLogIntervalMs) {
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

  char wx_ok[16];
  const uint32_t wx_age_ms = services::weather::dataAgeMs();
  if (wx_age_ms == UINT32_MAX) {
    strncpy(wx_ok, "never", sizeof(wx_ok) - 1);
  } else {
    snprintf(wx_ok, sizeof(wx_ok), "%lus", wx_age_ms / 1000UL);
  }
  wx_ok[sizeof(wx_ok) - 1] = '\0';

  const unsigned long sweep_gap_ms =
      (g_screen == AppScreen::Radar && g_radar_visible && g_last_sweep_done_ms != 0)
          ? millis() - g_last_sweep_done_ms
          : 0UL;

  const unsigned diag_ivl_sec =
      diag_ivl_ms > 0 ? static_cast<unsigned>(diag_ivl_ms / 1000UL) : 0U;
  const unsigned sweep_fps =
      diag_ivl_sec > 0 ? static_cast<unsigned>(g_diag_sweep_frames / diag_ivl_sec) : 0U;

  Serial.printf(
      "[diag] %s uptime=%uh%02um free=%u min=%u ivl_min=%u max_blk=%u psram=%u wifi=%s rssi=%d "
      "screen=%s radar_vis=%d ac=%u ac_in=%u adsb_ok=%s adsb_busy=%u adsb_fail=%u "
      "adsb_proc_max=%lums wx_ok=%s wx_valid=%d wx_busy=%d wx_ready=%d wx_backoff=%lus "
      "tz_auto=%d tz_resolved=%d tz_busy=%d "
      "https=%d idle=%d hold=%d idle_clk=%d full_draw=%d route_wkr=%d route_pause=%d "
      "sweep_gap=%lums sweep_fps=%u bg=%d ct=%d base=%d detail_sp=%d slow_loops=%u stk_loop=%u "
      "stk_adsb=%u loop_max=%lums lfs=%s\n",
      tag, hours, mins, ESP.getFreeHeap(), ESP.getMinFreeHeap(), g_diag_interval_min_heap,
      ESP.getMaxAllocHeap(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM), wifi_up ? "up" : "down",
      rssi, screenName(g_screen), g_radar_visible ? 1 : 0,
      static_cast<unsigned>(services::adsb::aircraftCount()),
      static_cast<unsigned>(ui::radarDisplayInRangeAircraftCount()), adsb_ok,
      services::adsb::fetchInProgress() ? 1U : 0U, services::adsb::fetchFailStreak(),
      g_diag_adsb_process_max_ms, wx_ok, services::weather::hasData() ? 1 : 0,
      services::weather::fetchInProgress() ? 1 : 0, services::weather::fetchReady() ? 1 : 0,
      services::weather::retryBackoffMs() / 1000UL, services::clock::useAutoTimezone() ? 1 : 0,
      services::tzlookup::hasResolvedTimezone() ? 1 : 0,
      services::tzlookup::lookupInProgress() ? 1 : 0,
      services::https::busy() ? 1 : 0,
      g_auto_idle_clock ? 1 : 0, g_hold_empty_radar ? 1 : 0,
      ui::displayPrefsAutoIdleClockEnabled() ? 1 : 0, g_radar_full_draw_pending ? 1 : 0,
      services::route::detailWorkerBusy() ? 1 : 0, services::route::detailAdsbFetchPaused() ? 1 : 0,
      sweep_gap_ms, sweep_fps, ui::radarDisplayDebugBgReady() ? 1 : 0,
      ui::radarDisplayDebugContentReady() ? 1 : 0, ui::radarDisplayDebugContentBaseValid() ? 1 : 0,
      ui::flightDetailSpriteReady() ? 1 : 0, g_diag_slow_loops, loop_stack,
      services::adsb::fetchTaskStackFreeBytes(), g_loop_max_ms, lfs_buf);

  g_loop_max_ms = 0;
  g_diag_sweep_frames = 0;
  g_diag_slow_loops = 0;
  g_diag_adsb_process_max_ms = 0;
  g_diag_interval_min_heap = ESP.getFreeHeap();
}

void tickDiagLog() {
  const unsigned long now = millis();
  if (g_last_heap_log_ms != 0 && now - g_last_heap_log_ms < config::kDiagLogIntervalMs) {
    return;
  }
  const unsigned long diag_ivl_ms =
      g_last_heap_log_ms != 0 ? now - g_last_heap_log_ms : config::kDiagLogIntervalMs;
  g_last_heap_log_ms = now;
  logDiagLine("tick", diag_ivl_ms);
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
  if (g_auto_idle_clock &&
      (g_screen == AppScreen::Clock || g_screen == AppScreen::Weather)) {
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

void noteClockWeatherActivity() { g_clock_weather_activity_ms = millis(); }

void releaseHttpsPressureMemory() {
  ui::flightDetailReleaseSprite();
  services::route::cancelDetailEnrichment();
  services::route::tickDetailSpriteRelease();
}

/** Drop radar PSRAM sprites and drain TLS when max_blk is still tight after detail. */
void recoverRadarHeapPressure(const char* reason, uint32_t drain_ms = 1200) {
  const uint32_t blk_now = ESP.getMaxAllocHeap();
  if (blk_now >= config::kMinContiguousHeapForAdsbTls) {
    return;
  }

  // Mid-TLS sprite teardown does not coalesce heap; it only forces expensive redraws.
  if ((services::https::busy() || services::adsb::fetchInProgress()) &&
      strcmp(reason, "detail_resume") != 0) {
    return;
  }

  static unsigned long s_last_recover_ms = 0;
  static uint32_t s_last_recover_blk = 0;
  const unsigned long now = millis();
  if (s_last_recover_ms != 0 && now - s_last_recover_ms < 10000UL &&
      blk_now + 512U >= s_last_recover_blk) {
    return;
  }

  const uint32_t free_before = ESP.getFreeHeap();
  ui::radarDisplayReleasePressureSprites();
  if (drain_ms > 0) {
    services::https::drainTlsHeapAfterSession(drain_ms);
  }
  s_last_recover_ms = now;
  s_last_recover_blk = ESP.getMaxAllocHeap();

  if (config::kRadarResumeDebug || config::kSerialTraceDebug) {
    static unsigned long s_last_log_ms = 0;
    if (now - s_last_log_ms >= 2000UL) {
      s_last_log_ms = now;
      Serial.printf("[radar] heap_pressure %s free=%u->%u blk=%u->%u\n", reason, free_before,
                    ESP.getFreeHeap(), blk_now, ESP.getMaxAllocHeap());
    }
  }
}

void noteRadarFullDrawComplete() {
  const unsigned long now = millis();
  g_last_sweep_done_ms = now;
  g_last_radar_frame_ms = now;
}

/** After flight detail, release detail/route pressure and let TLS buffers drain.
 *  Internal free heap settles at ~39KB post-enrichment; WiFi recycle does not
 *  restore it and stalls the UI for seconds — the ADS-B floor matches route APIs. */
void reclaimHeapAfterFlightDetail() {
  const uint32_t heap_before = ESP.getFreeHeap();
  const uint32_t blk_before = ESP.getMaxAllocHeap();

  releaseHttpsPressureMemory();
  services::https::drainTlsHeapAfterSession(2500);
  recoverRadarHeapPressure("detail_resume");

  if (config::kRadarResumeDebug || config::kSerialTraceDebug) {
    Serial.printf("[radar] resume_reclaim heap=%u->%u blk=%u->%u adsb_ok=%d\n", heap_before,
                  ESP.getFreeHeap(), blk_before, ESP.getMaxAllocHeap(),
                  services::https::heapReadyForAdsb() ? 1 : 0);
  }
}

bool heapBlocksPanel() {
  return ESP.getMaxAllocHeap() < config::kMinContiguousHeapForPanelSpi;
}

/** Flight detail draw uses RAM/sprite only — not the HTTPS lock.
 *  Only the brief sprite-release handshake (loop frees the PSRAM sprite for TLS
 *  heap) must hold off drawing; a merely-busy worker does not, so scroll redraws
 *  stay instant and the route line fills in async when enrichment lands. */
bool detailDrawBlocked() {
  return services::route::detailDrawUnsafe();
}


void completeDeferredRadarDraw() {
  if (!g_radar_full_draw_pending || g_screen != AppScreen::Radar || !g_radar_visible) {
    return;
  }
  if (heapBlocksPanel()) {
    recoverRadarHeapPressure("deferred_draw", 600);
  }
  if (heapBlocksPanel()) {
    if (config::kRadarResumeDebug) {
      static unsigned long s_last_defer_log_ms = 0;
      const unsigned long now = millis();
      if (now - s_last_defer_log_ms >= 2000UL) {
        logRadarDebugState("defer_spi_heap");
        s_last_defer_log_ms = now;
      }
    }
    return;
  }
  PanelSession panel(tft);
  ui::radarDisplayDraw();
  g_radar_full_draw_pending = false;
  g_radar_full_draw_pending_since_ms = 0;
  noteRadarFullDrawComplete();
  if (config::kSerialTraceDebug || config::kRadarResumeDebug) {
    Serial.println("[radar] deferred full draw complete");
  }
}

void showRadar() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    g_radar_full_draw_pending = false;
    g_radar_full_draw_pending_since_ms = 0;
    return;
  }
  ui::flightDetailReleaseSprite();
  g_radar_visible = true;
  g_last_adsb_fetch_ms = 0;

  if (heapBlocksPanel()) {
    recoverRadarHeapPressure("show_radar", 600);
  }

  if (heapBlocksPanel()) {
    g_radar_full_draw_pending = true;
    g_radar_full_draw_pending_since_ms = millis();
    if (config::kSerialTraceDebug || config::kRadarResumeDebug) {
      logRadarDebugState("show_defer_heap");
    }
    return;
  }

  PanelSession panel(tft);
  ui::radarDisplayDraw();
  g_radar_full_draw_pending = false;
  g_radar_full_draw_pending_since_ms = 0;
  noteRadarFullDrawComplete();
  if (config::kRadarResumeDebug) {
    logRadarDebugState("show_drew");
  }
}

void showFlightDetail() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (detailDrawBlocked() || heapBlocksPanel()) {
    g_flight_detail_draw_pending = true;
    if (config::kSerialTraceDebug || config::kRadarResumeDebug) {
      Serial.printf("[radar] detail_draw_block sprite_rel=%d route_wkr=%d heap=%u\n",
                    services::route::detailWorkerDebugSpriteReleasePending() ? 1 : 0,
                    services::route::detailWorkerBusy() ? 1 : 0, ESP.getFreeHeap());
    }
    g_radar_visible = false;
    return;
  }
  g_flight_detail_draw_pending = false;
  PanelSession panel(tft);
  ui::flightDetailDraw();
  if (!ui::flightDetailSpriteReady()) {
    g_flight_detail_draw_pending = true;
  }
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
  services::route::tickDetailSpriteRelease();
  services::route::tickDetailEnrichDebounce(now);
  services::route::tickDetailWorkerWatchdog(now);

  const char* ui_callsign = ui::flightDetailSelectedCallsign();
  const char* route_callsign = services::route::detailSelectionCallsign();
  if (ui_callsign != nullptr && route_callsign != nullptr &&
      strcmp(ui_callsign, route_callsign) != 0) {
    services::route::onFlightDetailSelected(ui_callsign, false);
  }

  if (services::route::detailEnrichmentReady()) {
    bool needs_redraw = true;
    if (services::route::detailEnrichmentConsume(&needs_redraw)) {
      s_detail_enrich_in_flight = false;
      if (needs_redraw) {
        if (config::kSerialTraceDebug) {
          const char* cs = ui::flightDetailSelectedCallsign();
          Serial.printf("[detail] enrich ready -> redraw %s\n",
                        cs != nullptr ? cs : "(none)");
        }
        showFlightDetail();
        ui::flightDetailMarkEnrichRedrawn(ui::flightDetailSelectedCallsign());
      } else if (config::kSerialTraceDebug) {
        const char* cs = ui::flightDetailSelectedCallsign();
        Serial.printf("[detail] enrich skip redraw %s (already on screen)\n",
                      cs != nullptr ? cs : "(none)");
      }
      return;
    }
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
  services::weather::requestOnScreenOpen();
  ui::clockScreenDraw();
  g_radar_visible = false;
  g_last_clock_minute_stamp = services::clock::localMinuteStamp();
  noteClockWeatherActivity();
}

void showClockSettings() {
  ui::flightDetailReleaseSprite();
  ui::clockSettingsScreenDraw();
  g_radar_visible = false;
}

void showWeather() {
  ui::flightDetailReleaseSprite();
  services::weather::requestOnScreenOpen();
  ui::weatherScreenDraw();
  g_radar_visible = false;
  noteClockWeatherActivity();
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
    case AppScreen::Weather:
      showWeather();
      break;
  }
  Serial.println("[settings] applied live");
}

void returnToRadar(bool from_idle_timeout = false, bool manual_navigation = false) {
  const AppScreen from_screen = g_screen;
  g_auto_idle_clock = false;
  g_hold_empty_radar = false;
  g_manual_radar_timed_visit = false;
  if (manual_navigation) {
    if (ui::displayPrefsAutoIdleClockEnabled()) {
      g_manual_radar_timed_visit = true;
      g_clock_weather_activity_ms = millis();
    } else {
      g_hold_empty_radar = true;
    }
  }
  if (g_screen == AppScreen::Clock || g_screen == AppScreen::Weather) {
    services::weather_icon::releaseBuffer();
  }
  if (from_screen == AppScreen::FlightDetail && config::kRadarResumeDebug) {
    const char* cs = ui::flightDetailSelectedCallsign();
    Serial.printf("[radar] resume_pre callsign=%s step=%s sprite_rel=%d detail_sp=%d "
                  "route_wkr=%d route_pause=%d https=%d adsb_busy=%d heap=%u\n",
                  cs != nullptr ? cs : "(none)", services::route::detailWorkerDebugStepTag(),
                  services::route::detailWorkerDebugSpriteReleasePending() ? 1 : 0,
                  ui::flightDetailSpriteReady() ? 1 : 0, services::route::detailWorkerBusy() ? 1 : 0,
                  services::route::detailAdsbFetchPaused() ? 1 : 0, services::https::busy() ? 1 : 0,
                  services::adsb::fetchInProgress() ? 1 : 0, ESP.getFreeHeap());
  }
  if (g_screen == AppScreen::FlightDetail) {
    reclaimHeapAfterFlightDetail();
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
  if (from_screen == AppScreen::FlightDetail && config::kRadarResumeDebug) {
    logRadarDebugState(from_idle_timeout ? "resume_timeout" : "resume");
  }
  Serial.println(from_idle_timeout ? "Screen: radar (timeout)"
                                   : "Screen: radar");
  logNavContext(from_idle_timeout ? "radar_timeout" : "radar");
}

void returnToClockFromIdleTimeout() {
  ui::clockSettingsResetFocus();
  inputDiscardPendingInteractions();
  g_screen = AppScreen::Clock;
  showClock();
  Serial.println("Screen: clock (timeout)");
  logNavContext("clock_timeout");
}

void openSettingsFromRadar() {
  ui::infoScreenResetToMain();
  g_screen = AppScreen::Settings;
  noteSecondaryActivity();
  showSettings();
  Serial.println("Screen: settings (1/3)");
  logNavContext("settings_p1");
}

void openClockFromRadar() {
  g_auto_idle_clock = false;
  g_hold_empty_radar = false;
  g_manual_radar_timed_visit = false;
  g_screen = AppScreen::Clock;
  showClock();
  Serial.println("Screen: clock");
  logNavContext("clock");
}

void openClockFromIdleRadar() {
  g_hold_empty_radar = false;
  g_manual_radar_timed_visit = false;
  g_auto_idle_clock = true;
  g_screen = AppScreen::Clock;
  showClock();
  Serial.println("Screen: clock (idle)");
  logNavContext("clock_idle");
}

void openDetailsFromRadar() {
  g_screen = AppScreen::Details;
  noteSecondaryActivity();
  showDetails();
  Serial.println("Screen: details");
  logNavContext("details");
}

void openWeatherFromClock() {
  g_screen = AppScreen::Weather;
  showWeather();
  Serial.println("Screen: weather");
  logNavContext("weather");
}

void openClockSettingsFromClock() {
  ui::clockSettingsResetFocus();
  g_screen = AppScreen::ClockSettings;
  noteSecondaryActivity();
  showClockSettings();
  Serial.println("Screen: clock settings");
  logNavContext("clock_settings");
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
  if (config::kRadarResumeDebug) {
    const char* cs = ui::flightDetailSelectedCallsign();
    Serial.printf("[radar] detail_open callsign=%s detail_sp=%d route_wkr=%d route_pause=%d "
                  "https=%d adsb_busy=%d heap=%u max_blk=%u\n",
                  cs != nullptr ? cs : "(none)", ui::flightDetailSpriteReady() ? 1 : 0,
                  services::route::detailWorkerBusy() ? 1 : 0,
                  services::route::detailAdsbFetchPaused() ? 1 : 0, services::https::busy() ? 1 : 0,
                  services::adsb::fetchInProgress() ? 1 : 0, ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
  }
  Serial.println("Screen: flight detail");
  logNavContext("flight_detail");
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
    recoverRadarHeapPressure("range_change", 400);
    if (heapBlocksPanel()) {
      g_radar_full_draw_pending = true;
      g_radar_full_draw_pending_since_ms = millis();
      return;
    }
    PanelSession panel(tft);
    ui::radarDisplayDraw();
    noteRadarFullDrawComplete();
  }
}

void noteSwipeNavigation() {
  g_ignore_swipes_until_ms = millis() + config::kSwipeNavDebounceMs;
  inputDiscardPendingInteractions();
}

void handleNavigation() {
  if (bootDetailsActive()) {
    inputConsumeSwipe();
    return;
  }

  if (millis() < g_ignore_swipes_until_ms) {
    inputConsumeSwipe();
    return;
  }

  const SwipeGesture swipe = inputConsumeSwipe();
  if (swipe == SwipeNone) {
    return;
  }

  if (g_screen != AppScreen::Radar && g_screen != AppScreen::Clock &&
      g_screen != AppScreen::Weather) {
    noteSecondaryActivity();
  }
  if (g_screen == AppScreen::Clock || g_screen == AppScreen::Weather) {
    noteClockWeatherActivity();
  }

  bool navigated = false;
  if (swipe == SwipeDown && g_screen == AppScreen::Radar) {
    openClockFromRadar();
    navigated = true;
  } else if (swipe == SwipeUp && g_screen == AppScreen::Radar) {
    openDetailsFromRadar();
    navigated = true;
  } else if (swipe == SwipeDown && g_screen == AppScreen::Details) {
    returnToRadar(false, true);
    navigated = true;
  } else if (swipe == SwipeUp && g_screen == AppScreen::Clock) {
    returnToRadar(false, true);
    navigated = true;
  } else if (swipe == SwipeUp && g_screen == AppScreen::Weather) {
    returnToRadar(false, true);
    navigated = true;
  } else if (swipe == SwipeRight && g_screen == AppScreen::Clock) {
    openWeatherFromClock();
    navigated = true;
  } else if (swipe == SwipeLeft && g_screen == AppScreen::Weather) {
    g_screen = AppScreen::Clock;
    showClock();
    Serial.println("Screen: clock");
    logNavContext("clock_swipe");
    navigated = true;
  } else if (swipe == SwipeLeft && g_screen == AppScreen::Clock) {
    openClockSettingsFromClock();
    navigated = true;
  } else if (swipe == SwipeRight && g_screen == AppScreen::ClockSettings) {
    g_screen = AppScreen::Clock;
    showClock();
    Serial.println("Screen: clock");
    logNavContext("clock_swipe");
    navigated = true;
  } else if (swipe == SwipeLeft && g_screen == AppScreen::Radar) {
    openSettingsFromRadar();
    navigated = true;
  } else if (swipe == SwipeLeft && g_screen == AppScreen::Settings &&
             ui::infoScreenPage() == ui::InfoSettingsPage::Main) {
    ui::infoScreenSetPage(ui::InfoSettingsPage::Display);
    ui::infoScreenResetDisplayFocus();
    showSettings();
    Serial.println("Screen: settings (2/3)");
    logNavContext("settings_p2");
    navigated = true;
  } else if (swipe == SwipeLeft && g_screen == AppScreen::Settings &&
             ui::infoScreenPage() == ui::InfoSettingsPage::Display) {
    ui::infoScreenSetPage(ui::InfoSettingsPage::Colors);
    ui::infoScreenResetColorsFocus();
    showSettings();
    Serial.println("Screen: settings (3/3)");
    logNavContext("settings_p3");
    navigated = true;
  } else if (swipe == SwipeRight && g_screen == AppScreen::FlightDetail) {
    returnToRadar(false, true);
    navigated = true;
  } else if (swipe == SwipeRight && g_screen == AppScreen::Settings &&
             ui::infoScreenPage() == ui::InfoSettingsPage::Colors) {
    ui::infoScreenSetPage(ui::InfoSettingsPage::Display);
    ui::infoScreenResetDisplayFocus();
    showSettings();
    Serial.println("Screen: settings (2/3)");
    logNavContext("settings_p2");
    navigated = true;
  } else if (swipe == SwipeRight && g_screen == AppScreen::Settings &&
             ui::infoScreenPage() == ui::InfoSettingsPage::Display) {
    ui::infoScreenSetPage(ui::InfoSettingsPage::Main);
    showSettings();
    Serial.println("Screen: settings (1/3)");
    logNavContext("settings_p1");
    navigated = true;
  } else if (swipe == SwipeRight && g_screen == AppScreen::Settings) {
    returnToRadar(false, true);
    navigated = true;
  }

  if (navigated) {
    noteSwipeNavigation();
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
  logNavContext("boot_splash_done");
}

void tickSecondaryScreenTimeout() {
  if (bootDetailsActive()) {
    return;
  }
  if (g_screen == AppScreen::Clock || g_screen == AppScreen::Weather) {
    const unsigned long timeout_ms = ui::displayPrefsClockWeatherTimeoutMs();
    if (timeout_ms != 0 && millis() - g_clock_weather_activity_ms >= timeout_ms) {
      returnToRadar(true);
    }
    return;
  }
  if (g_screen == AppScreen::Radar) {
    if (g_manual_radar_timed_visit && ui::displayPrefsAutoIdleClockEnabled()) {
      const unsigned long timeout_ms = ui::displayPrefsClockWeatherTimeoutMs();
      if (timeout_ms != 0 && millis() - g_clock_weather_activity_ms >= timeout_ms) {
        g_manual_radar_timed_visit = false;
        openClockFromIdleRadar();
      }
    }
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

void tickOffHours() {
  const unsigned long now = millis();
  if (now - g_off_hours_last_check_ms < config::kOffHoursCheckIntervalMs) {
    return;
  }
  g_off_hours_last_check_ms = now;

  if (g_off_hours_wake_override_until_ms != 0 && now < g_off_hours_wake_override_until_ms) {
    return;
  }
  if (g_off_hours_wake_override_until_ms != 0 && now >= g_off_hours_wake_override_until_ms) {
    g_off_hours_wake_override_until_ms = 0;
  }

  const bool was_active = g_off_hours_active;
  const bool is_active = services::offhours::active();
  g_off_hours_active = is_active;

  if (!was_active && is_active) {
    const auto mode = services::offhours::mode();
    if (mode == services::offhours::Mode::DisplayOff) {
      displaySleep();
      Serial.println("[offhours] entering night mode (display off)");
    } else {
      g_screen = AppScreen::Clock;
      showClock();
      hardware::displayBrightnessOverride(20);
      Serial.println("[offhours] entering night mode (dim)");
    }
  } else if (was_active && !is_active) {
    const auto mode = services::offhours::mode();
    if (mode == services::offhours::Mode::DisplayOff) {
      displayWake();
    }
    hardware::displayBrightnessRestore();
    g_auto_idle_clock = false;
    g_screen = AppScreen::Radar;
    if (WiFi.status() == WL_CONNECTED) {
      showRadar();
    }
    Serial.println("[offhours] exiting night mode");
  }
}

void tickAutoIdleClock() {
  if (bootDetailsActive()) {
    return;
  }
  if (g_screen == AppScreen::FlightDetail || g_screen == AppScreen::Settings ||
      g_screen == AppScreen::Details || g_screen == AppScreen::ClockSettings) {
    return;
  }

  // Only count aircraft inside the outer ring (drawn as airplanes). Off-screen
  // beyond-ring blips should not pull us back to the radar.
  const size_t ac = ui::radarDisplayInRangeAircraftCount();
  if (ac > 0) {
    g_hold_empty_radar = false;
    g_manual_radar_timed_visit = false;
  }

  if (g_auto_idle_clock && ac > 0 &&
      (g_screen == AppScreen::Clock || g_screen == AppScreen::Weather)) {
    returnToRadar(false);
    Serial.println("Screen: radar (aircraft)");
    logNavContext("radar_aircraft");
    return;
  }

  if (!g_auto_idle_clock && !g_hold_empty_radar && !g_manual_radar_timed_visit &&
      g_screen == AppScreen::Radar && g_radar_visible && ac == 0 &&
      ui::displayPrefsAutoIdleClockEnabled() &&
      millis() - g_last_radar_frame_ms >= config::kRadarMinVisibleMs) {
    openClockFromIdleRadar();
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

// Weather is fetched only while the clock or forecast screen is open: a missing
// or >30-min-stale cache triggers a background refresh, and a finished fetch
// redraws the live screen. No background polling on any other screen.
void tickWeather() {
  if (g_screen != AppScreen::Clock && g_screen != AppScreen::Weather) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    services::weather::requestRefresh(false);
  }
  if (services::weather::processReady()) {
    if (config::kOvernightPerfLog) {
      Serial.printf("[weather] data applied screen=%s age=%lus heap=%u\n", screenName(g_screen),
                    services::weather::dataAgeMs() / 1000UL, ESP.getFreeHeap());
    }
    if (g_screen == AppScreen::Clock) {
      showClock();
    } else {
      showWeather();
    }
  }
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
      if (inputConsumeKnobTap()) {
        noteSecondaryActivity();
        ui::infoScreenCycleColorsFocus();
        showSettings();
        return;
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
    if (config::kOvernightPerfLog || config::kRadarSweepTraceDebug || config::kRadarResumeDebug) {
      static unsigned long s_last_skip_log_ms = 0;
      const unsigned long now = millis();
      const unsigned long log_ivl =
          config::kRadarResumeDebug ? 2000UL
                                    : (config::kOvernightPerfLog ? 30000UL : 2000UL);
      if (now - s_last_skip_log_ms >= log_ivl) {
        if (config::kRadarResumeDebug) {
          logRadarDebugState("sweep_skip");
        } else {
          Serial.printf("[perf] sweep_skip full_draw_pending https=%d heap=%u\n",
                        services::https::busy() ? 1 : 0, ESP.getFreeHeap());
        }
        s_last_skip_log_ms = now;
      }
    }
    return;
  }
  if (g_screen != AppScreen::Radar || !g_radar_visible) {
    if (config::kRadarResumeDebug && g_screen == AppScreen::Radar) {
      static unsigned long s_last_vis_skip_ms = 0;
      const unsigned long now = millis();
      if (now - s_last_vis_skip_ms >= 3000UL) {
        logRadarDebugState("sweep_skip_vis");
        s_last_vis_skip_ms = now;
      }
    }
    return;
  }

  const unsigned long now = millis();
  if (now - g_last_radar_frame_ms < ui::radar::kSweepFrameMs) {
    return;
  }

  if (config::kOvernightPerfLog && config::kDiagSweepGapFrameMult > 0 &&
      g_last_sweep_done_ms != 0) {
    const unsigned long gap = now - g_last_sweep_done_ms;
    const unsigned long threshold =
        ui::radar::kSweepFrameMs * static_cast<unsigned long>(config::kDiagSweepGapFrameMult);
    if (gap > threshold) {
      Serial.printf("[perf] sweep_gap %lums https=%d fetch_busy=%d fetch_ready=%d full_draw=%d\n",
                    gap, services::https::busy() ? 1 : 0,
                    services::adsb::fetchInProgress() ? 1 : 0,
                    services::adsb::fetchReady() ? 1 : 0, g_radar_full_draw_pending ? 1 : 0);
    }
  } else if (config::kRadarSweepTraceDebug && g_last_sweep_done_ms != 0) {
    const unsigned long gap = now - g_last_sweep_done_ms;
    if (gap > ui::radar::kSweepFrameMs * 2) {
      Serial.printf("[sweep] gap %lums https=%d fetch_busy=%d fetch_ready=%d\n", gap,
                    services::https::busy() ? 1 : 0, services::adsb::fetchInProgress() ? 1 : 0,
                    services::adsb::fetchReady() ? 1 : 0);
    }
  }

  if (ESP.getMaxAllocHeap() < config::kMinContiguousHeapForPanelSpi) {
    static unsigned long s_last_spi_defer_ms = 0;
    if (now - s_last_spi_defer_ms >= 2000UL) {
      logRadarDebugState("defer_spi_heap");
      s_last_spi_defer_ms = now;
    }
    return;
  }

  g_last_radar_frame_ms = now;
  PanelSession panel(tft);
  ui::radarDisplayRefreshSweep();
  g_last_sweep_done_ms = millis();
  g_diag_sweep_frames++;
}

void tickRadarSweepHeartbeat() {
  if (!config::kRadarResumeDebug || g_screen != AppScreen::Radar || !g_radar_visible ||
      g_radar_full_draw_pending) {
    return;
  }
  static unsigned long s_last_hb_ms = 0;
  const unsigned long now = millis();
  if (now - s_last_hb_ms < 5000UL) {
    return;
  }
  logRadarDebugState("sweep_ok");
  s_last_hb_ms = now;
}

void tickRadarStallWatchdog() {
  if (!config::kRadarResumeDebug || g_screen != AppScreen::Radar || !g_radar_visible) {
    return;
  }
  static unsigned long s_last_stall_log_ms = 0;
  const unsigned long now = millis();
  if (now - s_last_stall_log_ms < 5000UL) {
    return;
  }

  const unsigned long sweep_age_ms =
      g_last_sweep_done_ms != 0 ? now - g_last_sweep_done_ms : 999999UL;
  const uint32_t adsb_age_ms = services::adsb::lastFetchOkAgeMs();
  const bool full_draw_stall =
      g_radar_full_draw_pending && g_radar_full_draw_pending_since_ms != 0 &&
      now - g_radar_full_draw_pending_since_ms >= 5000UL;
  const bool sweep_stall = !g_radar_full_draw_pending && sweep_age_ms >= 3000UL;
  const bool adsb_stall = adsb_age_ms != UINT32_MAX && adsb_age_ms >= 15000UL &&
                          !services::adsb::fetchInProgress() && !services::adsb::fetchReady();

  if (!full_draw_stall && !sweep_stall && !adsb_stall) {
    return;
  }

  if (full_draw_stall && sweep_stall && adsb_stall) {
    logRadarDebugState("stall_frozen");
  } else if (full_draw_stall) {
    logRadarDebugState("stall_full_draw");
  } else if (sweep_stall && adsb_stall) {
    logRadarDebugState("stall_frozen");
  } else if (sweep_stall) {
    logRadarDebugState("stall_no_sweep");
  } else if (adsb_stall) {
    logRadarDebugState("stall_no_adsb");
  }

  s_last_stall_log_ms = now;
}

bool canRecycleWifiForTlsMemory(unsigned long now) {
  return WiFi.status() == WL_CONNECTED && !services::adsb::fetchInProgress() &&
         !services::https::busy() &&
         now - g_last_adsb_ssl_recover_ms >= config::kTlsMemoryRecoverCooldownMs;
}

bool canRecycleWifiForTls(unsigned long now) {
  return WiFi.status() == WL_CONNECTED && !services::adsb::fetchInProgress() &&
         !services::https::busy() &&
         now - g_last_adsb_ssl_recover_ms >= config::kTlsRecoverCooldownMs;
}

void recycleWifiForTls(unsigned long now, const char* reason) {
  releaseHttpsPressureMemory();
  services::route::resetTlsHardFail();
  g_last_adsb_fetch_ms = 0;
  g_last_adsb_ssl_recover_ms = now;
  g_last_tls_proactive_refresh_ms = now;
  services::adsb::fetchResetFailStreak();
  Serial.printf("[adsb] %s — recycling WiFi\n", reason);
  wifiSoftRecycle();
}

void tryTlsWifiRefresh(unsigned long now) {
  if (services::route::tlsRecoverRequested() && canRecycleWifiForTlsMemory(now)) {
    services::route::consumeTlsRecoverRequest();
    recycleWifiForTls(now, "TLS memory pressure");
    return;
  }
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

unsigned long adsbFetchPollIntervalMs(bool on_detail) {
  if (services::adsb::fetchFailStreak() >= 2) {
    return config::kAdsbFetchBackoffMs;
  }
  if (on_detail) {
    return config::kAdsbFetchPollIntervalDetailMs;
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
    if (config::kRadarResumeDebug && g_screen == AppScreen::Radar) {
      static unsigned long s_last_inactive_ms = 0;
      if (now - s_last_inactive_ms >= 3000UL) {
        logRadarDebugState("fetch_inactive");
        s_last_inactive_ms = now;
      }
    }
    return;
  }

  if (on_detail) {
    services::route::tickDetailSpriteRelease();
  }

  if (services::adsb::fetchReady()) {
    // Route cache on ADS-B poll only while flight detail is open (radar tags
    // do not use route fields; skipping enrich avoids LittleFS work on the loop).
    const bool enrich_routes = g_screen == AppScreen::FlightDetail;
    const unsigned long process_start = millis();
    services::adsb::fetchProcessReady(enrich_routes);
    services::alert::checkNewAircraft(services::adsb::aircraftList(),
                                      services::adsb::aircraftCount());
    const unsigned long process_ms = millis() - process_start;
    if (process_ms > g_diag_adsb_process_max_ms) {
      g_diag_adsb_process_max_ms = process_ms;
    }
    if (config::kOvernightPerfLog && process_ms >= 10) {
      Serial.printf("[perf] adsb_process ms=%lu ac=%u ac_in=%u enrich=%d heap=%u\n", process_ms,
                    static_cast<unsigned>(services::adsb::aircraftCount()),
                    static_cast<unsigned>(ui::radarDisplayInRangeAircraftCount()),
                    enrich_routes ? 1 : 0, ESP.getFreeHeap());
    } else if (on_radar && config::kRadarSweepTraceDebug && process_ms >= 5) {
      Serial.printf("[sweep] fetch_ready process_ms=%lu ac=%u\n", process_ms,
                    static_cast<unsigned>(services::adsb::aircraftCount()));
    } else if (on_radar && config::kRadarResumeDebug) {
      Serial.printf("[fetch] done ac=%u ac_in=%u full_draw=%d proc_ms=%lu\n",
                    static_cast<unsigned>(services::adsb::aircraftCount()),
                    static_cast<unsigned>(ui::radarDisplayInRangeAircraftCount()),
                    g_radar_full_draw_pending ? 1 : 0, process_ms);
    }
    if (g_radar_full_draw_pending && g_screen == AppScreen::Radar && g_radar_visible) {
      completeDeferredRadarDraw();
      if (config::kRadarResumeDebug && g_radar_full_draw_pending) {
        static unsigned long s_last_ac_skip_log_ms = 0;
        const unsigned long ac_now = millis();
        if (ac_now - s_last_ac_skip_log_ms >= 2000UL) {
          logRadarDebugState("ac_refresh_skip");
          s_last_ac_skip_log_ms = ac_now;
        }
      }
    } else if (on_radar) {
      ui::radarDisplayRefreshAircraft();
      if (config::kRadarSweepTraceDebug) {
        Serial.println("[sweep] aircraft_refresh noted");
      }
    } else if (on_detail) {
      if (!detailDrawBlocked() && !heapBlocksPanel()) {
        PanelSession panel(tft);
        ui::flightDetailRefresh();
      }
    }
  }

  completeDeferredRadarDraw();

  if (g_flight_detail_draw_pending && on_detail && !detailDrawBlocked()) {
    showFlightDetail();
  }

  tryTlsWifiRefresh(now);

  if (now - g_last_adsb_fetch_ms < adsbFetchPollIntervalMs(on_detail)) {
    return;
  }
  if (services::adsb::rateLimitBackoffActive(now)) {
    logFetchDefer("adsb.fi rate limited");
    return;
  }
  if (services::adsb::fetchInProgress()) {
    return;
  }
  if (services::https::busy()) {
    logFetchDefer("https busy");
    return;
  }
  if (on_detail && services::route::detailAdsbFetchPaused()) {
    logFetchDefer("detail route active");
    return;
  }
  if (!adsbDnsReady(now)) {
    logFetchDefer("DNS not ready");
    return;
  }
  if (!services::https::heapReadyForAdsb()) {
    if (on_detail) {
      ui::flightDetailReleaseSprite();
    }
    static unsigned long s_last_heap_recover_ms = 0;
    static uint8_t s_heap_recover_attempts = 0;
    static uint8_t s_stuck_recycles = 0;
    if ((on_radar || prefetch || on_detail) && now - s_last_heap_recover_ms >= 5000UL) {
      s_last_heap_recover_ms = now;
      releaseHttpsPressureMemory();
      ++s_heap_recover_attempts;
      if (s_heap_recover_attempts >= 3 && (on_radar || on_detail)) {
        ui::radarDisplayReleasePressureSprites();
        Serial.printf("[fetch] sprite_release free=%u max_blk=%u\n", ESP.getFreeHeap(),
                      ESP.getMaxAllocHeap());
      }
      if (s_heap_recover_attempts >= 5 && canRecycleWifiForTlsMemory(now)) {
        // Last resort: recycling WiFi can only reclaim TLS/TCP memory. If the
        // fragmentation is held by something else (e.g. a stray long-lived
        // allocation splitting the heap), repeated recycles change nothing and
        // the device sits frozen forever — reboot instead; recovery takes ~15s.
        ++s_stuck_recycles;
        if (s_stuck_recycles >= 3) {
          Serial.printf("[fetch] heap unrecoverable after %u recycles free=%u max_blk=%u — restart\n",
                        s_stuck_recycles, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          delay(200);
          esp_restart();
        }
        recycleWifiForTls(now, "heap stuck — WiFi recycle");
        s_heap_recover_attempts = 0;
        s_last_heap_recover_ms = now;
        return;
      }
      if (config::kSerialTraceDebug || config::kRadarResumeDebug) {
        Serial.printf("[fetch] heap recover free=%u max_blk=%u\n", ESP.getFreeHeap(),
                      ESP.getMaxAllocHeap());
      }
    }
    if (!services::https::heapReadyForAdsb()) {
      logFetchDefer("heap low");
      return;
    }
    s_heap_recover_attempts = 0;
    s_stuck_recycles = 0;
  }

  const float fetch_km = ui::radar::adsbQueryRadiusKm();
  if (services::adsb::fetchRequest(services::map_center::latitude(),
                                   services::map_center::longitude(), fetch_km)) {
    g_last_adsb_fetch_ms = now;
    if (config::kSerialTraceDebug || config::kRadarResumeDebug) {
      const char* where = prefetch ? "prefetch" : (on_detail ? "detail" : "radar");
      Serial.printf("[fetch] queued screen=%s km=%.1f full_draw=%d heap=%u\n", where, fetch_km,
                    g_radar_full_draw_pending ? 1 : 0, ESP.getFreeHeap());
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.printf("[diag] boot reset=%s fw=%s overnight=%d radar_resume_dbg=%d diag_ivl=%lus slow>%lums\n",
                resetReasonName(), config::kFirmwareVersion, config::kOvernightPerfLog ? 1 : 0,
                config::kRadarResumeDebug ? 1 : 0, config::kDiagLogIntervalMs / 1000UL,
                config::kDiagSlowLoopMs);
  Serial.println("FlightScnr (T-Encoder Pro)");

  // The ADS-B fetch worker is pinned to core 0 so its CPU-bound mbedTLS handshake
  // never stalls the render loop on core 1. That heavy crypto, together with the
  // WiFi/lwIP tasks, can starve core 0's idle task past the 5s task-watchdog window
  // and reboot the device. We intentionally run heavy work there, so unsubscribe
  // core 0's idle task from the watchdog (core 1's loop watchdog stays active).
  disableCore0WDT();

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
  services::weather::bootLoad();
  services::tzlookup::bootLoad();
  services::offhours::bootLoad();
  services::alert::bootLoad();

  if (wifiSetupConnect()) {
    services::clock::startNtp();
    services::tzlookup::init();
    services::tzlookup::requestForMapCenter();
    services::route::init();
    services::adsb::fetchInit();
    services::weather::bootSanityCheck();
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
  tickOffHours();
  if (g_off_hours_active) {
    inputPoll();
    if (inputConsumeKnobPress() || inputConsumeScreenTap(nullptr, nullptr)) {
      g_off_hours_active = false;
      if (services::offhours::mode() == services::offhours::Mode::DisplayOff) {
        displayWake();
      }
      hardware::displayBrightnessRestore();
      g_off_hours_wake_override_until_ms = millis() + 5UL * 60UL * 1000UL;
      g_screen = AppScreen::Radar;
      if (WiFi.status() == WL_CONNECTED) {
        showRadar();
      }
      Serial.println("[offhours] manual wake (knob press, 5 min override)");
    } else {
      if (services::offhours::mode() == services::offhours::Mode::Dim) {
        tickClockDisplay();
      }
      settingsWebPoll();
      tickDiagLog();
      return;
    }
  }
  tickBootDetailsSplash();
  tickSecondaryScreenTimeout();
  tickAutoIdleClock();
  tickClockDisplay();
  tickWeather();
  services::tzlookup::tick();
  handleInput();
  settingsWebPoll();
  services::route::tickCacheFlush(millis());
  tickFlightDetailRouteEnrich();
  tickDiagLog();
  tickRadarSweepHeartbeat();
  tickRadarStallWatchdog();

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
        const unsigned long radar_loop_start = millis();
        tickRadarAnimation();
        const unsigned long after_sweep = millis();
        tickAdsbFetch();
        const unsigned long after_fetch = millis();
        if (config::kOvernightPerfLog || config::kRadarSweepTraceDebug) {
          const unsigned long total = after_fetch - radar_loop_start;
          const unsigned long sweep_ms = after_sweep - radar_loop_start;
          const unsigned long fetch_ms = after_fetch - after_sweep;
          const unsigned long stall_threshold = config::kOvernightPerfLog ? 30UL : 40UL;
          if (total >= stall_threshold) {
            Serial.printf("[perf] radar_tick total=%lums sweep=%lu fetch=%lu https=%d "
                          "fetch_busy=%d fetch_ready=%d\n",
                          total, sweep_ms, fetch_ms, services::https::busy() ? 1 : 0,
                          services::adsb::fetchInProgress() ? 1 : 0,
                          services::adsb::fetchReady() ? 1 : 0);
          }
        }
      }
    } else if (g_screen == AppScreen::FlightDetail) {
      tickAdsbFetch();
    } else if (g_auto_idle_clock &&
               (g_screen == AppScreen::Clock || g_screen == AppScreen::Weather)) {
      tickAdsbFetch();
    }
  }

  const unsigned long loop_ms = millis() - loop_start;
  if (loop_ms > g_loop_max_ms) {
    g_loop_max_ms = loop_ms;
  }
  const uint32_t free_heap = ESP.getFreeHeap();
  if (free_heap < g_diag_interval_min_heap) {
    g_diag_interval_min_heap = free_heap;
  }
  if (config::kOvernightPerfLog && config::kDiagSlowLoopMs > 0 &&
      loop_ms >= config::kDiagSlowLoopMs) {
    g_diag_slow_loops++;
    Serial.printf(
        "[perf] slow_loop ms=%lu screen=%s radar_vis=%d https=%d adsb_busy=%d wx_busy=%d "
        "tz_busy=%d full_draw=%d heap=%u max_blk=%u\n",
        loop_ms, screenName(g_screen), g_radar_visible ? 1 : 0, services::https::busy() ? 1 : 0,
        services::adsb::fetchInProgress() ? 1 : 0, services::weather::fetchInProgress() ? 1 : 0,
        services::tzlookup::lookupInProgress() ? 1 : 0,
        g_radar_full_draw_pending ? 1 : 0, free_heap, ESP.getMaxAllocHeap());
  }

  delay(1);
}
