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

/// Proportional navigation. The natural law for an intercept: it nulls the
/// line-of-sight rate rather than chasing the target's current position, which
/// is why it beats pursuit against anything moving.
///
/// Time until we and a constant-velocity target can occupy the same point, if
/// we fly at `speed`. Closed form; -1 when no positive root exists, i.e. the
/// target outruns us and is opening.
float TimeToIntercept(const Vec3& to_target, const Vec3& target_velocity,
                      float speed);

/// Point on the inbound's ground track we actually fly at (D39).
/// The closed-form meeting, pushed `lead` metres further in front of
/// them along their velocity. Early → we get to the chord first and they
/// fly into us; late → we are still ahead of current position, not abeam.
/// No radius-sized offset: midcourse aims for the predicted hostile origin.
constexpr float kBarrierLeadKills = 0.0f;
Vec3 BarrierAim(const Vec3& self, const Vec3& target_p, const Vec3& target_v,
                float speed, float lead);

/// A stern chase against an equally capable evader does NOT converge -- both
/// airframes have the same 6.7 m/s^2 lateral bound. Arrive from a geometry that
/// already leads, or do not commit.
///
/// Two laws, blended by range (D22): a closed-form lead intercept flown at
/// max_speed while there is still range to cover, handing over to a
/// zero-effort-miss law for the last 25-70 m. The aim point is the predicted
/// hostile origin on their ground track, so the friendly and hostile origins
/// are what the guidance tries to intersect.
/// Pure PN commanded almost nothing at a picket already on the inbound
/// bearing, so the drone sat still and was rammed at our own ring radius.
///
/// `navigation_gain` is the terminal gain, now on a zero-effort-miss law
/// rather than classic PN (D25): a = N * ZEM / t_go^2. 10 measured -- escapes
/// fall monotonically from N=3 to N=10 and the fixed-set score is flat, while
/// N=14 starts costing kills.
Vec3 ProNav(const Vec3& self_position, const Vec3& self_velocity,
            const Vec3& target_position, const Vec3& target_velocity,
            const Config& cfg, float navigation_gain = 10.0f);

/// Applied last, over every other decision.
///
/// The kill radius applies to EVERY pair with no exceptions: friendly-friendly,
/// friendly-civilian, and wreckage.
///
/// Known friendlies (heartbeat-matched tracks) use at least `friendly_margin`,
/// or v_close²/(2a)+4·kill when closing faster than cruise. A *picket*
/// cancels the closing component of its command against a mate, and panics
/// inside 3 kill-radii. An *interceptor* (`intercepting`) does not cancel:
/// that was ProNav being overwritten by a picket on the line of sight (D15).
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

/// Keep inside the arena. Leaving it is charged as a wasted loss.
Vec3 EnforceArena(const Vec3& desired, const Vec3& position, const Vec3& velocity,
                  const Config& cfg);

/// Evenly spaced ring station. `index` is the rank among `count` stations
/// (drone_id on a full fleet; live rank after a death, D19). No negotiation.
Vec3 RingSlot(uint32_t index, uint32_t count, const Vec3& centre,
              float radius, float altitude);

}  // namespace flight
}  // namespace sw

#endif  // SWARM_FLIGHT_H
