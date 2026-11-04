#include <unity.h>

#include "ui/label_layout.h"

namespace ll = ui::label_layout;

namespace {

// A 390x390 round panel, matching the T-Encoder Pro geometry the radar uses.
ll::Geometry panel() {
  ll::Geometry g;
  g.center_x = 195;
  g.center_y = 195;
  g.visible_radius = 187;
  g.symbol_half = 15;
  g.gap = 3;
  return g;
}

ll::Target makeTarget(uint32_t key, int x, int y, int priority) {
  ll::Target t;
  t.key = key;
  t.x = static_cast<int16_t>(x);
  t.y = static_cast<int16_t>(y);
  t.full_w = 70;
  t.full_h = 72;   // three ~24 px lines
  t.short_w = 60;
  t.short_h = 24;  // callsign only
  t.priority = priority;
  return t;
}

int overlap(const ll::Box& a, const ll::Box& b) {
  const int ox = (a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w) -
                 (a.x > b.x ? a.x : b.x);
  const int oy = (a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h) -
                 (a.y > b.y ? a.y : b.y);
  return (ox <= 0 || oy <= 0) ? 0 : ox * oy;
}

bool insideCircle(const ll::Box& box, const ll::Geometry& g) {
  const int corners[4][2] = {
      {box.x, box.y},
      {box.x + box.w, box.y},
      {box.x, box.y + box.h},
      {box.x + box.w, box.y + box.h},
  };
  for (const auto& c : corners) {
    const int dx = c[0] - g.center_x;
    const int dy = c[1] - g.center_y;
    if (dx * dx + dy * dy > g.visible_radius * g.visible_radius) {
      return false;
    }
  }
  return true;
}

}  // namespace

void setUp(void) { ll::reset(); }
void tearDown(void) {}

// (a) An isolated tag takes the preferred slot: toward the radar center, so a
//     target left of center gets its block on the right.
void test_isolated_tag_points_at_center(void) {
  const ll::Geometry g = panel();
  ll::Target t = makeTarget(1, 120, 195, 0);
  ll::resolve(&t, 1, g);

  const ll::Placement* p = ll::find(1);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL(ll::kTierFull, p->tier);
  TEST_ASSERT_TRUE(p->on_right);
  TEST_ASSERT_EQUAL(120 + 15 + 3, p->box.x);

  ll::reset();
  ll::Target r = makeTarget(2, 260, 195, 0);
  ll::resolve(&r, 1, g);
  const ll::Placement* pr = ll::find(2);
  TEST_ASSERT_NOT_NULL(pr);
  TEST_ASSERT_FALSE(pr->on_right);
  TEST_ASSERT_EQUAL(260 - 15 - 3 - 70, pr->box.x);
}

// (b) Two targets at the same spot must not stack their blocks on top of each
//     other — the whole point of resolving them as a set.
void test_colocated_tags_separate(void) {
  const ll::Geometry g = panel();
  ll::Target ts[2] = {makeTarget(1, 180, 195, 0), makeTarget(2, 182, 197, 1)};
  ll::resolve(ts, 2, g);

  const ll::Placement* a = ll::find(1);
  const ll::Placement* b = ll::find(2);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  if (a->tier != ll::kTierHidden && b->tier != ll::kTierHidden) {
    const int area = b->box.w * b->box.h;
    TEST_ASSERT_TRUE(overlap(a->box, b->box) * 100 / area <= ll::kHideCoveragePct);
  }
}

// (c) Priority wins: the closest target keeps the slot it would get alone, and
//     the lower-priority one is the one that has to move / degrade / hide.
void test_priority_keeps_best_slot(void) {
  const ll::Geometry g = panel();
  ll::Target alone = makeTarget(1, 150, 195, 0);
  ll::resolve(&alone, 1, g);
  const ll::Box solo = ll::find(1)->box;

  ll::reset();
  ll::Target ts[2] = {makeTarget(1, 150, 195, 0), makeTarget(2, 152, 198, 5000)};
  ll::resolve(ts, 2, g);
  TEST_ASSERT_TRUE(ll::find(1)->box.sameAs(solo));
}

// (d) Hysteresis: a target drifting a pixel or two must not swap sides. Without
//     slot memory this is exactly the jitter that made tags jump every poll.
void test_small_drift_keeps_slot(void) {
  const ll::Geometry g = panel();
  ll::Target t = makeTarget(1, 193, 195, 0);
  ll::resolve(&t, 1, g);
  const uint8_t first = ll::find(1)->slot;

  // Cross the center line, which flips the preferred order.
  for (int x = 193; x <= 199; ++x) {
    ll::Target moved = makeTarget(1, x, 195, 0);
    ll::resolve(&moved, 1, g);
    TEST_ASSERT_EQUAL(first, ll::find(1)->slot);
  }
}

