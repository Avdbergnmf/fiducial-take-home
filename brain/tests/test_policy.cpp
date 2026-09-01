// test_policy.cpp -- unique allocation, live-ring respacing, yield corridor.
// G4: one drone per inbound, not both neighbours of a silent facing slot.
// D17: yield is remaining flight to the predicted ram, not slot→hostile.
// D19: survivors re-space on the live ring; ownership uses that ring.

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

static void InitHeard(float* heard) {
    for (uint32_t i = 0; i < kMaxFleet; ++i) heard[i] = -1.0e9f;
}

static float Horiz(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

static void FillStations(Vec3* at, const Vec3& asset, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        at[i] = flight::RingSlot(i, n, asset, 75.0f, 30.0f);
}

static void TestLiveRingRespaces() {
    std::printf("live ring re-spaces a nearby death, ignores radio loss\n");
    float heard[kMaxFleet];
    Vec3 at[kMaxFleet];
    InitHeard(heard);
    const uint32_t n0 = 16;
    const float now = 20.0f;
    const float comm = 90.0f;
    const Vec3 asset(0, 0, 0);
    FillStations(at, asset, n0);
    const Vec3 self0 = at[0];
    const Vec3 self7 = at[7];
    const Vec3 self9 = at[9];
    const Vec3 self14 = at[14];

    CHECK(CountLive(n0, 0, heard, at, self0, comm, now) == 16);
    for (uint32_t id = 0; id < n0; ++id) {
        CHECK(LiveRank(id, n0, id, heard, at, at[id], comm, now) == id);
        CHECK(LiveId(id, n0, 0, heard, at, self0, comm, now) == id);
        CHECK(FacingSlot(at[id], asset, n0) == id);
    }

    // Opposite-side silence is radio loss, not death. Slot 0 still counts 16.
    heard[8] = now - 2.0f;
    CHECK(SlotAlive(8, 0, heard, now) == false);
    CHECK(RingAlive(8, 0, heard, at, self0, comm, now) == true);
    CHECK(CountLive(n0, 0, heard, at, self0, comm, now) == 16);

    // Neighbour of 8 sees a nearby death. 15 live; 9 slides toward the hole.
    CHECK(RingAlive(8, 7, heard, at, self7, comm, now) == false);
    CHECK(CountLive(n0, 7, heard, at, self7, comm, now) == 15);
    CHECK(LiveRank(7, n0, 7, heard, at, self7, comm, now) == 7);
    CHECK(LiveRank(9, n0, 9, heard, at, self9, comm, now) == 8);
    CHECK(LiveId(8, n0, 7, heard, at, self7, comm, now) == 9);

    const Vec3 new9 = flight::RingSlot(
        LiveRank(9, n0, 9, heard, at, self9, comm, now), 15, asset, 75.0f, 30.0f);
    CHECK(Horiz(new9, at[8]) + 1.0f < Horiz(at[9], at[8]));

    const uint32_t facing = FacingSlot(at[8], asset, 15);
    CHECK(LiveId(facing, n0, 7, heard, at, self7, comm, now) == 9);

    // Wrap: slot 15 dead, seen from 14. Live ring gives the inbound to 14.
    InitHeard(heard);
    FillStations(at, asset, n0);
    heard[15] = now - 2.0f;
    CHECK(CountLive(n0, 14, heard, at, self14, comm, now) == 15);
    const uint32_t f15 = FacingSlot(at[15], asset, 15);
    CHECK(LiveId(f15, n0, 14, heard, at, self14, comm, now) == 14);
    CHECK(UniqueOwner(15, n0, 0, heard, now) == 0);

    const Vec3 new14 = flight::RingSlot(
        LiveRank(14, n0, 14, heard, at, self14, comm, now), 15, asset, 75.0f, 30.0f);
    CHECK(Horiz(new14, at[15]) < 8.0f);
}

static void TestLeadIntercept() {
    std::printf("lead intercept solves the meeting point, and closes range\n");
    Config cfg;
    cfg.max_speed = 20.0f;
    cfg.max_accel = 15.0f;
    cfg.lateral_limit = 6.71f;

    // Head-on: target 100 m away closing at 20, we fly at 20. Closing speed is
    // 40, so they meet in 2.5 s.
    float tau = flight::TimeToIntercept(Vec3(100, 0, 0), Vec3(-20, 0, 0), 20.0f);
    CHECK(std::fabs(tau - 2.5f) < 1e-2f);

    // Crossing, target slower than us: there is a lead point, and flying to
    // it at our speed arrives exactly when the target does.
    tau = flight::TimeToIntercept(Vec3(100, 0, 0), Vec3(0, 10, 0), 20.0f);
    CHECK(tau > 0.0f);
    const Vec3 meet(100.0f, 10.0f * tau, 0.0f);
    CHECK(std::fabs(swarm::Length(meet) - 20.0f * tau) < 0.5f);

    // Crossing at OUR speed is a different answer, and the right one is "no".
    // Equal airframes make the quadratic linear, and with no component of the
    // target's velocity toward us there is no meeting point at all -- the same
    // reason a stern chase never converges. Returning a lead point here would
    // send an interceptor after something it can never reach.
    CHECK(flight::TimeToIntercept(Vec3(100, 0, 0), Vec3(0, 20, 0), 20.0f) < 0.0f);

    // Opening faster than we fly: no meeting point exists, and saying so is
    // the point -- a stern chase against an equal airframe never converges.
    CHECK(flight::TimeToIntercept(Vec3(100, 0, 0), Vec3(25, 0, 0), 20.0f) < 0.0f);

    // The bug this replaced: a picket sitting still on the hostile's inbound
    // bearing sees no line-of-sight rotation, so pure PN commanded nothing and
    // it was rammed at its own station. Measured on s1 at 0.2-0.4 m/s for four
    // seconds. Command must now point AT the target, not across it.
    const Vec3 self(70, 0, -30);
    const Vec3 still(0, 0, 0);
    const Vec3 hostile(170, 0, -30);
    const Vec3 inbound(-15, 0, 0);
    const Vec3 accel = flight::ProNav(self, still, hostile, inbound, cfg);
    const Vec3 los = hostile - self;
    const float range = swarm::Length(los);
    CHECK(range > 1.0f);
    const float along = swarm::Dot(accel, los / range);
    CHECK(along > 0.5f * cfg.lateral_limit);   // most of the budget, outbound
    CHECK(accel.x > 0.0f);
}

static void TestStationBisectsTheGap() {
    std::printf("station bisects the gap a run of deaths leaves\n");
    // The s2 leak, in numbers. Ring 70 m, 16 drones, comm 75. Slots 0, 1 and 2
    // died to rams in sequence; the next hostile came in at bearing 20 deg,
    // the centre of the arc they left. Measured on the trace: the survivors
    // held 335 deg and 68 deg and never closed the 93 deg hole, because the
    // old rank/CountLive re-space needs a roster nobody has -- at this radius
    // only +/-2 neighbours are inside comm_radius.
    float heard[kMaxFleet];
    Vec3 at[kMaxFleet];
    InitHeard(heard);
    const uint32_t n0 = 16;
    const float now = 70.0f;
    const float comm = 75.0f;
    const Vec3 asset(0, 0, 0);
    for (uint32_t i = 0; i < n0; ++i)
        at[i] = flight::RingSlot(i, n0, asset, 70.0f, 30.0f);
    const float step = 2.0f * 3.14159265358979f / 16.0f;

    // Full strength is a fixed point: nobody abandons their own sector.
    for (uint32_t id = 0; id < n0; ++id) {
        const float b = StationBearing(id, n0, id, heard, at, at[id], comm, now);
        CHECK(std::fabs(b - step * static_cast<float>(id)) < 1e-4f);
    }

    heard[0] = now - 3.0f;
    heard[1] = now - 3.0f;
    heard[2] = now - 3.0f;

    // How far the death signal reaches is RingAlive's business, and it is
    // shorter than the radio: silence only counts as a death inside
    // comm - cruise*silent - 10 = 44 m. On this ring the slot chords are 27 m,
    // 54 m, 80 m, so drone 3 registers 2 as dead and still believes in 1.
    CHECK(RingAlive(2, 3, heard, at, at[3], comm, now) == false);
    CHECK(RingAlive(1, 3, heard, at, at[3], comm, now) == true);

    // So each lip of the hole sees one empty slot on that side and one full
    // slot on the other, and bisects: half a slot inward.
    const float b3 = StationBearing(3, n0, 3, heard, at, at[3], comm, now);
    CHECK(std::fabs(b3 - (step * 3.0f - 0.5f * step)) < 1e-4f);

    const float b15 = StationBearing(15, n0, 15, heard, at, at[15], comm, now);
    CHECK(std::fabs(b15 - (step * 15.0f + 0.5f * step)) < 1e-4f);

    // Which is the point: the 90 deg hole closes by a slot from the two
    // drones that can see it, without anyone needing the full roster.
    const float two_pi = 2.0f * 3.14159265358979f;
    const float before = step * 3.0f - step * 15.0f + two_pi;
    const float after = b3 - b15 + two_pi;
    CHECK(after < before - 0.9f * step);

    // Far from the hole, nothing moves: this is local, not a global reshuffle.
    const float b8 = StationBearing(8, n0, 8, heard, at, at[8], comm, now);
    CHECK(std::fabs(b8 - step * 8.0f) < 1e-4f);

    // Whatever it believes, a picket never walks off its own sector: the
    // shift is capped at two slots. Reachable only on a wider ring, where
    // more than one neighbour falls inside the death radius.
    for (uint32_t id = 0; id < 13; ++id) heard[id] = now - 3.0f;
    const float b14 = StationBearing(14, n0, 14, heard, at, at[14], comm, now);
    CHECK(std::fabs(b14 - step * 14.0f) <= 2.0f * step + 1e-4f);
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
    TestLiveRingRespaces();
    TestLeadIntercept();
    TestStationBisectsTheGap();
    TestYieldHorizonIsRemainingFlight();

    if (g_failures == 0) {
        std::printf("policy: all passed\n");
        return 0;
    }
    std::printf("policy: %d failure(s)\n", g_failures);
    return 1;
}
