#include "services/airport_lookup.h"

#include <Arduino.h>

#include <cctype>
#include <cstring>

#include "data/airports_lookup.h"

namespace services::airport {

static_assert(sizeof(data::airports::Entry) == 12, "airport Entry must be 12 bytes");
static_assert(sizeof(data::airports::IataEntry) == 8, "airport IataEntry must be 8 bytes");

namespace {

void normalizeCode(const char* in, char* out, size_t out_len) {
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

void readEntryAt(size_t index, data::airports::Entry* out) {
  memcpy_P(out, &data::airports::kAirports[index], sizeof(*out));
}

void readIataEntryAt(size_t index, data::airports::IataEntry* out) {
  memcpy_P(out, &data::airports::kIataToIcao[index], sizeof(*out));
}

int compareFixed4(const char* a, const char* b) {
  return strncmp(a, b, 4);
}

int compareProgmemIcao(const char* a, size_t index) {
  data::airports::Entry entry;
  readEntryAt(index, &entry);
  return compareFixed4(a, entry.icao);
}

int compareProgmemIata(const char* a, size_t index) {
  data::airports::IataEntry entry;
  readIataEntryAt(index, &entry);
  return compareFixed4(a, entry.iata);
}

bool findIcaoIndex(const char* icao, size_t* out_index) {
  if (icao == nullptr || icao[0] == '\0' || out_index == nullptr) {
    return false;
  }

  size_t lo = 0;
  size_t hi = data::airports::kCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int cmp = compareProgmemIcao(icao, mid);
    if (cmp == 0) {
      *out_index = mid;
      return true;
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

bool findIataIndex(const char* iata, size_t* out_index) {
  if (iata == nullptr || iata[0] == '\0' || out_index == nullptr) {
    return false;
  }

  size_t lo = 0;
  size_t hi = data::airports::kIataCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int cmp = compareProgmemIata(iata, mid);
    if (cmp == 0) {
      *out_index = mid;
      return true;
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

void readNameAt(uint32_t offset, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  size_t i = 0;
  while (i + 1 < out_len) {
    const char c = static_cast<char>(pgm_read_byte(&data::airports::kNames[offset + i]));
    out[i] = c;
    if (c == '\0') {
      return;
    }
    ++i;
  }
  out[out_len - 1] = '\0';
}

bool resolveToIcao(const char* code, char* icao_out) {
  if (icao_out == nullptr) {
    return false;
  }
  icao_out[0] = '\0';

  char normalized[5];
  normalizeCode(code, normalized, sizeof(normalized));
  if (normalized[0] == '\0') {
    return false;
  }

  if (normalized[3] != '\0') {
    memcpy(icao_out, normalized, 4);
    icao_out[4] = '\0';
    return true;
  }

  char iata_key[4] = {normalized[0], normalized[1], normalized[2], '\0'};
  size_t index = 0;
  if (!findIataIndex(iata_key, &index)) {
    return false;
  }

  data::airports::IataEntry entry;
  readIataEntryAt(index, &entry);
  memcpy(icao_out, entry.icao, 4);
  icao_out[4] = '\0';
  return true;
}

bool lookupNameByIcao(const char* icao, char* out, size_t out_len) {
  if (icao == nullptr || icao[3] == '\0') {
    return false;
  }

  size_t index = 0;
  if (!findIcaoIndex(icao, &index)) {
    return false;
  }

  data::airports::Entry entry;
  readEntryAt(index, &entry);
  readNameAt(entry.name_offset, out, out_len);
  return out[0] != '\0';
}

bool lookupCoordsByIcao(const char* icao, float* lat_deg, float* lon_deg) {
  if (icao == nullptr || icao[3] == '\0') {
    return false;
  }

  size_t index = 0;
  if (!findIcaoIndex(icao, &index)) {
    return false;
  }

  data::airports::Entry entry;
  readEntryAt(index, &entry);
  if (entry.lat_centi == 0 && entry.lon_centi == 0) {
    return false;
  }
  if (lat_deg != nullptr) {
    *lat_deg = static_cast<float>(entry.lat_centi) / 100.0f;
  }
  if (lon_deg != nullptr) {
    *lon_deg = static_cast<float>(entry.lon_centi) / 100.0f;
  }
  return true;
}

}  // namespace

bool lookupName(const char* code, char* out, size_t out_len) {
  if (out_len > 0) {
    out[0] = '\0';
  }

  char icao[5];
  if (!resolveToIcao(code, icao)) {
    return false;
  }
  return lookupNameByIcao(icao, out, out_len);
}

bool normalizeRouteCode(const char* code, char* icao_out, size_t icao_len) {
  if (icao_len == 0 || icao_out == nullptr) {
    return false;
  }
  icao_out[0] = '\0';
  char resolved[5];
  if (!resolveToIcao(code, resolved)) {
    return false;
  }
  strncpy(icao_out, resolved, icao_len - 1);
  icao_out[icao_len - 1] = '\0';
  return icao_out[0] != '\0';
}

bool lookupCoords(const char* code, float* lat_deg, float* lon_deg) {
  char icao[5];
  if (!resolveToIcao(code, icao)) {
    return false;
  }
  return lookupCoordsByIcao(icao, lat_deg, lon_deg);
}

}  // namespace services::airport
