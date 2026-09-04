// policy.h -- what to do about what we believe.
//
// This is where judgement lives: whether to commit a drone, what to say and how
// often, what to declare. Deliberately separate from belief (which decides what
// things ARE) and from flight (which decides how to move). A new requirement
// should land in exactly one of the three.
#ifndef SWARM_POLICY_H
#define SWARM_POLICY_H

#include "belief.h"
#include "mode.h"
#include "protocol.h"

namespace sw {

/// Facing ring slot for a world position. Same angle convention as RingSlot.
/// `count` is the number of stations (live fleet after D19, original size
/// on a full ring). Near a slot bisector the clockwise id wins, so
/// two observers with a metre of track noise cannot both own the inbound.
/// `orbit_phase` is the ring's current rotation (kOrbitRate * now). It is
/// subtracted before quantising, so a bearing maps to the slot standing
/// there NOW rather than to where that slot sat at t=0. Getting this wrong
/// hands every inbound to a drone that has orbited away (D57).
uint32_t FacingSlot(const Vec3& position, const Vec3& asset, uint32_t count,
                    float orbit_phase = 0.0f);

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
/// Never-heard is alive (boot). Heard-then-silent is dead, latched until
/// a heartbeat (D56). Heartbeats hop, so the far side of the ring sees
/// the same deaths; "out of radio" no longer keeps a ghost interceptor.
/// `confirmed_dead` is that latch; null skips the latch (const callers).
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

/// Tangential orbit rate of the whole picket, rad/s. Zero is the static ring.
///
/// Every drone computes `kOrbitRate * obs.time()`, which is a shared function
/// of sim time, so the ring stays phase-locked with nothing on the wire.
///
/// The orbit exists so that a drone handed an inbound is already MOVING toward
/// its intercept, and only has to turn that velocity rather than build it:
/// turning speed v through angle φ costs about φ·v/a seconds against v/a to
/// accelerate from rest, so it pays only while the turn is under ~1 rad. That
/// bound is what sizes the handoff (D57), and it is why D23 -- which kept
/// facing-slot ownership -- measured the same rotation as a pure cost.
///
/// Cost is centripetal: v_t²/R of the 6.71 m/s² lateral budget, so the rate
/// has to stay small. At R≈90 m, 0.10 rad/s is 9 m/s and 0.9 m/s² (13%).
constexpr float kOrbitRate = 0.0f;

/// Hand an inbound to the drone with the cheapest InterceptCost rather than
/// to the one facing it. Off restores the facing-slot rule exactly.
///
/// This is the half D23 never had. Rotation alone keeps giving each inbound
/// to the drone whose tangential velocity is most nearly perpendicular to the
/// intercept -- the worst drone on the ring -- so it can only ever measure
/// the orbit as a cost. Scored on velocity, the orbit hands off backwards
/// along its own direction of travel, which is the point of orbiting at all.
constexpr bool kHandoff = true;

/// Seconds of InterceptCost a challenger must beat the incumbent by before
/// ownership moves. The facing-slot drone stays the incumbent, so this is a
/// pure override on the proven rule rather than a replacement for it.
///
/// It has to be "much better", not "better": with a bare comparison two
/// near-equal candidates trade the track every time a heartbeat lands, and
/// x4-cbeb2340 lost 13 airframes to exactly that thrash (D59).
constexpr float kHandoffMargin = 1.0f;

/// Bearing drone `id` should hold. Equal 2π / CountLive among RingAlive
/// ids (D56), plus the shared orbit phase. A fixed point at full strength
/// only when kOrbitRate is zero.
float StationBearing(uint32_t id, uint32_t fleet_size, uint32_t self_id,
                     const float* heard, const Vec3* heard_at,
                     const Vec3& self_pos, float comm_radius, float now,
                     uint8_t* confirmed_dead = nullptr);

/// Ring point at a bearing, rather than at a slot index. NED, so -altitude.
Vec3 StationAt(float bearing, const Vec3& centre, float radius, float altitude);

/// Velocity of that ring point as the picket orbits: tangential, |v| = ωR.
/// Zero when kOrbitRate is zero. Fed to GoTo so station keeping damps toward
/// the station's own motion instead of braking against it every tick.
Vec3 StationVelocityAt(float bearing, float radius);

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

/// Seconds for a drone at (position, velocity) to reach `aim`: turn what it
/// already has, then fly the rest. Turning speed v through angle φ costs
/// about φ·v/lateral_limit; building speed from rest costs v/max_speed. So a
/// drone already moving toward the aim is cheap and one moving across it is
/// not, which is the whole reason an orbiting picket has a best drone that is
/// NOT the one facing the inbound (D59).
///
/// At |v| = 0 the turn term vanishes and this is pure range, so a static ring
/// scores exactly as the facing-slot rule always did.
float InterceptCost(const Vec3& position, const Vec3& velocity, const Vec3& aim,
                    const Config& cfg);

/// Horizontal component of `velocity` toward `target`. Negative is receding.
float TowardTarget(const Vec3& position, const Vec3& velocity, const Vec3& target);

/// End of the remaining intercept flight, not the hostile's current pose.
/// Assumed cruise along LOS, t_meet = range / closing, along = cruise ·
/// min(t_meet + 0.5 s, abort 12 s). Returns `from` when that cruise is not
/// closing — there is no intercept to yield for (D17).
Vec3 CorridorHorizon(const Vec3& from, const Vec3& hostile_p, const Vec3& hostile_v);

/// Seconds of ground-track-through-cylinder before an owner leaves station
/// at an unidentified inbound. Full Hostile latch is still 0.6 s of aimed
/// geometry; this only starts the intercept flight (D44).
constexpr float kScrambleEvidence = 0.1f;

/// True if first sight was at or outside the picket ring (1 m slop).
bool BornOutsideRing(const Vec3& first, const Vec3& asset, float ring_radius);

/// How far a picket may leave its slot toward a not-yet-Hostile inbound.
/// Stopping distance at max_speed is ~30 m, so 40 m still reverses onto
/// station if Classify never latches.
constexpr float kStalkRange = 40.0f;

/// Target pose shifted `lead` metres along its track, then capped at `cap`
/// from `slot` so a picket can still reverse home.
Vec3 StalkAim(const Vec3& slot, const Vec3& target_p, const Vec3& target_v,
              float cap, float lead);

/// Push `goal` off the horizontal segment a→b when inside `clear`.
Vec3 YieldOffCorridor(const Vec3& goal, const Vec3& a, const Vec3& b, float clear);

class Policy {
public:
    void Configure(const Config& cfg, Rng rng);

