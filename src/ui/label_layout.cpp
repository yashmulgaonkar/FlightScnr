#include "ui/label_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ui::label_layout {
namespace {

/** (right of symbol, vertical alignment) for every slot. */
struct SlotDef {
  bool on_right;
  int8_t valign;  /** -1 above the center line, 0 centered, +1 below */
};

constexpr SlotDef kSlots[kSlotCount] = {
    {true, 0}, {false, 0}, {true, -1}, {true, 1}, {false, -1}, {false, 1},
};

// Slot indices are absolute, never preference-relative: a target drifting
// across the center line must not flip its label to the other side just
// because its preferred order changed. Preference only decides which
// candidates are tried first, and so how ties break.
constexpr uint8_t kOrderRight[kSlotCount] = {0, 1, 2, 3, 4, 5};
constexpr uint8_t kOrderLeft[kSlotCount] = {1, 0, 4, 5, 2, 3};

/** key -> slot/tier, kept across resolves so labels stay put. */
struct Memory {
  uint32_t key = 0;  /** 0 = empty entry */
  uint8_t slot = 0;
  uint8_t tier = kTierFull;
  uint8_t stale = 0;  /** resolves since this key was last seen */
};

constexpr size_t kMemorySize = kMaxTargets + 32;
Memory s_memory[kMemorySize];

uint32_t s_result_key[kMaxTargets];
Placement s_result[kMaxTargets];
size_t s_result_count = 0;

// Scratch for resolve(). Static rather than stack: this runs on the display
// task, which shares a 16 KB stack with the sprite rebuild path.
uint8_t s_order[kMaxTargets];
uint8_t s_rank[kMaxTargets];
Placement s_chosen[kMaxTargets];
Box s_scratch_boxes[kMaxTargets];

uint32_t s_signature = 0;
bool s_valid = false;

uint32_t fnvMix(uint32_t h, uint32_t v) {
  h ^= v;
  h *= 16777619u;
  return h;
}

Memory* memoryFind(uint32_t key) {
  if (key == 0) {
    return nullptr;
  }
  for (size_t m = 0; m < kMemorySize; ++m) {
    if (s_memory[m].key == key) {
      return &s_memory[m];
    }
  }
  return nullptr;
}

void memoryAge() {
  for (size_t m = 0; m < kMemorySize; ++m) {
    if (s_memory[m].key == 0) {
      continue;
    }
    if (s_memory[m].stale >= kEvictAfterResolves) {
      s_memory[m].key = 0;
    } else {
      ++s_memory[m].stale;
    }
  }
}

void memoryStore(uint32_t key, uint8_t slot, uint8_t tier) {
  if (key == 0) {
    return;
  }
  Memory* hit = memoryFind(key);
  if (hit != nullptr) {
    hit->slot = slot;
    hit->tier = tier;
    hit->stale = 0;
    return;
  }

  // Reuse an empty entry if there is one, else the entry unseen the longest.
  size_t victim = 0;
  int victim_rank = -1;
  for (size_t m = 0; m < kMemorySize; ++m) {
    const int rank = s_memory[m].key == 0 ? 0x7FFF : s_memory[m].stale;
    if (rank > victim_rank) {
      victim = m;
      victim_rank = rank;
    }
  }
  s_memory[victim].key = key;
  s_memory[victim].slot = slot;
  s_memory[victim].tier = tier;
  s_memory[victim].stale = 0;
}

/** Overlap of `a` (inflated by `pad` on every side) with `b`, in px^2. */
int overlapArea(const Box& a, const Box& b, int pad) {
  const int ox = std::min(static_cast<int>(a.x) + a.w + pad,
                          static_cast<int>(b.x) + b.w) -
                 std::max(static_cast<int>(a.x) - pad, static_cast<int>(b.x));
  if (ox <= 0) {
    return 0;
  }
  const int oy = std::min(static_cast<int>(a.y) + a.h + pad,
                          static_cast<int>(b.y) + b.h) -
                 std::max(static_cast<int>(a.y) - pad, static_cast<int>(b.y));
  if (oy <= 0) {
    return 0;
  }
  return ox * oy;
}

Box symbolBox(const Target& t, const Geometry& geom) {
  Box b;
  b.x = static_cast<int16_t>(t.x - geom.symbol_half);
  b.y = static_cast<int16_t>(t.y - geom.symbol_half);
  b.w = static_cast<int16_t>(geom.symbol_half * 2);
  b.h = static_cast<int16_t>(geom.symbol_half * 2);
  return b;
}

/**
 * Slide `box` inside the visible circle and return how far it had to move.
 *
 * The panel is round, so clamping to the square framebuffer would leave rim
 * tags hanging over the bezel. Horizontal room is the chord width at the box
 * edge furthest from center, which is what actually limits a multi-line block.
 */
int clampToCircle(Box& box, const Geometry& geom) {
  const int cx = geom.center_x;
  const int cy = geom.center_y;
  const int r = geom.visible_radius;

  int ny = box.y;
  if (box.h >= r * 2) {
    ny = cy - box.h / 2;
  } else {
    if (ny < cy - r) {
      ny = cy - r;
    }
    if (ny + box.h > cy + r) {
      ny = cy + r - box.h;
    }
  }

  const int dy = std::max(std::abs(ny - cy), std::abs(ny + box.h - cy));
  const int half_w =
      dy >= r ? 0
              : static_cast<int>(std::sqrt(static_cast<float>(r * r - dy * dy)));

  int nx = box.x;
  if (box.w >= half_w * 2) {
    nx = cx - box.w / 2;
  } else {
    if (nx < cx - half_w) {
      nx = cx - half_w;
    }
    if (nx + box.w > cx + half_w) {
      nx = cx + half_w - box.w;
    }
  }

  const int moved = std::abs(nx - box.x) + std::abs(ny - box.y);
  box.x = static_cast<int16_t>(nx);
  box.y = static_cast<int16_t>(ny);
  return moved;
}

void tierSize(const Target& t, uint8_t tier, int16_t* w, int16_t* h) {
  if (tier == kTierShort) {
    *w = t.short_w;
    *h = t.short_h;
  } else {
    *w = t.full_w;
    *h = t.full_h;
  }
}

/** Unclamped box for one slot; the caller clamps and pays the displacement. */
Box rawBoxFor(const Target& t, uint8_t slot, uint8_t tier, const Geometry& geom) {
  const SlotDef& def = kSlots[slot];
  Box box;
  tierSize(t, tier, &box.w, &box.h);
  box.x = def.on_right
              ? static_cast<int16_t>(t.x + geom.symbol_half + geom.gap)
              : static_cast<int16_t>(t.x - geom.symbol_half - geom.gap - box.w);
  if (def.valign < 0) {
    box.y = static_cast<int16_t>(t.y - box.h);
  } else if (def.valign > 0) {
    box.y = t.y;
  } else {
    box.y = static_cast<int16_t>(t.y - box.h / 2);
  }
  return box;
}

/** Overlap with already-placed tags only — this is what makes text unreadable. */
int labelOverlap(const Box& box, const Box* others, size_t others_count) {
  int total = 0;
  for (size_t o = 0; o < others_count; ++o) {
    total += overlapArea(box, others[o], 0);
  }
  return total;
}

int coveragePct(const Box& box, const Box* others, size_t others_count) {
  const int area = box.w * box.h;
  if (area <= 0) {
    return 100;
  }
  return labelOverlap(box, others, others_count) * 100 / area;
}

int cost(const Box& box, const Box* others, size_t others_count,
         const Target* targets, size_t count, const Geometry& geom, int clamp_moved) {
  int total = clamp_moved * kClampPenaltyPerPx;
  for (size_t o = 0; o < others_count; ++o) {
    total += overlapArea(box, others[o], kClearancePx);
  }
  // Covering another target's icon reads as missing traffic, so it costs too.
  for (size_t i = 0; i < count; ++i) {
    total += overlapArea(box, symbolBox(targets[i], geom), 0);
  }
  return total;
}

struct SlotPick {
  uint8_t slot = 0;
  Box box{};
};

/**
 * Cheapest slot for one tier.
 *
 * `*out_best` is the unbiased winner; `*out_pick` is what the label actually
 * takes, which is the remembered slot whenever that stays within the switch
 * margin. Both come back because the caller needs the unbiased answer to tell
 * a merely-worse sticky slot from an unreadable one.
 */
void bestSlot(const Target& t, uint8_t tier, const Box* others, size_t others_count,
              const Target* targets, size_t count, const Geometry& geom,
              const Memory* mem, SlotPick* out_best, SlotPick* out_pick) {
  const uint8_t* order =
      t.x < geom.center_x ? kOrderRight : kOrderLeft;  // label toward the center

  SlotPick best;
  best.slot = order[0];
  int best_cost = 0;
  bool have_best = false;

  for (uint8_t i = 0; i < kSlotCount; ++i) {
    const uint8_t slot = order[i];
    Box box = rawBoxFor(t, slot, tier, geom);
    const int moved = clampToCircle(box, geom);
    const int c = cost(box, others, others_count, targets, count, geom, moved);
    if (!have_best || c < best_cost) {
      best.slot = slot;
      best.box = box;
      best_cost = c;
      have_best = true;
    }
    if (c == 0) {
      break;
    }
  }

  *out_best = best;
  *out_pick = best;

  if (mem == nullptr || mem->slot >= kSlotCount || mem->slot == best.slot) {
    return;
  }

  int16_t tw = 0;
  int16_t th = 0;
  tierSize(t, tier, &tw, &th);
  const int margin = tw * th * kSwitchMarginPct / 100;
  Box keep = rawBoxFor(t, mem->slot, tier, geom);
  const int keep_moved = clampToCircle(keep, geom);
  const int keep_cost =
      cost(keep, others, others_count, targets, count, geom, keep_moved);
  if (keep_cost <= best_cost + margin) {
    out_pick->slot = mem->slot;
    out_pick->box = keep;
  }
}

Placement placementOf(const SlotPick& pick, uint8_t tier) {
  Placement out;
  out.box = tier == kTierHidden ? Box{} : pick.box;
  out.slot = pick.slot;
  out.tier = tier;
  out.on_right = kSlots[pick.slot].on_right;
  return out;
}

/** Pick slot and tier: full if readable, else callsign-only, else hidden. */
Placement choose(const Target& t, const Box* others, size_t others_count,
                 const Target* targets, size_t count, const Geometry& geom) {
  const Memory* mem = memoryFind(t.key);
  // No history yet: treat as full so the first placement can use the wider limit.
  const uint8_t prev_tier = mem != nullptr ? mem->tier : kTierFull;
  const int full_limit = coverageLimitPct(prev_tier, kTierFull);
  const int short_limit = coverageLimitPct(prev_tier, kTierShort);

  SlotPick best;
  SlotPick pick;
  bestSlot(t, kTierFull, others, others_count, targets, count, geom, mem, &best, &pick);

  // Hysteresis must not win an argument that makes the tag unreadable: when the
  // sticky slot is smothered while the best one is clear, move rather than
  // degrade.
  if (pick.slot != best.slot && coveragePct(pick.box, others, others_count) > full_limit &&
      coveragePct(best.box, others, others_count) <= full_limit) {
    pick = best;
  }

  if (pick.box.empty() || coveragePct(pick.box, others, others_count) <= full_limit) {
    return placementOf(pick, kTierFull);
  }

  if (t.short_w > 0 && t.short_h > 0) {
    SlotPick short_best;
    SlotPick short_pick;
    bestSlot(t, kTierShort, others, others_count, targets, count, geom, mem, &short_best,
             &short_pick);
    if (coveragePct(short_pick.box, others, others_count) > short_limit &&
        coveragePct(short_best.box, others, others_count) <= short_limit) {
      short_pick = short_best;
    }
    if (coveragePct(short_pick.box, others, others_count) <= short_limit) {
      return placementOf(short_pick, kTierShort);
    }
  }

  return placementOf(pick, kTierHidden);
}

uint32_t signatureOf(const Target* targets, size_t count, const Geometry& geom) {
  uint32_t sig = fnvMix(2166136261u, static_cast<uint32_t>(count));
  sig = fnvMix(sig, static_cast<uint32_t>(geom.center_x) << 16 |
                        static_cast<uint16_t>(geom.center_y));
  sig = fnvMix(sig, static_cast<uint32_t>(geom.visible_radius) << 16 |
                        static_cast<uint16_t>(geom.symbol_half));
  sig = fnvMix(sig, static_cast<uint32_t>(geom.gap));
  for (size_t i = 0; i < count; ++i) {
    const Target& t = targets[i];
    sig = fnvMix(sig, t.key);
    sig = fnvMix(sig, static_cast<uint32_t>(t.x) << 16 | static_cast<uint16_t>(t.y));
    sig = fnvMix(sig, static_cast<uint32_t>(t.full_w) << 16 |
                          static_cast<uint16_t>(t.full_h));
    sig = fnvMix(sig, static_cast<uint32_t>(t.short_w) << 16 |
                          static_cast<uint16_t>(t.short_h));
  }
  return sig;
}

}  // namespace

