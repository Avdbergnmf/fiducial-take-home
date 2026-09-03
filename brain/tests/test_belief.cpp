// test_belief.cpp -- the classification discriminant, tested against synthetic
// geometry. No simulator needed, which means you can iterate on the rule in
// seconds instead of minutes.
//
// The cases are drawn from s1.json: hostiles spawn at radius 170 m and dash at
// the asset at 16 m/s; civilians spawn at radius 150 m and cross on straight
// lines at a similar speed. They are physically identical, so these tests are
// the whole of what separates them.

#include "belief.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using namespace sw;

static const Vec3 kAsset(0.0f, 0.0f, 0.0f);

static void TestRangeRate() {
    std::printf("range rate signs\n");
    // Approaching from the north at 16 m/s: closing, so range rate is negative.
    CHECK(RangeRate(Vec3(170, 0, -40), Vec3(-16, 0, 0), kAsset) < -15.0f);
    // Receding.
    CHECK(RangeRate(Vec3(170, 0, -40), Vec3(16, 0, 0), kAsset) > 15.0f);
    // Pure tangential motion: neither.
    CHECK(std::fabs(RangeRate(Vec3(170, 0, -40), Vec3(0, 16, 0), kAsset)) < 0.5f);
}

static void TestApproachAlignment() {
    std::printf("approach alignment separates a dash from a crossing\n");
    // A hostile dashing straight at the asset.
    // Horizontal alignment, so the 40 m of altitude does not dilute it.
    const float hostile = ApproachAlignment(Vec3(170, 0, -40), Vec3(-16, 0, 0), kAsset);
    CHECK(hostile > 0.99f);

    // A civilian crossing at 90 degrees at the same speed.
    const float civilian = ApproachAlignment(Vec3(150, 0, -50), Vec3(0, 16, 0), kAsset);
    CHECK(std::fabs(civilian) < 0.1f);

    // Stationary: no evidence either way, and must not read as hostile.
    CHECK(std::fabs(ApproachAlignment(Vec3(60, 0, -30), Vec3(0, 0, 0), kAsset)) < 0.01f);
}

static void TestTimeToTarget() {
    std::printf("time to asset\n");
    // 160 m out at 16 m/s straight in: ten seconds.
    const float ttg = TimeToTarget(Vec3(160, 0, 0), Vec3(-16, 0, 0), kAsset);
    CHECK(ttg > 9.0f && ttg < 11.0f);

    // Not closing: effectively never.
    CHECK(TimeToTarget(Vec3(160, 0, 0), Vec3(0, 16, 0), kAsset) > 1000.0f);
}

static void TestTimeToCylinder() {
    std::printf("time to cylinder is ground range minus radius\n");
    // 160 m out, r = 30, 16 m/s: 130/16 = 8.125 s. TimeToTarget would be 10.
    const float ttg = TimeToCylinder(Vec3(160, 0, -40), Vec3(-16, 0, 0), kAsset, 30.0f);
    CHECK(ttg > 8.0f && ttg < 8.3f);

    CHECK(TimeToCylinder(Vec3(20, 0, -10), Vec3(-16, 0, 0), kAsset, 30.0f) == 0.0f);
    CHECK(TimeToCylinder(Vec3(160, 0, 0), Vec3(0, 16, 0), kAsset, 30.0f) > 1000.0f);
}

static void TestThreatWindowIsFiniteWhileSpooling() {
    std::printf("ThreatWindow uses dash speed while TimeToCylinder is huge\n");
    // Spawn-inside-sense: still accelerating, almost no horizontal closing.
    const Vec3 p(144.0f, 0.0f, -40.0f);
    const Vec3 v(-2.0f, 0.0f, 1.67f);
    CHECK(TimeToCylinder(p, v, kAsset, 35.0f) > 40.0f);
    const float w = ThreatWindow(p, v, kAsset, 35.0f, kHostileDash);
    // (144-35)/16 = 6.81 s — the short window D31's TTG gate could not see.
    CHECK(w > 6.5f && w < 7.2f);

    // s1-sized inbound already at dash: just above the compact gate.
    const float s1 = ThreatWindow(Vec3(170, 0, -40), Vec3(-16, 0, 0), kAsset,
                                  30.0f, kHostileDash);
    CHECK(s1 > kCompactWindow && s1 < 9.5f);

    // s2-sized inbound already at dash: still above the 10 s early-call gate.
    const float s2 = ThreatWindow(Vec3(220, 0, -45), Vec3(-16, 0, 0), kAsset,
                                  30.0f, kHostileDash);
    CHECK(s2 > 11.0f && s2 < 13.0f);
}

