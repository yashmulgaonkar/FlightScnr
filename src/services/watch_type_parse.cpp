#include "services/watch_type_parse.h"

#include <cctype>
#include <cstring>

namespace services::alert {

bool isValidWatchType(const char* type) {
  if (type == nullptr || type[0] == '\0') {
    return false;
  }
  const size_t len = strlen(type);
  if (len < 2 || len >= kWatchTypeLen) {
    return false;
  }
  for (size_t i = 0; type[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(type[i]);
    if (!isupper(c) && !isdigit(c)) {
      return false;
    }
  }
  return true;
}

bool normalizeWatchType(const char* in, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return false;
  }
  out[0] = '\0';
  if (in == nullptr) {
    return false;
  }
  size_t n = 0;
  for (size_t i = 0; in[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (isspace(c)) {
      continue;
    }
    if (!isalnum(c)) {
      out[0] = '\0';
      return false;
    }
    if (n + 1 >= out_len || n >= kWatchTypeLen - 1) {
      // Too long for an ICAO type designator (e.g. marketing "A330743").
      out[0] = '\0';
      return false;
    }
    out[n++] = static_cast<char>(toupper(c));
  }
  out[n] = '\0';
  return isValidWatchType(out);
}

size_t parseWatchTypeBlob(const char* blob, char dest[][kWatchTypeLen], size_t max_entries) {
  size_t count = 0;
  if (blob == nullptr || blob[0] == '\0' || dest == nullptr || max_entries == 0) {
    return 0;
  }
  // Scratch larger than ICAO type so over-long / marketing tokens can be rejected.
  char token[16];
  const char* start = blob;
  while (*start != '\0' && count < max_entries) {
    while (*start == ',' || isspace(static_cast<unsigned char>(*start))) {
      ++start;
    }
    if (*start == '\0') {
      break;
    }
    const char* end = start;
    while (*end != '\0' && *end != ',') {
      ++end;
    }
    const size_t len = static_cast<size_t>(end - start);
    if (len == 0) {
      start = end;
      continue;
    }
    size_t copy_len = len;
    if (copy_len >= sizeof(token)) {
      copy_len = sizeof(token) - 1;
    }
    memcpy(token, start, copy_len);
    token[copy_len] = '\0';
    // Raw token longer than an ICAO designator cannot be valid (avoid truncating
    // "A330-743" / "A330743" into a false positive like "A330").
    if (len >= kWatchTypeLen) {
      start = (*end == ',') ? end + 1 : end;
      continue;
    }
    char normalized[kWatchTypeLen];
    if (!normalizeWatchType(token, normalized, sizeof(normalized))) {
      start = (*end == ',') ? end + 1 : end;
      continue;
    }
    bool dup = false;
    for (size_t i = 0; i < count; ++i) {
      if (strcmp(dest[i], normalized) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      strncpy(dest[count], normalized, kWatchTypeLen - 1);
      dest[count][kWatchTypeLen - 1] = '\0';
      ++count;
    }
    start = (*end == ',') ? end + 1 : end;
  }
  return count;
}

void rebuildWatchTypeBlob(const char entries[][kWatchTypeLen], size_t count, char* out,
                          size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (entries == nullptr || count == 0) {
    return;
  }
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) {
      if (used + 1 < out_len) {
        out[used++] = ',';
        out[used] = '\0';
      } else {
        break;
      }
    }
    const size_t n = strlen(entries[i]);
    if (used + n >= out_len) {
      break;
    }
    memcpy(out + used, entries[i], n);
    used += n;
    out[used] = '\0';
  }
}

bool watchTypeListContains(const char entries[][kWatchTypeLen], size_t count,
                           const char* type) {
  if (type == nullptr || type[0] == '\0' || entries == nullptr || count == 0) {
    return false;
  }
  char normalized[kWatchTypeLen];
  if (!normalizeWatchType(type, normalized, sizeof(normalized))) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(entries[i], normalized) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace services::alert
