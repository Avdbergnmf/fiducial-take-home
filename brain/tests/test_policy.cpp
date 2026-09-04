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

static void TestFacingSlotAgreesOnABisector() {
    std::printf("two observers on a slot bisector name the same owner\n");
    const Vec3 asset(0, 0, 0);
    const uint32_t n = 14;
    // x1-e1257f: drones 12 and 13 logged these NED poses 40 ms apart.
    CHECK(FacingSlot(Vec3(95.5f, -78.6f, -25.0f), asset, n) ==
          FacingSlot(Vec3(96.6f, -77.0f, -25.0f), asset, n));

    const float u = (12.5f / 14.0f) * 2.0f * 3.14159265358979f;
    const float r = 120.0f;
    const Vec3 mid(r * std::cos(u), r * std::sin(u), -40.0f);
    CHECK(FacingSlot(mid, asset, n) ==
          FacingSlot(Vec3(mid.x + 1.0f, mid.y - 1.0f, mid.z), asset, n));
    CHECK(FacingSlot(mid, asset, n) ==
          FacingSlot(Vec3(mid.x - 1.0f, mid.y + 1.0f, mid.z), asset, n));
    for (uint32_t id = 0; id < n; ++id) {
        const Vec3 slot = flight::RingSlot(id, n, asset, 79.3f, 30.0f);
        CHECK(FacingSlot(slot, asset, n) == id);
    }
}

static void TestOtherInterceptorTieBreak() {
    std::printf("similar-range duplicate yields to the lower id\n");
    CHECK(OtherInterceptorWins(49.0f, 13, 49.0f, 12) == true);
    CHECK(OtherInterceptorWins(49.0f, 12, 49.0f, 13) == false);
    CHECK(OtherInterceptorWins(49.0f, 12, 46.0f, 13) == true);
    CHECK(OtherInterceptorWins(49.0f, 13, 49.0f, -1) == false);
}