static void TestLooksDivingAtAsset() {
    std::printf("dive + ground track is a hostile posture, a level chord is not\n");
    CHECK(LooksDivingAtAsset(Vec3(120, 0, -39.5f), Vec3(-19, 0, 1.67f), kAsset, 30.0f));
    CHECK(!LooksDivingAtAsset(Vec3(120, 0, -50), Vec3(-19, 0, 0.02f), kAsset, 30.0f));
}

static void TestGroundTrackHitsCylinder() {
    std::printf("ground track through the cylinder is independent of the dive\n");
    CHECK(GroundTrackHitsCylinder(Vec3(170, 0, -40), Vec3(-16, 0, 0), kAsset, 30.0f));
    CHECK(!GroundTrackHitsCylinder(Vec3(170, 0, -40), Vec3(0, 16, 0), kAsset, 30.0f));
    // Level dash at 40 m still breaches the cylinder (D10). Scramble uses this;
    // Classify still refuses the Hostile call (D7).
    CHECK(GroundTrackHitsCylinder(Vec3(170, 0, -40), Vec3(-16, 0, 0), kAsset, 30.0f));
    CHECK(!LooksDivingAtAsset(Vec3(170, 0, -40), Vec3(-16, 0, 0), kAsset, 30.0f));
}

static void TestClosingSpeed() {
    std::printf("relative closing uses both velocities\n");
    const Vec3 us(75, 0, -30);
    const Vec3 still(0, 0, 0);
    const Vec3 them(135, 0, -40);
    const Vec3 inbound(-16, 0, 0);

    // Head-on onto a picket: they close at 16 m/s.
    CHECK(ClosingSpeed(us, still, them, inbound) > 15.0f);

    // Stern chase: we sit behind them on the same ray, they pull away.
    const Vec3 behind(150, 0, -30);
    CHECK(ClosingSpeed(behind, still, them, inbound) < -15.0f);

    // Matching velocity on the same course: relative closing is zero.
    CHECK(std::fabs(ClosingSpeed(us, inbound, them, inbound)) < 0.5f);
}

static void TestBallistic() {
    std::printf("wreckage is identifiable from kinematics alone\n");
    const float dt = 0.1f;

    // Unpowered: gravity only. NED, so +z is down.
    Vec3 prev(10.0f, 0.0f, 0.0f);
    Vec3 now(10.0f, 0.0f, 0.981f);
    CHECK(LooksBallistic(now, prev, dt));

    // A drone holding station is not falling.
    CHECK(!LooksBallistic(Vec3(0, 0, 0), Vec3(0, 0, 0), dt));

    // A hostile diving under power pulls more than g and manoeuvres laterally.
    CHECK(!LooksBallistic(Vec3(16, 3, 2.0f), Vec3(16, 0, 0), dt));
}

static Vec3 Toward(const Vec3& from, const Vec3& to, float speed) {
    const Vec3 d = to - from;
    const float n = swarm::Length(d);
    CHECK(n > 1.0f);
    return d * (speed / n);
}

// ---------------------------------------------------------------------------
// The case that decides the tier-1 awareness score. A civilian on a chord that
// happens to point near the asset for a while is the expensive false positive:
// a wrong call costs -2 where a correct one earns +1.
// ---------------------------------------------------------------------------

