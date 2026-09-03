// flight.h -- guidance. Pure functions where possible, no state, no opinions
// about what anything is. Give it a goal, get an acceleration.
//
// All accelerations are inertial NED and go out as SW_CMD_ACCEL_NED. Gravity is
// compensated by the simulator's inner loop, so commanding zero holds velocity.
#ifndef SWARM_FLIGHT_H
#define SWARM_FLIGHT_H

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
/// PN and ZEM are the same law: a = N · ZEMn / t_go² is identical to
/// a = N · Vc · ω under constant closing. Augmented ZEM adds ½ At t_go²
/// so a weave that has already started is in the predicted miss.
///
/// Lead is on the hostile's predicted trajectory, not on our intercept
/// path: pretend they have already travelled `lead · kill_radius` metres
/// along that track (velocity, plus the weave accel), then intercept that
/// virtual state. A picket starts at rest, so the same command also
/// accelerates along the LOS until we are flying at them.
constexpr float kPnGain = 6.0f;
constexpr float kPnLeadKillRadii = 0.5f;
Vec3 ProNav(const Vec3& self_position, const Vec3& self_velocity,
            const Vec3& target_position, const Vec3& target_velocity,
            const Config& cfg,
            const Vec3& target_accel = {},
            float lead_kill_radii = kPnLeadKillRadii,
            float navigation_gain = kPnGain);

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

/// Keep inside the arena. Leaving it is a wasted loss.
///
/// The band is stopping distance to the actual wall, not a fixed 20 m
/// halo. Vertical uses `max_accel` (the real bound). A 20 m ground buffer
/// braked interceptors at 15 m while the hostile dived under them (D45).
Vec3 EnforceArena(const Vec3& desired, const Vec3& position, const Vec3& velocity,
                  const Config& cfg);

/// Evenly spaced ring station. `index` is the rank among `count` stations
/// (drone_id on a full fleet; live rank after a death, D19). No negotiation.
Vec3 RingSlot(uint32_t index, uint32_t count, const Vec3& centre,
              float radius, float altitude);

}  // namespace flight
}  // namespace sw

#endif  // SWARM_FLIGHT_H
