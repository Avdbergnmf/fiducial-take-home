// policy.h -- what to do about what we believe.
//
// This is where judgement lives: whether to commit a drone, what to say and how
// often, what to declare. Deliberately separate from belief (which decides what
// things ARE) and from flight (which decides how to move). A new requirement
// should land in exactly one of the three.
#ifndef SWARM_POLICY_H
#define SWARM_POLICY_H

#include "belief.h"
#include "protocol.h"

namespace sw {

enum class Stance : uint8_t {
    Forming,      // take up a ring slot
    Picketing,    // hold the ring, watch
    Committed,    // running an intercept on a specific track
};

/// Facing ring slot for a world position. Same angle convention as RingSlot.
/// `count` is the number of stations (live fleet after D19, original size
/// on a full ring). Near a slot bisector the clockwise id wins, so
/// two observers with a metre of track noise cannot both own the inbound.
uint32_t FacingSlot(const Vec3& position, const Vec3& asset, uint32_t count);

/// Picket radius for the currently live fleet. The live ring preserves the
/// full-fleet station chord and is bounded by sensing, spawn, reaction, and
/// asset standoff constraints.
float PicketRadius(const Config& cfg, uint32_t live_count);

/// True if the other interceptor should keep this inbound. Clearly closer
/// (2 m) wins regardless of id; similar range, lower fleet id (D38).
/// `them_id` < 0 means unidentified — only the range rule applies.
bool OtherInterceptorWins(float us_range, uint32_t us_id,
                          float them_range, int them_id);

/// True if this id is still treated as on station. Self is always alive.
/// Never-heard is assumed alive so we do not steal sectors before the first
/// heartbeat. 1.5 s of silence (three missed 2 Hz beats) is death.
bool SlotAlive(uint32_t drone_id, uint32_t self_id, const float* heard, float now);

/// True if this id still holds a ring station. Self is always alive.
/// Never-heard is alive (boot). Silence is a nearby death when we are still
/// in radio of the last heartbeat pose (D19/D30). That death *latches*:
/// leaving the stale bubble does not resurrect them — that reverse was the
/// ping-pong (D37). An interceptor who went silent while far was never
/// latched, so their station stays. A heartbeat clears the latch.
/// `confirmed_dead` is that latch; null keeps the pure D30 rule (tests).
bool RingAlive(uint32_t drone_id, uint32_t self_id, const float* heard,
               const Vec3* heard_at, const Vec3& self_pos, float comm_radius,
               float now, uint32_t fleet_size = 0,
               uint8_t* confirmed_dead = nullptr);

/// Live drones in id order under RingAlive. At least 1 (self).
uint32_t CountLive(uint32_t fleet_size, uint32_t self_id, const float* heard,
                   const Vec3* heard_at, const Vec3& self_pos, float comm_radius,
                   float now, uint8_t* confirmed_dead = nullptr);

/// Rank of `drone_id` among RingAlive ids (0 .. CountLive-1).
uint32_t LiveRank(uint32_t drone_id, uint32_t fleet_size, uint32_t self_id,
                  const float* heard, const Vec3* heard_at, const Vec3& self_pos,
                  float comm_radius, float now,
                  uint8_t* confirmed_dead = nullptr);

/// The `rank`-th RingAlive id. `rank` wraps CountLive.
uint32_t LiveId(uint32_t rank, uint32_t fleet_size, uint32_t self_id,
                const float* heard, const Vec3* heard_at, const Vec3& self_pos,
                float comm_radius, float now,
                uint8_t* confirmed_dead = nullptr);

/// Bearing drone `id` should hold, bisecting the gap between the nearest
/// drones either side of it that WE still believe are flying (D21). A fixed
/// point at full strength; slides toward a hole as neighbours fall silent.
float StationBearing(uint32_t id, uint32_t fleet_size, uint32_t self_id,
                     const float* heard, const Vec3* heard_at,
                     const Vec3& self_pos, float comm_radius, float now,
                     uint8_t* confirmed_dead = nullptr);

/// Ring point at a bearing, rather than at a slot index. NED, so -altitude.
Vec3 StationAt(float bearing, const Vec3& centre, float radius, float altitude);

/// First live drone clockwise from `facing` on the *original* id ring,
/// including facing. Kept so the G4 tests still pin the old walk. Allocation
/// and stations use the live ring (D19).
uint32_t UniqueOwner(uint32_t facing, uint32_t fleet_size, uint32_t self_id,
                     const float* heard, float now);

/// Same walk as UniqueOwner, but skip `facing` when that drone is receding
/// from the inbound (D35). Clockwise neighbour takes it. One skip only —
/// walking every receding slot would leave nobody.
uint32_t InboundOwner(uint32_t facing, uint32_t fleet_size, uint32_t self_id,
                      const float* heard, float now, bool facing_receding);

/// Horizontal component of `velocity` toward `target`. Negative is receding.
float TowardTarget(const Vec3& position, const Vec3& velocity, const Vec3& target);

/// End of the remaining intercept flight, not the hostile's current pose.
/// Assumed cruise along LOS, t_meet = range / closing, along = cruise ·
/// min(t_meet + 0.5 s, abort 12 s). Returns `from` when that cruise is not
/// closing — there is no intercept to yield for (D17).
Vec3 CorridorHorizon(const Vec3& from, const Vec3& hostile_p, const Vec3& hostile_v);

/// How far a picket may leave its slot toward a not-yet-Hostile inbound.
/// Stopping distance at max_speed is ~30 m, so 40 m still reverses onto
/// station if Classify never latches.
constexpr float kStalkRange = 40.0f;

/// Lead point of the committed intercept, leashed `cap` metres from `slot`.
/// Same `BarrierAim` ProNav midcourse flies (D34/D39), not the LOS to
/// where the inbound is now. `lead` is metres in front of them on their
/// track; default is 8 m (8·kill on s1).
Vec3 StalkAim(const Vec3& slot, const Vec3& target_p, const Vec3& target_v,
              float speed, float cap, float lead = 8.0f);

/// Push `goal` off the horizontal segment a→b when inside `clear`.
Vec3 YieldOffCorridor(const Vec3& goal, const Vec3& a, const Vec3& b, float clear);

class Policy {
public:
    void Configure(const Config& cfg, Rng rng);