static void TestMissDistanceSeparatesTheHardCase() {
    std::printf("miss distance separates a level overflight from a dive\n");

    // A civilian crossing on a straight line that passes 40 m from the asset,
    // level at 50 m. Ground miss is 40 m; 3D miss is dominated by altitude.
    const Vec3 civ_p(150.0f, 40.0f, -50.0f);
    const Vec3 civ_v(-16.0f, 0.0f, 0.0f);

    // A hostile diving at the origin from a similar bearing.
    const Vec3 hos_p(170.0f, 0.0f, -40.0f);
    const Vec3 hos_v = Toward(hos_p, kAsset, 16.0f);

    // Alignment CANNOT tell them apart -- both read as closing and well aimed
    // in the horizontal plane. This is exactly the -2 that a naive classifier
    // pays, and why miss is 3D while alignment stays flat.
    CHECK(ApproachAlignment(civ_p, civ_v, kAsset) > 0.9f);
    CHECK(ApproachAlignment(hos_p, hos_v, kAsset) > 0.9f);

    const float civ_miss = ClosestApproachDistance(civ_p, civ_v, kAsset);
    const float hos_miss = ClosestApproachDistance(hos_p, hos_v, kAsset);
    CHECK(civ_miss > 60.0f);
    CHECK(hos_miss < 1.0f);

    std::printf("    civilian misses by %.1f m, hostile by %.1f m\n",
                static_cast<double>(civ_miss), static_cast<double>(hos_miss));
}

static void TestMissDistanceIgnoresThePast() {
    std::printf("something already receding misses by its current range\n");
    // Past the asset and running: closest approach is behind it, so the miss
    // distance must be the range now, not a negative-time extrapolation.
    const float miss = ClosestApproachDistance(Vec3(50, 0, -40), Vec3(16, 0, 0), kAsset);
    CHECK(miss > 63.0f && miss < 65.0f);
}

// ---------------------------------------------------------------------------
// AimedAtAsset is the gate Classify actually uses.
// ---------------------------------------------------------------------------

static void TestAimedAtAssetRejectsTheG1aChord() {
    std::printf("AimedAtAsset rejects a 20 m ground chord that alignment would call hostile\n");
    const float asset_radius = 30.0f;

    // G1a: civilian on a chord that passes 20 m from the origin, level at 50 m.
    const Vec3 civ_p(150.0f, 20.0f, -50.0f);
    const Vec3 civ_v(-16.0f, 0.0f, 0.0f);
    CHECK(ApproachAlignment(civ_p, civ_v, kAsset) > 0.9f);
    const float civ_miss = ClosestApproachDistance(civ_p, civ_v, kAsset);
    CHECK(civ_miss > 50.0f);
    CHECK(!AimedAtAsset(civ_miss, civ_miss, asset_radius));

    // s1 hostile diving at the origin: 3D miss ~0 from first sight.
    const Vec3 hos_p(170.0f, 0.0f, -40.0f);
    const Vec3 hos_v = Toward(hos_p, kAsset, 16.0f);
    const float hos_miss = ClosestApproachDistance(hos_p, hos_v, kAsset);
    CHECK(hos_miss < 1.0f);
    CHECK(AimedAtAsset(hos_miss, hos_miss, asset_radius));
}

static void TestAimedAtAssetShrinkVsNoise() {
    std::printf("AimedAtAsset treats a 5 m miss drop as steering, 0.3 m as noise\n");
    const float asset_radius = 30.0f;
    CHECK(AimedAtAsset(15.0f, 20.0f, asset_radius));
    CHECK(!AimedAtAsset(19.7f, 20.0f, asset_radius));
}

