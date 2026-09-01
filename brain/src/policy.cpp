#include "policy.h"

#include "flight.h"

#include <cmath>
#include <cstdio>

namespace sw {
namespace {

constexpr float kHeartbeatHz = 2.0f;
constexpr float kAbortAfter = 12.0f;        // s; one spawn interval is 14 s
constexpr float kReportEvery = 0.5f;        // s between reports on one track
constexpr float kPi = 3.14159265358979f;
constexpr float kMinClosing = 1.0f;         // m/s; below this is not a closing intercept
constexpr float kCatchSlack = 0.5f;         // s; must arrive this much before the cylinder
constexpr float kCruise = 14.0f;            // must match Fly() in brain.cpp
constexpr float kFreshHostile = 6.0f;       // s; older Hostile calls are latch-ghosts
constexpr float kRecommitHold = 2.0f;       // s; do not re-chase a track we just aborted

constexpr float kChasingToward = 5.0f;     // m/s along LOS; pickets sit below this
constexpr float kCloserBy = 2.0f;          // m; farther duplicate aborts
constexpr float kOwnerSilent = 1.5f;       // s; three missed 2 Hz heartbeats
constexpr float kNeverHeard = -1.0e8f;

constexpr uint8_t kPrioTrack = 3;
constexpr uint8_t kPrioHeartbeat = 5;       // identity first: claims/reports starved this and neighbours stole intercepts

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

void Policy::Configure(const Config& cfg, Rng rng) {
    cfg_ = cfg;
    rng_ = rng;

    // Ring sizing: neighbours must be able to hear each other, so spacing is
    // driven by comm_radius, and the ring must sit outside the asset radius.
    // TODO(next): the ring shrinks as drones are spent. Decide whether the
    // survivors re-space, and say why in DESIGN.md -- the brief's own note
    // says the late arrivals are where fleets leak.
    ring_radius_ = cfg.asset_radius + cfg.comm_radius * 0.5f;
    ring_altitude_ = 30.0f;
    picket_goal_ = flight::RingSlot(cfg.drone_id, cfg.fleet_size, cfg.asset,
                                    ring_radius_, ring_altitude_);
    for (uint32_t i = 0; i < kMaxFleet; ++i) heard_[i] = -1.0e9f;
}

void Policy::NoteAlive(uint8_t drone_id, float now) {
    if (drone_id >= kMaxFleet) return;
    heard_[drone_id] = now;
}

void Policy::Decide(TrackStore& store, const swarm::Observation& obs) {
    const float now = obs.time();
    last_log_[0] = '\0';

    // Re-resolve the target pointer every tick: the store's storage moves as
    // tracks are erased, so a pointer held across a tick is a dangling read.
    // Find() only matches local ids, including 0 — a hearsay row also sits at
    // 0 with has_local_id false, so we must not treat target_id_ == 0 as
    // "no target" (that stuck drones in Committed while Fly saw a null and
    // cruised the ring). D11.
    target_ = nullptr;
    if (stance_ == Stance::Committed) {
        target_ = store.Find(target_id_);
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
            last_abort_at_ = now;
            target_ = nullptr;
            target_id_ = 0;
            stance_ = Stance::Picketing;
        }
    }

    if (stance_ != Stance::Committed) {
        Track* candidate = nullptr;
        float best_key = 1.0e6f;
        for (Track& t : store.tracks()) {
            if (!ShouldCommit(t, obs)) continue;
            if (CloserChaser(t, store, obs)) continue;
            const float ttg = TimeToCylinder(t.position, t.velocity, cfg_.asset,
                                             cfg_.asset_radius);
            const float d = swarm::Distance(t.position, obs.position());
            const float key = ttg + d * 0.001f;
            if (key < best_key) { best_key = key; candidate = &t; }
        }
        if (candidate) {
            target_ = candidate;
            target_id_ = candidate->track_id;
            committed_at_ = now;
            stance_ = Stance::Committed;
            const float rng = swarm::Distance(candidate->position, obs.position());
            const float closing = ClosingSpeed(obs.position(), obs.velocity(),
                                               candidate->position, candidate->velocity);
            const float miss = ClosestApproachDistance(candidate->position,
                                                       candidate->velocity, cfg_.asset);
            const float ttg = TimeToCylinder(candidate->position, candidate->velocity,
                                             cfg_.asset, cfg_.asset_radius);
            std::snprintf(last_log_, sizeof(last_log_),
                          "commit trk=%u score=%.2f miss=%.1f rng=%.0f close=%.1f ttg=%.1f",
                          candidate->track_id, candidate->closing_score, miss, rng,
                          closing, ttg);
        }
    }

