// test_policy.cpp -- unique allocation and the yield corridor.
// G4: one drone per inbound, not both neighbours of a silent facing slot.
// D17: yield is remaining flight to the predicted ram, not slot→hostile.

#include "flight.h"
#include "policy.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using namespace sw;

static void TestFacingSlotMatchesRing() {
    std::printf("facing slot matches RingSlot angle\n");
    const Vec3 asset(0, 0, 0);
    const uint32_t n = 16;
    for (uint32_t id = 0; id < n; ++id) {
        const Vec3 slot = flight::RingSlot(id, n, asset, 75.0f, 30.0f);
        CHECK(FacingSlot(slot, asset, n) == id);
        const Vec3 inbound(slot.x * 2.0f, slot.y * 2.0f, -40.0f);
        CHECK(FacingSlot(inbound, asset, n) == id);
    }
    CHECK(FacingSlot(asset, asset, n) == 0);
}

static void TestUniqueOwnerIsOneDrone() {
    std::printf("unique owner is facing, then first live clockwise\n");
    float heard[kMaxFleet];
    for (uint32_t i = 0; i < kMaxFleet; ++i) heard[i] = -1.0e9f;
    const uint32_t n = 16;
    const float now = 20.0f;

    // Everyone never-heard: facing slot owns.
    CHECK(UniqueOwner(5, n, 5, heard, now) == 5);
    CHECK(UniqueOwner(5, n, 4, heard, now) == 5);
    CHECK(UniqueOwner(5, n, 6, heard, now) == 5);

    // Facing died. Old rule let both neighbours go. New rule: clockwise only.
    heard[5] = now - 2.0f;
    CHECK(SlotAlive(5, 6, heard, now) == false);
    CHECK(UniqueOwner(5, n, 6, heard, now) == 6);
    CHECK(UniqueOwner(5, n, 4, heard, now) == 6);
    CHECK(UniqueOwner(5, n, 7, heard, now) == 6);

    // Clockwise neighbour also dead: walk one more.
    heard[6] = now - 2.0f;
    CHECK(UniqueOwner(5, n, 7, heard, now) == 7);
    CHECK(UniqueOwner(5, n, 4, heard, now) == 7);

    // Wrap: slot 15 dead, 0 is next.
    std::memset(heard, 0, sizeof(heard));
    for (uint32_t i = 0; i < kMaxFleet; ++i) heard[i] = -1.0e9f;
    heard[15] = now - 2.0f;
    CHECK(UniqueOwner(15, n, 0, heard, now) == 0);
    CHECK(UniqueOwner(15, n, 14, heard, now) == 0);
}

static float Horiz(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

static void TestYieldHorizonIsRemainingFlight() {
    std::printf("yield corridor is remaining flight, not the full chord\n");
    // s1-like: picket on the 75 m ring, hostile 95 m further inbound at 16 m/s.
    // Assumed cruise 14 + inbound 16 = 30 m/s closing. t_meet = 95/30 ≈ 3.2 s.
    // Horizon is ~51 m along the LOS (cruise × (t_meet + 0.5 s)), not 95 m.
    const Vec3 from(75.0f, 0.0f, -30.0f);
    const Vec3 hostile(170.0f, 0.0f, -40.0f);
    const Vec3 inbound(-16.0f, 0.0f, 0.0f);
    const float clear = 19.0f;

    const Vec3 end = CorridorHorizon(from, hostile, inbound);
    CHECK(end.x > 120.0f && end.x < 135.0f);
    CHECK(std::fabs(end.y) < 0.5f);
    CHECK(Horiz(from, end) < Horiz(from, hostile) - 20.0f);

    // A picket sitting beside the hostile's *current* pose is on the old
    // full chord (12 m off a 95 m line) and off the remaining flight
    // (≈40 m from the horizon). Full-chord yield was the nonsense move.
    const Vec3 far(165.0f, 12.0f, -30.0f);
    const Vec3 old_goal = YieldOffCorridor(far, from, hostile, clear);
    CHECK(Horiz(old_goal, far) > 1.0f);
    const Vec3 new_goal = YieldOffCorridor(far, from, end, clear);
    CHECK(Horiz(new_goal, far) < 0.1f);

    // A picket on the remaining path still steps off.
    const Vec3 on_path(100.0f, 8.0f, -30.0f);
    const Vec3 stepped = YieldOffCorridor(on_path, from, end, clear);
    CHECK(Horiz(stepped, on_path) > 1.0f);

    // Outbound: assumed cruise is not closing. Degenerate corridor, no yield.
    const Vec3 outbound(16.0f, 0.0f, 0.0f);
    const Vec3 none = CorridorHorizon(from, hostile, outbound);
    CHECK(Horiz(none, from) < 0.1f);
    CHECK(Horiz(YieldOffCorridor(far, from, none, clear), far) < 0.1f);
}

int main() {
    TestFacingSlotMatchesRing();
    TestUniqueOwnerIsOneDrone();
    TestYieldHorizonIsRemainingFlight();

    if (g_failures == 0) {
        std::printf("policy: all passed\n");
        return 0;
    }
    std::printf("policy: %d failure(s)\n", g_failures);
    return 1;
}
