#include "services/airline_lookup.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace services::airline {

namespace {

struct OverrideEntry {
  const char* code;
  const char* name;
};

/** Regional / brand overrides (plane-tracker utilities/airlines.py). */
constexpr OverrideEntry kOverrides[] = {
    {"ENY", "American Eagle"},
    {"JIA", "American Eagle"},
    {"PDT", "American Eagle"},
    {"PSA", "American Eagle"},
    {"GJS", "United Express"},
    {"CPZ", "United Express"},
    {"ASH", "United Express"},
    {"G7", "United Express"},
    {"UCA", "United Express"},
    {"EDV", "Delta Connection"},
    {"ASQ", "Delta Connection"},
    {"RPA", "Republic Airways"},
    {"SKW", "SkyWest Airlines"},
    {"QXE", "Horizon Air"},
    {"TIV", "Thrive Aviation"},
    {"AAL", "American Airlines"},
    {"UAL", "United Airlines"},
    {"DAL", "Delta Air Lines"},
    {"SWA", "Southwest Airlines"},
    {"JBU", "JetBlue Airways"},
    {"BAW", "British Airways"},
    {"VIR", "Virgin Atlantic"},
    {"VLG", "Vueling"},
    {"VOI", "Volaris"},
    {"FLJ", "Flexjet"},
    {"FDX", "FedEx"},
    {"UPS", "UPS"},
    {"ETD", "Etihad Airways"},
    {"LOG", "Loganair"},
    {"DLH", "Lufthansa"},
    {"KLM", "KLM"},
    {"AFR", "Air France"},
    {"UAE", "Emirates"},
    {"QTR", "Qatar Airways"},
    {"ACA", "Air Canada"},
    {"RYR", "Ryanair"},
    {"EZS", "easyJet"},
    {"VOZ", "Virgin Australia"},
};

struct IataIcaoEntry {
  const char* iata;
  const char* icao;
};

/** Common IATA flight-prefix → ICAO operator (for logo lookup). */
constexpr IataIcaoEntry kIataToIcao[] = {
    {"AA", "AAL"}, {"AS", "ASA"}, {"B6", "JBU"}, {"DL", "DAL"}, {"F9", "FFT"},
    {"HA", "HAL"}, {"NK", "NKS"}, {"UA", "UAL"}, {"WN", "SWA"}, {"WS", "WJA"},
    {"AC", "ACA"}, {"PD", "POE"}, {"BA", "BAW"}, {"AF", "AFR"}, {"LH", "DLH"},
    {"KL", "KLM"}, {"LX", "SWR"}, {"OS", "AUA"}, {"SK", "SAS"}, {"AY", "FIN"},
    {"IB", "IBE"}, {"AZ", "ITY"}, {"TP", "TAP"}, {"EI", "EIN"}, {"VY", "VLG"},
    {"U2", "EZS"}, {"FR", "RYR"}, {"DY", "NAX"}, {"WF", "WIF"}, {"VS", "VIR"},
    {"EK", "UAE"}, {"QR", "QTR"}, {"EY", "ETD"}, {"TK", "THY"}, {"SQ", "SIA"},
    {"CX", "CPA"}, {"NH", "ANA"}, {"JL", "JAL"}, {"KE", "KAL"}, {"OZ", "AAR"},
    {"MU", "CES"}, {"CZ", "CSN"}, {"CA", "CCA"}, {"AI", "AIC"}, {"QF", "QFA"},
    {"NZ", "ANZ"}, {"VA", "VOZ"}, {"AM", "AMX"}, {"LA", "LAN"}, {"AV", "AVA"},
    {"FX", "FDX"}, {"5X", "UPS"}, {"LM", "LOG"},
};

const char* iataFromIcao(const char* icao) {
  if (icao == nullptr || strlen(icao) != 3) {
    return nullptr;
  }
  for (const IataIcaoEntry& e : kIataToIcao) {
    if (strcmp(e.icao, icao) == 0) {
      return e.iata;
    }
  }
  return nullptr;
}

const char* icaoFromIata(const char* iata) {
  if (iata == nullptr || strlen(iata) != 2) {
    return nullptr;
  }
  for (const IataIcaoEntry& e : kIataToIcao) {
    if (strcmp(e.iata, iata) == 0) {
      return e.icao;
    }
  }
  return nullptr;
}

const char* overrideName(const char* code) {
  if (code == nullptr) {
    return nullptr;
  }
  for (const OverrideEntry& e : kOverrides) {
    if (strcmp(e.code, code) == 0) {
      return e.name;
    }
  }
  return nullptr;
}

void normalizeCallsign(const char* in, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (in == nullptr) {
    return;
  }
  while (*in == ' ') {
    ++in;
  }
  size_t n = 0;
  while (in[n] != '\0' && n + 1 < out_len) {
    const unsigned char c = static_cast<unsigned char>(in[n]);
    out[n] = static_cast<char>(islower(c) ? toupper(c) : in[n]);
    ++n;
  }
  while (n > 0 && out[n - 1] == ' ') {
    --n;
  }
  out[n] = '\0';
}

bool isIcaoRadioCallsign(const char* cs) {
  if (cs == nullptr || strlen(cs) < 4) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!isupper(static_cast<unsigned char>(cs[i]))) {
      return false;
    }
  }
  for (size_t i = 3; cs[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(cs[i]);
    if (!isupper(c) && !isdigit(c)) {
      return false;
    }
  }
  return true;
}

