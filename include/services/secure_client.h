#pragma once

#include <WiFiClientSecure.h>

namespace services::https {

/**
 * WiFiClientSecure that cannot close a descriptor it no longer owns.
 *
 * In arduino-esp32 2.0.x, WiFiClientSecure::stop() ends in stop_ssl_socket(),
 * which memset()s the whole context and leaves `socket == 0` instead of -1.
 * Any further stop() (HTTPClient::end() followed by client.stop(), then the
 * destructor's own stop()) sees `socket >= 0` and calls close(0).
 *
 * Descriptor 0 is not stdin here: with USB-CDC console the first open file gets
 * it, so the stray close() slams shut whichever LittleFS file is open at that
 * moment (route cache flush). The visible effects were EBADF in the middle of
 * a read, short writes, a read loop that never ended, and `lfs_mlist_isopen`
 * asserts that reboot the board.
 *
 * A real lwIP socket is never descriptor 0, so a 0 here is always the stale
 * value. stop() and the destructor turn it back into -1 first; a live socket
 * is untouched and is closed exactly as before.
 */
class SecureClient : public WiFiClientSecure {
 public:
  using WiFiClientSecure::WiFiClientSecure;

  void stop() override {
    clearStaleSocket();
    WiFiClientSecure::stop();
  }

  ~SecureClient() { clearStaleSocket(); }

 private:
  void clearStaleSocket() {
    if (sslclient != nullptr && sslclient->socket == 0) {
      sslclient->socket = -1;
    }
  }
};

}  // namespace services::https