void reset() {
  for (size_t m = 0; m < kMemorySize; ++m) {
    s_memory[m] = Memory{};
  }
  s_result_count = 0;
  s_signature = 0;
  s_valid = false;
}

uint32_t hashKey(const char* s) {
  uint32_t h = 2166136261u;
  if (s != nullptr) {
    while (*s != '\0') {
      h ^= static_cast<uint8_t>(*s++);
      h *= 16777619u;
    }
  }
  return h != 0 ? h : 1u;
}

void uniquifyKeys(uint32_t* keys, const int32_t* sort_a, const int32_t* sort_b,
                  size_t count) {
  if (keys == nullptr || count == 0) {
    return;
  }
  if (count > kMaxTargets) {
    count = kMaxTargets;
  }

  // Copy first: rewriting keys[i] in place would hide later collisions.
  static uint32_t base[kMaxTargets];
  for (size_t i = 0; i < count; ++i) {
    base[i] = keys[i];
  }
  for (size_t i = 0; i < count; ++i) {
    int rank = 0;
    bool collided = false;
    const int32_t ia = sort_a != nullptr ? sort_a[i] : 0;
    const int32_t ib = sort_b != nullptr ? sort_b[i] : 0;
    for (size_t j = 0; j < count; ++j) {
      if (j == i || base[j] != base[i]) {
        continue;
      }
      collided = true;
      const int32_t ja = sort_a != nullptr ? sort_a[j] : 0;
      const int32_t jb = sort_b != nullptr ? sort_b[j] : 0;
      const bool j_before = ja < ia || (ja == ia && (jb < ib || (jb == ib && j < i)));
      if (j_before) {
        ++rank;
      }
    }
    if (!collided) {
      continue;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "dup:%lu:%d", static_cast<unsigned long>(base[i]), rank);
    keys[i] = hashKey(buf);
  }
}