static void TestDiveIsAHostileSignatureWhenTimeIsShort() {
    std::printf("a diving track on the cylinder is hostile without the wait\n");
    // The 3D miss test is what keeps civilian overflights out of the hostile
    // call, but it can only fire once the dive has developed -- measured, 2.5 s
    // of a window about 4 s long. The dive itself is visible at once, and
    // civilians never produce it: on x1-ae01dd every hostile ramps to vz +3.6
    // m/s within 2 s while every civilian sits between 0.00 and 0.14.
    const Vec3 asset(0, 0, 0);
    const float asset_radius = 30.0f;

    // Hostile, 0.5 s after spawn: still high, so the 3D miss is far too big for
    // AimedAtAsset, but it is descending and its GROUND track is on the asset.
    const Vec3 h_pos(120.0f, 0.0f, -39.5f);
    const Vec3 h_vel(-19.0f, 0.0f, 1.67f);
    const float h_miss3 = ClosestApproachDistance(h_pos, h_vel, asset);
    CHECK(h_miss3 > 5.0f);                                  // patient test says no
    CHECK(!AimedAtAsset(h_miss3, h_miss3, asset_radius));
    const float h_ground = ClosestApproachDistance(
        Vec3(h_pos.x, h_pos.y, 0), Vec3(h_vel.x, h_vel.y, 0), asset);
    CHECK(h_ground < asset_radius);                         // dive test says yes
    CHECK(h_vel.z > 1.0f);
    CHECK(TimeToCylinder(h_pos, h_vel, asset, asset_radius) < 10.0f);
    CHECK(LooksDivingAtAsset(h_pos, h_vel, asset, asset_radius));

    // The spawn-inside-sense clock: almost no horizontal closing yet, so
    // TimeToCylinder is infinite, but they are diving at the cylinder.
    const Vec3 spool_pos(144.0f, 0.0f, -39.5f);
    const Vec3 spool_vel(-2.0f, 0.0f, 1.67f);
    CHECK(TimeToCylinder(spool_pos, spool_vel, asset, asset_radius) > 40.0f);
    CHECK(LooksDivingAtAsset(spool_pos, spool_vel, asset, asset_radius));

    // Civilian overflying the same ground track, level at 50 m. Its ground miss
    // is just as small -- which is exactly why the ground track alone cannot be
    // the test -- but it is not descending, so the dive path never opens.
    const Vec3 c_vel(-19.0f, 0.0f, 0.02f);
    const Vec3 c_pos(120.0f, 0.0f, -50.0f);
    const float c_ground = ClosestApproachDistance(
        Vec3(c_pos.x, c_pos.y, 0), Vec3(c_vel.x, c_vel.y, 0), asset);
    CHECK(c_ground < asset_radius);
    CHECK(!(c_vel.z > 1.0f));
    CHECK(!AimedAtAsset(ClosestApproachDistance(c_pos, c_vel, asset),
                        ClosestApproachDistance(c_pos, c_vel, asset),
                        asset_radius));
}

static void TestLevelOverflightIsNotAimed() {
    std::printf("AimedAtAsset rejects a radial level overflight (x1-a drone 2)\n");
    const float asset_radius = 38.1f;

    // Recorded civilian 28 at t=5.57 when drone 2 called it: ground miss 5.2 m
    // (inside kSureHit / shrink), altitude 30 m, vz = 0. Horizontal miss shrank
    // from a noisy first sight (10.7 -> 5.2) and fired. 3D miss is ~altitude
    // and the drop is ~1.5 m, below kShrink.
    const Vec3 p(-17.0f, 89.8f, -30.1f);
    const Vec3 v(1.9f, -7.6f, 0.0f);
    CHECK(ApproachAlignment(p, v, kAsset) > 0.95f);
    const float miss = ClosestApproachDistance(p, v, kAsset);
    CHECK(miss > 29.0f && miss < 32.0f);
    CHECK(!AimedAtAsset(miss, miss, asset_radius));
    // The 2D first-sight was 10.7 m; in 3D that is hypot(10.7, 30.1) ≈ 32 m,
    // a 1.5 m drop, below kShrink.
    CHECK(!AimedAtAsset(miss, 32.0f, asset_radius));
}