    if (stance_ == Stance::Forming) {
        const Vec3 slot = flight::RingSlot(cfg_.drone_id, cfg_.fleet_size, cfg_.asset,
                                           ring_radius_, ring_altitude_);
        if (swarm::Distance(obs.position(), slot) < 8.0f) {
            stance_ = Stance::Picketing;
            std::snprintf(last_log_, sizeof(last_log_), "picket");
        }
    }

    if (stance_ != Stance::Committed)
        picket_goal_ = PicketGoal(store, now);
}

uint32_t FacingSlot(const Vec3& position, const Vec3& asset, uint32_t fleet_size) {
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    const float hx = position.x - asset.x;
    const float hy = position.y - asset.y;
    if (hx * hx + hy * hy < 1e-8f) return 0;
    float u = std::atan2(hy, hx);
    if (u < 0.0f) u += 2.0f * kPi;
    return static_cast<uint32_t>(
        std::lround(u * static_cast<float>(n) / (2.0f * kPi))) % n;
}

bool SlotAlive(uint32_t drone_id, uint32_t self_id, const float* heard, float now) {
    if (drone_id == self_id) return true;
    if (drone_id >= kMaxFleet || heard == nullptr) return false;
    // Never heard: assume alive so we do not steal a sector at boot before
    // the first heartbeat. After that, 1.5 s of silence is three missed beats.
    if (heard[drone_id] < kNeverHeard) return true;
    return now - heard[drone_id] < kOwnerSilent;
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

bool Policy::OwnsInbound(const Track& t, float now) const {
    const uint32_t facing = FacingSlot(t.position, cfg_.asset, cfg_.fleet_size);
    return UniqueOwner(facing, cfg_.fleet_size, cfg_.drone_id, heard_, now)
           == cfg_.drone_id;
}

bool Policy::CloserChaser(const Track& hostile, const TrackStore& store,
                          const swarm::Observation& obs) const {
    // A Friendly already flying at this hostile is the interceptor. Do not
    // stack. Picket station-keeping is a few m/s; cruise is 14.
    const float us_range = swarm::Distance(obs.position(), hostile.position);
    for (const Track& t : store.tracks()) {
        if (t.belief != Belief::Friendly) continue;
        if (t.has_local_id && hostile.has_local_id && t.track_id == hostile.track_id)
            continue;
        const Vec3 los(hostile.position.x - t.position.x,
                       hostile.position.y - t.position.y, 0.0f);
        const float range_h = swarm::Length(los);
        if (range_h < 1.0f) continue;
        const float toward = swarm::Dot(
            Vec3(t.velocity.x, t.velocity.y, 0.0f), los / range_h);
        if (toward < kChasingToward) continue;
        const float closing = ClosingSpeed(t.position, t.velocity,
                                           hostile.position, hostile.velocity);
        if (closing < kMinClosing) continue;
        const float d = swarm::Distance(t.position, hostile.position);
        if (d < us_range - kCloserBy) return true;
    }
    return false;
}

namespace {

Vec3 YieldOffCorridor(const Vec3& goal, const Vec3& a, const Vec3& b, float clear) {
    const Vec3 ab(b.x - a.x, b.y - a.y, 0.0f);
    const float ab2 = ab.x * ab.x + ab.y * ab.y;
    if (ab2 < 1e-4f || clear <= 0.0f) return goal;
    const Vec3 ap(goal.x - a.x, goal.y - a.y, 0.0f);
    float t = (ap.x * ab.x + ap.y * ab.y) / ab2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float cx = a.x + t * ab.x;
    const float cy = a.y + t * ab.y;
    Vec3 off(goal.x - cx, goal.y - cy, 0.0f);
    const float dist = std::sqrt(off.x * off.x + off.y * off.y);
    if (dist >= clear) return goal;
    Vec3 dir = off;
    if (dist < 1e-3f) dir = Vec3(-ab.y, ab.x, 0.0f);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len < 1e-6f) return goal;
    const float need = (dist < 1e-3f) ? clear : (clear - dist);
    return Vec3(goal.x + dir.x / len * need,
                goal.y + dir.y / len * need,
                goal.z);
}

}  // namespace

Vec3 Policy::PicketGoal(const TrackStore& store, float now) const {
    // Hold the ring, but step off anyone else's intercept corridor so we
    // are not the traffic that spoils ProNav. Owner slot → hostile; neighbours
    // on a 16-drone 75 m ring sit 29 m off that line and do not move.
    Vec3 goal = flight::RingSlot(cfg_.drone_id, cfg_.fleet_size, cfg_.asset,
                                 ring_radius_, ring_altitude_);
    for (const Track& t : store.tracks()) {
        if (t.belief != Belief::Hostile) continue;
        if (!t.has_local_id) continue;
        if (OwnsInbound(t, now)) continue;
        const uint32_t facing = FacingSlot(t.position, cfg_.asset, cfg_.fleet_size);
        const uint32_t owner = UniqueOwner(facing, cfg_.fleet_size, cfg_.drone_id,
                                           heard_, now);
        const Vec3 from = flight::RingSlot(owner, cfg_.fleet_size, cfg_.asset,
                                           ring_radius_, ring_altitude_);
        goal = YieldOffCorridor(goal, from, t.position, cfg_.friendly_margin);
    }
    return goal;
}

bool Policy::ShouldCommit(const Track& t, const swarm::Observation& obs) const {
    // Spend the airframe only on a local Hostile we can actually catch.
    // Hearsay sits at track_id 0; committing to it left Fly with a null
    // target and the drone "committed" on the ring until timeout.
    if (!t.has_local_id) return false;
    if (t.belief != Belief::Hostile) return false;
    // A Hostile latch that is older than a real intercept is wreckage or a
    // mate we failed to ID. Neighbours of a spent owner chased those for 12 s
    // and missed s1 hostile_4, which they had already called.
    if (obs.time() - t.belief_since > kFreshHostile) return false;
    if (t.track_id == last_abort_id_ &&
        obs.time() - last_abort_at_ < kRecommitHold) return false;
    if (!OwnsInbound(t, obs.time())) return false;

    const float range = swarm::Distance(t.position, obs.position());
    if (range > cfg_.sense_radius) return false;
    const float closing = ClosingSpeed(obs.position(), obs.velocity(),
                                       t.position, t.velocity);
    // flight.h: a stern chase against the same 6.7 m/s² bound does not
    // converge. The old `closing > -2 || ttg < 12` admitted those.
    if (closing < kMinClosing) return false;

    // Stationary t_meet (range/closing) is the picket waiting for them.
    // Neighbours of a spent owner have to fly to the intercept; count the
    // cruise we will add along the line of sight (same 14 m/s as Fly).
    const Vec3 los = Vec3(t.position.x - obs.position().x,
                          t.position.y - obs.position().y, 0.0f);
    const float range_h = swarm::Length(los);
    float extra = kCruise;
    if (range_h > 1e-3f) {
        const float our_toward = swarm::Dot(
            Vec3(obs.velocity().x, obs.velocity().y, 0.0f), los / range_h);
        extra = kCruise - our_toward;
        if (extra < 0.0f) extra = 0.0f;
    }
    const float closing_go = closing + extra;
    const float ttg = TimeToCylinder(t.position, t.velocity, cfg_.asset,
                                     cfg_.asset_radius);
    const float t_meet = range / closing_go;
    return t_meet + kCatchSlack < ttg;
}

const char* Policy::AbortReason(const Track& t, const swarm::Observation& obs) const {
    const float now = obs.time();
    if (now - committed_at_ > kAbortAfter) return "timeout";
    if (t.belief != Belief::Hostile) return "not-hostile";
    if (!t.has_local_id) return "lost";

    const float closing = ClosingSpeed(obs.position(), obs.velocity(),
                                       t.position, t.velocity);
    // Evasion (3.5 m/s² weave inside 40 m) can flip the sign for a beat.
    // Immediate receding abort dropped s1 hostile_4 with the interceptor
    // 5 m out. Six seconds is a fair chance to close; after that, give up.
    if (now - committed_at_ > 6.0f && closing < kMinClosing) return "not-closing";
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
    switch (stance_) {
        case Stance::Committed:
            if (target_) return target_->position;
            break;
        case Stance::Forming:
        case Stance::Picketing:
        default:
            break;
    }
    (void)obs;
    return picket_goal_;
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
        if (now - t.last_reported < kReportEvery) continue;

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
        m.confidence = static_cast<uint8_t>(
            t.closing_score > 2.0f ? 255 : static_cast<int>(t.closing_score * 120.0f));
        m.Write(w);

        if (w.ok() && outbox.Push(buffer, w.size(), kPrioTrack, now)) {
            t.last_reported = now;
        }
    }

    // Claims stay off the wire. D11: unread Claims won the one frame/tick,
    // interceptors went silent, neighbours stacked. G4 allocation is
    // UniqueOwner (facing slot, then first live clockwise) plus a closer
    // chaser abort — radio-free, so two drones cannot disagree on a dropped
    // Claim. Heartbeat remains the highest priority.

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
