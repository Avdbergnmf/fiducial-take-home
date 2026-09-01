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
    // Survivors re-space in place when a *nearby* heartbeat goes silent
    // (D19). Opposite-side radio loss is not a death. Radius stays; the
    // hole in bearing is what leaked late arrivals.
    ring_radius_ = cfg.asset_radius + cfg.comm_radius * 0.5f;
    ring_altitude_ = 30.0f;
    picket_goal_ = flight::RingSlot(cfg.drone_id, cfg.fleet_size, cfg.asset,
                                    ring_radius_, ring_altitude_);
    for (uint32_t i = 0; i < kMaxFleet; ++i) heard_[i] = -1.0e9f;
}

void Policy::NoteAlive(uint8_t drone_id, const Vec3& position, float now) {
    if (drone_id >= kMaxFleet) return;
    heard_[drone_id] = now;
    heard_at_[drone_id] = position;
}

void Policy::Decide(TrackStore& store, const swarm::Observation& obs) {
    const float now = obs.time();
    last_log_[0] = '\0';

    // Re-resolve the target every tick: the store's storage moves as tracks
    // are erased. Hearsay has no local id, so we key on store_id and
    // re-associate by geometry if a local track absorbed the row (D18).
    target_ = nullptr;
    if (stance_ == Stance::Committed) {
        target_ = ResolveTarget(store);
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
            stance_ = Stance::Picketing;
        } else {
            BindTarget(target_);
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
            BindTarget(candidate);
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
                          "commit trk=%u score=%.2f miss=%.1f rng=%.0f close=%.1f ttg=%.1f n=%.0f e=%.0f%s",
                          candidate->has_local_id ? candidate->track_id : 0,
                          candidate->closing_score, miss, rng, closing, ttg,
                          candidate->position.x, candidate->position.y,
                          candidate->has_local_id ? "" : " peer");
        }
    }

    if (stance_ == Stance::Forming) {
        const uint32_t n = CountLive(cfg_.fleet_size, cfg_.drone_id, heard_,
                                     heard_at_, obs.position(), cfg_.comm_radius,
                                     now);
        const uint32_t rank = LiveRank(cfg_.drone_id, cfg_.fleet_size,
                                       cfg_.drone_id, heard_, heard_at_,
                                       obs.position(), cfg_.comm_radius, now);
        const Vec3 slot = flight::RingSlot(rank, n, cfg_.asset,
                                           ring_radius_, ring_altitude_);
        if (swarm::Distance(obs.position(), slot) < 8.0f) {
            stance_ = Stance::Picketing;
            std::snprintf(last_log_, sizeof(last_log_), "picket");
        }
    }

    if (stance_ != Stance::Committed)
        picket_goal_ = PicketGoal(store, obs);
}

