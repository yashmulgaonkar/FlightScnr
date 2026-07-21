#include "services/watch_reg_parse.h"

#include <cctype>
#include <cstring>

namespace services::alert {
namespace {

/** Hyphen/space-stripped uppercase key for compare (e.g. CS-TPQ -> CSTPQ). */
bool matchKey(const char* in, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return false;
  }
  out[0] = '\0';
  if (in == nullptr) {
    return false;
  }
  size_t n = 0;
  bool has_alpha = false;
  for (size_t i = 0; in[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (isspace(c) || c == '-') {
      continue;
    }
    if (!isalnum(c)) {
      out[0] = '\0';
      return false;
    }
    if (n + 1 >= out_len) {
      out[0] = '\0';
      return false;
    }
    const char u = static_cast<char>(toupper(c));
    if (isalpha(static_cast<unsigned char>(u))) {
      has_alpha = true;
    }
    out[n++] = u;
  }
  out[n] = '\0';
  // Civil marks are short but need at least one letter (N2136U, CSTPQ, GABCD…).
  return has_alpha && n >= 3 && n <= 12;
}

}  // namespace

bool isValidWatchReg(const char* reg) {
  if (reg == nullptr || reg[0] == '\0') {
    return false;
  }
  const size_t len = strlen(reg);
  if (len < 3 || len >= kWatchRegLen) {
    return false;
  }
  bool has_alpha = false;
  bool has_hyphen = false;
  for (size_t i = 0; reg[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(reg[i]);
    if (c == '-') {
      has_hyphen = true;
      continue;
    }
    if (!isupper(c) && !isdigit(c)) {
      return false;
    }
    if (isupper(c)) {
      has_alpha = true;
    }
  }
  if (!has_alpha) {
    return false;
  }
  // No leading/trailing/double hyphen.
  if (reg[0] == '-' || reg[len - 1] == '-') {
    return false;
  }
  if (has_hyphen) {
    for (size_t i = 1; reg[i] != '\0'; ++i) {
      if (reg[i] == '-' && reg[i - 1] == '-') {
        return false;
      }
    }
  }
  char key[kWatchRegLen];
  return matchKey(reg, key, sizeof(key));
}

bool normalizeWatchReg(const char* in, char* out, size_t out_len) {
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
    if (c != '-' && !isalnum(c)) {
      out[0] = '\0';
      return false;
    }
    if (n + 1 >= out_len || n >= kWatchRegLen - 1) {
      out[0] = '\0';
      return false;
    }
    out[n++] = (c == '-') ? '-' : static_cast<char>(toupper(c));
  }
  out[n] = '\0';
  return isValidWatchReg(out);
}

size_t parseWatchRegBlob(const char* blob, char dest[][kWatchRegLen], size_t max_entries) {
  size_t count = 0;
  if (blob == nullptr || blob[0] == '\0' || dest == nullptr || max_entries == 0) {
    return 0;
  }
  char token[24];
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
    if (len >= sizeof(token) || len >= kWatchRegLen) {
      start = (*end == ',') ? end + 1 : end;
      continue;
    }
    memcpy(token, start, len);
    token[len] = '\0';
    char normalized[kWatchRegLen];
    if (!normalizeWatchReg(token, normalized, sizeof(normalized))) {
      start = (*end == ',') ? end + 1 : end;
      continue;
    }
    char key[kWatchRegLen];
    if (!matchKey(normalized, key, sizeof(key))) {
      start = (*end == ',') ? end + 1 : end;
      continue;
    }
    bool dup = false;
    for (size_t i = 0; i < count; ++i) {
      char existing[kWatchRegLen];
      if (matchKey(dest[i], existing, sizeof(existing)) && strcmp(existing, key) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      strncpy(dest[count], normalized, kWatchRegLen - 1);
      dest[count][kWatchRegLen - 1] = '\0';
      ++count;
    }
    start = (*end == ',') ? end + 1 : end;
  }
  return count;
}

void rebuildWatchRegBlob(const char entries[][kWatchRegLen], size_t count, char* out,
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

bool watchRegListContains(const char entries[][kWatchRegLen], size_t count,
                          const char* registration) {
  if (registration == nullptr || registration[0] == '\0' || entries == nullptr ||
      count == 0) {
    return false;
  }
  char needle[kWatchRegLen];
  if (!matchKey(registration, needle, sizeof(needle))) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    char existing[kWatchRegLen];
    if (matchKey(entries[i], existing, sizeof(existing)) &&
        strcmp(existing, needle) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace services::alert
