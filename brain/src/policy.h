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
/// `orbit_phase` is the ring's current rotation (kOrbitRate * now), subtracted
/// before quantising so a bearing maps to the slot standing there NOW, not to
/// where that slot sat at t=0. Left out with the ring orbiting, every inbound
/// goes to a drone that has rotated away and the fleet collapses (D66).
uint32_t FacingSlot(const Vec3& position, const Vec3& asset, uint32_t count,
                    float orbit_phase = 0.0f);

/// Tangential orbit rate of the whole picket, rad/s. 0 is the static ring and
/// restores the previous behaviour exactly.
///
/// Every drone computes `kOrbitRate * obs.time()`, a shared function of sim
/// time, so the ring stays phase-locked with nothing on the wire.
///
/// The point is not coverage -- awareness is already 52-58 of 60. It is that a
/// drone handed an inbound is already MOVING toward its intercept and only has
/// to turn that velocity rather than build it. Turning speed v through angle
/// phi costs about phi*v/a against v/a from rest, so it pays only while the
/// turn is under ~1 rad -- which means the drone on the inbound bearing, whose
/// tangential velocity is square across the corridor, is the WORST choice.
/// Orbiting is therefore only worth anything with the handoff below; D23
/// measured rotation while keeping facing-slot ownership and could only ever
/// see it as a cost.
///
/// Cost is centripetal, v^2/R out of the 6.71 m/s^2 lateral budget: at R = 25 m
/// even 4 m/s is 0.64 m/s^2. Small rings punish this much harder than the 90 m
/// ring this was first tried on.
///
/// D70: leftover-Reach vs ω on a just-closed 6-picket ring peaks at ~0.05
/// (handoff takes the approaching neighbour; centripetal is 3% of lat).
/// Measured 0.04 / 0.06 / 0.08 on identity+canary+fa56 after cover slack:
/// 0.06 holds. 0.04 wastes on s1. 0.08 re-opens s2 and fa56.
constexpr float kOrbitRate = 0.06f;

/// Hand an inbound to the drone with the best intercept solution rather than
/// to the one facing it. Off restores the facing-slot rule exactly.
constexpr bool kHandoff = true;

/// Seconds of InterceptScore a challenger must beat the facing incumbent
/// by. The unit is the score itself: `t_go` when both connect, `kNoHit +
/// leftover miss` when they do not.
///
/// Measured D69: 0 / 0.25 / 0.5 / 0.75 / 1.0 / 1.5 / 2.0 on the identity 8
/// plus canary / fa56 / 02e2 (flat), then the same values on 24 fresh ids
/// (8 t1 + 16 t2, one token list). 0.25 is the peak. 0 re-opens a breach
/// D65 named; 1.0 is strictly worse on fresh t2 with no identity gain.
/// Receding-incumbent ties are NOT this tax — they use `kHandoffAspect`.
constexpr float kHandoffMargin = 0.25f;

/// m/s of TowardTarget a challenger must beat a receding incumbent by to
/// take a tied InterceptScore. Heartbeat dead-reckon jitters toward by
/// a few tenths; 0.5 m/s is above that and well below the ~5 m/s gap
/// between a tangent facing drone and its already-closing neighbour (D67).
constexpr float kHandoffAspect = 0.5f;

/// True when the challenger's InterceptScore (lower is better) takes the
/// inbound from the facing incumbent. Score win: must beat by
/// `kHandoffMargin` seconds. Receding incumbent, same t_go bin: also if
/// the challenger is closing faster by `kHandoffAspect`.
bool BeatsIncumbent(float challenger_score, float incumbent_score,
                    float challenger_toward, float incumbent_toward);

/// How well a drone at (position, velocity) can intercept a target, in
/// seconds. LOWER IS BETTER. Two regimes:
///
///   hits  (leftover miss <= kill_radius)  ->  t_go, so the soonest kill wins
///   misses                                ->  kNoHit + leftover miss
///
/// so any drone that connects beats every drone that does not, and among
/// those that connect the earliest wins -- which is what the urgency-scaled
/// reward actually pays for (§9.1).
///
/// The quantity is `Course::miss`: the ZEM the airframe still cannot close
/// after saturating. That matters because acceleration is ANISOTROPIC --
/// LimitAccel caps horizontal at lateral_limit (6.71) and vertical at
/// max_accel (~15-20), so a drone that must descend 20 m is far better placed
/// than one that must translate 20 m, and a score built on range or on turn
/// angle cannot see that. Scoring with the solver the ram will actually fly
/// gets it for free, along with the speed cap and the lead geometry.
float InterceptScore(const Vec3& position, const Vec3& velocity,
                     const Vec3& target_position, const Vec3& target_velocity,
                     const Config& cfg);
constexpr float kNoHit = 1000.0f;

/// Picket radius for the currently live fleet. Radio, spawn, reaction, and
/// chord-preservation caps first; then the largest radius at `altitude`
/// whose unique-owner Voronoi-edge inbound (picket elevation) still has
/// leftover Reach ≥ `kCoverSlack` — same first-sight / Reach model as the
/// kill-envelope bracelet. If no radius meets the slack, fall back to
/// leftover ≥ 0 (D58). Cover may shrink toward the asset cylinder; that is
/// not a collision. Never grows past the caps.
float PicketRadius(const Config& cfg, uint32_t live_count,
                   float altitude = kRayDefaultAlt);

