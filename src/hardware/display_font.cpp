#include "hardware/display_font.h"

#include <algorithm>
#include <cstdlib>

#include "fonts/MontserratBold10pt4aa.h"
#include "fonts/MontserratBold11pt4aa.h"
#include "fonts/MontserratBold12pt4aa.h"
#include "fonts/MontserratBold14pt4aa.h"
#include "fonts/MontserratBold18pt4aa.h"
#include "fonts/MontserratBold24pt4aa.h"
#include "fonts/MontserratBold36pt4aa.h"
#include "fonts/MontserratBold8pt4aa.h"
#include "fonts/MontserratBold9pt4aa.h"

namespace {

const AaFont* kFonts[] = {
    &MontserratBold8pt4aa,
    &MontserratBold9pt4aa,
    &MontserratBold10pt4aa,
    &MontserratBold11pt4aa,
    &MontserratBold12pt4aa,
    &MontserratBold14pt4aa,
    &MontserratBold18pt4aa,
};

constexpr size_t kFontCount = sizeof(kFonts) / sizeof(kFonts[0]);

int absDiff(int a, int b) { return std::abs(a - b); }

}  // namespace

UiTextStyle displayFontTitle() { return UiTextStyle{&MontserratBold18pt4aa}; }

UiTextStyle displayFontBody() { return UiTextStyle{&MontserratBold12pt4aa}; }

UiTextStyle displayFontDetail() { return UiTextStyle{&MontserratBold10pt4aa}; }

UiTextStyle displayFontCardinal() { return UiTextStyle{&MontserratBold12pt4aa}; }

UiTextStyle displayFontScale() { return UiTextStyle{&MontserratBold9pt4aa}; }

UiTextStyle displayFontTag() { return UiTextStyle{&MontserratBold11pt4aa}; }

UiTextStyle displayFontClockTime() { return UiTextStyle{&MontserratBold36pt4aa}; }

UiTextStyle displayFontClockAmPm() { return UiTextStyle{&MontserratBold24pt4aa}; }

UiTextStyle displayFontClockDate() { return UiTextStyle{&MontserratBold18pt4aa}; }

UiTextStyle displayFontPickForHeight(PlaneGfx& gfx, int target_px, size_t lo_index,
                                     size_t hi_index) {
  if (kFontCount == 0) {
    return displayFontBody();
  }
  lo_index = std::min(lo_index, kFontCount - 1);
  hi_index = std::min(hi_index, kFontCount - 1);
  if (hi_index < lo_index) {
    std::swap(lo_index, hi_index);
  }

  size_t best = lo_index;
  int best_diff = absDiff(displayFontHeight(gfx, UiTextStyle{kFonts[best]}), target_px);
  for (size_t i = lo_index; i <= hi_index; ++i) {
    const int diff =
        absDiff(displayFontHeight(gfx, UiTextStyle{kFonts[i]}), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = i;
    }
  }
  return UiTextStyle{kFonts[best]};
}

void displayFontApply(PlaneGfx& gfx, UiTextStyle style) {
  gfx.setTextWrap(false);
  gfx.setTextSize(1);
  gfx.setFont(style.font);
}

int displayFontHeight(PlaneGfx& gfx, UiTextStyle style) {
  displayFontApply(gfx, style);
  return gfx.fontHeight();
}

int displayFontWidth(PlaneGfx& gfx, UiTextStyle style, const char* text) {
  displayFontApply(gfx, style);
  return gfx.textWidth(text);
}
