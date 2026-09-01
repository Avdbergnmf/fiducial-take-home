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

// ---------------------------------------------------------------------------
// The case that decides the tier-1 awareness score. A civilian on a chord that
// happens to point near the asset for a while is the expensive false positive:
// a wrong call costs -2 where a correct one earns +1.
// ---------------------------------------------------------------------------

static void TestMissDistanceSeparatesTheHardCase() {
    std::printf("miss distance separates a chord-crossing civilian from a dash\n");

    // A civilian crossing on a straight line that passes 40 m from the asset.
    const Vec3 civ_p(150.0f, 40.0f, -50.0f);
    const Vec3 civ_v(-16.0f, 0.0f, 0.0f);

    // A hostile dashing in from a similar bearing.
    const Vec3 hos_p(170.0f, 0.0f, -40.0f);
    const Vec3 hos_v(-16.0f, 0.0f, 0.0f);

    // Alignment CANNOT tell them apart -- both read as closing and well aimed.
    // This is exactly the -2 that a naive classifier pays.
    CHECK(ApproachAlignment(civ_p, civ_v, kAsset) > 0.9f);
    CHECK(ApproachAlignment(hos_p, hos_v, kAsset) > 0.9f);

    // Miss distance does.
    const float civ_miss = ClosestApproachDistance(civ_p, civ_v, kAsset);
    const float hos_miss = ClosestApproachDistance(hos_p, hos_v, kAsset);
    CHECK(civ_miss > 35.0f);
    CHECK(hos_miss < 1.0f);

    std::printf("    civilian misses by %.1f m, hostile by %.1f m\n",
                static_cast<double>(civ_miss), static_cast<double>(hos_miss));
}

static void TestMissDistanceIgnoresThePast() {
    std::printf("something already receding misses by its current range\n");
    // Past the asset and running: closest approach is behind it, so the miss
    // distance must be the range now, not a negative-time extrapolation.
    const float miss = ClosestApproachDistance(Vec3(50, 0, -40), Vec3(16, 0, 0), kAsset);
    CHECK(miss > 49.0f && miss < 51.0f);
}

// ---------------------------------------------------------------------------
// AimedAtAsset is the gate Classify actually uses. The 40 m civilian above
// never entered the old 24 m gate, so that test could not catch G1a.
// ---------------------------------------------------------------------------

static void TestAimedAtAssetRejectsTheG1aChord() {
    std::printf("AimedAtAsset rejects a 20 m chord that alignment would call hostile\n");
    const float asset_radius = 30.0f;

    // G1a: civilian on a chord that passes 20 m from the origin. Alignment and
    // closing look like a dash; the old 24 m gate called this enemy before t=8.
    const Vec3 civ_p(150.0f, 20.0f, -50.0f);
    const Vec3 civ_v(-16.0f, 0.0f, 0.0f);
    CHECK(ApproachAlignment(civ_p, civ_v, kAsset) > 0.9f);
    const float civ_miss = ClosestApproachDistance(civ_p, civ_v, kAsset);
    CHECK(civ_miss > 19.0f && civ_miss < 21.0f);
    CHECK(!AimedAtAsset(civ_miss, civ_miss, asset_radius));

    // s1 hostile dashing at the origin: CPA miss ~0 from first sight.
    const Vec3 hos_p(170.0f, 0.0f, -40.0f);
    const Vec3 hos_v(-16.0f, 0.0f, 0.0f);
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

int main() {
    TestRangeRate();
    TestApproachAlignment();
    TestTimeToTarget();
    TestBallistic();
    TestMissDistanceSeparatesTheHardCase();
    TestMissDistanceIgnoresThePast();
    TestAimedAtAssetRejectsTheG1aChord();
    TestAimedAtAssetShrinkVsNoise();

    if (g_failures == 0) {
        std::printf("belief: all passed\n");
        return 0;
    }
    std::printf("belief: %d failure(s)\n", g_failures);
    return 1;
}