static void TestLevelDashIsNotAHit() {
    // Classification only. A level dash at 40 m *would* breach the cylinder
    // (ground miss 0); 3D miss is the altitude, so AimedAtAsset stays false (D7, D10).
    std::printf("a level dash at 40 m misses in 3D by its altitude\n");
    const Vec3 p(170.0f, 0.0f, -40.0f);
    const Vec3 v(-16.0f, 0.0f, 0.0f);
    const float miss = ClosestApproachDistance(p, v, kAsset);
    CHECK(miss > 39.0f && miss < 41.0f);
    CHECK(!AimedAtAsset(miss, miss, 30.0f));
}

static void TestHeartbeatRangeCorroboration() {
    std::printf("heartbeat range must match the claimed position\n");
    const Vec3 self(0.0f, 0.0f, -30.0f);
    const Vec3 claimed(40.0f, 0.0f, -30.0f);
    const float r = swarm::Distance(self, claimed);
    CHECK(HeartbeatPlausible(self, claimed, r, 0.5f));
    // Within 3σ + 2 m of a 0.5 m sigma (~3.5 m).
    CHECK(HeartbeatPlausible(self, claimed, r + 1.0f, 0.5f));
    // A replay from the far side of the arena is tens of metres off.
    CHECK(!HeartbeatPlausible(self, claimed, r + 50.0f, 0.5f));
    CHECK(!HeartbeatPlausible(self, claimed, 5.0f, 0.5f));
}

static void TestPeerReportAssociatesByGeometry() {
    std::printf("peer reports associate by geometry, not track_id\n");
    TrackStore store;
    const Vec3 p(170.0f, 0.0f, -40.0f);
    const Vec3 v(-13.0f, 0.0f, 0.0f);

    store.MergePeerReport(p, v, Belief::Hostile, 255, 10.0f, /*origin=*/3, /*hops=*/2);
    CHECK(store.tracks().size() == 1);
    CHECK(!store.tracks()[0].has_local_id);
    CHECK(store.tracks()[0].belief == Belief::Hostile);
    CHECK(store.tracks()[0].last_origin == 3);
    CHECK(store.tracks()[0].last_hops == 2);
    CHECK(store.FindByStoreId(store.tracks()[0].store_id) == &store.tracks()[0]);
    CHECK(store.Find(0) == nullptr);   // hearsay must not collide with local id 0

    // A fresh call used to send confidence 72 (score*120). One report must
    // still latch: the facing owner cannot wait for three 0.5 s packets.
    TrackStore slow;
    slow.MergePeerReport(p, v, Belief::Hostile, 72, 10.0f, 3, 0);
    CHECK(slow.tracks().size() == 1);
    CHECK(slow.tracks()[0].belief == Belief::Hostile);

    // Same aircraft, a few metres off (two fix biases). One row.
    store.MergePeerReport(Vec3(174.0f, 2.0f, -40.0f), v, Belief::Hostile, 255,
                          10.4f, 3, 2);
    CHECK(store.tracks().size() == 1);

    // A different aircraft 30 m away. Second row.
    store.MergePeerReport(Vec3(170.0f, 30.0f, -40.0f), v, Belief::Hostile, 255,
                          10.5f, 4, 1);
    CHECK(store.tracks().size() == 2);
}

int main() {
    TestRangeRate();
    TestApproachAlignment();
    TestTimeToTarget();
    TestTimeToCylinder();
    TestThreatWindowIsFiniteWhileSpooling();
    TestLooksDivingAtAsset();
    TestGroundTrackHitsCylinder();
    TestClosingSpeed();
    TestBallistic();
    TestMissDistanceSeparatesTheHardCase();
    TestMissDistanceIgnoresThePast();
    TestAimedAtAssetRejectsTheG1aChord();
    TestAimedAtAssetShrinkVsNoise();
    TestDiveIsAHostileSignatureWhenTimeIsShort();
    TestLevelOverflightIsNotAimed();
    TestLevelDashIsNotAHit();
    TestHeartbeatRangeCorroboration();
    TestPeerReportAssociatesByGeometry();

    if (g_failures == 0) {
        std::printf("belief: all passed\n");
        return 0;
    }
    std::printf("belief: %d failure(s)\n", g_failures);
    return 1;
}