// (e) Identity, not array position, carries the memory: the same aircraft keeps
//     its slot when the feed hands it back in a different order.
void test_memory_survives_reorder(void) {
  const ll::Geometry g = panel();
  ll::Target ts[2] = {makeTarget(11, 140, 180, 0), makeTarget(22, 150, 260, 1)};
  ll::resolve(ts, 2, g);
  const uint8_t slot11 = ll::find(11)->slot;
  const uint8_t slot22 = ll::find(22)->slot;

  ll::Target swapped[2] = {makeTarget(22, 150, 260, 1), makeTarget(11, 140, 180, 0)};
  ll::resolve(swapped, 2, g);
  TEST_ASSERT_EQUAL(slot11, ll::find(11)->slot);
  TEST_ASSERT_EQUAL(slot22, ll::find(22)->slot);
}

// (f) Heavy traffic in one spot: tags degrade to callsign-only or hide rather
//     than painting unreadable stacked text.
void test_crowd_degrades_or_hides(void) {
  const ll::Geometry g = panel();
  ll::Target ts[8];
  for (int i = 0; i < 8; ++i) {
    ts[i] = makeTarget(static_cast<uint32_t>(100 + i), 190 + i, 195 + i, i);
  }
  ll::resolve(ts, 8, g);

  int degraded_or_hidden = 0;
  for (int i = 0; i < 8; ++i) {
    const ll::Placement* p = ll::find(static_cast<uint32_t>(100 + i));
    TEST_ASSERT_NOT_NULL(p);
    if (p->tier != ll::kTierFull) {
      ++degraded_or_hidden;
    }
  }
  TEST_ASSERT_TRUE(degraded_or_hidden > 0);

  // Whatever is still drawn has to be readable.
  for (int i = 0; i < 8; ++i) {
    const ll::Placement* p = ll::find(static_cast<uint32_t>(100 + i));
    if (p->tier == ll::kTierHidden) {
      continue;
    }
    int covered = 0;
    for (int j = 0; j < 8; ++j) {
      if (j == i) {
        continue;
      }
      const ll::Placement* q = ll::find(static_cast<uint32_t>(100 + j));
      if (q->tier != ll::kTierHidden) {
        covered += overlap(p->box, q->box);
      }
    }
    TEST_ASSERT_TRUE(covered * 100 / (p->box.w * p->box.h) <= ll::kHideCoveragePct);
  }
}

// (g) Rim traffic: the block stays inside the round bezel instead of hanging
//     off the square framebuffer edge.
void test_rim_tag_stays_inside_bezel(void) {
  const ll::Geometry g = panel();
  const int spots[4][2] = {{360, 195}, {30, 195}, {195, 30}, {195, 360}};
  for (int i = 0; i < 4; ++i) {
    ll::reset();
    ll::Target t = makeTarget(static_cast<uint32_t>(7 + i), spots[i][0], spots[i][1], 0);
    ll::resolve(&t, 1, g);
    const ll::Placement* p = ll::find(static_cast<uint32_t>(7 + i));
    TEST_ASSERT_NOT_NULL(p);
    if (p->tier != ll::kTierHidden) {
      TEST_ASSERT_TRUE(insideCircle(p->box, g));
    }
  }
}

// (h) Re-resolving unchanged input is stable (and short-circuits), so a redraw
//     between polls cannot move a label.
void test_unchanged_input_is_stable(void) {
  const ll::Geometry g = panel();
  ll::Target ts[3] = {makeTarget(1, 150, 150, 0), makeTarget(2, 160, 158, 1),
                      makeTarget(3, 240, 220, 2)};
  ll::resolve(ts, 3, g);
  const ll::Box b1 = ll::find(1)->box;
  const ll::Box b2 = ll::find(2)->box;
  const ll::Box b3 = ll::find(3)->box;

  for (int i = 0; i < 5; ++i) {
    ll::resolve(ts, 3, g);
  }
  TEST_ASSERT_TRUE(ll::find(1)->box.sameAs(b1));
  TEST_ASSERT_TRUE(ll::find(2)->box.sameAs(b2));
  TEST_ASSERT_TRUE(ll::find(3)->box.sameAs(b3));
}

// (i) An empty set clears the previous answer, so nothing stale is drawn or
//     erased after the last aircraft leaves range.
void test_empty_set_clears_results(void) {
  const ll::Geometry g = panel();
  ll::Target t = makeTarget(1, 150, 150, 0);
  ll::resolve(&t, 1, g);
  TEST_ASSERT_NOT_NULL(ll::find(1));

  ll::resolve(nullptr, 0, g);
  TEST_ASSERT_NULL(ll::find(1));
}

