#include "belief.h"

namespace sw {
namespace {

constexpr float kDropAfter = 3.0f;         // s without an update before we forget
constexpr float kEvidenceForCall = 1.2f;   // integrated score needed to commit
constexpr float kScoreDecay = 0.6f;        // per second, toward zero

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
    const Vec3 offset = Flat(position - target);
    const Vec3 v = Flat(velocity);
    const float speed_sq = swarm::LengthSq(v);
    if (speed_sq < 0.25f) return swarm::Length(offset);   // not going anywhere

    // Time of closest approach, never in the past: something already receding
    // has its closest approach behind it and misses by its current range.
    float t = -swarm::Dot(offset, v) / speed_sq;
    if (t < 0.0f) t = 0.0f;

    return swarm::Length(offset + v * t);
}

float TimeToTarget(const Vec3& position, const Vec3& velocity, const Vec3& target) {
    const float range = swarm::Distance(position, target);
    const float closing = -RangeRate(position, velocity, target);
    if (closing <= 0.1f) return 1.0e6f;
    return range / closing;
}

bool LooksBallistic(const Vec3& velocity, const Vec3& prev_velocity, float dt) {
    if (dt < 1e-4f) return false;
    const Vec3 accel = (velocity - prev_velocity) / dt;
    // NED: gravity is +z. Unpowered means vertical accel near g and little else.
    const bool falling = accel.z > 6.0f && accel.z < 14.0f;
    const float lateral = std::sqrt(accel.x * accel.x + accel.y * accel.y);
    return falling && lateral < 4.0f;
}

// ---------------------------------------------------------------------------

Track* TrackStore::Find(uint32_t track_id) {
    for (Track& t : tracks_)
        if (t.has_local_id && t.track_id == track_id) return &t;
    return nullptr;
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
            t->track_id = raw.track_id;
            t->has_local_id = true;
            t->first_seen = raw.first_seen;
            t->belief = Belief::Unknown;
            t->belief_since = now;
            t->last_velocity = raw.velocity;
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

    // Forget anything stale. Iterate backwards so erase() cannot skip an entry.
    for (uint32_t i = tracks_.size(); i > 0; --i) {
        if (now - tracks_[i - 1].last_update > kDropAfter) tracks_.erase(i - 1);
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

    // --- hostile: sustained, deliberate approach to the asset ---------------
    //
    // v0 DISCRIMINANT. The three classes are physically identical, so the only
    // evidence is behaviour. On s1 a hostile dashes at the asset from 170 m and
    // a civilian crosses on a straight line at a similar speed -- so the signal
    // is closing GEOMETRY, not speed.
    //
    // Evidence is integrated over time rather than tested per tick, because a
    // civilian whose straight line happens to point at the asset for a moment
    // is exactly the false positive that costs -2.
    //
    // TODO(next): a civilian on a chord through the arena can hold alignment
    // for several seconds. Distinguishing it needs either the miss distance at
    // closest approach or corroboration from a second observer.
    const float alignment = ApproachAlignment(t.position, t.velocity, cfg_.asset);
    const float closing = -RangeRate(t.position, t.velocity, cfg_.asset);
    const float miss = ClosestApproachDistance(t.position, t.velocity, cfg_.asset);

    // Miss distance carries the weight. A civilian crossing on a chord can hold
    // high alignment for several seconds, so alignment alone would call it
    // hostile and cost -2. Its miss distance stays bounded away from zero.
    const bool aimed = miss < cfg_.asset_radius * 0.8f;

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
    // Wrong costs -2, unknown costs 0. Staying quiet until the evidence is
    // there is cheap. But note the term is clamped at zero, so a brain that
    // never declares anything scores the same as one that guesses badly: the
    // goal is confident calls, not silence.
    //
    // TODO(next): friendlies are identifiable once heartbeats are running --
    // a peer whose claimed position matches this track's position, corroborated
    // by the measured range on the frame, is a friendly. That is free accuracy
    // on a third of the airspace and it needs the protocol, not the sensors.
    if (t.belief != Belief::Unknown && t.closing_score < 0.2f) {
        t.belief = Belief::Unknown;
        t.belief_since = now;
    }
}

void TrackStore::MergePeerReport(const Vec3& position, const Vec3& velocity,
                                 Belief peer_belief, uint8_t confidence, float now) {
    // Association gate: both our fix and theirs are wrong by a bit, and the
    // report is a few hundred ms stale, so the gate has to be generous.
    // TODO(next): size this from fix_sigma and the measured link latency
    // rather than a constant.
    constexpr float kGate = 12.0f;

    Track* t = NearestTo(position, kGate);
    if (!t) {
        t = tracks_.emplace();
        if (!t) return;
        t->has_local_id = false;          // hearsay; Find() must not match it
        t->first_seen = now;
        t->belief_since = now;
        t->last_velocity = velocity;
    }
    t->position = position;
    t->velocity = velocity;
    t->last_update = now;
    t->last_seen = now;

    // A peer's opinion is evidence, not truth. It moves the score; it does not
    // set the verdict. From tier 3 on, an unverified peer may be a hostile
    // replaying your own traffic, and from tier 5 it may be one of yours,
    // sincerely wrong.
    if (peer_belief == Belief::Hostile) {
        t->closing_score = Clamp(
            t->closing_score + static_cast<float>(confidence) / 255.0f, -2.0f, 3.0f);
    }
    if (t->closing_score > kEvidenceForCall && t->belief != Belief::Hostile) {
        t->belief = Belief::Hostile;
        t->belief_since = now;
    }
}

Track* TrackStore::MostUrgentHostile(const Vec3& self_position, float now) {
    (void)now;
    Track* best = nullptr;
    float best_ttg = 1.0e6f;

    for (Track& t : tracks_) {
        if (t.belief != Belief::Hostile) continue;
        const float ttg = TimeToTarget(t.position, t.velocity, cfg_.asset);
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
