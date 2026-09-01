// test_policy.cpp -- unique allocation. G4: one drone per inbound, not both
// neighbours of a silent facing slot.

#include "flight.h"
#include "policy.h"

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

int main() {
    TestFacingSlotMatchesRing();
    TestUniqueOwnerIsOneDrone();

    if (g_failures == 0) {
        std::printf("policy: all passed\n");
        return 0;
    }
    std::printf("policy: %d failure(s)\n", g_failures);
    return 1;
}
