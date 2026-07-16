#pragma once

#include <cstddef>
#include <cstdint>

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved credentials. After repeated all-SSID failures, opens
 *  the setup portal (keeps saved networks) so a new network can be added. */
bool wifiReconnect();

/** Disconnect and reconnect STA without erasing saved credentials (TLS recovery). */
bool wifiSoftRecycle();

/** Number of saved networks (0..kWifiMaxNetworks). */
uint8_t wifiNetsCount();
/** Copy saved SSID at slot index; returns false if empty/out of range. */
bool wifiNetsGetSsid(uint8_t index, char* out, size_t out_len);
/** True if this slot is temporarily demoted for the current boot/session. */
bool wifiNetsIsDemoted(uint8_t index);
/**
 * Add a new SSID or update password if SSID already exists.
 * Returns false when the store is full (and SSID is new); optional err buffer.
 */
bool wifiNetsAddOrUpdate(const char* ssid, const char* pass, char* err = nullptr,
                         size_t err_len = 0);
bool wifiNetsUpdatePassword(uint8_t index, const char* pass, char* err = nullptr,
                            size_t err_len = 0);
bool wifiNetsRemove(uint8_t index);
/** Swap two occupied slots (reorder preference). */
bool wifiNetsSwap(uint8_t index_a, uint8_t index_b);
bool wifiNetsMoveUp(uint8_t index);
bool wifiNetsMoveDown(uint8_t index);