/// Metres of leftover Reach the bracelet inbound must keep. D58 took the
/// largest still-closed R (leftover ≈ 0), which is the knife-edge belt.
/// 5 m is a few kill-radii of margin and does not bind a full 16-picket
/// radio ring (those have ~26 m). Layouts that cannot make 5 m still
/// close at leftover 0 rather than giving up and leaving the radio hole.
constexpr float kCoverSlack = 5.0f;

/// Lowest station height that still keeps the kill-envelope bracelet off
/// the dirt. Two cuts, both the Cover model the overlay draws:
///
///   1. `R · tan(kEnvelopeMinElevDeg)` — the bracelet is not allowed to
///      sit on the horizon cell (0°) of the 0–40° grid.
///   2. `kEnvelopeFloorFrac · Reach(sense/maxv, lat, maxv)` — half the
///      from-rest pancake, using Cover's divert, so the downward lobe is
///      not mostly buried.
///
/// Clamped to `[kRayFloorAlt, kRayDefaultAlt]`. A fitted inbound ray may
/// not pull the ring below this (D68).
constexpr float kEnvelopeMinElevDeg = 10.0f;
constexpr float kEnvelopeFloorFrac = 0.5f;
float PicketFloorAltitude(const Config& cfg, float radius);

/// True if the other interceptor should keep this inbound. Clearly closer
/// (2 m) wins regardless of id; similar range, lower fleet id (D38).
/// `them_id` < 0 means unidentified — only the range rule applies.
bool OtherInterceptorWins(float us_range, uint32_t us_id,
                          float them_range, int them_id);

/// True when a Friendly on the picket ring is the wall this inbound is
/// running onto. Toward can be negative — the hostile flies into them,
/// so they never look like a chaser (`kChasingToward` = 5). Duplicate
/// abort uses this so a sitting interceptor still sends the farther
/// neighbour home, and only when they are at least 12 m closer. Yield
/// still requires a chaser. Mates outside `ring_radius + 10 m` are not
/// walls (s2's outer seer).
bool SittingWall(const Track& craft, const Track& hostile,
                 const Vec3& asset, float ring_radius);

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

/// Bearing drone `id` should hold. Equal 2π / CountLive among RingAlive
/// ids (D56). A fixed point at full strength.
float StationBearing(uint32_t id, uint32_t fleet_size, uint32_t self_id,
                     const float* heard, const Vec3* heard_at,
                     const Vec3& self_pos, float comm_radius, float now,
                     uint8_t* confirmed_dead = nullptr);

/// Velocity of that ring point as the picket orbits: tangential, |v| = wR.
/// Zero when kOrbitRate is zero. Fed to GoTo so station keeping damps toward
/// the station's own motion instead of braking against it.
Vec3 StationVelocityAt(float bearing, float radius);

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

/// True if this mate has already stepped off a→b (fully clear, or peeling
/// away faster than `kYieldAway`). First yielder wins; same-tick both
/// still on the line both yield (D59, ACK later).
bool MateAlreadyYielded(const Vec3& pos, const Vec3& vel,
                        const Vec3& a, const Vec3& b, float clear);

/// Yield for a mate's remaining intercept, unless they have already
/// yielded. Null mate pre-clears the owner slot, except when we ourselves
/// are already flying at the inbound.
Vec3 YieldForMate(const Vec3& goal, const Vec3& self_p, const Vec3& self_v,
                  const Vec3& owner_slot, const Vec3& hostile_p,
                  const Vec3& hostile_v, const Vec3* mate_p, const Vec3* mate_v,
                  float clear);

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
    /// Velocity of the goal point when the goal is the orbiting station; zero
    /// when the goal is something else (a stalk aim) or the ring is static.
    /// Feed-forward for GoTo, not a command in its own right.
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
    /// Heartbeats already carry velocity for dead reckoning; it was parsed and
    /// discarded. Keeping it costs nothing on the wire and is what lets every
    /// drone score every peer without a Claim frame (D66).
    void NoteAlive(uint8_t drone_id, const Vec3& position, const Vec3& velocity,
                   float now);

    /// Record our own pose as broadcast. Every drone then scores every
    /// candidate -- ourselves included -- from the same last-broadcast poses,
    /// so all of them reach the SAME owner. Scoring self from its true pose
    /// and peers from stale beats is what makes two drones disagree and both
    /// commit (D66).
    void NoteSelfBroadcast(const Vec3& position, const Vec3& velocity, float now);

    /// Ring owner before any handoff: facing slot on the live ring, stepped
    /// one clockwise if that drone is receding (D35). The incumbent.
    uint32_t FacingOwner(const Track& t, const TrackStore& store,
                         const swarm::Observation& obs) const;

    /// Live drone with the best InterceptScore for this track. Equal t_go
    /// prefers the one already moving toward the inbound; remaining ties
    /// go to the lower id so two observers name the same drone.
    uint32_t BestInterceptor(const Track& t, const swarm::Observation& obs) const;

    /// InterceptScore for one id from its last broadcast pose, dead reckoned.
    /// kNoHit*2 if that id has no pose to score.
    float ScoreFor(uint32_t id, const Track& t, const swarm::Observation& obs) const;

    /// Horizontal closing of that id's last broadcast velocity onto `t`.
    float TowardFor(uint32_t id, const Track& t, const swarm::Observation& obs) const;

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
    char last_log_[384]{};

    uint16_t next_seq_ = 0;
    float last_heartbeat_ = -1.0e9f;
    float ring_radius_ = 60.0f;  // overwritten in Reset from PicketRadius(cfg)
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