static void TestInterceptorKeepsGoingAtTheMerge() {
    std::printf("interceptor does not brake for a wingman at the ram\n");
    Config cfg;
    cfg.kill_radius = 1.0f;
    cfg.lateral_limit = 6.71f;
    cfg.friendly_margin = 19.0f;
    cfg.separation_margin = 4.0f;
    cfg.max_accel = 15.0f;

    Track hostile{};
    hostile.track_id = 9;
    hostile.has_local_id = true;
    hostile.position = Vec3(50, 0, -20);
    hostile.velocity = Vec3(-14, 0, 0);
    hostile.belief = Belief::Hostile;

    Track mate{};
    mate.track_id = 6;
    mate.has_local_id = true;
    mate.position = Vec3(8, 8, -20);
    mate.velocity = Vec3(14, 0, 0);
    mate.belief = Belief::Friendly;

    FixedVec<Track, kMaxTracks> wing;
    wing.push(hostile);
    wing.push(mate);

    const Vec3 us(0, 0, -20);
    const Vec3 us_v(14, 0, 0);
    const Vec3 desired(6.71f, 0, 0);
    const Vec3 out = flight::EnforceSeparation(desired, us, us_v, wing, cfg,
                                               &hostile, true);
    CHECK(out.x > 3.0f);
    CHECK(std::fabs(out.y) < out.x);

    Track picket = mate;
    picket.position = Vec3(10, 0, -20);
    picket.velocity = Vec3(0, 0, 0);
    FixedVec<Track, kMaxTracks> front;
    front.push(hostile);
    front.push(picket);
    const Vec3 braked = flight::EnforceSeparation(desired, us, us_v, front, cfg,
                                                  &hostile, true);
    CHECK(braked.x < desired.x - 0.5f);
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

static void TestInboundOwnerSkipsARecedingFacing() {
    std::printf("receding facing yields to the first live clockwise\n");
    float heard[kMaxFleet];
    for (uint32_t i = 0; i < kMaxFleet; ++i) heard[i] = -1.0e9f;
    const uint32_t n = 10;
    const float now = 20.0f;

    CHECK(InboundOwner(0, n, 0, heard, now, false) == 0);
    CHECK(InboundOwner(0, n, 1, heard, now, false) == 0);
    CHECK(InboundOwner(0, n, 1, heard, now, true) == 1);
    CHECK(InboundOwner(0, n, 0, heard, now, true) == 1);

    // Clockwise neighbour dead: walk continues, still not the receding facing.
    heard[1] = now - 2.0f;
    CHECK(InboundOwner(0, n, 2, heard, now, true) == 2);
    CHECK(InboundOwner(0, n, 0, heard, now, true) == 2);

    // x1-b403 numbers: drone 0 at (78.5, 15.9) going (1.1, -5.4), hostile
    // at (111.9, 57.3). Toward is negative; a picket at rest is not.
    CHECK(TowardTarget(Vec3(78.5f, 15.9f, -30.0f), Vec3(1.1f, -5.4f, 0.0f),
                       Vec3(111.9f, 57.3f, -40.0f)) < -2.0f);
    CHECK(TowardTarget(Vec3(45.8f, 66.0f, -30.0f), Vec3(0.2f, 0.3f, 0.0f),
                       Vec3(111.9f, 57.3f, -40.0f)) > 0.0f);
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
    std::printf("live ring re-spaces a death the whole fleet can hear\n");
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

    // Heard, then silent: dead everywhere, not only next door (D56).
    heard[8] = now - 2.0f;
    CHECK(SlotAlive(8, 0, heard, now) == false);
    CHECK(RingAlive(8, 0, heard, at, self0, comm, now) == false);
    CHECK(CountLive(n0, 0, heard, at, self0, comm, now) == 15);
    CHECK(RingAlive(8, 7, heard, at, self7, comm, now) == false);
    CHECK(LiveRank(7, n0, 7, heard, at, self7, comm, now) == 7);
    CHECK(LiveRank(9, n0, 9, heard, at, self9, comm, now) == 8);
    CHECK(LiveId(8, n0, 7, heard, at, self7, comm, now) == 9);

    const Vec3 new9 = flight::RingSlot(
        LiveRank(9, n0, 9, heard, at, self9, comm, now), 15, asset, 75.0f, 30.0f);
    CHECK(Horiz(new9, at[8]) + 1.0f < Horiz(at[9], at[8]));

    const uint32_t facing = FacingSlot(at[8], asset, 15);
    CHECK(LiveId(facing, n0, 7, heard, at, self7, comm, now) == 9);

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

static void TestRingStaysInsideTheSpawnCircle() {
    std::printf("ring is capped well inside where hostiles enter\n");
    // The failure this exists for: a small arena with a generous radio put the
    // ring at 0.78 of the spawn radius, so hostiles were born 27 m outside it
    // and were inside the picket before they could be called. -519 on that id.
    Config cfg;
    cfg.drone_id = 0;
    cfg.fleet_size = 16;
    cfg.asset = Vec3(0, 0, 0);
    cfg.asset_radius = 30.0f;
    cfg.comm_radius = 118.0f;          // generous radio...
    cfg.sense_radius = 92.0f;
    cfg.arena_min = Vec3(-158, -158, -120);   // ...small arena
    cfg.arena_max = Vec3(158, 158, 0);

    Policy p;
    p.Configure(cfg, Rng());
    const float spawn_radius = 0.80f * 158.0f;
    CHECK(p.ring_radius() <= spawn_radius * 0.65f + 1e-3f);
    CHECK(p.ring_radius() > cfg.asset_radius);

    // A ring can be comfortably inside the spawn circle by ratio and still be
    // only a couple of seconds away from it at closing speed, which is the same
    // failure wearing a different number. x1-814fd5e7: spawn 144 m, ring 90 m,
    // so 0.62 of the spawn radius -- under the ratio cap -- but 54 m of margin,
    // about 2.8 s, barely the time to classify. It scored 0/3 with 3 breaches.
    Config tight;
    tight.drone_id = 0;
    tight.fleet_size = 16;
    tight.asset = Vec3(0, 0, 0);
    tight.asset_radius = 30.0f;
    tight.comm_radius = 87.5f;
    tight.sense_radius = 71.4f;
    tight.max_speed = 24.2f;
    tight.arena_min = Vec3(-180, -180, -120);
    tight.arena_max = Vec3(180, 180, 0);
    Policy narrow;
    narrow.Configure(tight, Rng());
    const float spawn = 0.80f * 180.0f;
    CHECK(narrow.ring_radius() < spawn * 0.65f);      // ratio alone would allow more
    CHECK(spawn - narrow.ring_radius() >= 3.5f * tight.max_speed - 1e-3f);

    // A big arena leaves the radio rule in charge: the cap must not bite when
    // there is plenty of room, or it would give away standoff for nothing.
    cfg.arena_min = Vec3(-400, -400, -120);
    cfg.arena_max = Vec3(400, 400, 0);
    Policy wide;
    wide.Configure(cfg, Rng());
    CHECK(std::fabs(wide.ring_radius()
                    - (cfg.asset_radius + cfg.comm_radius * 0.625f)) < 1e-3f);

    const float full_radius = PicketRadius(cfg, 16);
    const float fifteen_radius = PicketRadius(cfg, 15);
    CHECK(fifteen_radius < full_radius);
    CHECK(std::fabs(fifteen_radius - full_radius *
                    std::sin(3.14159265358979f / 16.0f) /
                    std::sin(3.14159265358979f / 15.0f)) < 1e-3f);
}

static Config WideRadioCfg() {
    Config cfg;
    cfg.drone_id = 0;
    cfg.fleet_size = 16;
    cfg.asset = Vec3(0, 0, 0);
    cfg.asset_radius = 30.0f;
    cfg.comm_radius = 90.0f;
    cfg.sense_radius = 60.0f;
    cfg.max_speed = 20.0f;
    cfg.lateral_limit = 6.7f;
    cfg.kill_radius = 1.0f;
    cfg.arena_min = Vec3(-400, -400, -120);
    cfg.arena_max = Vec3(400, 400, 0);
    return cfg;
}

static void TestClosedCoverLeavesAFullS1Ring() {
    std::printf("s1-like full ring is already catchable; radio still sizes it\n");
    const Config cfg = WideRadioCfg();
    const float radio = cfg.asset_radius + cfg.comm_radius * 0.625f;
    CHECK(std::fabs(PicketRadius(cfg, 16, 30.0f) - radio) < 1e-3f);
    CHECK(std::fabs(PicketRadius(cfg, 16, 25.0f) - radio) < 1e-3f);
}

static void TestClosedCoverPullsASparseRingIn() {
    std::printf("six even stations shrink until leftover Reach meets kCoverSlack\n");
    Config cfg = WideRadioCfg();
    cfg.fleet_size = 6;
    const float radio = cfg.asset_radius + cfg.comm_radius * 0.625f;
    const float r = PicketRadius(cfg, 6, 30.0f);
    CHECK(kCoverSlack > 4.0f && kCoverSlack < 6.0f);
    CHECK(r < radio - 1.0f);
    CHECK(r > 66.0f);
    CHECK(r < 74.0f);
}

static void TestClosedCoverBindsOnTightSense() {
    std::printf("sense 35 m cannot tile an 86 m 16-picket ring\n");
    Config cfg = WideRadioCfg();
    cfg.sense_radius = 35.0f;
    const float radio = cfg.asset_radius + cfg.comm_radius * 0.625f;
    const float r = PicketRadius(cfg, 16, 30.0f);
    CHECK(r < radio - 1.0f);
    CHECK(r > 70.0f);
    CHECK(r < 84.0f);
}

static void TestDefaultPicketAltitudeIsTwentyFive() {
    std::printf("boot picket sits at 25 m; fitted cone may still rise to 30\n");
    Policy p;
    p.Configure(WideRadioCfg(), Rng());
    CHECK(p.ring_altitude() > kRayDefaultAlt - 0.1f &&
          p.ring_altitude() < kRayDefaultAlt + 0.1f);
    CHECK(kRayDefaultAlt == 25.0f);
    CHECK(kRayCapAlt == 30.0f);
    CHECK(kRayCapAlt > kRayDefaultAlt);
}

static void TestAimAheadUsesConfiguredDistance() {
    std::printf("aim lead moves the target estimate along its velocity\n");
    const Vec3 target(10, 20, -30);
    const Vec3 velocity(3, 4, 0);
    const Vec3 aimed = flight::AimAhead(target, velocity, 1.0f);
    CHECK(std::fabs(aimed.x - 10.6f) < 1e-4f);
    CHECK(std::fabs(aimed.y - 20.8f) < 1e-4f);
    CHECK(std::fabs(aimed.z + 30.0f) < 1e-4f);
    CHECK(std::fabs(swarm::Distance(aimed, target) - 1.0f) < 1e-4f);
    CHECK(swarm::Distance(flight::AimAhead(target, Vec3(), 1.0f), target) < 1e-4f);
}

static void TestProNavSteersAtTheZem() {
    std::printf("ProNav is ZEM across the LOS; a weave shows up in AZEM\n");
    Config cfg;
    cfg.max_speed = 20.0f;
    cfg.max_accel = 15.0f;
    cfg.lateral_limit = 6.71f;
    cfg.kill_radius = 1.0f;

    const Vec3 self(0, 0, -30);
    const Vec3 self_v(20, 0, 0);
    const Vec3 tgt(20, 1.5f, -30);
    const Vec3 tgt_v(-20, 0, 0);
    const Vec3 accel = flight::ProNav(self, self_v, tgt, tgt_v, cfg);
    CHECK(accel.y > 0.5f * cfg.lateral_limit);

    const Vec3 explicit_lead = flight::ProNav(
        self, self_v, tgt, tgt_v, cfg, Vec3(), flight::kPnLeadKillRadii);
    CHECK(swarm::Distance(accel, explicit_lead) < 1e-5f);

    const Vec3 no_lead = flight::ProNav(self, self_v, tgt, tgt_v, cfg, Vec3(), 0.0f);
    CHECK(swarm::Distance(accel, no_lead) > 1e-4f);

    const Vec3 mirrored = flight::ProNav(
        self, self_v, Vec3(20, -1.5f, -30), tgt_v, cfg);
    CHECK(mirrored.y < -0.5f * cfg.lateral_limit);

    const Vec3 straight = flight::ProNav(
        self, self_v, Vec3(20, 0, -30), tgt_v, cfg);
    CHECK(std::fabs(straight.y) < 0.1f * cfg.lateral_limit);

    // Collision course from rest: ZEMn is ~0. We still accelerate along the
    // LOS so we are a ram, not a sitting target. A weave adds AZEM across it.
    const Vec3 sit = flight::ProNav(Vec3(70, 0, -30), Vec3(),
                                    Vec3(170, 0, -30), Vec3(-15, 0, 0), cfg);
    CHECK(sit.x > 0.5f * cfg.lateral_limit);
    CHECK(std::fabs(sit.y) < 0.2f * cfg.lateral_limit);

    const Vec3 weave = flight::ProNav(Vec3(70, 0, -30), Vec3(),
                                      Vec3(170, 0, -30), Vec3(-15, 0, 0), cfg,
                                      Vec3(0, 6.0f, 0));
    CHECK(weave.y > 0.1f * cfg.lateral_limit);

    // Lead is "they have already flown 0.5 kr along their track", then
    // intercept that state — the same command as ProNav on AimAhead with
    // lead 0. It is not an extra 0.5 kr past the intercept of the real body.
    const float lead_m = flight::kPnLeadKillRadii * cfg.kill_radius;
    const Vec3 shifted = flight::AimAhead(tgt, tgt_v, lead_m);
    const Vec3 via_shift = flight::ProNav(
        self, self_v, shifted, tgt_v, cfg, Vec3(), 0.0f);
    CHECK(swarm::Distance(accel, via_shift) < 1e-4f);

    // A weave already in the predicted trajectory is part of that same
    // lead: advancing along p + v t + ½ a t², not along v alone.
    const Vec3 weave_a(0, 6.0f, 0);
    const Vec3 with_traj = flight::ProNav(
        self, self_v, tgt, tgt_v, cfg, weave_a, flight::kPnLeadKillRadii);
    const Vec3 along_v_only = flight::ProNav(
        self, self_v, shifted, tgt_v, cfg, weave_a, 0.0f);
    CHECK(swarm::Distance(with_traj, along_v_only) > 1e-4f);
}

static void TestInterceptScorePrefersTheOneThatConnects() {
    std::printf("score: a hit beats a miss, and the sooner hit wins (D66)\n");
    Config cfg;
    cfg.max_speed = 20.0f;
    cfg.max_accel = 15.0f;
    cfg.lateral_limit = 6.71f;
    cfg.kill_radius = 1.0f;

    // Hostile running in along -x at 30 m/s.
    const Vec3 tp(120.0f, 0.0f, -30.0f), tv(-30.0f, 0.0f, 0.0f);

    // Near drone, parked on the hostile's line: connects.
    const float near = InterceptScore(Vec3(40, 0, -30), Vec3(), tp, tv, cfg);
    // Far drone way off to the side: cannot get across in time.
    const float far = InterceptScore(Vec3(-150, 160, -30), Vec3(), tp, tv, cfg);
    CHECK(near < kNoHit);          // it hits, so the score is its t_go
    CHECK(far >= kNoHit);          // it does not, so it is ranked below every hit
    CHECK(near < far);

    // Among two that both connect, the one arriving sooner scores lower.
    const float closer = InterceptScore(Vec3(80, 0, -30), Vec3(), tp, tv, cfg);
    CHECK(closer < kNoHit && closer <= near);
}

static void TestScoreSeesTheVerticalBudget() {
    std::printf("score prices the anisotropy: z has max_accel, xy has tilt\n");
    Config cfg;
    cfg.max_speed = 20.0f;
    cfg.max_accel = 15.0f;         // vertical authority
    cfg.lateral_limit = 6.71f;     // horizontal is 2.2x smaller
    cfg.kill_radius = 1.0f;

    const Vec3 tp(120.0f, 0.0f, -30.0f), tv(-30.0f, 0.0f, 0.0f);

    // Same 25 m of offset from the hostile's track, once vertical and once
    // horizontal. The drone that must climb has the bigger budget, so it is
    // the better interceptor -- which a range-only score cannot tell apart.
    const float vertical = InterceptScore(Vec3(60, 0, -5), Vec3(), tp, tv, cfg);
    const float lateral = InterceptScore(Vec3(60, 25, -30), Vec3(), tp, tv, cfg);
    CHECK(vertical <= lateral);
}

static void TestRecedingIncumbentHandsEqualScore() {
    std::printf("receding incumbent: equal t_go is a handoff, not a score tax (D67)\n");
    // x2-fa56ef171718281fef54383d2dba36d6 t=12.5: facing drone 1 and the
    // neighbour already closing both hit in the 2.80 s bin. A score margin
    // kept the receding facing drone; it passed at 1.4 m and the inbound
    // breached. Aspect 0.5 m/s is the noise floor on that tie. Approaching
    // incumbents still pay kHandoffMargin (0.25 s, D69).
    CHECK(BeatsIncumbent(2.80f, 2.80f, 3.8f, -1.2f));
    CHECK(!BeatsIncumbent(2.80f, 2.80f, 3.8f, 3.8f));
    CHECK(!BeatsIncumbent(3.80f, 2.80f, 3.8f, -1.2f));
    CHECK(BeatsIncumbent(1.70f, 2.80f, 3.8f, 3.8f));
    CHECK(!BeatsIncumbent(2.80f, 2.80f, -1.1f, -1.2f));
    CHECK(BeatsIncumbent(2.50f, 2.80f, 3.8f, 3.8f));
    CHECK(!BeatsIncumbent(2.60f, 2.80f, 3.8f, 3.8f));
    CHECK(kHandoffMargin > 0.24f && kHandoffMargin < 0.26f);
    CHECK(kHandoffAspect > 0.2f);
}

static void TestStationYawFollowsVelocity() {
    std::printf("station yaw follows velocity when moving; outward when parked\n");
    const Vec3 pos(80.0f, 0.0f, -25.0f);
    const Vec3 asset(0.0f, 0.0f, 0.0f);
    const float moving = flight::DesiredYaw(Mode::Picketing, pos,
                                            Vec3(0.0f, 4.5f, 0.0f), asset,
                                            nullptr);
    CHECK(std::fabs(moving - std::atan2(4.5f, 0.0f)) < 0.05f);
    const float parked = flight::DesiredYaw(Mode::Picketing, pos, Vec3(),
                                            asset, nullptr);
    CHECK(std::fabs(parked) < 0.05f);
}

static void TestFacingSlotDeSpinsTheOrbit() {
    std::printf("ownership de-spins the ring: a bearing maps to who is there now\n");
    const Vec3 asset(0, 0, 0);
    const uint32_t n = 12;
    const float step = 2.0f * 3.14159265358979f / static_cast<float>(n);

    // Slot `id` has orbited to bearing step*id + phase. Quantising that world
    // bearing with the same phase must name `id` again. Without the de-spin it
    // names whoever used to stand there, which is how the fleet collapses.
    static const float kPhases[] = {0.0f, 0.35f, 2.9f, 6.0f};
    for (float phase : kPhases) {
        for (uint32_t id = 0; id < n; ++id) {
            const float b = step * static_cast<float>(id) + phase;
            const Vec3 out(90.0f * std::cos(b), 90.0f * std::sin(b), -30.0f);
            CHECK(FacingSlot(out, asset, n, phase) == id);
        }
    }
}

static void TestArenaAllowsADiveIntercept() {
    std::printf("arena floor is stopping distance, not a 20 m halo\n");
    Config cfg;
    cfg.max_accel = 19.7f;
    cfg.lateral_limit = 6.71f;
    cfg.arena_min = Vec3(-200, -200, -100);
    cfg.arena_max = Vec3(200, 200, 0);

    // Drone 7 at t=27.6: 12.6 m up, diving 8 m/s. Old 20 m edge pushed UP
    // at 10 m/s^2 and killed the intercept. Stopping distance is ~4 m.
    const Vec3 dive = flight::EnforceArena(
        Vec3(0, 0, 15.0f), Vec3(0, 0, -12.6f), Vec3(0, 0, 8.4f), cfg);
    CHECK(dive.z > 5.0f);

    // Inside stopping distance of the dirt: reduce the dive. (v=10 at 2 m
    // cannot actually stop — that is why the band starts earlier.)
    const Vec3 floor = flight::EnforceArena(
        Vec3(0, 0, 15.0f), Vec3(0, 0, -2.0f), Vec3(0, 0, 10.0f), cfg);
    CHECK(floor.z < dive.z - 1.0f);
}

static void TestProNavDoesNotBrakeAlongTheLos() {
    std::printf("a ram never commands away from the target along the LOS\n");
    Config cfg;
    cfg.max_speed = 24.6f;
    cfg.max_accel = 19.7f;
    cfg.lateral_limit = 6.71f;
    cfg.kill_radius = 1.907f;

    // Drone 7 / hostile_1 at t=27.8: 3.4 m, closing already gone. Old PN
    // used t_go = range/max_speed = 0.14 s and saturated 19.7 m/s^2 UP.
    const Vec3 self(-37.9f, -39.5f, -11.3f);
    const Vec3 self_v(-1.0f, 5.0f, 5.06f);
    const Vec3 tgt(-38.8f, -36.4f, -10.0f);
    const Vec3 tgt_v(12.7f, 12.2f, 3.32f);
    const Vec3 a = flight::ProNav(self, self_v, tgt, tgt_v, cfg);
    const Vec3 los = tgt - self;
    const float along = swarm::Dot(a, los);
    CHECK(along > 0.0f);
    CHECK(a.z > 0.0f);
}

static void TestCollisionCourseCutsOffACrossingInbound() {
    std::printf("collision course aims at the meeting point, not the current body\n");
    Config cfg;
    cfg.max_speed = 24.6f;
    cfg.max_accel = 19.7f;
    cfg.lateral_limit = 6.71f;
    cfg.kill_radius = 1.907f;

    // Head-on from rest: intercept is along the LOS, so +x, not a dive.
    const Vec3 sit = flight::CollisionCourse(
        Vec3(70, 0, -30), Vec3(), Vec3(170, 0, -30), Vec3(-15, 0, 0), cfg);
    CHECK(sit.x > 0.5f * cfg.lateral_limit);
    CHECK(std::fabs(sit.y) < 0.2f * cfg.lateral_limit);
    CHECK(std::fabs(sit.z) < 0.3f * cfg.max_accel);

    // Drone 3 vs hostile_0 on x1-06b926af at ~17.5 s (NED). Hostile flies
    // west/down in a straight line. Current body is still east of us;
    // the meeting point is west. PN N=6 + along-LOS close saturates z
    // (19.7) and over-dives; collision course aims at I, not the body.
    const Vec3 self(-7.45f, 57.2f, -32.5f);
    const Vec3 self_v(0.0f, 2.5f, 2.3f);
    const Vec3 tgt(6.87f, 90.5f, -17.1f);
    const Vec3 tgt_v(-1.2f, -17.9f, 3.32f);
    const Vec3 los = tgt - self;
    const Vec3 cc = flight::CollisionCourse(self, self_v, tgt, tgt_v, cfg);
    const Vec3 pn = flight::ProNav(self, self_v, tgt, tgt_v, cfg);
    CHECK(cc.x > 0.5f);          // north, onto their track
    CHECK(cc.y < 0.0f);          // west, to the intercept ahead of the body
    CHECK(cc.y * los.y < 0.0f);  // not pursuing the current east LOS
    CHECK(cc.z > 0.0f);          // down, but not the leftover z-budget
    CHECK(cc.z < pn.z - 4.0f);
    CHECK(pn.z > 15.0f);

    Track focus;
    focus.position = tgt;
    focus.velocity = tgt_v;
    focus.last_velocity = tgt_v;
    const Vec3 ram = flight::DesiredAccel(
        Mode::Ramming, self, self_v, Vec3(), &focus, false, 0.01f, cfg);
    CHECK(swarm::Distance(ram, cc) < 1e-4f);
    const Vec3 scramble = flight::DesiredAccel(
        Mode::Scrambling, self, self_v, Vec3(), &focus, false, 0.01f, cfg);
    CHECK(swarm::Distance(scramble, cc) < 1e-4f);

    // Drone 7 / hostile_1 at t=27.8: 3.4 m, closing gone. Command must
    // still point at the intercept, not up off it (D45).
    const Vec3 a7 = flight::CollisionCourse(
        Vec3(-37.9f, -39.5f, -11.3f), Vec3(-1.0f, 5.0f, 5.06f),
        Vec3(-38.8f, -36.4f, -10.0f), Vec3(12.7f, 12.2f, 3.32f), cfg);
    const Vec3 to_tgt = Vec3(-38.8f, -36.4f, -10.0f)
                        - Vec3(-37.9f, -39.5f, -11.3f);
    CHECK(swarm::Dot(a7, to_tgt) > 0.0f);

    Config settled;
    settled.max_speed = 23.9f;
    settled.max_accel = 19.1f;
    settled.lateral_limit = 6.71f;
    settled.kill_radius = 1.157f;
    settled.max_tilt = 0.6f;
    CHECK(flight::TiltSettle(settled) < 1e-4f);  // rate unpublished
    settled.max_body_rate = 8.0f;
    CHECK(std::fabs(flight::TiltSettle(settled) - 0.6f / 8.0f) < 1e-4f);

    const Vec3 p0(60.85f, 41.5f, -26.11f);
    const Vec3 q0(82.55f, 98.41f, -20.95f);
    const Vec3 w0(-11.66f, -13.9f, 2.96f);
    const flight::Course c0 = flight::SolveCollisionCourse(
        p0, Vec3(), q0, w0, settled);
    CHECK(c0.t_go > 0.5f);
    CHECK(swarm::Length(c0.meeting - q0) > 1.0f);
    const flight::Course c1 = flight::SolveCollisionCourse(
        p0, Vec3(), q0, w0, settled, Vec3(), Vec3(), c0.t_go - 0.01f);
    CHECK(std::fabs(c1.t_go - (c0.t_go - 0.01f)) < 1e-3f);

    const Vec3 slew = flight::SlewHorizontal(
        Vec3(6.71f, 0.0f, 1.0f), Vec3(0.0f, 6.71f, -2.0f), 0.01f, settled);
    const float da = std::sqrt((slew.x - 6.71f) * (slew.x - 6.71f)
                               + slew.y * slew.y);
    CHECK(da <= 9.81f * 8.0f * 0.01f + 1e-3f);
    CHECK(std::fabs(slew.z + 2.0f) < 1e-4f);
}

static void TestStationEvensTheLiveRing() {
    std::printf("stations even out over the live roster\n");
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
    // The ring may be orbiting, in which case every bearing carries the same
    // kOrbitRate*now phase. The invariant under test is even spacing in rank
    // order, so measure against the phase rather than against zero (D66).
    const float phase = kOrbitRate * now;

    for (uint32_t id = 0; id < n0; ++id) {
        const float b = StationBearing(id, n0, id, heard, at, at[id], comm, now);
        CHECK(std::fabs(b - phase - step * static_cast<float>(id)) < 1e-4f);
    }

    heard[0] = now - 3.0f;
    heard[1] = now - 3.0f;
    heard[2] = now - 3.0f;

    CHECK(CountLive(n0, 3, heard, at, at[3], comm, now) == 13);
    CHECK(CountLive(n0, 8, heard, at, at[8], comm, now) == 13);

    const float live_step = 2.0f * 3.14159265358979f / 13.0f;
    const float b3 = StationBearing(3, n0, 3, heard, at, at[3], comm, now);
    CHECK(std::fabs(b3 - phase - 0.0f) < 1e-4f);

    const float b8 = StationBearing(8, n0, 8, heard, at, at[8], comm, now);
    CHECK(std::fabs(b8 - phase - live_step * 5.0f) < 1e-4f);

    const float b15 = StationBearing(15, n0, 15, heard, at, at[15], comm, now);
    CHECK(std::fabs(b15 - phase - live_step * 12.0f) < 1e-4f);
}

static void TestHeardSilenceIsDeadEverywhere() {
    std::printf("heard-then-silent is dead at any range\n");
    float heard[kMaxFleet];
    Vec3 at[kMaxFleet];
    uint8_t dead[kMaxFleet]{};
    InitHeard(heard);
    const uint32_t n0 = 16;
    const float now = 20.0f;
    const float comm = 90.0f;
    const Vec3 asset(0, 0, 0);
    FillStations(at, asset, n0);

    heard[1] = now - 2.0f;
    CHECK(RingAlive(1, 0, heard, at, at[8], comm, now, n0, dead) == false);
    CHECK(dead[1] != 0);
    CHECK(RingAlive(1, 0, heard, at, at[0], comm, now, n0, dead) == false);

    heard[1] = now;
    CHECK(RingAlive(1, 0, heard, at, at[0], comm, now, n0, dead) == true);
    CHECK(dead[1] == 0);
}

static void TestApproachingFarSilenceDoesNotKill() {
    std::printf("a confirmed death stays dead until a heartbeat\n");
    float heard[kMaxFleet];
    Vec3 at[kMaxFleet];
    uint8_t dead[kMaxFleet]{};
    InitHeard(heard);
    const uint32_t n0 = 16;
    const float now = 20.0f;
    const float comm = 90.0f;
    const Vec3 asset(0, 0, 0);
    FillStations(at, asset, n0);

    heard[8] = now - 2.0f;
    CHECK(RingAlive(8, 0, heard, at, at[0], comm, now, n0, dead) == false);
    CHECK(dead[8] != 0);
    CHECK(RingAlive(8, 0, heard, at, at[8], comm, now, n0, dead) == false);

    heard[8] = now;
    CHECK(RingAlive(8, 0, heard, at, at[0], comm, now, n0, dead) == true);
    CHECK(dead[8] == 0);
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

static void TestFirstYielderKeepsTheIntercept() {
    std::printf("a mate already peeling off the corridor is not yielded to\n");
    const Vec3 slot(75.0f, 0.0f, -30.0f);
    const Vec3 hostile(170.0f, 0.0f, -40.0f);
    const Vec3 inbound(-16.0f, 0.0f, 0.0f);
    const float clear = 19.0f;
    const Vec3 end = CorridorHorizon(slot, hostile, inbound);

    // Still on the line, chasing: not yielded.
    CHECK(!MateAlreadyYielded(Vec3(100.0f, 0.5f, -30.0f), Vec3(14.0f, 0.0f, 0.0f),
                              slot, end, clear));
    // Off by a metre but still flying along the LOS: interceptor weave, stay.
    CHECK(!MateAlreadyYielded(Vec3(100.0f, 1.0f, -30.0f), Vec3(14.0f, 0.5f, 0.0f),
                              slot, end, clear));
    // Peeling +y at 8 m/s, 8 m off: first yielder.
    CHECK(MateAlreadyYielded(Vec3(100.0f, 8.0f, -30.0f), Vec3(2.0f, 8.0f, 0.0f),
                             slot, end, clear));
    // Fully clear of keep-out, even if still pointed inbound.
    CHECK(MateAlreadyYielded(Vec3(100.0f, 20.0f, -30.0f), Vec3(14.0f, 0.0f, 0.0f),
                             slot, end, clear));

    const Vec3 self(110.0f, 2.0f, -30.0f);
    const Vec3 chase_v(14.0f, 0.0f, 0.0f);
    const Vec3 peeled(100.0f, 8.0f, -30.0f);
    const Vec3 peel_v(2.0f, 8.0f, 0.0f);
    // We are flying at it and they already yielded: keep the intercept.
    const Vec3 kept = YieldForMate(self, self, chase_v, slot, hostile, inbound,
                                   &peeled, &peel_v, clear);
    CHECK(Horiz(kept, self) < 0.1f);

    // They are still on the corridor: we step off (we do not own).
    const Vec3 mate(90.0f, 1.0f, -30.0f);
    const Vec3 mate_v(14.0f, 0.0f, 0.0f);
    const Vec3 parked(110.0f, 5.0f, -30.0f);
    const Vec3 still = YieldForMate(parked, parked, Vec3(), slot, hostile, inbound,
                                    &mate, &mate_v, clear);
    CHECK(Horiz(still, parked) > 1.0f);
}

static void TestStalkAimLeadsNotPursues() {
    std::printf("stalk aim uses the same small lead and stays leashed\n");
    const Vec3 slot(70, 0, -30);
    const float cap = 40.0f;
    const float lead = 0.5f;

    // A crossing target shifts the leashed goal slightly along +y.
    const Vec3 crossing = StalkAim(slot, Vec3(170, 0, -30), Vec3(0, 10, 0),
                                   cap, lead);
    CHECK(crossing.y > 0.1f);
    CHECK(swarm::Distance(crossing, slot) <= cap + 1e-3f);
    CHECK(crossing.x > slot.x);

    const Vec3 headon = StalkAim(slot, Vec3(170, 0, -30), Vec3(-15, 0, 0),
                                 cap, lead);
    CHECK(std::fabs(headon.y) < 0.5f);
    CHECK(headon.x > slot.x);
    CHECK(swarm::Distance(headon, slot) <= cap + 1e-3f);

    // Cap binds: a lead hundreds of metres out still sits 40 m off station.
    CHECK(std::fabs(swarm::Distance(headon, slot) - cap) < 0.5f);
}

static void TestBornOutsideRing() {
    std::printf("first sight outside the ring is an inbound, inside is behind us\n");
    const Vec3 asset(0, 0, 0);
    CHECK(BornOutsideRing(Vec3(170, 0, -40), asset, 90.0f));
    CHECK(BornOutsideRing(Vec3(90, 0, -30), asset, 90.0f));
    CHECK(!BornOutsideRing(Vec3(40, 0, -30), asset, 90.0f));
    CHECK(kScrambleEvidence > 0.05f && kScrambleEvidence < 0.2f);
}

static Config TestCfg() {
    Config cfg;
    cfg.drone_id = 0;
    cfg.fleet_size = 1;
    cfg.asset = Vec3(0, 0, 0);
    cfg.asset_radius = 30.0f;
    cfg.comm_radius = 90.0f;
    cfg.kill_radius = 1.907f;
    cfg.max_speed = 24.6f;
    cfg.max_accel = 19.7f;
    cfg.lateral_limit = 6.71f;
    cfg.arena_min = Vec3(-200, -200, -100);
    cfg.arena_max = Vec3(200, 200, 0);
    return cfg;
}

static swarm::Observation MakeObs(SwObservation& raw, const Vec3& p,
                                  const Vec3& v, float t) {
    raw = {};
    raw.struct_size = sizeof(SwObservation);
    raw.time = t;
    raw.dt = 0.01f;
    raw.self.position = p;
    raw.self.velocity = v;
    return swarm::Observation(raw);
}

static void TestRingAltitudeFollowsInboundRay() {
    std::printf("a hopped inbound cone lowers the picket\n");
    Policy p;
    p.Configure(TestCfg(), Rng());
    CHECK(p.ring_altitude() > kRayDefaultAlt - 1.0f &&
          p.ring_altitude() < kRayDefaultAlt + 1.0f);

    p.NoteRay(0.2f, 0.189f, 16.0f, /*hops=*/0);
    TrackStore store;
    SwObservation raw{};
    auto obs = MakeObs(raw, p.station(), Vec3(), 1.0f);
    p.Decide(store, obs);
    CHECK(p.inbound_ray().ready());
    CHECK(p.ring_altitude() < 20.0f);
    CHECK(p.ring_altitude() > PicketFloorAltitude(TestCfg(), p.ring_radius()) - 0.2f);
}

static void TestPicketFloorKeepsTheEnvelopeUp() {
    std::printf("picket floor keeps the kill-envelope bracelet off the dirt (D68)\n");
    Config cfg = TestCfg();
    cfg.sense_radius = 64.4f;
    cfg.max_speed = 20.4f;
    cfg.lateral_limit = 6.7f;
    const float r = 49.1f;
    const float h = PicketFloorAltitude(cfg, r);
    CHECK(h > 12.0f);
    CHECK(h <= kRayDefaultAlt + 1e-3f);
    CHECK(h > kRayFloorAlt + 1.0f);

    Policy p;
    p.Configure(cfg, Rng());
    p.NoteRay(0.0f, 0.0f, 16.0f, /*hops=*/0);
    TrackStore store;
    SwObservation raw{};
    auto obs = MakeObs(raw, p.station(), Vec3(), 1.0f);
    p.Decide(store, obs);
    CHECK(p.ring_altitude() > 12.0f);
    CHECK(p.ring_altitude() + 0.2f >= PicketFloorAltitude(cfg, p.ring_radius()));
}

static void TestFormingBecomesPicketingAtEightMetres() {
    std::printf("forming becomes picketing within 8 m of the slot\n");
    Policy p;
    p.Configure(TestCfg(), Rng());
    TrackStore store;
    const Vec3 slot = p.station();
    SwObservation raw{};
    auto obs = MakeObs(raw, slot + Vec3(20.0f, 0, 0), Vec3(), 1.0f);
    p.Decide(store, obs);
    CHECK(p.mode() == Mode::Forming);

    obs = MakeObs(raw, slot + Vec3(3.0f, 0, 0), Vec3(), 1.1f);
    p.Decide(store, obs);
    CHECK(p.mode() == Mode::Picketing);
    CHECK(std::strcmp(p.last_log(), "picket") == 0);
}

static void TestCatchableRamUsesDivert() {
    std::printf("a ram is catchable inside 2·kill, or if reach can close the leftover\n");
    Config cfg = TestCfg();

    CHECK(flight::CatchableRam(
        Vec3(20.0f, 0, -30.0f), Vec3(-15.0f, 0, 0),
        Vec3(0, 0, -30.0f), Vec3(), cfg));

    CHECK(flight::CatchableRam(
        Vec3(0, 0, -30.0f), Vec3(),
        Vec3(1.0f, 0, -30.0f), Vec3(), cfg));

    // Drone 7 at 3.4 m: still inside 2·kill (~3.8 m). Stay in the merge.
    CHECK(flight::CatchableRam(
        Vec3(-37.9f, -39.5f, -11.3f), Vec3(-1.0f, 5.0f, 5.06f),
        Vec3(-38.8f, -36.4f, -10.0f), Vec3(12.7f, 12.2f, 3.32f), cfg));

    // Same geometry, already past and 10 m out: obvious miss.
    CHECK(!flight::CatchableRam(
        Vec3(80.0f, 0, -30.0f), Vec3(),
        Vec3(0, 0, -30.0f), Vec3(-1.0f, 0, 0), cfg));

    // 10 m parallel, 80 m out: time to divert.
    CHECK(flight::CatchableRam(
        Vec3(80.0f, 10.0f, -30.0f), Vec3(-15.0f, 0, 0),
        Vec3(0, 0, -30.0f), Vec3(), cfg));

    // 40 m abeam, parked, target flying past: already at CPA, outside 2·kill.
    CHECK(!flight::CatchableRam(
        Vec3(0, 40.0f, -30.0f), Vec3(),
        Vec3(0, 0, -30.0f), Vec3(20.0f, 0, 0), cfg));
}

static void TestArenaSpringsOnStation() {
    std::printf("on station all three push; intercepting keeps only the floor\n");
    Config cfg;
    cfg.max_accel = 19.7f;
    cfg.lateral_limit = 6.71f;
    cfg.arena_min = Vec3(-200, -200, -100);
    cfg.arena_max = Vec3(200, 200, 0);

    const Vec3 wall = flight::EnforceArena(
        Vec3(10.0f, 0, 0), Vec3(198.0f, 0, -30.0f), Vec3(5.0f, 0, 0), cfg);
    CHECK(wall.x < 9.0f);

    const Vec3 roof = flight::EnforceArena(
        Vec3(0, 0, -10.0f), Vec3(0, 0, -98.0f), Vec3(0, 0, -8.0f), cfg);
    CHECK(roof.z > -10.0f);

    const Vec3 floor = flight::EnforceArena(
        Vec3(0, 0, 15.0f), Vec3(0, 0, -2.0f), Vec3(0, 0, 10.0f), cfg);
    CHECK(floor.z < 14.0f);

    // D64: while intercepting the walls and ceiling come off, but not the
    // floor. A ram must not be steered around an obstacle; the dirt is not
    // an obstacle, it is the end of the airframe, and no hostile is under it.
    const Vec3 ram_wall = flight::EnforceArena(
        Vec3(10.0f, 0, 0), Vec3(198.0f, 0, -30.0f), Vec3(5.0f, 0, 0), cfg, true);
    CHECK(std::fabs(ram_wall.x - 10.0f) < 1e-3f);

    const Vec3 ram_roof = flight::EnforceArena(
        Vec3(0, 0, -10.0f), Vec3(0, 0, -98.0f), Vec3(0, 0, -8.0f), cfg, true);
    CHECK(std::fabs(ram_roof.z + 10.0f) < 1e-3f);

    const Vec3 ram_floor = flight::EnforceArena(
        Vec3(0, 0, 15.0f), Vec3(0, 0, -2.0f), Vec3(0, 0, 10.0f), cfg, true);
    CHECK(ram_floor.z < 14.0f);

    // D68: non-ramming replaces az in the stopping band. Commanding 15 down
    // at 2 m AGL, diving 10 m/s, must climb — the old 0.5/0.8 PD still
    // netted downward.
    const Vec3 hard = flight::EnforceArena(
        Vec3(0, 0, 15.0f), Vec3(0, 0, -2.0f), Vec3(0, 0, 10.0f),
        cfg, false, true);
    CHECK(hard.z < 0.0f);
}

static void TestModeNames() {
    std::printf("mode names match the state log tokens\n");
    CHECK(std::strcmp(ModeName(Mode::Forming), "forming") == 0);
    CHECK(std::strcmp(ModeName(Mode::Picketing), "picket") == 0);
    CHECK(std::strcmp(ModeName(Mode::Watching), "watch") == 0);
    CHECK(std::strcmp(ModeName(Mode::Stalking), "stalk") == 0);
    CHECK(std::strcmp(ModeName(Mode::Scrambling), "scramble") == 0);
    CHECK(std::strcmp(ModeName(Mode::Ramming), "ram") == 0);
    CHECK(Intercepting(Mode::Scrambling) && Intercepting(Mode::Ramming));
    CHECK(!Intercepting(Mode::Watching) && !Intercepting(Mode::Stalking));
}

int main() {
    TestFacingSlotMatchesRing();
    TestFacingSlotAgreesOnABisector();
    TestOtherInterceptorTieBreak();
    TestUniqueOwnerIsOneDrone();
    TestInboundOwnerSkipsARecedingFacing();
    TestLiveRingRespaces();
    TestRingStaysInsideTheSpawnCircle();
    TestClosedCoverLeavesAFullS1Ring();
    TestClosedCoverPullsASparseRingIn();
    TestClosedCoverBindsOnTightSense();
    TestDefaultPicketAltitudeIsTwentyFive();
    TestAimAheadUsesConfiguredDistance();
    TestProNavSteersAtTheZem();
    TestArenaAllowsADiveIntercept();
    TestFacingSlotDeSpinsTheOrbit();
    TestInterceptScorePrefersTheOneThatConnects();
    TestScoreSeesTheVerticalBudget();
    TestRecedingIncumbentHandsEqualScore();
    TestStationYawFollowsVelocity();
    TestProNavDoesNotBrakeAlongTheLos();
    TestCollisionCourseCutsOffACrossingInbound();
    TestStationEvensTheLiveRing();
    TestHeardSilenceIsDeadEverywhere();
    TestApproachingFarSilenceDoesNotKill();
    TestYieldHorizonIsRemainingFlight();
    TestFirstYielderKeepsTheIntercept();
    TestStalkAimLeadsNotPursues();
    TestInterceptorKeepsGoingAtTheMerge();
    TestBornOutsideRing();
    TestRingAltitudeFollowsInboundRay();
    TestPicketFloorKeepsTheEnvelopeUp();
    TestFormingBecomesPicketingAtEightMetres();
    TestCatchableRamUsesDivert();
    TestArenaSpringsOnStation();
    TestModeNames();

    if (g_failures == 0) {
        std::printf("policy: all passed\n");
        return 0;
    }
    std::printf("policy: %d failure(s)\n", g_failures);
    return 1;
}
