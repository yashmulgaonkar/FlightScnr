#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Stable slot assignment for radar aircraft tags.
 *
 * Placing each tag independently (right of the symbol when the plane sits left
 * of center, otherwise left) stacks unreadable text wherever traffic bunches
 * up, and re-deciding from scratch on every ADS-B poll makes tags jump as
 * planes drift across the center line. This module solves all tags together:
 * a greedy pass in priority order followed by a few relaxation sweeps, with
 * each label remembering its slot so it only moves when a rival beats it by a
 * clear margin.
 *
 * Deliberately free of any graphics dependency — callers pass measured block
 * sizes in and get boxes back, which also lets the whole thing run under the
 * `native` test env.
 */
namespace ui::label_layout {

/** Upper bound on simultaneously placed tags (matches services::adsb::kMaxAircraft). */
constexpr size_t kMaxTargets = 64;

/** Candidate positions per target; see the slot table in label_layout.cpp. */
constexpr uint8_t kSlotCount = 6;

/** Detail level of a placed tag, best first. */
enum Tier : uint8_t {
  kTierFull = 0,    /** callsign + type + altitude */
  kTierShort = 1,   /** callsign only — a third the height, so it often still fits */
  kTierHidden = 2,  /** not drawn */
};

/**
 * A remembered slot keeps the label unless a rival beats it by more than this
 * percent of the block's own area. This margin is what stops two near-tied
 * slots from trading the label back and forth on every poll.
 */
constexpr int kSwitchMarginPct = 8;
/**
 * Drop to a worse tier above kHideCoveragePct of the block covered by
 * higher-priority tags. Step back up to a better tier only below
 * kUnhideCoveragePct, so a tag hovering on either boundary cannot flicker.
 */
constexpr int kHideCoveragePct = 35;
constexpr int kUnhideCoveragePct = 15;

/** Coverage allowed when choosing `candidate_tier` given the tier from last poll. */
constexpr int coverageLimitPct(uint8_t previous_tier, uint8_t candidate_tier) {
  return candidate_tier < previous_tier ? kUnhideCoveragePct : kHideCoveragePct;
}
/** Ranking pad (px) so slots that merely touch still cost something. */
constexpr int kClearancePx = 4;
/** Cost per px a candidate box is displaced by bezel clamping. */
constexpr int kClampPenaltyPerPx = 6;
/**
 * Re-pick passes after the greedy pass. Each label is scored only against
 * higher-priority tags, so a closer aircraft keeps the slot it earned and a
 * farther one can still move aside once those tags have settled.
 */
constexpr int kRelaxSweeps = 3;
/**
 * Resolves a vanished aircraft keeps its remembered slot. Resolves run once
 * per ADS-B poll (~2 s), so this rides out roughly a minute of dropouts.
 */
constexpr uint8_t kEvictAfterResolves = 40;

struct Box {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;

  bool empty() const { return w <= 0 || h <= 0; }
  bool sameAs(const Box& o) const {
    return x == o.x && y == o.y && w == o.w && h == o.h;
  }
};

/** One tag awaiting a slot. Sizes are measured by the caller. */
struct Target {
  uint32_t key = 0;      /** stable identity; survives the aircraft array being rebuilt */
  int16_t x = 0;         /** symbol center */
  int16_t y = 0;
  int16_t full_w = 0;    /** complete block */
  int16_t full_h = 0;
  int16_t short_w = 0;   /** callsign-only block; 0 disables the degrade tier */
  int16_t short_h = 0;
  int32_t priority = 0;  /** lower places first (closest to radar center wins) */
};

struct Placement {
  Box box{};
  uint8_t slot = 0;
  uint8_t tier = kTierHidden;
  bool on_right = false;  /** block sits right of the symbol (text is left-aligned) */
};

/** Screen the tags are placed into. */
struct Geometry {
  int16_t center_x = 0;
  int16_t center_y = 0;
  /** Tags stay inside this radius so text clears the round bezel. */
  int16_t visible_radius = 0;
  int16_t symbol_half = 0;  /** aircraft icon half-extent */
  int16_t gap = 0;          /** icon edge to block */
};

/** Drop all remembered placement (tests, and font / panel geometry changes). */
void reset();

/**
 * Assign every target a slot and tier.
 *
 * Cheap no-op when nothing that affects placement changed, so callers may
 * invoke it on any path that needs current boxes (drawing, dirty rects)
 * without paying for the O(n^2) scoring pass more than once per poll.
 */
void resolve(const Target* targets, size_t count, const Geometry& geom);

/** Placement for `key` from the last resolve, or nullptr when not placed. */
const Placement* find(uint32_t key);

/** FNV-1a over a C string; never returns 0 (0 marks an empty memory entry). */
uint32_t hashKey(const char* s);

/**
 * Give colliding keys distinct values. Rank is (sort_a, sort_b, index), so the
 * same aircraft keeps its key when the array is reordered. Keys that are
 * already unique are left unchanged. `count` above kMaxTargets is truncated.
 */
void uniquifyKeys(uint32_t* keys, const int32_t* sort_a, const int32_t* sort_b,
                  size_t count);

}  // namespace ui::label_layout