bool isIataRadioCallsign(const char* cs) {
  if (cs == nullptr || strlen(cs) < 3) {
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    if (!isupper(static_cast<unsigned char>(cs[i]))) {
      return false;
    }
  }
  for (size_t i = 2; cs[i] != '\0'; ++i) {
    if (!isdigit(static_cast<unsigned char>(cs[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool isNNumber(const char* cs) {
  if (cs == nullptr || cs[0] != 'N') {
    return false;
  }
  for (size_t i = 1; cs[i] != '\0'; ++i) {
    if (!isdigit(static_cast<unsigned char>(cs[i])) &&
        !isupper(static_cast<unsigned char>(cs[i]))) {
      return false;
    }
  }
  return cs[1] != '\0';
}

bool lookupByCode(const char* code, char* out, size_t out_len) {
  if (out_len > 0) {
    out[0] = '\0';
  }
  if (code == nullptr || code[0] == '\0') {
    return false;
  }
  const char* name = overrideName(code);
  if (name == nullptr) {
    return false;
  }
  strncpy(out, name, out_len - 1);
  out[out_len - 1] = '\0';
  return true;
}

void resolveFromCallsign(const char* callsign, bool has_flight_field, char* out,
                         size_t out_len) {
  if (out_len > 0) {
    out[0] = '\0';
  }
  if (!has_flight_field || callsign == nullptr || callsign[0] == '\0') {
    return;
  }

  char normalized[16];
  normalizeCallsign(callsign, normalized, sizeof(normalized));
  if (normalized[0] == '\0') {
    return;
  }

  if (isNNumber(normalized)) {
    strncpy(out, "Private", out_len - 1);
    out[out_len - 1] = '\0';
    return;
  }

  char prefix[4];
  if (isIcaoRadioCallsign(normalized)) {
    memcpy(prefix, normalized, 3);
    prefix[3] = '\0';
    if (lookupByCode(prefix, out, out_len)) {
      return;
    }
  }

  if (isIataRadioCallsign(normalized)) {
    memcpy(prefix, normalized, 2);
    prefix[2] = '\0';
    if (lookupByCode(prefix, out, out_len)) {
      return;
    }
    const char* icao = icaoFromIata(prefix);
    if (icao != nullptr) {
      lookupByCode(icao, out, out_len);
    }
  }
}

bool resolveIcaoFromCallsign(const char* callsign, bool has_flight_field, char* out,
                             size_t out_len) {
  if (out_len > 0) {
    out[0] = '\0';
  }
  if (!has_flight_field || callsign == nullptr || callsign[0] == '\0') {
    return false;
  }

  char normalized[16];
  normalizeCallsign(callsign, normalized, sizeof(normalized));
  if (normalized[0] == '\0' || isNNumber(normalized)) {
    return false;
  }

  if (isIcaoRadioCallsign(normalized)) {
    if (out_len < 4) {
      return false;
    }
    memcpy(out, normalized, 3);
    out[3] = '\0';
    return true;
  }

  if (isIataRadioCallsign(normalized)) {
    char iata[3];
    memcpy(iata, normalized, 2);
    iata[2] = '\0';
    const char* icao = icaoFromIata(iata);
    if (icao != nullptr && out_len >= 4) {
      strncpy(out, icao, out_len - 1);
      out[out_len - 1] = '\0';
      return true;
    }
  }
  return false;
}

bool buildFlightIataFromCallsign(const char* callsign, char* out, size_t out_len) {
  if (out == nullptr || out_len < 5 || callsign == nullptr) {
    return false;
  }
  out[0] = '\0';
  char normalized[16];
  normalizeCallsign(callsign, normalized, sizeof(normalized));
  if (strlen(normalized) < 4) {
    return false;
  }
  char icao[4] = {normalized[0], normalized[1], normalized[2], '\0'};
  const char* iata = iataFromIcao(icao);
  if (iata == nullptr) {
    return false;
  }
  const int n = snprintf(out, out_len, "%s%s", iata, normalized + 3);
  return n > 0 && static_cast<size_t>(n) < out_len;
}

bool buildCallsignApiVariant(const char* callsign, char* out, size_t out_len) {
  if (out == nullptr || out_len < 5 || callsign == nullptr) {
    return false;
  }
  out[0] = '\0';
  char normalized[16];
  normalizeCallsign(callsign, normalized, sizeof(normalized));
  if (!isIcaoRadioCallsign(normalized)) {
    return false;
  }
  const char* rest = normalized + 3;
  size_t digit_len = 0;
  while (rest[digit_len] != '\0' && isdigit(static_cast<unsigned char>(rest[digit_len]))) {
    ++digit_len;
  }
  if (digit_len < 2) {
    return false;
  }
  if (rest[digit_len] == '\0') {
    return false;
  }
  if (!isalpha(static_cast<unsigned char>(rest[digit_len]))) {
    return false;
  }
  if (rest[digit_len + 1] != '\0') {
    return false;
  }
  const int n = snprintf(out, out_len, "%.3s%.*s", normalized, static_cast<int>(digit_len), rest);
  if (n <= 0 || static_cast<size_t>(n) >= out_len) {
    return false;
  }
  return strcmp(out, normalized) != 0;
}

}  // namespace services::airline