void resolve(const Target* targets, size_t count, const Geometry& geom) {
  if (targets == nullptr) {
    count = 0;
  }
  if (count > kMaxTargets) {
    count = kMaxTargets;
  }

  const uint32_t sig = signatureOf(targets, count, geom);
  if (s_valid && sig == s_signature) {
    return;
  }
  s_signature = sig;
  s_valid = true;
  s_result_count = 0;

  memoryAge();
  if (count == 0) {
    return;
  }

  // Priority order, tie-broken on identity: the aircraft array is rebuilt and
  // reordered every poll, so equal-distance ties would otherwise flip the
  // processing order (and with it, who gets the good slot) between updates.
  for (size_t i = 0; i < count; ++i) {
    s_order[i] = static_cast<uint8_t>(i);
  }
  for (size_t i = 1; i < count; ++i) {
    const uint8_t key_idx = s_order[i];
    const Target& key_t = targets[key_idx];
    size_t j = i;
    while (j > 0) {
      const Target& prev = targets[s_order[j - 1]];
      const bool prev_after = prev.priority > key_t.priority ||
                              (prev.priority == key_t.priority && prev.key > key_t.key);
      if (!prev_after) {
        break;
      }
      s_order[j] = s_order[j - 1];
      --j;
    }
    s_order[j] = key_idx;
  }
  for (size_t i = 0; i < count; ++i) {
    s_rank[s_order[i]] = static_cast<uint8_t>(i);
  }

  // Pass 0: greedy in priority order, so a label only ever yields to one that
  // matters more than it does.
  size_t placed_count = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t idx = s_order[i];
    s_chosen[idx] =
        choose(targets[idx], s_scratch_boxes, placed_count, targets, count, geom);
    if (s_chosen[idx].tier != kTierHidden) {
      s_scratch_boxes[placed_count++] = s_chosen[idx].box;
    }
  }

  // Re-score against higher-priority tags only. Including a farther label here
  // let it land on a closer one and degrade or hide that closer tag.
  for (int sweep = 0; sweep < kRelaxSweeps; ++sweep) {
    bool moved = false;
    for (size_t i = 0; i < count; ++i) {
      const size_t idx = s_order[i];
      size_t others_count = 0;
      for (size_t j = 0; j < count; ++j) {
        if (s_rank[j] < s_rank[idx] && s_chosen[j].tier != kTierHidden) {
          s_scratch_boxes[others_count++] = s_chosen[j].box;
        }
      }
      const Placement next =
          choose(targets[idx], s_scratch_boxes, others_count, targets, count, geom);
      if (next.slot != s_chosen[idx].slot || next.tier != s_chosen[idx].tier) {
        moved = true;
      }
      s_chosen[idx] = next;
    }
    if (!moved) {
      break;
    }
  }

  for (size_t i = 0; i < count; ++i) {
    memoryStore(targets[i].key, s_chosen[i].slot, s_chosen[i].tier);
    s_result_key[s_result_count] = targets[i].key;
    s_result[s_result_count] = s_chosen[i];
    ++s_result_count;
  }
}

const Placement* find(uint32_t key) {
  if (key == 0) {
    return nullptr;
  }
  for (size_t i = 0; i < s_result_count; ++i) {
    if (s_result_key[i] == key) {
      return &s_result[i];
    }
  }
  return nullptr;
}

}  // namespace ui::label_layout
