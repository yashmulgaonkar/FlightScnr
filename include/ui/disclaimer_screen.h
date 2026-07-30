#pragma once

#include <cstdint>

namespace ui {

/** Full-screen round disclaimer with Accept control near the bottom. */
void disclaimerScreenDraw();

/** True if (x,y) is inside the Accept button hit target. */
bool disclaimerScreenHitAccept(int16_t x, int16_t y);

}  // namespace ui
