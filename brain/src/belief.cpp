#include "belief.h"

#include <cmath>

namespace sw {
namespace {

constexpr float kHearsayDrop = 2.0f;       // s; local tracks drop with the sensor picture
// D36: the working window is 0.58–0.60 s. 0.55 reopens x1-b403; 0.65 misses 814.
constexpr float kEvidenceForCall = 0.6f;   // s of aimed geometry to name Hostile
constexpr float kScoreDecay = 0.6f;        // per second, toward zero
constexpr float kSureHit = 5.0f;           // m; aimed-dash CPA, well above fix_sigma 0.35
constexpr float kDiveRate = 1.0f;         // m/s down; civilians measure 0.00-0.14
constexpr float kShrink = 3.0f;            // m of miss drop since first sight → steering
constexpr float kFriendlyGate = 8.0f;      // m; heartbeat → sensor track
constexpr float kFriendlyHold = 2.5f;      // s; 2 Hz heartbeat, covers a few losses
constexpr float kAssociateGate = 8.0f;     // m; peer report → existing track (D14)

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// Drop the vertical component. NED, so z is down and altitude is -z.
Vec3 Flat(const Vec3& v) { return Vec3(v.x, v.y, 0.0f); }

}  // namespace

// ---------------------------------------------------------------------------

float RangeRate(const Vec3& position, const Vec3& velocity, const Vec3& target) {
    const Vec3 to_target = Flat(target - position);
    const float range = swarm::Length(to_target);
    if (range < 1e-3f) return 0.0f;
    return -swarm::Dot(Flat(velocity), to_target / range);
}

float ApproachAlignment(const Vec3& position, const Vec3& velocity, const Vec3& target) {
    const Vec3 flat_velocity = Flat(velocity);
    const float speed = swarm::Length(flat_velocity);
    if (speed < 0.5f) return 0.0f;
    const Vec3 to_target = Flat(target - position);
    const float range = swarm::Length(to_target);
    if (range < 1e-3f) return 0.0f;
    return swarm::Dot(flat_velocity / speed, to_target / range);
}

float ClosestApproachDistance(const Vec3& position, const Vec3& velocity,
                              const Vec3& target) {
    const Vec3 offset = position - target;
    const Vec3 v = velocity;
    const float speed_sq = swarm::LengthSq(v);
    if (speed_sq < 0.25f) return swarm::Length(offset);   // not going anywhere

    // Time of closest approach, never in the past: something already receding
    // has its closest approach behind it and misses by its current range.
    float t = -swarm::Dot(offset, v) / speed_sq;
    if (t < 0.0f) t = 0.0f;

    return swarm::Length(offset + v * t);
}

bool AimedAtAsset(float miss, float miss_at_first, float asset_radius) {
    if (miss < kSureHit) return true;
    return miss < asset_radius && miss < miss_at_first - kShrink;
}

float TimeToTarget(const Vec3& position, const Vec3& velocity, const Vec3& target) {
    const float range = swarm::Distance(position, target);
    const float closing = -RangeRate(position, velocity, target);
    if (closing <= 0.1f) return 1.0e6f;
    return range / closing;
}

float TimeToCylinder(const Vec3& position, const Vec3& velocity,
                     const Vec3& centre, float radius) {
    const Vec3 flat = Flat(position - centre);
    const float ground = swarm::Length(flat);
    if (ground <= radius) return 0.0f;
    const float closing = -RangeRate(position, velocity, centre);
    if (closing <= 0.1f) return 1.0e6f;
    return (ground - radius) / closing;
}

float ThreatWindow(const Vec3& position, const Vec3& velocity,
                   const Vec3& centre, float radius, float dash_speed) {
    const Vec3 flat = Flat(position - centre);
    const float ground = swarm::Length(flat);
    if (ground <= radius) return 0.0f;
    const float closing = -RangeRate(position, velocity, centre);
    float speed = dash_speed > 0.1f ? dash_speed : kHostileDash;
    if (closing > speed) speed = closing;
    return (ground - radius) / speed;
}

bool GroundTrackHitsCylinder(const Vec3& position, const Vec3& velocity,
                             const Vec3& asset, float asset_radius) {
    const float ground_miss = ClosestApproachDistance(
        Flat(position), Flat(velocity), Flat(asset));
    return ground_miss < asset_radius;
}

bool LooksDivingAtAsset(const Vec3& position, const Vec3& velocity,
                        const Vec3& asset, float asset_radius) {
    if (velocity.z <= kDiveRate) return false;
    return GroundTrackHitsCylinder(position, velocity, asset, asset_radius);
}

float ClosingSpeed(const Vec3& observer_p, const Vec3& observer_v,
                   const Vec3& target_p, const Vec3& target_v) {
    const Vec3 los = Flat(target_p - observer_p);
    const float range = swarm::Length(los);
    if (range < 1e-3f) return 0.0f;
    const Vec3 rel = Flat(target_v - observer_v);
    return -swarm::Dot(rel, los / range);
}

bool LooksBallistic(const Vec3& velocity, const Vec3& prev_velocity, float dt) {
    if (dt < 1e-4f) return false;
    const Vec3 accel = (velocity - prev_velocity) / dt;
    // NED: gravity is +z. Unpowered means vertical accel near g and little else.
    const bool falling = accel.z > 6.0f && accel.z < 14.0f;
    const float lateral = std::sqrt(accel.x * accel.x + accel.y * accel.y);
    return falling && lateral < 4.0f;
}

bool HeartbeatPlausible(const Vec3& self, const Vec3& claimed,
                        float measured_range, float range_sigma) {
    const float claimed_range = swarm::Distance(self, claimed);
    const float sigma = range_sigma > 0.1f ? range_sigma : 1.0f;
    // Three sigma plus a couple of metres for quantisation and a few ticks of
    // latency. A replay from the far side of the arena misses this by tens of
    // metres, not by noise.
    const float tol = 3.0f * sigma + 2.0f;
    return std::fabs(claimed_range - measured_range) <= tol;
}

// ---------------------------------------------------------------------------

bool FitInboundRay(const Vec3& position, const Vec3& velocity, const Vec3& asset,
                   float& h0, float& slope) {
    const float dx = position.x - asset.x;
    const float dy = position.y - asset.y;
    const float r = std::sqrt(dx * dx + dy * dy);
    if (r < kRayMinRange) return false;
    // Outward radial speed. Inbound is negative.
    const float dr = (dx * velocity.x + dy * velocity.y) / r;
    if (dr > -kRayMinClosing) return false;
    const float h = -position.z;
    const float dh = -velocity.z;
    float b = dh / dr;
    if (b < 0.0f) b = 0.0f;
    if (b > kRayMaxSlope) b = kRayMaxSlope;
    h0 = h - b * r;
    slope = b;
    return true;
}

void InboundRay::Decay(float dt) {
    if (dt <= 0.0f || weight_ <= 0.0f) return;
    weight_ *= std::exp(-dt / kRayTau);
    if (weight_ < 1.0e-3f) weight_ = 0.0f;
}

bool InboundRay::Blend(float h0, float slope, float w, float at_range,
                       float at_alt) {
    if (w <= 0.0f) return false;
    if (slope < 0.0f) slope = 0.0f;
    if (slope > kRayMaxSlope) slope = kRayMaxSlope;
    if (weight_ >= kRayReady) {
        float pred_h, sample_h;
        if (at_range > 1.0f) {
            pred_h = h0_ + slope_ * at_range;
            sample_h = at_alt;
        } else {
            constexpr float kRef = 80.0f;
            pred_h = h0_ + slope_ * kRef;
            sample_h = h0 + slope * kRef;
        }
        if (std::fabs(sample_h - pred_h) > kRayOutlier) return false;
    }
    const float sum = weight_ + w;
    h0_ = (h0_ * weight_ + h0 * w) / sum;
    slope_ = (slope_ * weight_ + slope * w) / sum;
    weight_ = sum;
    return true;
}

bool InboundRay::Sample(const Vec3& position, const Vec3& velocity,
                        const Vec3& asset, float w) {
    float a = 0.0f, b = 0.0f;
    if (!FitInboundRay(position, velocity, asset, a, b)) return false;
    const float dx = position.x - asset.x;
    const float dy = position.y - asset.y;
    const float r = std::sqrt(dx * dx + dy * dy);
    return Blend(a, b, w, r, -position.z);
}

float InboundRay::HeightAt(float radius) const {
    if (radius < 0.0f) radius = 0.0f;
    return h0_ + slope_ * radius;
}

// ---------------------------------------------------------------------------

Track* TrackStore::Find(uint32_t track_id) {
    for (Track& t : tracks_)
        if (t.has_local_id && t.track_id == track_id) return &t;
    return nullptr;
}

Track* TrackStore::FindByStoreId(uint32_t store_id) {
    if (store_id == 0) return nullptr;
    for (Track& t : tracks_)
        if (t.store_id == store_id) return &t;
    return nullptr;
}

uint32_t TrackStore::Birth(Track& t) {
    t.store_id = ++next_id_;
    return t.store_id;
}

Track* TrackStore::NearestTo(const Vec3& p, float max_distance) {
    Track* best = nullptr;
    float best_d = max_distance;
    for (Track& t : tracks_) {
        const float d = swarm::Distance(t.position, p);
        if (d < best_d) { best_d = d; best = &t; }
    }
    return best;
}

Track* TrackStore::NearestHostile(const Vec3& p, float max_distance) {
    Track* best = nullptr;
    float best_d = max_distance;
    for (Track& t : tracks_) {
        if (t.belief != Belief::Hostile) continue;
        const float d = swarm::Distance(t.position, p);
        if (d < best_d) { best_d = d; best = &t; }
    }
    return best;
}

void TrackStore::Update(const swarm::Observation& obs) {
    const float now = obs.time();
    const float dt = (last_time_ > 0.0f) ? (now - last_time_) : obs.dt();
    last_time_ = now;

    // Fold in what we can see. obs.tracks() is ascending by track_id, so this
    // loop's order is defined by the simulator and not by our own bookkeeping.
    for (const SwTrack& raw : obs.tracks()) {
        Track* t = Find(raw.track_id);
        if (!t) {
            t = tracks_.emplace();
            if (!t) continue;              // at capacity; drop rather than grow
            Birth(*t);
            t->track_id = raw.track_id;
            t->has_local_id = true;
            t->first_seen = raw.first_seen;
            t->belief = Belief::Unknown;
            t->belief_since = now;
            t->last_velocity = raw.velocity;
            t->first_position = raw.position;
        }
        else {
            t->last_velocity = t->velocity;   // for the ballistic test below
        }
        t->position = raw.position;
        t->velocity = raw.velocity;
        t->attitude = raw.attitude;
        t->last_seen = raw.last_seen;
        t->last_update = now;

        Classify(*t, now, dt);
    }

    // A local track appearing on top of a hearsay row is the same aircraft
    // now in view. Fold the peer evidence in and drop the ghost so a commit
    // keyed on the hearsay store_id can re-associate (D18).
    AbsorbHearsay();

    // Local tracks: the simulator already dropped them from obs.tracks()
    // (destroyed vanish the next tick; out-of-range after an unpublished
    // few seconds — CHALLENGE.md §3). Holding them 3 s was a frozen Hostile
    // at the last pose, which is how D11's neighbours chased ghosts.
    // Hearsay is not in the sensor picture; it ages out on last_update.
    for (uint32_t i = tracks_.size(); i > 0; --i) {
        Track& t = tracks_[i - 1];
        const float age = now - t.last_update;
        const bool stale = t.has_local_id ? age > 1e-4f : age > kHearsayDrop;
        if (stale) tracks_.erase(i - 1);
    }
}

void TrackStore::Classify(Track& t, float now, float dt) {
    // --- wreckage: cheapest and most certain test we have -------------------
    if (LooksBallistic(t.velocity, t.last_velocity, dt)) {
        // Clamped, not accumulated without bound: 200 s of falling would
        // otherwise take 300 s to decay back below the threshold.
        t.ballistic_score = Clamp(t.ballistic_score + dt * 2.0f, 0.0f, 1.5f);
    } else {
        t.ballistic_score = Clamp(t.ballistic_score - dt * kScoreDecay, 0.0f, 3.0f);
    }
    if (t.ballistic_score > 0.4f) {
        if (t.belief != Belief::Wreckage) { t.belief = Belief::Wreckage; t.belief_since = now; }
        return;
    }

    // A live heartbeat pins this as a mate. Geometry that looks like a dash
    // is exactly what an interceptor does on the way in; calling it hostile
    // is how pickets ram their own. Hold expires if the radio goes quiet.
    if (t.belief == Belief::Friendly) {
        if (now <= t.friendly_until) return;
        t.belief = Belief::Unknown;
        t.belief_since = now;
    }

    // --- hostile: sustained, deliberate approach to the asset ---------------
    //
    // Alignment and closing are horizontal: a dive from 40 m is still aimed
    // in the plane that scores, and 3D alignment would dilute it. Miss is 3D.
    // A civilian whose ground track goes through the origin still flies tens
    // of metres over it (level, vz ~ 0); a hostile dives, so 3D miss shrinks
    // toward the origin. Horizontal miss called those overflights — drone 2
    // on x1-a, ground miss 5.2 m, 30 m up. 3D miss is ~altitude and does not
    // shrink, so AimedAtAsset stays false. The *breach* is still a cylinder
    // (D10): ground range ≤ asset_radius, any altitude. See D7.
    const float alignment = ApproachAlignment(t.position, t.velocity, cfg_.asset);
    const float closing = -RangeRate(t.position, t.velocity, cfg_.asset);
    const float miss = ClosestApproachDistance(t.position, t.velocity, cfg_.asset);
    if (t.miss_at_first < 0.0f) t.miss_at_first = miss;

    // A hostile dives; a civilian does not. The 3D miss test above is what
    // keeps civilian overflights out of the hostile call (D7), but it can only
    // fire once the dive has developed: measured, a hostile's 3D miss needs
    // 2.5 s to fall under kSureHit, out of a window that is about 4 s long.
    //
    // The dive itself is visible immediately and civilians never produce it.
    // Measured on x1-ae01dd: hostiles ramp to vz +3.6 m/s within 2 s and hold
    // it, passing +1.67 by 0.5 s with a ground miss already at 0.1 m, while
    // every civilian in the same run sits between 0.00 and 0.14 m/s at
    // altitudes from 31 to 67 m. So a track that is descending AND whose
    // GROUND track enters the cylinder is a hostile now -- and the breach is a
    // cylinder at any altitude (D10), so the ground track is the right test for
    // it. Wreckage falls too, but the ballistic test above has already returned.
    // Only when there is no time left to wait for it, though. Calling early is
    // not free: the owner commits early, leaves station early and is away
    // longer, which on a spawner that enters where nobody is standing costs
    // more than the early call wins (D31). TimeToCylinder is the wrong clock
    // for that decision when the hostile is still spooling up: it returns 1e6
    // until horizontal closing exceeds 0.1 m/s, which is exactly the
    // spawn-inside-sense case (first seen while accelerating from rest). Those
    // inbounds are the short window — ThreatWindow using dash speed is ~7 s —
    // and waiting for 3D miss to fall under 5 m spends most of it. A hostile
    // already at dash that we first see coming in from outside has a real TTG
    // and the original gate is enough.
    const bool diving = LooksDivingAtAsset(t.position, t.velocity,
                                          cfg_.asset, cfg_.asset_radius);
    const float horiz_speed = swarm::Length(Flat(t.velocity));
    const bool spooling = horiz_speed < kHostileDash * 0.6f;
    const float window = ThreatWindow(t.position, t.velocity, cfg_.asset,
                                      cfg_.asset_radius, kHostileDash);
    // Compact spawn-inside-sense: window ~7 s, and they are still spooling
    // when first seen. s1's 170 m inbound is ~8.8 s — leave that to the
    // patient TTG gate so we do not empty a sector for a long intercept (D31).
    const bool compact_spool = spooling && window < kCompactWindow;
    const bool short_window =
        TimeToCylinder(t.position, t.velocity, cfg_.asset, cfg_.asset_radius) < 10.0f
        || compact_spool;
    const bool aimed = AimedAtAsset(miss, t.miss_at_first, cfg_.asset_radius)
                       || (short_window && diving);

    if (GroundTrackHitsCylinder(t.position, t.velocity, cfg_.asset,
                                cfg_.asset_radius))
        t.cylinder_score = Clamp(t.cylinder_score + dt, 0.0f, 3.0f);
    else
        t.cylinder_score = Clamp(t.cylinder_score - dt * kScoreDecay, 0.0f, 3.0f);

    if (aimed && alignment > 0.8f && closing > 4.0f) {
        t.closing_score = Clamp(t.closing_score + dt, -2.0f, 3.0f);
    } else if (!aimed || alignment < 0.3f) {
        t.closing_score = Clamp(t.closing_score - dt * kScoreDecay, -2.0f, 4.0f);
    }

    if (t.closing_score > kEvidenceForCall) {
        if (t.belief != Belief::Hostile) { t.belief = Belief::Hostile; t.belief_since = now; }
        return;
    }

    // --- otherwise: say nothing ---------------------------------------------
    //
    // Wrong declarations cost -2, unknown 0. Classify may still hold Hostile
    // for intercept; Policy::Declare is what the awareness term reads, and
    // it only publishes 2:1 calls (D9). This reset is so a decaying dash
    // does not stay Hostile in the store after the geometry has gone.
    if (t.belief != Belief::Unknown && t.belief != Belief::Friendly &&
        t.closing_score < 0.2f) {
        t.belief = Belief::Unknown;
        t.belief_since = now;
    }
}

void TrackStore::AbsorbHearsay() {
    bool drop[kMaxTracks]{};
    for (uint32_t i = 0; i < tracks_.size(); ++i) {
        if (!tracks_[i].has_local_id) continue;
        for (uint32_t j = 0; j < tracks_.size(); ++j) {
            if (j == i || tracks_[j].has_local_id || drop[j]) continue;
            if (swarm::Distance(tracks_[i].position, tracks_[j].position) > kAssociateGate)
                continue;
            Track& local = tracks_[i];
            const Track& peer = tracks_[j];
            if (local.belief != Belief::Friendly) {
                if (peer.closing_score > local.closing_score)
                    local.closing_score = peer.closing_score;
                if (peer.last_origin != 0) {
                    local.last_origin = peer.last_origin;
                    local.last_hops = peer.last_hops;
                }
                if (peer.belief == Belief::Hostile && local.belief != Belief::Hostile) {
                    local.belief = Belief::Hostile;
                    local.belief_since = peer.belief_since;
                }
            }
            drop[j] = true;
        }
    }
    for (uint32_t j = tracks_.size(); j > 0; --j) {
        if (drop[j - 1]) tracks_.erase(j - 1);
    }
}

void TrackStore::MergePeerReport(const Vec3& position, const Vec3& velocity,
                                 Belief peer_belief, uint8_t confidence, float now,
                                 uint8_t origin, uint8_t hops) {
    // Association after extrapolating by measured age (D14). 4 m (sigmas
    // only) duplicated the same aircraft: 2089 class transitions on s1 and
    // comms 26 vs 36. Leftover after extrapolation is two biases plus any
    // unmodelled turn while a report sat in the outbox:
    // 2·1.2 + 3·√2·0.35 + 0.2 s · 16 m/s ≈ 7 m. 8 m is that, rounded.

    Track* t = NearestTo(position, kAssociateGate);
    if (t && t->belief == Belief::Friendly)
        return;                           // drop; do not spawn a ghost hostile on a mate
    if (!t) {
        t = tracks_.emplace();
        if (!t) return;
        Birth(*t);
        t->has_local_id = false;          // hearsay; Find() must not match it
        t->first_seen = now;
        t->belief_since = now;
        t->last_velocity = velocity;
        t->first_position = position;
    }
    t->position = position;
    t->velocity = velocity;
    t->last_update = now;
    t->last_seen = now;
    t->last_origin = origin;
    t->last_hops = hops;

    // A peer's Hostile call is a completed classify, not 0.28 of one.
    // score*120 confidence needed three 0.5 s reports before the facing
    // owner would latch, and the inbound on x1-b403 covered 18 m in that
    // wait (D35). Still refuse to overwrite a Friendly.
    if (peer_belief == Belief::Hostile) {
        const float bump = static_cast<float>(confidence) / 255.0f;
        const float add = bump > kEvidenceForCall ? bump : (kEvidenceForCall + 0.01f);
        t->closing_score = Clamp(t->closing_score + add, -2.0f, 3.0f);
    }
    if (t->closing_score > kEvidenceForCall && t->belief != Belief::Hostile
        && t->belief != Belief::Friendly) {
        t->belief = Belief::Hostile;
        t->belief_since = now;
    }
}

void TrackStore::MarkFriendly(const Vec3& claimed, const Vec3& self,
                              float measured_range, float range_sigma, float now) {
    if (!HeartbeatPlausible(self, claimed, measured_range, range_sigma)) return;

    Track* t = NearestTo(claimed, kFriendlyGate);
    if (!t || !t->has_local_id) return;

    if (t->belief != Belief::Friendly) {
        t->belief = Belief::Friendly;
        t->belief_since = now;
        t->closing_score = 0.0f;
    }
    t->friendly_until = now + kFriendlyHold;
}

Track* TrackStore::MostUrgentHostile(const Vec3& self_position, float now) {
    (void)now;
    Track* best = nullptr;
    float best_ttg = 1.0e6f;

    for (Track& t : tracks_) {
        if (t.belief != Belief::Hostile) continue;
        const float ttg = TimeToCylinder(t.position, t.velocity, cfg_.asset,
                                         cfg_.asset_radius);
        // Weighted sum, NOT a lexicographic order, and track_id is not
        // consulted: range is worth 1 s of time-to-go per 1000 m, so it only
        // separates near-ties. An exact tie still resolves by iteration order,
        // so the determinism hazard is made unlikely here, not removed.
        const float d = swarm::Distance(t.position, self_position);
        const float key = ttg + d * 0.001f;
        if (key < best_ttg) { best_ttg = key; best = &t; }
    }
    return best;
}

}  // namespace sw
