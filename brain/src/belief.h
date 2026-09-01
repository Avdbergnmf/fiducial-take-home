// belief.h -- what is out there and what we think it is.
//
// Knows nothing about flight or the radio. It consumes observations and peer
// reports and produces classified tracks. Everything that decides WHAT an
// aircraft is lives here and nowhere else.
#ifndef SWARM_BELIEF_H
#define SWARM_BELIEF_H

#include "world.h"

namespace sw {

class TrackStore {
public:
    void Configure(const Config& cfg) { cfg_ = cfg; }

    /// Fold this tick's sensor picture in. Tracks not seen for a while are
    /// dropped: a destroyed entity vanishes from the list with no notification,
    /// so absence is all the evidence you ever get.
    void Update(const swarm::Observation& obs);

    /// Fold in a peer's report. Associates by geometry and time, because
    /// track_id is observer-local and means nothing here.
    void MergePeerReport(const Vec3& position, const Vec3& velocity,
                         Belief peer_belief, uint8_t confidence, float now);

    /// A heartbeat that matches a sensor track is a mate. The RF range is
    /// our measurement, not their claim — a replay from the wrong side of
    /// the arena fails that check (tier 3). Until then, origin + geometry
    /// is enough to stop us ramming our own fleet.
    void MarkFriendly(const Vec3& claimed, const Vec3& self,
                      float measured_range, float range_sigma, float now);

    FixedVec<Track, kMaxTracks>& tracks() { return tracks_; }
    const FixedVec<Track, kMaxTracks>& tracks() const { return tracks_; }

    Track* Find(uint32_t track_id);
    Track* NearestTo(const Vec3& p, float max_distance);

    /// Threat ordering: which hostile is most urgent. Time-to-asset, not range.
    /// Returns nullptr when nothing qualifies.
    Track* MostUrgentHostile(const Vec3& self_position, float now);

private:
    void Classify(Track& t, float now, float dt);

    Config cfg_;
    FixedVec<Track, kMaxTracks> tracks_;
    float last_time_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Classification helpers, exposed so they can be unit-tested against synthetic
// tracks without a simulator.
// ---------------------------------------------------------------------------

/// Rate of change of range to a point. Negative means closing.
///
/// HORIZONTAL only. Altitude rarely matters for a threat assessment, and
/// including it dilutes the signal: a hostile dashing at an asset on the ground
/// from 40 m up is aimed straight at it in the plane that counts, but its 3D
/// alignment is only 0.97 because of the descent. swarm.hpp's GroundRange
/// carries the same warning.
float RangeRate(const Vec3& position, const Vec3& velocity, const Vec3& target);

/// How directly this thing is heading at the target, 1 straight at it, -1 away.
/// Horizontal.
float ApproachAlignment(const Vec3& position, const Vec3& velocity, const Vec3& target);

/// Seconds to reach the target at current closing speed; large if not closing.
float TimeToTarget(const Vec3& position, const Vec3& velocity, const Vec3& target);

/// Seconds until ground range hits the asset cylinder. This is the breach
/// clock (D10), not 3D range to the origin. Zero if already inside; large
/// if not closing in the horizontal plane.
float TimeToCylinder(const Vec3& position, const Vec3& velocity,
                     const Vec3& centre, float radius);

/// Horizontal closing speed of `target` on `observer`, using both
/// velocities. Positive means the range is shrinking. RangeRate only sees
/// the target's velocity, so a picket chasing an outbound looks "closing"
/// when the interceptor is the one moving. ProNav flies the relative
/// geometry; commit has to as well (D11).
float ClosingSpeed(const Vec3& observer_p, const Vec3& observer_v,
                   const Vec3& target_p, const Vec3& target_v);

/// How close this thing will pass to the target if it does not turn. 3D.
///
/// Alignment and closing stay horizontal. Miss cannot: a civilian whose
/// ground track is radial still flies at constant altitude, and a hostile
/// dives at the origin. Horizontal miss called those overflights (D7).
///
/// This is a classification number, not the breach test. The asset is a
/// vertical cylinder of radius `asset_radius` (D10); a hostile is lost
/// when ground range hits that radius, even if 3D CPA is its altitude.
float ClosestApproachDistance(const Vec3& position, const Vec3& velocity,
                              const Vec3& target);

/// True if this miss looks like a dash at the asset, not a constant-miss flyby.
///
/// A civilian's 3D closest-approach is ~its altitude and does not shrink.
/// An s1 hostile aimed at the origin is already inside kSureHit once it
/// dives. A hostile that started off-axis and is turning toward the origin
/// will shrink miss by more than kShrink (above sensor noise) while still
/// on course to enter the cylinder (`miss < asset_radius` on the 3D CPA —
/// sufficient for a dive, stricter than the actual breach test, which is
/// ground range). A level attack above `asset_radius` would still breach
/// and would not be called; not observed on s1 (they arrive at ~7 m).
bool AimedAtAsset(float miss, float miss_at_first, float asset_radius);

/// Wreckage is in unpowered ballistic flight, so its acceleration is g downward
/// and it has no thrust. Nothing in the track marks it as debris, but the
/// kinematics do. NED: +z is down, so falling means positive dz.
bool LooksBallistic(const Vec3& velocity, const Vec3& prev_velocity, float dt);

/// True if a heartbeat's claimed position is consistent with the range our
/// receiver measured. Used to reject a frame that did not come from where
/// it claims to be.
bool HeartbeatPlausible(const Vec3& self, const Vec3& claimed,
                        float measured_range, float range_sigma);

}  // namespace sw

#endif  // SWARM_BELIEF_H