    /// Decide the stance and target for this tick.
    void Decide(TrackStore& store, const swarm::Observation& obs);

    /// Where flight should take us, given the stance.
    Vec3 DesiredPosition(const swarm::Observation& obs) const;

    Stance stance() const { return stance_; }
    const Track* target() const { return target_; }
    /// Not-yet-Hostile inbound we are already flying an intercept at, still
    /// Picketing. Null when holding station. Fly uses ProNav on this with a
    /// leash at `station()`; not an exempt intercept target.
    const Track* stalk() const { return stalk_; }
    Vec3 station() const { return station_; }
    const char* last_log() const { return last_log_; }
    float ring_radius() const { return ring_radius_; }
    float ring_altitude() const { return ring_altitude_; }

    /// Report what we currently believe. Scored once a second on the MOST
    /// RECENT declaration per aircraft: correct +1, wrong -2, unknown 0.
    /// Local tracks only (D9). Friendly and Hostile; everything else UNKNOWN.
    void Declare(const swarm::Host& host, const TrackStore& store) const;

    /// Queue whatever is worth saying this tick. One broadcast per tick, so
    /// this only queues -- Pump() does the sending. Non-const because it
    /// records when each track was last reported, which is how the rate limit
    /// survives across ticks.
    void Compose(Outbox<24>& outbox, TrackStore& store,
                 const swarm::Observation& obs);

    /// Send at most one frame, respecting the remaining byte budget.
    void Pump(const swarm::Host& host, Outbox<24>& outbox, const swarm::Observation& obs);

    /// A heartbeat from this origin, with the claimed pose.
    void NoteAlive(uint8_t drone_id, const Vec3& position, float now);

    /// Nearby-death latch transitions: `gone id=` / `live id=` (D37). Separate
    /// from last_log_ so a commit on the same tick is not overwritten.
    void LogRing(const swarm::Host& host);

private:
    bool ShouldCommit(const Track& t, const TrackStore& store,
                      const swarm::Observation& obs) const;
    bool OwnsInbound(const Track& t, const TrackStore& store,
                     const swarm::Observation& obs) const;
    bool FacingReceding(uint32_t facing, const Track& hostile,
                        const TrackStore& store,
                        const swarm::Observation& obs) const;
    bool CloserChaser(const Track& hostile, const TrackStore& store,
                      const swarm::Observation& obs) const;
    Vec3 PicketGoal(const TrackStore& store, const swarm::Observation& obs);
    const char* AbortReason(const Track& t, const swarm::Observation& obs) const;
    void LogAbort(const char* why, uint32_t trk, const Track* t,
                  const swarm::Observation& obs);
    Track* ResolveTarget(TrackStore& store);
    void BindTarget(Track* t);
    int MateId(const Track& mate) const;

    Config cfg_;
    Rng rng_;                 // unused today; the hook for jittering send times

    Stance stance_ = Stance::Forming;
    Track* target_ = nullptr;
    uint32_t target_id_ = 0;
    uint32_t target_store_id_ = 0;
    Vec3 last_target_pos_{};
    float committed_at_ = 0.0f;
    uint32_t last_abort_id_ = 0;
    uint32_t last_abort_store_id_ = 0;
    float last_abort_at_ = -1.0e9f;
    char last_log_[256]{};

    uint16_t next_seq_ = 0;
    float last_heartbeat_ = -1.0e9f;
    float ring_radius_ = 60.0f;
    float ring_altitude_ = 30.0f;
    Vec3 picket_goal_{};
    Vec3 station_{};
    const Track* stalk_ = nullptr;
    float heard_[kMaxFleet]{};
    Vec3 heard_at_[kMaxFleet]{};
    uint8_t confirmed_dead_[kMaxFleet]{};
    uint8_t announced_dead_[kMaxFleet]{};
};

}  // namespace sw

#endif  // SWARM_POLICY_H