// (k) Callsign-only promotes back to a full tag only under the stricter limit.
//     Degrading still uses the wider limit. This is what stops full/short flicker.
void test_tier_promotion_uses_stricter_coverage(void) {
  TEST_ASSERT_EQUAL(ll::kHideCoveragePct, ll::coverageLimitPct(ll::kTierFull, ll::kTierFull));
  TEST_ASSERT_EQUAL(ll::kHideCoveragePct, ll::coverageLimitPct(ll::kTierFull, ll::kTierShort));
  TEST_ASSERT_EQUAL(ll::kUnhideCoveragePct, ll::coverageLimitPct(ll::kTierShort, ll::kTierFull));
  TEST_ASSERT_EQUAL(ll::kHideCoveragePct, ll::coverageLimitPct(ll::kTierShort, ll::kTierShort));
  TEST_ASSERT_EQUAL(ll::kUnhideCoveragePct, ll::coverageLimitPct(ll::kTierHidden, ll::kTierFull));
  TEST_ASSERT_EQUAL(ll::kUnhideCoveragePct, ll::coverageLimitPct(ll::kTierHidden, ll::kTierShort));
}

// (l) A pile of farther aircraft must not take the closest tag's slot or tier.
void test_closer_tag_survives_lower_priority_crowd(void) {
  const ll::Geometry g = panel();
  ll::Target alone = makeTarget(1, 150, 195, 0);
  ll::resolve(&alone, 1, g);
  const ll::Box solo = ll::find(1)->box;
  TEST_ASSERT_EQUAL(ll::kTierFull, ll::find(1)->tier);

  ll::reset();
  ll::Target ts[8];
  ts[0] = makeTarget(1, 150, 195, 0);
  for (int i = 1; i < 8; ++i) {
    ts[i] = makeTarget(static_cast<uint32_t>(100 + i), static_cast<int16_t>(150 + (i % 3)),
                       static_cast<int16_t>(195 + (i % 2)), 1000 + i);
  }
  ll::resolve(ts, 8, g);
  const ll::Placement* p = ll::find(1);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL(ll::kTierFull, p->tier);
  TEST_ASSERT_TRUE(p->box.sameAs(solo));
}

// (m) Two aircraft that share a base key stay distinct, and reordering the
//     array does not swap which key belongs to which aircraft.
void test_duplicate_keys_stable_across_reorder(void) {
  uint32_t keys_a[3] = {5, 5, 7};
  const int32_t lat_a[3] = {20, 10, 0};
  const int32_t lon_a[3] = {1, 2, 3};
  ll::uniquifyKeys(keys_a, lat_a, lon_a, 3);

  uint32_t keys_b[3] = {7, 5, 5};
  const int32_t lat_b[3] = {0, 10, 20};
  const int32_t lon_b[3] = {3, 2, 1};
  ll::uniquifyKeys(keys_b, lat_b, lon_b, 3);

  TEST_ASSERT_NOT_EQUAL(keys_a[0], keys_a[1]);
  TEST_ASSERT_EQUAL(7u, keys_a[2]);
  // lat 10 is rank 0, lat 20 is rank 1, in both array orders.
  TEST_ASSERT_EQUAL(keys_a[1], keys_b[1]);
  TEST_ASSERT_EQUAL(keys_a[0], keys_b[2]);
  TEST_ASSERT_EQUAL(keys_a[2], keys_b[0]);
}

// (j) A target with no callsign has no degrade tier: it is full or nothing.
void test_no_short_tier_without_callsign(void) {
  const ll::Geometry g = panel();
  ll::Target ts[6];
  for (int i = 0; i < 6; ++i) {
    ts[i] = makeTarget(static_cast<uint32_t>(200 + i), 195 + i, 195 + i, i);
    ts[i].short_w = 0;
    ts[i].short_h = 0;
  }
  ll::resolve(ts, 6, g);
  for (int i = 0; i < 6; ++i) {
    const ll::Placement* p = ll::find(static_cast<uint32_t>(200 + i));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(p->tier == ll::kTierFull || p->tier == ll::kTierHidden);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_isolated_tag_points_at_center);
  RUN_TEST(test_colocated_tags_separate);
  RUN_TEST(test_priority_keeps_best_slot);
  RUN_TEST(test_small_drift_keeps_slot);
  RUN_TEST(test_memory_survives_reorder);
  RUN_TEST(test_crowd_degrades_or_hides);
  RUN_TEST(test_rim_tag_stays_inside_bezel);
  RUN_TEST(test_unchanged_input_is_stable);
  RUN_TEST(test_empty_set_clears_results);
  RUN_TEST(test_no_short_tier_without_callsign);
  RUN_TEST(test_tier_promotion_uses_stricter_coverage);
  RUN_TEST(test_closer_tag_survives_lower_priority_crowd);
  RUN_TEST(test_duplicate_keys_stable_across_reorder);
  return UNITY_END();
}