    /// Decide the mode and target for this tick.
    void Decide(TrackStore& store, const swarm::Observation& obs);

    /// Where station-keeping should take us, given the mode.
    Vec3 DesiredPosition(const swarm::Observation& obs) const;

    Mode mode() const { return mode_; }
    const Track* target() const { return target_; }
    /// Watch, stalk, or intercept track — the one Fly should look at.
    const Track* focus() const;
    /// Not-yet-Hostile inbound we are already closing on. Null on station.
    const Track* stalk() const { return stalk_; }
    /// Inbound we own and are facing while we ID it. Null if none. Yaw only.
    const Track* watch() const { return watch_; }
    bool leashed() const { return leashed_; }
    Vec3 station() const { return station_; }
    /// Velocity of the goal point when the goal is the orbiting station;
    /// zero whenever the goal is something else (a stalk aim) or the ring
    /// is static. Feed-forward for GoTo, not a command in its own right.
    Vec3 goal_velocity() const { return goal_vel_; }
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
    void NoteAlive(uint8_t drone_id, const Vec3& position, float now) {
        NoteAlive(drone_id, position, Vec3(), now);
    }
    /// Heartbeats already carry velocity for dead reckoning; keeping it costs
    /// nothing on the wire and is what lets every drone score every peer's
    /// intercept without a Claim frame (D59).
    void NoteAlive(uint8_t drone_id, const Vec3& position, const Vec3& velocity,
                   float now);

    /// Live drone with the cheapest InterceptCost to this track. Ties go to
    /// the lower id so two observers name the same drone. Peers are scored
    /// from their last heartbeat, dead reckoned to now.
    uint32_t BestInterceptor(const Track& t, const swarm::Observation& obs) const;

    /// InterceptCost for one drone id, scored from its last heartbeat (own
    /// true pose for self). Large if that id has no pose to score.
    float CostFor(uint32_t id, const Track& t, const swarm::Observation& obs) const;

    /// A hopped inbound-ray fit. Weight is discounted by hops so a far
    /// rumour cannot overwrite a local cone (D52).
    void NoteRay(float h0, float slope, float weight, uint8_t hops);

    const InboundRay& inbound_ray() const { return ray_; }

    /// Nearby-death latch transitions: `gone id=` / `live id=` (D37). Separate
    /// from last_log_ so a commit on the same tick is not overwritten.
    void LogRing(const swarm::Host& host);

    /// `state` verb on mode change. Separate from last_log_ so a commit on
    /// the same tick is not overwritten (D46).
    void LogMode(const swarm::Host& host);

private:
    bool ShouldCommit(const Track& t, const TrackStore& store,
                      const swarm::Observation& obs) const;
    bool ShouldScramble(const Track& t, const TrackStore& store,
                        const swarm::Observation& obs) const;
    bool OwnsInbound(const Track& t, const TrackStore& store,
                     const swarm::Observation& obs) const;
    bool OwnsAHostile(const TrackStore& store,
                    const swarm::Observation& obs) const;
    bool EnteredFromOutside(const Track& t) const;
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
    void AssignStationMode(const swarm::Observation& obs);
    void ObserveInbounds(const TrackStore& store, float dt);
    void ApplyRayAltitude();

    Config cfg_;
    Rng rng_;                 // unused today; the hook for jittering send times

    Mode mode_ = Mode::Forming;
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
    float ring_altitude_ = kRayDefaultAlt;
    InboundRay ray_;
    float last_ray_send_ = -1.0e9f;
    bool sampled_local_ = false;
    Vec3 picket_goal_{};
    Vec3 station_{};
    Vec3 goal_vel_{};
    const Track* stalk_ = nullptr;
    const Track* watch_ = nullptr;
    bool leashed_ = false;
    bool announced_picket_ = false;
    bool mode_logged_ = false;
    Mode logged_mode_ = Mode::Forming;
    float heard_[kMaxFleet]{};
    Vec3 heard_at_[kMaxFleet]{};
    Vec3 heard_vel_[kMaxFleet]{};
    uint8_t confirmed_dead_[kMaxFleet]{};
    uint8_t announced_dead_[kMaxFleet]{};
};

}  // namespace sw

#endif  // SWARM_POLICY_H
