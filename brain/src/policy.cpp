#include "policy.h"

#include "flight.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace sw {
namespace {

constexpr float kHeartbeatHz = 2.0f;
constexpr float kAbortAfter = 12.0f;        // s; one spawn interval is 14 s
constexpr float kReportEvery = 0.5f;        // s between reports on one track
constexpr float kReportBurst = 0.1f;        // s; first 1.5 s after the call (D35)
constexpr float kReportBurstFor = 1.5f;
constexpr float kPi = 3.14159265358979f;
constexpr float kMinClosing = 1.0f;         // m/s; below this is not a closing intercept
constexpr float kCatchSlack = 0.5f;         // s; must arrive this much before the cylinder
constexpr float kCruise = 14.0f;            // must match flight::DesiredAccel
constexpr float kFreshHostile = 6.0f;       // s; older Hostile calls are latch-ghosts
constexpr float kRecommitHold = 2.0f;       // s; do not re-chase a track we just aborted
constexpr float kScrambleDrop = 0.02f;     // abort the early chase if that LOS is gone
constexpr float kScrambleCivHold = 1.0f;   // s; level overflight never dives
constexpr float kUncatchableHold = 0.4f;   // s; one weave beat must not abort (D11)

constexpr float kChasingToward = 5.0f;     // m/s along LOS; pickets sit below this
constexpr float kCloserBy = 2.0f;          // m; farther duplicate aborts
constexpr float kFacingTie = 0.08f;        // slots; bisector band so two observers agree (D38)
constexpr float kMateIdGate = 20.0f;       // m; heartbeat pose → sensor track (same as FacingReceding)
constexpr float kOwnerSilent = 1.5f;       // s; three missed 2 Hz heartbeats
constexpr float kNeverHeard = -1.0e8f;
constexpr float kYieldAway = 3.0f;         // m/s further off the corridor: already yielded (D59)

constexpr uint8_t kPrioTrack = 3;
constexpr uint8_t kPrioTrackFirst = 6;      // first Hostile report, above heartbeat
constexpr uint8_t kPrioHeartbeat = 5;       // identity first: claims/reports starved this and neighbours stole intercepts
constexpr float kRayEvery = 0.5f;           // s; cone hops between routine tracks
constexpr float kReceding = 2.0f;           // m/s away; facing owner yields to clockwise

/// Same closed form as flight.cpp Reach (anonymous) and the kill-envelope
/// cue: divert from rest at accel `a`, coast once |v| hits `vmax`.
float CoverReach(float t, float a, float vmax) {
    if (t <= 0.0f || a < 1e-6f) return 0.0f;
    if (vmax < 1e-3f) return 0.5f * a * t * t;
    const float tv = vmax / a;
    if (t <= tv) return 0.5f * a * t * t;
    return vmax * t - 0.5f * vmax * vmax / a;
}

/// Outer intersection of the outbound ray asset + s·u (s>0) with the
/// sense sphere around the picket. Matches ReachCover.FirstSight.
bool CoverFirstSight(const Vec3& asset, const Vec3& picket, const Vec3& u,
                     float sense, Vec3& hit) {
    const Vec3 d = picket - asset;
    const float b = swarm::Dot(u, d);
    const float disc = b * b - swarm::Dot(d, d) + sense * sense;
    if (disc < 0.0f) return false;
    const float s = b + std::sqrt(disc);
    if (s < 0.5f) return false;
    hit = asset + u * s;
    return true;
}

/// Horizontal cylinder around the asset (NED xy). 0 miss, 1 already
/// inside, 2 hits at t. Matches ReachCover.CylinderCase.
int CoverCylinder(const Vec3& asset, float radius, const Vec3& pos,
                  const Vec3& vel, float& t) {
    t = 0.0f;
    const Vec3 h0(pos.x - asset.x, pos.y - asset.y, 0.0f);
    const Vec3 vh(vel.x, vel.y, 0.0f);
    const float r2 = radius * radius;
    const float h2 = swarm::LengthSq(h0);
    if (h2 <= r2) return 1;
    const float a = swarm::LengthSq(vh);
    const float b = 2.0f * swarm::Dot(h0, vh);
    const float c = h2 - r2;
    if (a < 1e-8f) return 0;
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return 0;
    const float s = std::sqrt(disc);
    const float t0 = (-b - s) / (2.0f * a);
    const float t1 = (-b + s) / (2.0f * a);
    const float pick = t0 > 0.02f ? t0 : t1;
    if (pick <= 0.02f) return 1;
    t = pick;
    return 2;
}

/// Unique-owner catch from rest. Same samples and margins as the
/// kill-envelope cue, in NED (viewer is Y-up).
bool CoverCanCatch(const Vec3& asset, float asset_radius, const Vec3& picket,
                   const Vec3& u, float sense, float max_speed, float accel,
                   float kill) {
    Vec3 hit;
    if (!CoverFirstSight(asset, picket, u, sense, hit)) return false;
    const Vec3 vel = u * (-max_speed);
    float t_hit = 0.0f;
    const int cyl = CoverCylinder(asset, asset_radius, hit, vel, t_hit);
    if (cyl == 0) return true;
    if (cyl == 1 || t_hit <= 0.04f)
        return swarm::Distance(picket, hit) <= kill + 0.5f;
    constexpr int kSamples = 12;
    for (int i = 0; i <= kSamples; ++i) {
        const float t = t_hit * (static_cast<float>(i) / kSamples);
        if (t < 0.04f) continue;
        const Vec3 meet = hit + vel * t;
        const float need = swarm::Distance(picket, meet) - kill;
        if (need <= 0.0f) return true;
        if (CoverReach(t, accel, max_speed) >= need) return true;
    }
    return false;
}

/// Unique-owner Voronoi-edge inbound at picket elevation — the bracelet
/// the kill-envelope cue scores as closed/hole. Horizon (el=0) is not
/// AND-ed in: a six-picket ring never catches a ground-level bisector,
/// and that would abort the shrink and leave the radio radius.
bool CoverCloses(const Config& cfg, float radius, float altitude,
                 uint32_t count) {
    if (count < 2) return true;
    if (radius < 1.0f) return false;
    if (cfg.sense_radius < 1.0f || cfg.max_speed < 0.1f) return true;
    const float accel = cfg.lateral_limit > 0.1f ? cfg.lateral_limit
                                                 : 6.7f;
    const float h = altitude > 0.0f ? altitude : 0.0f;
    const float elev = std::atan2(h, radius);
    const float alpha = kPi / static_cast<float>(count);
    const float ce = std::cos(elev);
    const float se = std::sin(elev);
    const Vec3 picket(cfg.asset.x + radius,
                      cfg.asset.y,
                      cfg.asset.z - h);
    const Vec3 u(ce * std::cos(alpha), ce * std::sin(alpha), -se);
    return CoverCanCatch(cfg.asset, cfg.asset_radius, picket, u,
                         cfg.sense_radius, cfg.max_speed, accel,
                         cfg.kill_radius);
}

/// Shrink-only. If the radio/spawn ring already tiles, keep the standoff
/// (D21: growing past ~0.75·comm outruns recovery). If it does not, take
/// the largest still-closed radius down to `floor_r`. If even the floor
/// is open, shrinking cannot help — leave the caps alone.
float ClampToClosedCover(const Config& cfg, float radius, float altitude,
                         uint32_t count, float floor_r) {
    if (count < 2) return radius;
    if (CoverCloses(cfg, radius, altitude, count)) return radius;
    if (floor_r >= radius - 0.5f) return radius;
    if (!CoverCloses(cfg, floor_r, altitude, count)) return radius;
    float lo = floor_r;
    float hi = radius;
    float best = floor_r;
    for (int i = 0; i < 20; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (CoverCloses(cfg, mid, altitude, count)) {
            best = mid;
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return best;
}

float ComputePicketRadius(const Config& cfg, uint32_t live_count,
                          float altitude) {
    const float span_x = cfg.arena_max.x - cfg.arena_min.x;
    const float span_y = cfg.arena_max.y - cfg.arena_min.y;
    const float arena_half = 0.5f * (span_x < span_y ? span_x : span_y);
    const float spawn_radius = 0.80f * arena_half;
    const float full_radius = cfg.asset_radius + cfg.comm_radius * 0.625f;
    const float spawn_cap = spawn_radius * 0.65f;
    const float react_cap = spawn_radius - 3.5f * cfg.max_speed;
    const float floor_r = cfg.asset_radius + 10.0f;
    const uint32_t full_count = cfg.fleet_size > 0 ? cfg.fleet_size : 1;
    const uint32_t count = live_count > 0 ? live_count : 1;

    float radius = full_radius;
    if (count < full_count && count >= 2) {
        // Preserve the full-fleet chord as the ring thins, then keep adjacent
        // stations within one sense radius where that bound is tighter.
        const float full_half_angle = kPi / static_cast<float>(full_count);
        const float live_half_angle = kPi / static_cast<float>(count);
        radius *= std::sin(full_half_angle) / std::sin(live_half_angle);
        if (cfg.sense_radius > 0.0f) {
            const float sense_cap = cfg.sense_radius /
                                   (2.0f * std::sin(live_half_angle));
            if (radius > sense_cap) radius = sense_cap;
        }
    }
    if (radius > spawn_cap) radius = spawn_cap;
    if (radius > react_cap) radius = react_cap;
    if (radius < floor_r) radius = floor_r;
    return ClampToClosedCover(cfg, radius, altitude, count, floor_r);
}

/// Scoring hook, and the only channel the viewer has for "this drone called
/// enemy." Intercept still uses Track.belief. Local Friendly and Hostile
/// only — hearsay is skipped at the call site (track_id 0). Wreckage and
/// quiet stay UNKNOWN (naming wreckage anything is −2, measured).
SwClass PublishedClass(const Track& t) {
    switch (t.belief) {
        case Belief::Friendly: return SW_CLASS_FRIENDLY;
        case Belief::Hostile:  return SW_CLASS_ENEMY;
        default:               return SW_CLASS_UNKNOWN;
    }
}

}  // namespace

float PicketRadius(const Config& cfg, uint32_t live_count, float altitude) {
    return ComputePicketRadius(cfg, live_count, altitude);
}

void Policy::Configure(const Config& cfg, Rng rng) {
    cfg_ = cfg;
    rng_ = rng;

    // Ring sizing: neighbours must be able to hear each other, so spacing is
    // driven by comm_radius, and the ring must sit outside the asset radius.
    // Survivors even out over the live roster (D56). Heartbeats hop, so
    // that roster is a fleet fact. Radius still shrinks with CountLive so
    // the neighbour chord stays inside sense (D21).
    //
    // The standoff and the gap rule are ONE decision, not two. Meeting a
    // hostile further out is paid straight into the score -- W_kill scales
    // with (1 - t_engage/t_free) -- but every metre of radius also widens
    // the hole a death leaves, because the gap between two slots is
    // 2*R*sin(pi/n). Pushed out on its own, 0.75 lost more to leaks than it
    // won in reward (sweep mean -30.5 -> -30.9). With the gap closing behind
    // each loss it stops being a leak and starts being reward: 0.625 beats
    // the old 0.5 on all eight fixed scenarios and on 20 unseen ids
    // (mean -52.5 -> -20.8). Past ~0.75 the ring outruns its own recovery --
    // a leaker at that range cannot be run down, since a hostile has our
    // lateral limit -- and tier-2 layouts collapse. See notes/decisions.md.
    //
    // Closed-cover (D58) then shrinks that radius if the unique-owner
    // Voronoi-edge inbound at picket elevation is not catchable from rest.
    // Same Reach / first-sight arithmetic as the kill-envelope bracelet.
    // It does not grow: a larger ring that "buys time" is the D21 failure
    // past F0.75. Height is an input because a lower station (default 25 m,
    // or a fitted cone) changes the divert.
    ring_altitude_ = kRayDefaultAlt;
    ring_radius_ = PicketRadius(cfg, cfg.fleet_size, ring_altitude_);

    // ...but never out near the circle hostiles enter on (D28). A picket that
    // sits just inside it meets its first hostile already born on top of it,
    // with no room left to detect, classify and turn -- measured, the two worst
    // generated layouts put the ring at 0.78 and 0.66 of the spawn radius and
    // scored -519 and -395, while every healthy one sits near 0.50.
    //
    // spawn.enemy_radius is not in SwBootInfo, but it is a fixed fraction of
    // the arena, which is: 0.80 of the half-extent on every generated scenario
    // measured (0.85 and 0.88 on the two hand-written ones, so 0.80 is the
    // conservative read and the cap binds a little early there rather than
    // late). comm_radius is a radio property and says nothing about how far out
    // the threat starts, which is why the ring needed a second bound at all.
    // And leave enough FLIGHT TIME between the spawn circle and the picket, not
    // just enough distance (D33). The ratio above catches a ring parked on top
    // of the spawn circle; it does not catch one that is nominally inside it but
    // only a couple of seconds away at closing speed, which is the same failure
    // wearing a different number. Measured: x1-814fd5e7 sits at 0.625 of the
    // spawn radius -- under the cap -- yet leaves only 54 m, about 2.8 s, which
    // is barely the time to classify at all; hostiles there spawn already inside
    // sense range and it scores 0/3. Healthy layouts leave 4.9 s.
    //
    // Enemy speed is not in SwBootInfo, but ours is and the airframes are
    // comparable (measured 19-21 m/s against our 17-24), so max_speed is the
    // proxy. This is the same reasoning as the spawn radius itself: a number the
    // brain is not given, derived from one it is.
    ray_ = InboundRay{};
    last_ray_send_ = -1.0e9f;
    sampled_local_ = false;
    picket_goal_ = flight::RingSlot(cfg.drone_id, cfg.fleet_size, cfg.asset,
                                    ring_radius_, ring_altitude_);
    station_ = picket_goal_;
    stalk_ = nullptr;
    watch_ = nullptr;
    leashed_ = false;
    announced_picket_ = false;
    mode_logged_ = false;
    logged_mode_ = Mode::Forming;
    mode_ = Mode::Forming;
    for (uint32_t i = 0; i < kMaxFleet; ++i) {
        heard_[i] = -1.0e9f;
        confirmed_dead_[i] = 0;
        announced_dead_[i] = 0;
    }
}

void Policy::NoteAlive(uint8_t drone_id, const Vec3& position, float now) {
    if (drone_id >= kMaxFleet) return;
    heard_[drone_id] = now;
    heard_at_[drone_id] = position;
    confirmed_dead_[drone_id] = 0;
}

void Policy::NoteRay(float h0, float slope, float weight, uint8_t hops) {
    float w = weight;
    if (w > 16.0f) w = 16.0f;
    if (w < 0.0f) w = 0.0f;
    w /= (1.0f + static_cast<float>(hops));
    ray_.Blend(h0, slope, w);
}

void Policy::ObserveInbounds(const TrackStore& store, float dt) {
    if (dt <= 0.0f) dt = 0.01f;
    const float ring = ring_radius_;
    for (const Track& t : store.tracks()) {
        if (t.belief == Belief::Friendly || t.belief == Belief::Wreckage) continue;
        const float dx = t.position.x - cfg_.asset.x;
        const float dy = t.position.y - cfg_.asset.y;
        const float r = std::sqrt(dx * dx + dy * dy);
        // Inside the ring is us, wreckage, or a hostile already past the
        // picket. The cone is for arrivals.
        if (r <= ring + 1.0f) continue;

        bool inbound = t.belief == Belief::Hostile;
        if (!inbound) {
            if (t.belief == Belief::Civilian) continue;
            const float miss = ClosestApproachDistance(t.position, t.velocity,
                                                       cfg_.asset);
            const float first = t.miss_at_first < 0.0f ? miss : t.miss_at_first;
            inbound = AimedAtAsset(miss, first, cfg_.asset_radius) &&
                      LooksDivingAtAsset(t.position, t.velocity, cfg_.asset,
                                         cfg_.asset_radius);
        }
        if (!inbound) continue;

        float w = dt;
        const float closing = -(dx * t.velocity.x + dy * t.velocity.y) / r;
        if (closing > 1.0f) w *= closing;
        if (t.has_local_id) w *= 2.0f;
        else w *= 1.0f / (1.0f + static_cast<float>(t.last_hops));
        if (t.belief == Belief::Hostile) w *= 2.0f;
        if (ray_.Sample(t.position, t.velocity, cfg_.asset, w) && t.has_local_id)
            sampled_local_ = true;
    }
}

void Policy::ApplyRayAltitude() {
    if (!ray_.ready()) return;
    float h = ray_.HeightAt(ring_radius_);
    if (h < kRayFloorAlt) h = kRayFloorAlt;
    if (h > kRayCapAlt) h = kRayCapAlt;
    ring_altitude_ = h;
}

void Policy::LogRing(const swarm::Host& host) {
    const uint32_t n = cfg_.fleet_size < kMaxFleet ? cfg_.fleet_size : kMaxFleet;
    for (uint32_t id = 0; id < n; ++id) {
        if (id == cfg_.drone_id) continue;
        const bool dead = confirmed_dead_[id] != 0;
        const bool told = announced_dead_[id] != 0;
        if (dead && !told) {
            host.Logf("gone id=%u", id);
            announced_dead_[id] = 1;
        } else if (!dead && told) {
            host.Logf("live id=%u", id);
            announced_dead_[id] = 0;
        }
    }
}

void Policy::Decide(TrackStore& store, const swarm::Observation& obs) {
    const float now = obs.time();
    last_log_[0] = '\0';

    ray_.Decay(obs.dt());
    sampled_local_ = false;
    ObserveInbounds(store, obs.dt());

    // Re-resolve the target every tick: the store's storage moves as tracks
    // are erased. Hearsay has no local id, so we key on store_id and
    // re-associate by geometry if a local track absorbed the row (D18).
    target_ = nullptr;
    if (Intercepting(mode_)) {
        target_ = ResolveTarget(store);
        if (target_ && mode_ == Mode::Scrambling && target_->belief == Belief::Hostile)
            mode_ = Mode::Ramming;
        const char* why = nullptr;
        if (!target_)
            why = "lost";
        else if (CloserChaser(*target_, store, obs))
            why = "duplicate";
        else
            why = AbortReason(*target_, obs);
        if (why) {
            LogAbort(why, target_id_, target_, obs);
            last_abort_id_ = target_id_;
            last_abort_store_id_ = target_store_id_;
            last_abort_at_ = now;
            BindTarget(nullptr);
            mode_ = Mode::Picketing;
        } else {
            BindTarget(target_);
        }
    }

    if (!Intercepting(mode_)) {
        Track* candidate = nullptr;
        float best_key = 1.0e6f;
        bool early = false;
        for (Track& t : store.tracks()) {
            if (CloserChaser(t, store, obs)) continue;
            if (ShouldCommit(t, store, obs)) {
                const float ttg = TimeToCylinder(t.position, t.velocity, cfg_.asset,
                                                 cfg_.asset_radius);
                const float d = swarm::Distance(t.position, obs.position());
                const float key = ttg + d * 0.001f;
                if (key < best_key) { best_key = key; candidate = &t; early = false; }
            } else if (ShouldScramble(t, store, obs)) {
                const float d = swarm::Distance(t.position, obs.position());
                const float key = 1.0e5f + d;
                if (key < best_key) { best_key = key; candidate = &t; early = true; }
            }
        }
        if (candidate) {
            BindTarget(candidate);
            committed_at_ = now;
            mode_ = early ? Mode::Scrambling : Mode::Ramming;
            const float rng = swarm::Distance(candidate->position, obs.position());
            const float closing = ClosingSpeed(obs.position(), obs.velocity(),
                                               candidate->position, candidate->velocity);
            const float miss = ClosestApproachDistance(candidate->position,
                                                       candidate->velocity, cfg_.asset);
            const float ttg = TimeToCylinder(candidate->position, candidate->velocity,
                                             cfg_.asset, cfg_.asset_radius);
            const Vec3 weave = flight::EstimatedAccel(
                candidate->velocity, candidate->last_velocity, obs.dt(),
                cfg_.lateral_limit);
            const flight::Course course = flight::SolveCollisionCourse(
                obs.position(), obs.velocity(), candidate->position,
                candidate->velocity, cfg_, weave,
                flight::InertialAccel(obs.attitude(), obs.accel()));
            std::snprintf(last_log_, sizeof(last_log_),
                          "commit trk=%u score=%.2f miss=%.1f rng=%.0f close=%.1f ttg=%.1f n=%.1f e=%.1f alt=%.1f vn=%.1f ve=%.1f in=%.1f ie=%.1f ialt=%.1f%s%s",
                          candidate->has_local_id ? candidate->track_id : 0,
                          candidate->closing_score, miss, rng, closing, ttg,
                          candidate->position.x, candidate->position.y,
                          -candidate->position.z,
                          candidate->velocity.x, candidate->velocity.y,
                          course.meeting.x, course.meeting.y, -course.meeting.z,
                          candidate->has_local_id ? "" : " peer",
                          early ? " early" : "");
        }
    }

    ApplyRayAltitude();

    if (!announced_picket_ && !Intercepting(mode_)) {
        // The same station function PicketGoal flies to. They agree at boot,
        // when nobody has died; they would not after a loss, and a drone that
        // re-forms would call itself on picket at a slot it is not holding.
        const Vec3 slot = StationAt(
            StationBearing(cfg_.drone_id, cfg_.fleet_size, cfg_.drone_id,
                           heard_, heard_at_, obs.position(), cfg_.comm_radius,
                           now, confirmed_dead_),
            cfg_.asset, ring_radius_, ring_altitude_);
        if (swarm::Distance(obs.position(), slot) < 8.0f) {
            announced_picket_ = true;
            if (mode_ == Mode::Forming) mode_ = Mode::Picketing;
            // Do not overwrite commit/abort on the same tick (D3).
            if (last_log_[0] == '\0')
                std::snprintf(last_log_, sizeof(last_log_), "picket");
        }
    }

    if (!Intercepting(mode_)) {
        picket_goal_ = PicketGoal(store, obs);
        AssignStationMode(obs);
    } else {
        ApplyRayAltitude();
        stalk_ = nullptr;
        watch_ = nullptr;
        leashed_ = false;
    }
}

uint32_t FacingSlot(const Vec3& position, const Vec3& asset, uint32_t count) {
    const uint32_t n = count > 0 ? count : 1;
    const float hx = position.x - asset.x;
    const float hy = position.y - asset.y;
    if (hx * hx + hy * hy < 1e-8f) return 0;
    float u = std::atan2(hy, hx);
    if (u < 0.0f) u += 2.0f * kPi;
    const float x = u * static_cast<float>(n) / (2.0f * kPi);
    // Nearest slot, except a band around the Voronoi edge. lround alone lets
    // two drones on a bisector inbound each round to themselves (D38).
    // Clockwise of the pair: same walk as UniqueOwner, and a hole's station
    // (exact .5 on the live ring) stays the sliding neighbour, not the far one.
    const float lo = std::floor(x);
    if (std::fabs(x - (lo + 0.5f)) < kFacingTie)
        return (static_cast<uint32_t>(lo) + 1u) % n;
    return static_cast<uint32_t>(std::lround(x)) % n;
}

bool SlotAlive(uint32_t drone_id, uint32_t self_id, const float* heard, float now) {
    if (drone_id == self_id) return true;
    if (drone_id >= kMaxFleet || heard == nullptr) return false;
    // Never heard: assume alive so we do not steal a sector at boot before
    // the first heartbeat. After that, 1.5 s of silence is three missed beats.
    if (heard[drone_id] < kNeverHeard) return true;
    return now - heard[drone_id] < kOwnerSilent;
}

bool RingAlive(uint32_t drone_id, uint32_t self_id, const float* heard,
               const Vec3* heard_at, const Vec3& self_pos, float comm_radius,
               float now, uint32_t fleet_size, uint8_t* confirmed_dead) {
    (void)heard_at;
    (void)self_pos;
    (void)comm_radius;
    (void)fleet_size;
    if (SlotAlive(drone_id, self_id, heard, now)) {
        if (confirmed_dead != nullptr && drone_id < kMaxFleet)
            confirmed_dead[drone_id] = 0;
        return true;
    }
    // Heard them, then silence. Hopped heartbeats (D56) reach the far side
    // of the ring, so "out of radio" is no longer an excuse to keep a ghost
    // station. Latch so a gap in the flood cannot resurrect them.
    if (drone_id >= kMaxFleet) return false;
    if (confirmed_dead != nullptr) confirmed_dead[drone_id] = 1;
    return false;
}

uint32_t CountLive(uint32_t fleet_size, uint32_t self_id, const float* heard,
                   const Vec3* heard_at, const Vec3& self_pos, float comm_radius,
                   float now, uint8_t* confirmed_dead) {
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    const uint32_t cap = n < kMaxFleet ? n : kMaxFleet;
    uint32_t live = 0;
    for (uint32_t id = 0; id < cap; ++id) {
        if (RingAlive(id, self_id, heard, heard_at, self_pos, comm_radius, now,
                      fleet_size, confirmed_dead))
            ++live;
    }
    return live > 0 ? live : 1;
}

uint32_t LiveRank(uint32_t drone_id, uint32_t fleet_size, uint32_t self_id,
                  const float* heard, const Vec3* heard_at, const Vec3& self_pos,
                  float comm_radius, float now, uint8_t* confirmed_dead) {
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    const uint32_t cap = n < kMaxFleet ? n : kMaxFleet;
    uint32_t rank = 0;
    for (uint32_t id = 0; id < cap; ++id) {
        if (!RingAlive(id, self_id, heard, heard_at, self_pos, comm_radius, now,
                       fleet_size, confirmed_dead))
            continue;
        if (id == drone_id) return rank;
        ++rank;
    }
    return 0;
}

uint32_t LiveId(uint32_t rank, uint32_t fleet_size, uint32_t self_id,
                const float* heard, const Vec3* heard_at, const Vec3& self_pos,
                float comm_radius, float now, uint8_t* confirmed_dead) {
    const uint32_t live = CountLive(fleet_size, self_id, heard, heard_at,
                                    self_pos, comm_radius, now, confirmed_dead);
    const uint32_t want = rank % live;
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    const uint32_t cap = n < kMaxFleet ? n : kMaxFleet;
    uint32_t i = 0;
    for (uint32_t id = 0; id < cap; ++id) {
        if (!RingAlive(id, self_id, heard, heard_at, self_pos, comm_radius, now,
                       fleet_size, confirmed_dead))
            continue;
        if (i == want) return id;
        ++i;
    }
    return self_id;
}

/// Where drone `id` should stand. Equal bearings among RingAlive ids
/// (D56). Heartbeats hop, so the live set is a fleet fact, not a radio
/// neighbourhood. Full strength is a fixed point (rank == id). A death
/// re-ranks everyone; the 2-slot cap on gap bisection is gone — that is
/// why s2 never evened out.
float StationBearing(uint32_t id, uint32_t fleet_size, uint32_t self_id,
                     const float* heard, const Vec3* heard_at,
                     const Vec3& self_pos, float comm_radius, float now,
                     uint8_t* confirmed_dead) {
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    const uint32_t live = CountLive(n, self_id, heard, heard_at, self_pos,
                                    comm_radius, now, confirmed_dead);
    const uint32_t rank = LiveRank(id, n, self_id, heard, heard_at, self_pos,
                                   comm_radius, now, confirmed_dead);
    return 2.0f * kPi * static_cast<float>(rank) / static_cast<float>(live);
}

Vec3 StationAt(float bearing, const Vec3& centre, float radius, float altitude) {
    return Vec3(centre.x + radius * std::cos(bearing),
                centre.y + radius * std::sin(bearing),
                -altitude);
}

uint32_t UniqueOwner(uint32_t facing, uint32_t fleet_size, uint32_t self_id,
                     const float* heard, float now) {
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = (facing + i) % n;
        if (SlotAlive(id, self_id, heard, now)) return id;
    }
    return facing % n;
}

uint32_t InboundOwner(uint32_t facing, uint32_t fleet_size, uint32_t self_id,
                      const float* heard, float now, bool facing_receding) {
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    uint32_t start = facing % n;
    if (facing_receding) start = (start + 1) % n;
    return UniqueOwner(start, fleet_size, self_id, heard, now);
}

float TowardTarget(const Vec3& position, const Vec3& velocity, const Vec3& target) {
    const Vec3 los(target.x - position.x, target.y - position.y, 0.0f);
    const float range = swarm::Length(los);
    if (range < 1.0f) return 0.0f;
    return swarm::Dot(Vec3(velocity.x, velocity.y, 0.0f), los / range);
}

bool Policy::FacingReceding(uint32_t facing, const Track& hostile,
                            const TrackStore& store,
                            const swarm::Observation& obs) const {
    Vec3 p, v;
    if (facing == cfg_.drone_id) {
        p = obs.position();
        v = obs.velocity();
    } else {
        // Need a live heartbeat pose so we do not match a Friendly near the
        // origin (never-heard heard_at is zero). Unseen facing still owns.
        if (facing >= kMaxFleet) return false;
        if (heard_[facing] < kNeverHeard) return false;
        const Track* mate = nullptr;
        float best = 20.0f;
        for (const Track& t : store.tracks()) {
            if (t.belief != Belief::Friendly) continue;
            const float d = swarm::Distance(t.position, heard_at_[facing]);
            if (d < best) { best = d; mate = &t; }
        }
        if (!mate) return false;
        p = mate->position;
        v = mate->velocity;
    }
    return TowardTarget(p, v, hostile.position) < -kReceding;
}

bool Policy::OwnsInbound(const Track& t, const TrackStore& store,
                         const swarm::Observation& obs) const {
    // Stations and ownership share the live ring (D56). Facing slot among
    // CountLive equally spaced stations, then that LiveId. A receding owner
    // yields one step clockwise on the live ring (D35).
    const float now = obs.time();
    const uint32_t live = CountLive(cfg_.fleet_size, cfg_.drone_id, heard_,
                                    heard_at_, obs.position(), cfg_.comm_radius,
                                    now, nullptr);
    uint32_t face = FacingSlot(t.position, cfg_.asset, live);
    uint32_t owner = LiveId(face, cfg_.fleet_size, cfg_.drone_id, heard_,
                            heard_at_, obs.position(), cfg_.comm_radius, now,
                            nullptr);
    if (FacingReceding(owner, t, store, obs))
        owner = LiveId(face + 1, cfg_.fleet_size, cfg_.drone_id, heard_,
                       heard_at_, obs.position(), cfg_.comm_radius, now,
                       nullptr);
    return owner == cfg_.drone_id;
}

namespace {

/// A Friendly already flying at this hostile is the interceptor. Picket
/// station-keeping is a few m/s; cruise is 14.
bool FlyingAt(const Track& craft, const Track& hostile) {
    if (craft.belief != Belief::Friendly) return false;
    if (craft.has_local_id && hostile.has_local_id &&
        craft.track_id == hostile.track_id)
        return false;
    const Vec3 los(hostile.position.x - craft.position.x,
                   hostile.position.y - craft.position.y, 0.0f);
    const float range_h = swarm::Length(los);
    if (range_h < 1.0f) return false;
    const float toward = swarm::Dot(
        Vec3(craft.velocity.x, craft.velocity.y, 0.0f), los / range_h);
    if (toward < kChasingToward) return false;
    const float closing = ClosingSpeed(craft.position, craft.velocity,
                                       hostile.position, hostile.velocity);
    return closing >= kMinClosing;
}

}  // namespace

int Policy::MateId(const Track& mate) const {
    const uint32_t n = cfg_.fleet_size < kMaxFleet ? cfg_.fleet_size : kMaxFleet;
    float best = kMateIdGate;
    int id = -1;
    for (uint32_t i = 0; i < n; ++i) {
        if (i == cfg_.drone_id) continue;
        if (heard_[i] < kNeverHeard) continue;
        const float d = swarm::Distance(mate.position, heard_at_[i]);
        if (d < best) { best = d; id = static_cast<int>(i); }
    }
    return id;
}

bool OtherInterceptorWins(float us_range, uint32_t us_id,
                          float them_range, int them_id) {
    if (them_range < us_range - kCloserBy) return true;
    if (them_id < 0) return false;
    if (them_range > us_range + kCloserBy) return false;
    return static_cast<uint32_t>(them_id) < us_id;
}

bool Policy::CloserChaser(const Track& hostile, const TrackStore& store,
                          const swarm::Observation& obs) const {
    const float us_range = swarm::Distance(obs.position(), hostile.position);
    for (const Track& t : store.tracks()) {
        if (!FlyingAt(t, hostile)) continue;
        const float d = swarm::Distance(t.position, hostile.position);
        if (OtherInterceptorWins(us_range, cfg_.drone_id, d, MateId(t)))
            return true;
    }
    return false;
}

Vec3 CorridorHorizon(const Vec3& from, const Vec3& hostile_p, const Vec3& hostile_v) {
    // Remaining flight is speed × time-to-meet, not the chord to where the
    // hostile is now. A ram 3 s out is ~50 m of keep-out; the other 80 m
    // of slot→hostile is empty air the interceptor will never fly (D17).
    const float dx = hostile_p.x - from.x;
    const float dy = hostile_p.y - from.y;
    const float range = std::sqrt(dx * dx + dy * dy);
    if (range < 1.0f) return hostile_p;
    const Vec3 dir(dx / range, dy / range, 0.0f);
    const Vec3 iv(dir.x * kCruise, dir.y * kCruise, 0.0f);
    const float closing = ClosingSpeed(from, iv, hostile_p, hostile_v);
    if (closing < kMinClosing) return from;
    float t_meet = range / closing + kCatchSlack;
    if (t_meet > kAbortAfter) t_meet = kAbortAfter;
    float along = kCruise * t_meet;
    if (along > range) along = range;
    return Vec3(from.x + dir.x * along, from.y + dir.y * along, from.z);
}

Vec3 StalkAim(const Vec3& slot, const Vec3& target_p, const Vec3& target_v,
              float cap, float lead) {
    const Vec3 aim = flight::AimAhead(target_p, target_v, lead);
    const Vec3 to_aim = aim - slot;
    const float len = swarm::Length(to_aim);
    if (len < 1.0f) return slot;
    const float along = (len < cap) ? len : cap;
    return slot + to_aim * (along / len);
}

float CorridorOffset(const Vec3& p, const Vec3& a, const Vec3& b, Vec3& closest) {
    const Vec3 ab(b.x - a.x, b.y - a.y, 0.0f);
    const float ab2 = ab.x * ab.x + ab.y * ab.y;
    closest = Vec3(p.x, p.y, p.z);
    if (ab2 < 1e-4f) return 1.0e9f;
    const Vec3 ap(p.x - a.x, p.y - a.y, 0.0f);
    float t = (ap.x * ab.x + ap.y * ab.y) / ab2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    closest = Vec3(a.x + t * ab.x, a.y + t * ab.y, p.z);
    const float dx = p.x - closest.x;
    const float dy = p.y - closest.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool MateAlreadyYielded(const Vec3& pos, const Vec3& vel,
                        const Vec3& a, const Vec3& b, float clear) {
    Vec3 closest;
    const float dist = CorridorOffset(pos, a, b, closest);
    if (dist >= 1.0e8f) return false;
    if (clear > 0.0f && dist >= clear) return true;
    if (dist < 1.5f) return false;
    const Vec3 off(pos.x - closest.x, pos.y - closest.y, 0.0f);
    const float away = swarm::Dot(Vec3(vel.x, vel.y, 0.0f), off / dist);
    return away > kYieldAway;
}

Vec3 YieldOffCorridor(const Vec3& goal, const Vec3& a, const Vec3& b, float clear) {
    Vec3 closest;
    const float dist = CorridorOffset(goal, a, b, closest);
    if (dist >= 1.0e8f || clear <= 0.0f) return goal;
    if (dist >= clear) return goal;
    const Vec3 ab(b.x - a.x, b.y - a.y, 0.0f);
    Vec3 dir(goal.x - closest.x, goal.y - closest.y, 0.0f);
    if (dist < 1e-3f) dir = Vec3(-ab.y, ab.x, 0.0f);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len < 1e-6f) return goal;
    const float need = (dist < 1e-3f) ? clear : (clear - dist);
    return Vec3(goal.x + dir.x / len * need,
                goal.y + dir.y / len * need,
                goal.z);
}

Vec3 YieldForMate(const Vec3& goal, const Vec3& self_p, const Vec3& self_v,
                  const Vec3& owner_slot, const Vec3& hostile_p,
                  const Vec3& hostile_v, const Vec3* mate_p, const Vec3* mate_v,
                  float clear) {
    const Vec3 slot_end = CorridorHorizon(owner_slot, hostile_p, hostile_v);
    const bool they_chase = mate_p && mate_v &&
        !MateAlreadyYielded(*mate_p, *mate_v, owner_slot, slot_end, clear);
    const bool we_chase =
        TowardTarget(self_p, self_v, hostile_p) >= kChasingToward &&
        ClosingSpeed(self_p, self_v, hostile_p, hostile_v) >= kMinClosing;

    if (!they_chase) {
        // First mover already stepped off, or nobody is coming. If we are
        // already flying at this inbound, keep it (D59). Else pre-clear
        // the owner's remaining flight (D15 / D17).
        if (we_chase) return goal;
        return YieldOffCorridor(goal, owner_slot, slot_end, clear);
    }
    const Vec3 end = CorridorHorizon(*mate_p, hostile_p, hostile_v);
    return YieldOffCorridor(goal, *mate_p, end, clear);
}

Vec3 Policy::PicketGoal(const TrackStore& store, const swarm::Observation& obs) {
    // Hold the live ring, but step off anyone else's remaining intercept.
    // After a nearby death the survivors re-space (D19). Past the predicted
    // ram they stay put (D17).
    const float now = obs.time();
    const uint32_t live = CountLive(
        cfg_.fleet_size, cfg_.drone_id, heard_, heard_at_,
        obs.position(), cfg_.comm_radius, now, confirmed_dead_);
    ApplyRayAltitude();
    ring_radius_ = PicketRadius(cfg_, live, ring_altitude_);
    ApplyRayAltitude();
    const Vec3 slot = StationAt(StationBearing(cfg_.drone_id, cfg_.fleet_size,
                                        cfg_.drone_id, heard_, heard_at_,
                                        obs.position(), cfg_.comm_radius, now,
                                        confirmed_dead_),
                          cfg_.asset, ring_radius_, ring_altitude_);
    station_ = slot;
    stalk_ = nullptr;
    watch_ = nullptr;
    Vec3 goal = slot;

    // Ease toward a likely inbound before the Hostile latch, still close
    // enough to get back on station if it never confirms.
    for (const Track& t : store.tracks()) {
        if (t.belief == Belief::Friendly || t.belief == Belief::Wreckage) continue;
        if (!LooksDivingAtAsset(t.position, t.velocity, cfg_.asset, cfg_.asset_radius))
            continue;
        if (!OwnsInbound(t, store, obs)) continue;
        const float horiz = swarm::Length(Vec3(t.velocity.x, t.velocity.y, 0.0f));
        if (horiz >= kHostileDash * 0.6f) continue;
        if (ThreatWindow(t.position, t.velocity, cfg_.asset,
                         cfg_.asset_radius, kHostileDash) >= kCompactWindow)
            continue;
        const Vec3 aim = StalkAim(
            slot, t.position, t.velocity, kStalkRange,
            flight::kPnLeadKillRadii * cfg_.kill_radius);
        if (swarm::Distance(aim, slot) < 1.0f) continue;
        goal = aim;
        stalk_ = &t;
        break;
    }

    // Face an inbound we own as soon as it is in sense from outside the ring.
    // Yaw only — movement waits on kScrambleEvidence of cylinder LOS (D44).
    {
        const Track* best = nullptr;
        float best_key = 1.0e9f;
        const bool own_hostile = OwnsAHostile(store, obs);
        for (const Track& t : store.tracks()) {
            if (!t.has_local_id) continue;
            if (t.belief == Belief::Friendly || t.belief == Belief::Wreckage) continue;
            if (!OwnsInbound(t, store, obs)) continue;
            const float d = swarm::Distance(obs.position(), t.position);
            if (t.belief == Belief::Hostile) {
                if (d < best_key) { best_key = d; best = &t; }
                continue;
            }
            if (own_hostile) continue;
            if (!EnteredFromOutside(t)) continue;
            const float key = -t.cylinder_score * 1000.0f + d;
            if (key < best_key) { best_key = key; best = &t; }
        }
        watch_ = best;
    }

    for (const Track& t : store.tracks()) {
        if (t.belief != Belief::Hostile) continue;
        if (OwnsInbound(t, store, obs)) continue;
        const uint32_t face = FacingSlot(t.position, cfg_.asset, live);
        uint32_t owner = LiveId(face, cfg_.fleet_size, cfg_.drone_id, heard_,
                                heard_at_, obs.position(), cfg_.comm_radius,
                                now, confirmed_dead_);
        if (FacingReceding(owner, t, store, obs))
            owner = LiveId(face + 1, cfg_.fleet_size, cfg_.drone_id, heard_,
                           heard_at_, obs.position(), cfg_.comm_radius, now,
                           confirmed_dead_);
        const Vec3 owner_slot = StationAt(
            StationBearing(owner, cfg_.fleet_size, cfg_.drone_id, heard_,
                           heard_at_, obs.position(), cfg_.comm_radius, now,
                           confirmed_dead_),
            cfg_.asset, ring_radius_, ring_altitude_);
        const Track* chase = nullptr;
        float best_d = 1.0e9f;
        const Vec3 slot_end = CorridorHorizon(owner_slot, t.position, t.velocity);
        for (const Track& m : store.tracks()) {
            if (!FlyingAt(m, t)) continue;
            if (MateAlreadyYielded(m.position, m.velocity, owner_slot, slot_end,
                                   cfg_.friendly_margin))
                continue;
            const float d = swarm::Distance(m.position, t.position);
            if (d < best_d) { best_d = d; chase = &m; }
        }
        const Vec3* mate_p = chase ? &chase->position : nullptr;
        const Vec3* mate_v = chase ? &chase->velocity : nullptr;
        goal = YieldForMate(goal, obs.position(), obs.velocity(), owner_slot,
                            t.position, t.velocity, mate_p, mate_v,
                            cfg_.friendly_margin);
    }
    return goal;
}

bool Policy::ShouldCommit(const Track& t, const TrackStore& store,
                          const swarm::Observation& obs) const {
    // Spend the airframe on a fresh Hostile we can catch. Hearsay is allowed
    // on s2: the drone that sees a 220 m inbound is not the one that can
    // intercept it, and a report that stops at one hop never gets there (D18).
    // track_id 0 used to park a picket on the ring; BindTarget keys on
    // store_id so Fly still has a pose.
    if (t.belief != Belief::Hostile) return false;
    // A Hostile latch that is older than a real intercept is wreckage or a
    // mate we failed to ID. Neighbours of a spent owner chased those for 12 s
    // and missed s1 hostile_4, which they had already called.
    if (obs.time() - t.belief_since > kFreshHostile) return false;
    if (obs.time() - last_abort_at_ < kRecommitHold) {
        if (t.store_id != 0 && t.store_id == last_abort_store_id_) return false;
        if (t.has_local_id && last_abort_id_ != 0 && t.track_id == last_abort_id_)
            return false;
    }
    if (!OwnsInbound(t, store, obs)) return false;

    const float range = swarm::Distance(t.position, obs.position());
    const float closing = ClosingSpeed(obs.position(), obs.velocity(),
                                       t.position, t.velocity);
    // flight.h: a stern chase against the same 6.7 m/s² bound does not
    // converge. The old `closing > -2 || ttg < 12` admitted those.
    if (closing < kMinClosing) return false;

    // Stationary t_meet (range/closing) is the picket waiting for them.
    // After a death the live ring re-ranks (D56); still count
    // cruise along the line of sight if we are not yet on the new station.
    // extra = cruise - toward used to CANCEL an outbound velocity and treat
    // a reverse as free. A drone sliding away from the inbound (D19 respace)
    // then looked as catchable as one sitting still (D35). Pay the reverse.
    const float our_toward = TowardTarget(obs.position(), obs.velocity(),
                                          t.position);
    float extra = kCruise - our_toward;
    if (extra < 0.0f) extra = 0.0f;
    const float closing_go = closing + extra;
    const float ttg = TimeToCylinder(t.position, t.velocity, cfg_.asset,
                                     cfg_.asset_radius);
    float t_meet = range / closing_go;
    if (our_toward < 0.0f)
        t_meet += (-our_toward) / cfg_.lateral_limit;
    return t_meet + kCatchSlack < ttg;
}

bool BornOutsideRing(const Vec3& first, const Vec3& asset, float ring_radius) {
    const float dx = first.x - asset.x;
    const float dy = first.y - asset.y;
    const float g = std::sqrt(dx * dx + dy * dy);
    return g >= ring_radius - 1.0f;
}

bool Policy::EnteredFromOutside(const Track& t) const {
    return BornOutsideRing(t.first_position, cfg_.asset, ring_radius_);
}

bool Policy::OwnsAHostile(const TrackStore& store,
                          const swarm::Observation& obs) const {
    for (const Track& t : store.tracks()) {
        if (t.belief != Belief::Hostile) continue;
        if (obs.time() - t.belief_since > kFreshHostile) continue;
        if (OwnsInbound(t, store, obs)) return true;
    }
    return false;
}

bool Policy::ShouldScramble(const Track& t, const TrackStore& store,
                           const swarm::Observation& obs) const {
    // Leave station ~0.1 s after the ground track crosses the cylinder,
    // before the Hostile latch. Local sense only, and only inbounds that
    // first appeared outside the ring so a civilian from behind the picket
    // does not pull us off (D44). Unique owner still holds.
    if (t.belief == Belief::Friendly || t.belief == Belief::Wreckage) return false;
    if (t.belief == Belief::Hostile) return false;
    if (!t.has_local_id) return false;
    if (t.cylinder_score < kScrambleEvidence) return false;
    if (!EnteredFromOutside(t)) return false;
    if (OwnsAHostile(store, obs)) return false;
    if (obs.time() - last_abort_at_ < kRecommitHold) {
        if (t.store_id != 0 && t.store_id == last_abort_store_id_) return false;
        if (t.has_local_id && last_abort_id_ != 0 && t.track_id == last_abort_id_)
            return false;
    }
    if (!OwnsInbound(t, store, obs)) return false;
    if (CloserChaser(t, store, obs)) return false;
    return true;
}

const char* Policy::AbortReason(const Track& t, const swarm::Observation& obs) const {
    const float now = obs.time();
    if (now - committed_at_ > kAbortAfter) return "timeout";
    if (mode_ == Mode::Scrambling) {
        if (t.belief == Belief::Friendly || t.belief == Belief::Wreckage)
            return "not-hostile";
        if (t.cylinder_score < kScrambleDrop && now - committed_at_ > 0.3f)
            return "not-threat";
        const float miss = ClosestApproachDistance(t.position, t.velocity, cfg_.asset);
        if (now - committed_at_ >= kScrambleCivHold &&
            !LooksDivingAtAsset(t.position, t.velocity, cfg_.asset, cfg_.asset_radius) &&
            miss > 8.0f)
            return "not-threat";
        return nullptr;
    }
    if (t.belief != Belief::Hostile) return "not-hostile";

    const float closing = ClosingSpeed(obs.position(), obs.velocity(),
                                       t.position, t.velocity);
    // Evasion (3.5 m/s² weave inside 40 m) can flip the sign for a beat.
    // Immediate receding abort dropped s1 hostile_4 with the interceptor
    // 5 m out. Six seconds is a fair chance to close; after that, give up.
    if (now - committed_at_ > 6.0f && closing < kMinClosing) return "not-closing";
    if (now - committed_at_ > kUncatchableHold &&
        !flight::CatchableRam(obs.position(), obs.velocity(),
                              t.position, t.velocity, cfg_))
        return "uncatchable";
    return nullptr;
}

void Policy::LogAbort(const char* why, uint32_t trk, const Track* t,
                      const swarm::Observation& obs) {
    const float held = obs.time() - committed_at_;
    if (!t) {
        std::snprintf(last_log_, sizeof(last_log_),
                      "abort trk=%u %s held=%.1f", trk, why, held);
        return;
    }
    const float rng = swarm::Distance(t->position, obs.position());
    const float closing = ClosingSpeed(obs.position(), obs.velocity(),
                                       t->position, t->velocity);
    const float ttg = TimeToCylinder(t->position, t->velocity, cfg_.asset,
                                     cfg_.asset_radius);
    std::snprintf(last_log_, sizeof(last_log_),
                  "abort trk=%u %s close=%.1f rng=%.0f ttg=%.1f held=%.1f now=%s",
                  trk, why, closing, rng, ttg, held, BeliefName(t->belief));
}

Vec3 Policy::DesiredPosition(const swarm::Observation& obs) const {
    if (Intercepting(mode_) && target_) return target_->position;
    (void)obs;
    return picket_goal_;
}

const Track* Policy::focus() const {
    if (Intercepting(mode_)) return target_;
    if (mode_ == Mode::Stalking) return stalk_;
    return watch_;
}

void Policy::AssignStationMode(const swarm::Observation& obs) {
    leashed_ = stalk_ != nullptr &&
               swarm::Distance(obs.position(), station_) < kStalkRange;
    if (stalk_) {
        mode_ = Mode::Stalking;
        return;
    }
    if (watch_ && announced_picket_) {
        mode_ = Mode::Watching;
        return;
    }
    if (announced_picket_)
        mode_ = Mode::Picketing;
    else
        mode_ = Mode::Forming;
}

void Policy::LogMode(const swarm::Host& host) {
    if (mode_logged_ && mode_ == logged_mode_)
        return;
    const char* from = mode_logged_ ? ModeName(logged_mode_) : "";
    const Track* f = focus();
    const uint32_t trk = (f && f->has_local_id) ? f->track_id : 0;
    char extra[96]{};
    if (from[0] != '\0')
        std::snprintf(extra, sizeof(extra), " from=%s", from);
    if (mode_ == Mode::Stalking) {
        const size_t n = std::strlen(extra);
        std::snprintf(extra + n, sizeof(extra) - n, " leashed=%d", leashed_ ? 1 : 0);
    }
    if (f)
        host.Logf("state %s trk=%u%s", ModeName(mode_), trk, extra);
    else
        host.Logf("state %s%s", ModeName(mode_), extra);
    logged_mode_ = mode_;
    mode_logged_ = true;
}

Track* Policy::ResolveTarget(TrackStore& store) {
    if (target_store_id_ != 0) {
        if (Track* t = store.FindByStoreId(target_store_id_)) return t;
    }
    if (target_id_ != 0) {
        if (Track* t = store.Find(target_id_)) return t;
    }
    return store.NearestHostile(last_target_pos_, 40.0f);
}

void Policy::BindTarget(Track* t) {
    target_ = t;
    if (!t) {
        target_id_ = 0;
        target_store_id_ = 0;
        return;
    }
    target_id_ = t->has_local_id ? t->track_id : 0;
    target_store_id_ = t->store_id;
    last_target_pos_ = t->position;
}

void Policy::Declare(const swarm::Host& host, const TrackStore& store) const {
    // G2 / D9: local tracks only. track_id is observer-local; a hearsay row
    // sits at id 0 and would be scored against whoever our sensors labelled 0.
    // Friends and hostiles we actually see get published so the viewer (and
    // the awareness term) can see the call. Unknown/wreckage stay UNKNOWN.
    for (const Track& t : store.tracks()) {
        if (!t.has_local_id) continue;
        host.DeclareTrack(t.track_id, PublishedClass(t));
    }
    // TODO(tier 5): declare_identity is a SEPARATE hook and the two are not
    // interchangeable -- an insider only counts if named by key. Naming an
    // innocent costs 120 once per identity and that term is NOT clamped.
}

void Policy::Compose(Outbox<24>& outbox, TrackStore& store,
                     const swarm::Observation& obs) {
    const float now = obs.time();
    uint8_t buffer[SW_MTU];

    // Heartbeat: cheap, periodic, and the thing that lets peers identify each
    // other as friendly. The comms term is zero for a fleet that never
    // transmits, so silence is not efficiency.
    if (now - last_heartbeat_ >= 1.0f / kHeartbeatHz) {
        Writer w(buffer, sizeof(buffer));
        Header h;
        h.type = MsgType::Heartbeat;
        h.origin = static_cast<uint8_t>(cfg_.drone_id);
        h.seq = next_seq_++;
        h.sent_time = now;
        h.Write(w);

        HeartbeatMsg m;
        m.position = obs.position();
        m.velocity = obs.velocity();
        m.Write(w);

        if (w.ok() && outbox.Push(buffer, w.size(), kPrioHeartbeat, now)) {
            last_heartbeat_ = now;
        }
    }

    // Track reports on anything we have called hostile. On s2 the arrival is
    // 220 m out where only one drone can see it, so this is the message that
    // decides the tier.
    //
    // TODO(next): rate-limit per track and batch several reports into one
    // frame. From tier 3 a 64-byte signature on a 48-byte payload is more than
    // half the frame, so one-message-per-fact does not survive.
    for (Track& t : store.tracks()) {
        if (t.belief != Belief::Hostile) continue;
        if (!t.has_local_id) continue;    // hearsay rides the author's frame (D18)
        const bool first = t.last_reported < 0.0f;
        const float every = (now - t.belief_since < kReportBurstFor)
                                ? kReportBurst : kReportEvery;
        if (!first && now - t.last_reported < every) continue;

        Writer w(buffer, sizeof(buffer));
        Header h;
        h.type = MsgType::TrackReport;
        h.origin = static_cast<uint8_t>(cfg_.drone_id);
        h.seq = next_seq_++;
        h.sent_time = now;
        h.Write(w);

        TrackReportMsg m;
        m.position = t.position;
        m.velocity = t.velocity;
        m.belief = t.belief;
        // The seer already paid kEvidenceForCall. One packet must be enough
        // for the facing owner to latch — score*120 was 72 on a fresh call
        // and needed three reports (D35 / x1-b403).
        m.confidence = 255;
        m.Write(w);

        const uint8_t prio = first ? kPrioTrackFirst : kPrioTrack;
        if (w.ok() && outbox.Push(buffer, w.size(), prio, now)) {
            t.last_reported = now;
        }
    }

    // Inbound cone. Hopped like a TrackReport so the far side of the ring
    // sits at the predicted height before that hostile is in sense (D52).
    if (ray_.ready() && sampled_local_ && now - last_ray_send_ >= kRayEvery) {
        Writer w(buffer, sizeof(buffer));
        Header h;
        h.type = MsgType::Ray;
        h.origin = static_cast<uint8_t>(cfg_.drone_id);
        h.seq = next_seq_++;
        h.sent_time = now;
        h.Write(w);
        RayMsg m;
        m.h0 = ray_.h0();
        m.slope = ray_.slope();
        m.weight = ray_.weight();
        m.Write(w);
        if (w.ok() && outbox.Push(buffer, w.size(), kPrioRay, now)) {
            last_ray_send_ = now;
        }
    }

    // Claims stay off the wire. D11: unread Claims won the one frame/tick,
    // interceptors went silent, neighbours stacked. G4 allocation is
    // UniqueOwner on the live ring (D56) plus a closer
    // chaser abort — radio-free, so two drones cannot disagree on a dropped
    // Claim. Stations even over that same live roster. Heartbeat remains
    // the highest priority; relays of it sit at kPrioRelay.

    outbox.Expire(now, 2.0f);
}

void Policy::Pump(const swarm::Host& host, Outbox<24>& outbox,
                  const swarm::Observation& obs) {
    const int32_t i = outbox.Best();
    if (i < 0) return;

    const OutFrame& f = outbox.At(static_cast<uint32_t>(i));

    // Leave headroom rather than spending to zero: a drone that has talked
    // itself out of budget cannot report the thing that actually matters.
    if (obs.tx_budget() < f.len + 64) return;

    if (host.Broadcast(f.bytes, f.len) >= 0) {
        outbox.Remove(static_cast<uint32_t>(i));
    }
}

}  // namespace sw