uint32_t FacingSlot(const Vec3& position, const Vec3& asset, uint32_t count) {
    const uint32_t n = count > 0 ? count : 1;
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

bool RingAlive(uint32_t drone_id, uint32_t self_id, const float* heard,
               const Vec3* heard_at, const Vec3& self_pos, float comm_radius,
               float now) {
    if (SlotAlive(drone_id, self_id, heard, now)) return true;
    if (drone_id >= kMaxFleet || heard_at == nullptr) return false;
    // Silent after a heartbeat. Neighbours cannot leave radio in 1.5 s, so
    // that is a death. Opposite-side drones leave comm range as the ring
    // spreads — keep their station or the live ring collapses to whoever
    // we can still hear (D19).
    const float dx = heard_at[drone_id].x - self_pos.x;
    const float dy = heard_at[drone_id].y - self_pos.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    const float keep = comm_radius - kCruise * kOwnerSilent - 10.0f;
    return d > keep;
}

uint32_t CountLive(uint32_t fleet_size, uint32_t self_id, const float* heard,
                   const Vec3* heard_at, const Vec3& self_pos, float comm_radius,
                   float now) {
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    const uint32_t cap = n < kMaxFleet ? n : kMaxFleet;
    uint32_t live = 0;
    for (uint32_t id = 0; id < cap; ++id) {
        if (RingAlive(id, self_id, heard, heard_at, self_pos, comm_radius, now))
            ++live;
    }
    return live > 0 ? live : 1;
}

uint32_t LiveRank(uint32_t drone_id, uint32_t fleet_size, uint32_t self_id,
                  const float* heard, const Vec3* heard_at, const Vec3& self_pos,
                  float comm_radius, float now) {
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    const uint32_t cap = n < kMaxFleet ? n : kMaxFleet;
    uint32_t rank = 0;
    for (uint32_t id = 0; id < cap; ++id) {
        if (!RingAlive(id, self_id, heard, heard_at, self_pos, comm_radius, now))
            continue;
        if (id == drone_id) return rank;
        ++rank;
    }
    return 0;
}

uint32_t LiveId(uint32_t rank, uint32_t fleet_size, uint32_t self_id,
                const float* heard, const Vec3* heard_at, const Vec3& self_pos,
                float comm_radius, float now) {
    const uint32_t live = CountLive(fleet_size, self_id, heard, heard_at,
                                    self_pos, comm_radius, now);
    const uint32_t want = rank % live;
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    const uint32_t cap = n < kMaxFleet ? n : kMaxFleet;
    uint32_t i = 0;
    for (uint32_t id = 0; id < cap; ++id) {
        if (!RingAlive(id, self_id, heard, heard_at, self_pos, comm_radius, now))
            continue;
        if (i == want) return id;
        ++i;
    }
    return self_id;
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

bool Policy::OwnsInbound(const Track& t, const swarm::Observation& obs) const {
    // Allocation stays on the original id ring: facing slot, then first
    // live clockwise (D15). Stations re-space (D19); who may spend does not,
    // so a ghost far-side silence cannot hand the inbound to nobody.
    const uint32_t facing = FacingSlot(t.position, cfg_.asset, cfg_.fleet_size);
    return UniqueOwner(facing, cfg_.fleet_size, cfg_.drone_id, heard_, obs.time())
           == cfg_.drone_id;
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

Vec3 CorridorOrigin(const TrackStore& store, const Track& hostile, const Vec3& slot) {
    const Track* best = nullptr;
    float best_d = 1.0e9f;
    for (const Track& t : store.tracks()) {
        if (!FlyingAt(t, hostile)) continue;
        const float d = swarm::Distance(t.position, hostile.position);
        if (d < best_d) { best_d = d; best = &t; }
    }
    return best ? best->position : slot;
}

}  // namespace

bool Policy::CloserChaser(const Track& hostile, const TrackStore& store,
                          const swarm::Observation& obs) const {
    const float us_range = swarm::Distance(obs.position(), hostile.position);
    for (const Track& t : store.tracks()) {
        if (!FlyingAt(t, hostile)) continue;
        const float d = swarm::Distance(t.position, hostile.position);
        if (d < us_range - kCloserBy) return true;
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

Vec3 Policy::PicketGoal(const TrackStore& store, const swarm::Observation& obs) const {
    // Hold the live ring, but step off anyone else's *remaining* intercept so
    // we are not the traffic that spoils ProNav. After a nearby death the
    // survivors take evenly spaced stations (D19); yield uses those stations.
    // Past the predicted ram they do not move (D17).
    const float now = obs.time();
    const uint32_t n = CountLive(cfg_.fleet_size, cfg_.drone_id, heard_,
                                 heard_at_, obs.position(), cfg_.comm_radius,
                                 now);
    const uint32_t rank = LiveRank(cfg_.drone_id, cfg_.fleet_size,
                                   cfg_.drone_id, heard_, heard_at_,
                                   obs.position(), cfg_.comm_radius, now);
    Vec3 goal = flight::RingSlot(rank, n, cfg_.asset,
                                 ring_radius_, ring_altitude_);
    for (const Track& t : store.tracks()) {
        if (t.belief != Belief::Hostile) continue;
        if (OwnsInbound(t, obs)) continue;
        const uint32_t facing = FacingSlot(t.position, cfg_.asset, cfg_.fleet_size);
        const uint32_t owner = UniqueOwner(facing, cfg_.fleet_size, cfg_.drone_id,
                                           heard_, now);
        const uint32_t owner_rank = LiveRank(owner, cfg_.fleet_size, cfg_.drone_id,
                                             heard_, heard_at_, obs.position(),
                                             cfg_.comm_radius, now);
        const Vec3 slot = flight::RingSlot(owner_rank, n, cfg_.asset,
                                           ring_radius_, ring_altitude_);
        const Vec3 from = CorridorOrigin(store, t, slot);
        const Vec3 end = CorridorHorizon(from, t.position, t.velocity);
        goal = YieldOffCorridor(goal, from, end, cfg_.friendly_margin);
    }
    return goal;
}

bool Policy::ShouldCommit(const Track& t, const swarm::Observation& obs) const {
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
    if (!OwnsInbound(t, obs)) return false;

    const float range = swarm::Distance(t.position, obs.position());
    const float closing = ClosingSpeed(obs.position(), obs.velocity(),
                                       t.position, t.velocity);
    // flight.h: a stern chase against the same 6.7 m/s² bound does not
    // converge. The old `closing > -2 || ttg < 12` admitted those.
    if (closing < kMinClosing) return false;

    // Stationary t_meet (range/closing) is the picket waiting for them.
    // After a death the live ring slides into the hole (D19); still count
    // cruise along the line of sight if we are not yet on the new station.
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
    // Claim. Stations re-space on nearby death (D19). Heartbeat remains
    // the highest priority.

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
