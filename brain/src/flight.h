// flight.h -- guidance. Pure functions where possible, no state, no opinions
// about what anything is. Give it a goal, get an acceleration.
//
// All accelerations are inertial NED and go out as SW_CMD_ACCEL_NED. Gravity is
// compensated by the simulator's inner loop, so commanding zero holds velocity.
#ifndef SWARM_FLIGHT_H
#define SWARM_FLIGHT_H

#include "mode.h"
#include "world.h"

namespace sw {
namespace flight {

/// Split the horizontal and vertical components against their real limits.
/// Clamping the 3-vector against max_accel is wrong: horizontal authority is
/// g*tan(max_tilt), roughly 6.7 against a max_accel of 15, and a manoeuvre
/// sized on the wrong one silently under-delivers exactly when it matters.
Vec3 LimitAccel(const Vec3& desired, const Config& cfg);

/// PD station keeping. The workhorse: formation, waypoints and loiter are all
/// this with a different target.
Vec3 GoTo(const Vec3& target, const Vec3& position, const Vec3& velocity,
          const Config& cfg, float pos_gain = 0.8f, float vel_gain = 1.6f);

/// Travel toward a point at a commanded speed, decelerating into it.
Vec3 Cruise(const Vec3& target, const Vec3& position, const Vec3& velocity,
            float cruise_speed, const Config& cfg);

/// Shift a pose `distance` metres along its velocity. Zero if it is parked.
Vec3 AimAhead(const Vec3& position, const Vec3& velocity, float distance);

/// Seconds until `p` is inside `radius` of `q`, using relative closing.
/// 0 if already inside; large if not approaching.
float TimeToClose(const Vec3& p, const Vec3& v, const Vec3& q, const Vec3& w,
                  float radius);

/// Finite-difference acceleration, clamped to `cap` so a first-sight jump
/// cannot look like a 100 m/s² weave.
Vec3 EstimatedAccel(const Vec3& velocity, const Vec3& last_velocity,
                    float dt, float cap);

/// Proportional navigation, Zarchan 3-D ZEM form (D42 / D43).
///
/// Used for Stalking (leashed, short). Scramble / ram use CollisionCourse
/// (D49): PN's t_go = range/closing is "when they reach us if we sit", and
/// LimitAccel then dumps the z-budget on that body while xy saturates.
constexpr float kPnGain = 6.0f;
constexpr float kPnLeadKillRadii = 0.5f;
Vec3 ProNav(const Vec3& self_position, const Vec3& self_velocity,
            const Vec3& target_position, const Vec3& target_velocity,
            const Config& cfg,
            const Vec3& target_accel = {},
            float lead_kill_radii = kPnLeadKillRadii,
            float navigation_gain = kPnGain);

/// Time to establish max tilt at published max_body_rate. 0 if rate is
/// unpublished (tests). s1: 0.6 rad / 8 rad/s ≈ 75 ms, not a 0.4 s lag.
float TiltSettle(const Config& cfg);

/// Specific force (body FRD, includes gravity reaction) → inertial NED.
Vec3 InertialAccel(const swarm::Quat& attitude, const Vec3& specific_force);

/// Rotate / scale horizontal accel toward `to` at max_body_rate.
/// z is thrust and is copied from `to`. Identity when rate is unpublished.
Vec3 SlewHorizontal(const Vec3& from, const Vec3& to, float dt, const Config& cfg);

/// Vector ZEM intercept for scramble / ram (D49 / D62).
///
/// Full 3-D ZEM, not ZEMn + along-LOS. t_go is a held intercept clock,
/// not a fresh earliest-bin every tick (bin jumps rotated xy 20–40° and
/// the attitude loop never settled). N=2 is a = 2 ZEM / t². Saturates
/// with LimitAccel (5.4 cylinder: leftover z does not steal xy, D51).
/// Do not add g. Do not add tilt settle onto t_go (that softens the
/// command; D49 / D61). `prefer_t` is last t_go minus dt; keep it while
/// it still hits.
struct Course {
    Vec3 accel;
    Vec3 meeting;
    float t_go = 0.0f;
};
Course SolveCollisionCourse(const Vec3& self_p, const Vec3& self_v,
                            const Vec3& tgt_p, const Vec3& tgt_v,
                            const Config& cfg,
                            const Vec3& tgt_a = {},
                            const Vec3& self_a = {},
                            float prefer_t = 0.0f);
Vec3 CollisionCourse(const Vec3& self_p, const Vec3& self_v,
                     const Vec3& tgt_p, const Vec3& tgt_v,
                     const Config& cfg,
                     const Vec3& tgt_a = {},
                     const Vec3& self_a = {},
                     float prefer_t = 0.0f);

/// True if a ram is still possible. Inside 2·kill we stay in the merge.
/// Past CPA and outside that bubble, or a leftover miss `Reach` (½ a t²
/// capped by max_speed, LimitAccel along the miss) cannot close, is
/// `abort uncatchable` on Ramming only (D48).
bool CatchableRam(const Vec3& self_p, const Vec3& self_v,
                  const Vec3& tgt_p, const Vec3& tgt_v, const Config& cfg);

/// Accel for the named mode. Constraints (separation, arena) are applied
/// after this, in Fly.
Vec3 DesiredAccel(Mode mode, const Vec3& position, const Vec3& velocity,
                  const Vec3& goal, const Track* focus, bool leashed,
                  float dt, const Config& cfg, const Vec3& self_a = {});

/// Yaw for the named mode. Outward on station, at the watch target, or
/// along velocity when intercepting / stalking.
float DesiredYaw(Mode mode, const Vec3& position, const Vec3& velocity,
                 const Vec3& asset, const Track* focus);

/// Applied last, over every other decision.
///
/// The kill radius applies to EVERY pair with no exceptions: friendly-friendly,
/// friendly-civilian, and wreckage.
///
/// Known friendlies (heartbeat-matched tracks) use at least `friendly_margin`,
/// or v_close²/(2a)+4·kill when closing faster than cruise. A *picket*
/// cancels the closing component of its command against a mate, and panics
/// inside 3 kill-radii. An *interceptor* (`intercepting`) does not cancel:
/// that would overwrite its guidance near the target (D15).
/// If the ram on `exempt` is at the same time as a mate collision, or
/// first, the mate is ignored — braking then misses the hostile (D38).
/// A picket still in front of the intercept is traffic and is still avoided.
///
/// Unknown / civilian / wreckage use the same arrest distance when closing
/// (D12). The 4 m `separation_margin` is the floor for tracks that are not
/// closing — a static 19 m around every unknown collapses the ring (D8).
/// The intercept target is still a blend, not a cancel: we have to be
/// allowed to ram it. `exempt` is that track. A mate is never exempt unless
/// D38 says the hostile ram comes first.
Vec3 EnforceSeparation(const Vec3& desired, const Vec3& position, const Vec3& velocity,
                       const FixedVec<Track, kMaxTracks>& tracks,
                       const Config& cfg, const Track* exempt,
                       bool intercepting = false);

/// Keep inside the arena. Leaving it is a wasted loss (CHALLENGE.md 9.2).
///
/// The band is stopping distance to the actual wall, not a fixed halo.
/// Vertical uses `max_accel`.
///
/// `ground_only` keeps the floor and drops the walls and ceiling. That is the
/// intercepting case: D48 is right that a ram which can still hit must not be
/// steered around a wall, but the ground is not a wall. Clipping a wall costs
/// an arena exit only if we actually leave; the dirt ends the drone outright,
/// and no hostile is ever below it -- so a ram diving into the floor is one
/// whose geometry has already failed, and pulling it up forfeits no intercept
/// (D64).
Vec3 EnforceArena(const Vec3& desired, const Vec3& position, const Vec3& velocity,
                  const Config& cfg, bool ground_only = false);

/// Evenly spaced ring station. `index` is the rank among `count` stations
/// (drone_id on a full fleet; live rank after a death, D19). No negotiation.
Vec3 RingSlot(uint32_t index, uint32_t count, const Vec3& centre,
              float radius, float altitude);

}  // namespace flight
}  // namespace sw

#endif  // SWARM_FLIGHT_H
