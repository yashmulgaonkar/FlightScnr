#include "services/aircraft_type_lookup.h"

#include <Arduino.h>

#include <cstring>

#include "data/icao_types_lookup.h"

namespace services::aircraft_type {

namespace {

void normalizeIcaoType(const char* in, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (in == nullptr) {
    return;
  }
  size_t n = 0;
  while (in[n] != '\0' && n + 1 < out_len && n < 4) {
    const unsigned char c = static_cast<unsigned char>(in[n]);
    out[n] = static_cast<char>(isupper(c) ? c : toupper(c));
    ++n;
  }
  out[n] = '\0';
}

int compareProgmemCode(const char* a, const data::icao_types::Entry* entry) {
  char code[5];
  memcpy_P(code, entry->code, sizeof(code));
  return strcmp(a, code);
}

const data::icao_types::Entry* findEntry(const char* code) {
  if (code == nullptr || code[0] == '\0') {
    return nullptr;
  }

  size_t lo = 0;
  size_t hi = data::icao_types::kCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const data::icao_types::Entry* entry = &data::icao_types::kTypes[mid];
    const int cmp = compareProgmemCode(code, entry);
    if (cmp == 0) {
      return entry;
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return nullptr;
}

void readNameAt(uint16_t offset, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  size_t i = 0;
  while (i + 1 < out_len) {
    const char c = static_cast<char>(pgm_read_byte(&data::icao_types::kNames[offset + i]));
    out[i] = c;
    if (c == '\0') {
      return;
    }
    ++i;
  }
  out[out_len - 1] = '\0';
}

}  // namespace

bool lookupDescription(const char* icao_type, char* out, size_t out_len) {
  if (out_len > 0) {
    out[0] = '\0';
  }
  char code[5];
  normalizeIcaoType(icao_type, code, sizeof(code));
  if (code[0] == '\0') {
    return false;
  }

  const data::icao_types::Entry* entry = findEntry(code);
  if (entry == nullptr) {
    return false;
  }

  uint16_t offset = 0;
  memcpy_P(&offset, &entry->name_offset, sizeof(offset));
  readNameAt(offset, out, out_len);
  return out[0] != '\0';
}

void formatRadarTagLabel(const char* icao_type, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  // Radar tags stay compact: ICAO type code only (e.g. B738), not the full name.
  normalizeIcaoType(icao_type, out, out_len);
}

}  // namespace services::aircraft_type
