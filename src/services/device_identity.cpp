#include "services/device_identity.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "config.h"

namespace services::device_identity {

namespace {

bool s_ready = false;
char s_suffix_upper[5];
char s_suffix_lower[5];
char s_ap_name[24];
char s_hostname[24];
char s_host_url[32];

void build() {
  if (s_ready) {
    return;
  }
  const uint64_t mac = ESP.getEfuseMac();
  const unsigned suffix = static_cast<unsigned>(mac & 0xFFFFu);
  snprintf(s_suffix_upper, sizeof(s_suffix_upper), "%04X", suffix);
  snprintf(s_suffix_lower, sizeof(s_suffix_lower), "%04x", suffix);
  snprintf(s_ap_name, sizeof(s_ap_name), "%s-%s", config::kPortalApNamePrefix,
           s_suffix_upper);
  snprintf(s_hostname, sizeof(s_hostname), "%s-%s", config::kPortalHostnamePrefix,
           s_suffix_lower);
  snprintf(s_host_url, sizeof(s_host_url), "%s.local", s_hostname);
  s_ready = true;
}

}  // namespace

void init() { build(); }

const char* idSuffix() {
  build();
  return s_suffix_upper;
}

const char* portalApName() {
  build();
  return s_ap_name;
}

const char* portalHostname() {
  build();
  return s_hostname;
}

const char* portalHostUrl() {
  build();
  return s_host_url;
}

}  // namespace services::device_identity
