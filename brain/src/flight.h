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
/// A stern chase against an equally capable evader does NOT converge -- both
/// airframes have the same 6.7 m/s^2 lateral bound. Arrive from a geometry that
/// already leads, or do not commit.
Vec3 ProNav(const Vec3& self_position, const Vec3& self_velocity,
            const Vec3& target_position, const Vec3& target_velocity,
            const Config& cfg, float navigation_gain = 3.5f);

/// Applied last, over every other decision.
///
/// The kill radius applies to EVERY pair with no exceptions: friendly-friendly,
/// friendly-civilian, and wreckage.
///
/// Known friendlies (heartbeat-matched tracks) use at least `friendly_margin`,
/// or v_close²/(2a)+4·kill when closing faster than cruise. The closing
/// component of the command is cancelled. Inside 3 kill-radii the intercept
/// is abandoned and we accelerate away. Unknown traffic still gets the small
/// `separation_margin` blend — a tendency, not a guarantee.
///
/// `exempt` is the one track we are deliberately ramming, if any. A mate is
/// never exempt.
Vec3 EnforceSeparation(const Vec3& desired, const Vec3& position, const Vec3& velocity,
                       const FixedVec<Track, kMaxTracks>& tracks,
                       const Config& cfg, const Track* exempt);

/// Keep inside the arena. Leaving it is charged as a wasted loss.
Vec3 EnforceArena(const Vec3& desired, const Vec3& position, const Vec3& velocity,
                  const Config& cfg);

/// Evenly spaced ring slot, derived from drone_id alone. No negotiation, so it
/// works before the radio does and cannot disagree between instances.
Vec3 RingSlot(uint32_t drone_id, uint32_t fleet_size, const Vec3& centre,
              float radius, float altitude);

}  // namespace flight
}  // namespace sw

#endif  // SWARM_FLIGHT_H
