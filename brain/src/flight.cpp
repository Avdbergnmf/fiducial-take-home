#include "flight.h"

namespace sw {
namespace flight {
namespace {

constexpr float kPi = 3.14159265358979f;

}  // namespace

Vec3 LimitAccel(const Vec3& desired, const Config& cfg) {
    Vec3 out = desired;

    const float horiz = std::sqrt(out.x * out.x + out.y * out.y);
    if (horiz > cfg.lateral_limit && horiz > 1e-6f) {
        const float s = cfg.lateral_limit / horiz;
        out.x *= s;
        out.y *= s;
    }

    if (out.z > cfg.max_accel) out.z = cfg.max_accel;
    if (out.z < -cfg.max_accel) out.z = -cfg.max_accel;

    return out;
}

Vec3 GoTo(const Vec3& target, const Vec3& position, const Vec3& velocity,
          const Config& cfg, float pos_gain, float vel_gain) {
    const Vec3 error = target - position;
    Vec3 accel = error * pos_gain - velocity * vel_gain;
    return LimitAccel(accel, cfg);
}

Vec3 Cruise(const Vec3& target, const Vec3& position, const Vec3& velocity,
            float cruise_speed, const Config& cfg) {
    const Vec3 offset = target - position;
    const float range = swarm::Length(offset);
    if (range < 1e-3f) return LimitAccel(velocity * -1.6f, cfg);

    // Decelerate into the target: the speed we may still be doing at this range
    // if we are to stop on it, given the lateral bound.
    const float stopping = std::sqrt(2.0f * cfg.lateral_limit * range);
    float speed = cruise_speed < stopping ? cruise_speed : stopping;
    if (speed > cfg.max_speed) speed = cfg.max_speed;

    const Vec3 wanted = (offset / range) * speed;
    return LimitAccel((wanted - velocity) * 2.0f, cfg);
}

Vec3 ProNav(const Vec3& self_position, const Vec3& self_velocity,
            const Vec3& target_position, const Vec3& target_velocity,
            const Config& cfg, float navigation_gain) {
    const Vec3 los = target_position - self_position;
    const float range = swarm::Length(los);
    if (range < 1e-3f) return Vec3();

    const Vec3 rel_velocity = target_velocity - self_velocity;

    // Line-of-sight rotation rate: omega = (r x v) / (r . r)
    const Vec3 omega = swarm::Cross(los, rel_velocity) / (range * range);

    const float closing = -swarm::Dot(rel_velocity, los / range);

    // Commanded acceleration perpendicular to the line of sight.
    Vec3 accel = swarm::Cross(omega, los / range) * (navigation_gain * closing);

    // Closing speed has to come from somewhere: add a term along the line of
    // sight when we are not already overtaking.
    if (closing < 12.0f) accel += (los / range) * (cfg.lateral_limit * 0.6f);

    return LimitAccel(accel, cfg);
}

Vec3 EnforceSeparation(const Vec3& desired, const Vec3& position, const Vec3& velocity,
                       const FixedVec<Track, kMaxTracks>& tracks,
                       const Config& cfg, const Track* exempt) {
    Vec3 avoid;
    bool any = false;

    for (const Track& t : tracks) {
        if (exempt && t.track_id == exempt->track_id) continue;

        const Vec3 offset = position - t.position;
        const float d = swarm::Length(offset);
        if (d > cfg.separation_margin || d < 1e-4f) continue;

        // Closing speed along the line between us. Only react to things
        // actually getting closer -- a neighbour drifting apart at the margin
        // is not a problem and reacting to it wastes authority.
        const Vec3 rel_velocity = velocity - t.velocity;
        const float closing = -swarm::Dot(rel_velocity, offset / d);

        const float urgency = (cfg.separation_margin - d) / cfg.separation_margin;
        float strength = urgency * urgency;
        if (closing > 0.0f) strength += closing * 0.15f;

        avoid += (offset / d) * (strength * cfg.lateral_limit * 2.0f);
        any = true;
    }

    if (!any) return desired;

    // Avoidance is added at full weight and the result re-limited. Note this
    // is a blend: when both saturate, the sum can still point somewhere that
    // closes. See the header for what a real guarantee would take.
    return LimitAccel(desired + avoid, cfg);
}

Vec3 EnforceArena(const Vec3& desired, const Vec3& position, const Vec3& velocity,
                  const Config& cfg) {
    constexpr float kEdge = 20.0f;
    Vec3 push;

    auto axis = [&](float p, float lo, float hi, float v, float& out) {
        if (p < lo + kEdge) out += (lo + kEdge - p) * 0.5f - v * 0.8f;
        else if (p > hi - kEdge) out -= (p - (hi - kEdge)) * 0.5f + v * 0.8f;
    };

    axis(position.x, cfg.arena_min.x, cfg.arena_max.x, velocity.x, push.x);
    axis(position.y, cfg.arena_min.y, cfg.arena_max.y, velocity.y, push.y);

    // NED: z is down. arena_min.z is the ceiling, arena_max.z is the ground.
    axis(position.z, cfg.arena_min.z, cfg.arena_max.z, velocity.z, push.z);

    if (swarm::LengthSq(push) < 1e-6f) return desired;
    return LimitAccel(desired + push, cfg);
}

Vec3 RingSlot(uint32_t drone_id, uint32_t fleet_size, const Vec3& centre,
              float radius, float altitude) {
    const uint32_t n = fleet_size > 0 ? fleet_size : 1;
    const float angle = (2.0f * kPi * static_cast<float>(drone_id)) / static_cast<float>(n);
    return Vec3(centre.x + radius * std::cos(angle),
                centre.y + radius * std::sin(angle),
                -altitude);   // NED: altitude is -z
}

}  // namespace flight
}  // namespace sw
