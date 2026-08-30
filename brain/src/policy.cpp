#include "policy.h"

#include "flight.h"

namespace sw {
namespace {

constexpr float kHeartbeatHz = 2.0f;
constexpr float kCommitMaxRange = 120.0f;   // do not chase what we cannot catch
constexpr float kAbortAfter = 25.0f;        // s of unproductive pursuit
constexpr float kClaimHold = 6.0f;          // s a claim stands without renewal
constexpr float kReportEvery = 0.5f;        // s between reports on one track

constexpr uint8_t kPrioHeartbeat = 1;
constexpr uint8_t kPrioTrack = 3;
constexpr uint8_t kPrioClaim = 4;
constexpr uint8_t kPrioAccuse = 5;

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
}

void Policy::Decide(TrackStore& store, const swarm::Observation& obs) {
    const float now = obs.time();

    // Re-resolve the target pointer every tick: the store's storage moves as
    // tracks are erased, so a pointer held across a tick is a dangling read.
    target_ = nullptr;
    if (stance_ == Stance::Committed && target_id_ != 0) {
        target_ = store.Find(target_id_);
        if (!target_ || ShouldAbort(*target_, obs)) {
            target_ = nullptr;
            target_id_ = 0;
            stance_ = Stance::Picketing;
        }
    }

    if (stance_ != Stance::Committed) {
        Track* candidate = store.MostUrgentHostile(obs.position(), now);
        if (candidate && ShouldCommit(*candidate, obs)) {
            target_ = candidate;
            target_id_ = candidate->track_id;
            committed_at_ = now;
            stance_ = Stance::Committed;
        }
    }

    if (stance_ == Stance::Forming) {
        const Vec3 slot = flight::RingSlot(cfg_.drone_id, cfg_.fleet_size, cfg_.asset,
                                           ring_radius_, ring_altitude_);
        if (swarm::Distance(obs.position(), slot) < 8.0f) stance_ = Stance::Picketing;
    }
}

bool Policy::ShouldCommit(const Track& t, const swarm::Observation& obs) const {
    // Committing is spending a drone: the airframe is the weapon. So the bar is
    // evidence, not opportunity.
    //
    // TODO(next): this commits on our own belief alone, which means every drone
    // that can see a hostile commits to it -- three drones per hostile is how
    // naive swarms bleed, and two interceptors converging on one target are
    // also converging on each other. Needs the allocation rule from B3:
    // deterministic tie-break on (target position, drone_id) so two drones
    // reach the same answer without negotiating.
    if (t.belief != Belief::Hostile) return false;
    if (t.closing_score < 1.5f) return false;

    const float range = swarm::Distance(t.position, obs.position());
    if (range > kCommitMaxRange) return false;

    // Do not commit to something already past us and running away: the lateral
    // bound is symmetric, so a stern chase does not converge.
    const float closing = -RangeRate(t.position, t.velocity, obs.position());
    const float ttg = TimeToTarget(t.position, t.velocity, cfg_.asset);
    return closing > -2.0f || ttg < 12.0f;
}

bool Policy::ShouldAbort(const Track& t, const swarm::Observation& obs) const {
    const float now = obs.time();
    if (now - committed_at_ > kAbortAfter) return true;
    if (t.belief != Belief::Hostile) return true;

    // Not converging: range not shrinking after a fair chance to close.
    if (now - committed_at_ > 6.0f) {
        const float closing = -RangeRate(t.position, t.velocity, obs.position());
        if (closing < 1.0f) return true;
    }
    return false;
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
    return flight::RingSlot(cfg_.drone_id, cfg_.fleet_size, cfg_.asset,
                            ring_radius_, ring_altitude_);
}

void Policy::Declare(const swarm::Host& host, const TrackStore& store) const {
    for (const Track& t : store.tracks()) {
        host.DeclareTrack(t.track_id, ToSwClass(t.belief));
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

    // Claim, so two drones do not spend themselves on the same target.
    // Claims EXPIRE and are never acknowledged: death is silent, and a drone
    // that claimed and then died would otherwise hold the target forever.
    if (stance_ == Stance::Committed && target_) {
        Writer w(buffer, sizeof(buffer));
        Header h;
        h.type = MsgType::Claim;
        h.origin = static_cast<uint8_t>(cfg_.drone_id);
        h.seq = next_seq_++;
        h.sent_time = now;
        h.Write(w);

        ClaimMsg m;
        m.target_position = target_->position;
        m.expires_at = now + kClaimHold;
        m.Write(w);

        if (w.ok()) outbox.Push(buffer, w.size(), kPrioClaim, now);
    }

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
