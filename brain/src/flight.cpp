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
    Vec3 panic;
    bool any = false;
    bool hard = false;

    for (const Track& t : tracks) {
        const bool mate = t.belief == Belief::Friendly;
        // Never exempt a mate: ramming one costs two drones. The intercept
        // target is the only track we are allowed to close on.
        if (!mate && exempt && t.track_id == exempt->track_id) continue;

        const Vec3 offset = position - t.position;
        const float d = swarm::Length(offset);
        if (d < 1e-4f) continue;

        const Vec3 away = offset / d;
        const Vec3 rel_velocity = velocity - t.velocity;
        const float closing = -swarm::Dot(rel_velocity, away);

        // Floor is cruise-vs-picket (friendly_margin, ~19 m on s1) so the
        // ring still fits. Two interceptors close faster than cruise; size
        // the bubble from this pair's closing speed so we start in time.
        float margin = mate ? cfg.friendly_margin : cfg.separation_margin;
        if (mate && closing > 0.0f) {
            const float stop = (closing * closing) / (2.0f * cfg.lateral_limit)
                               + 4.0f * cfg.kill_radius;
            if (stop > margin) margin = stop;
        }
        if (d > margin) continue;

        if (mate && d < cfg.kill_radius * 3.0f) {
            panic += away * cfg.lateral_limit;
            hard = true;
            continue;
        }

        const float urgency = (margin - d) / margin;
        float strength = urgency * urgency;
        if (closing > 0.0f) strength += closing * 0.15f;
        avoid += away * (strength * cfg.lateral_limit * (mate ? 2.5f : 2.0f));
        any = true;

        if (mate) {
            const float closing_cmd = -swarm::Dot(desired, away);
            if (closing_cmd > 0.0f)
                avoid += away * closing_cmd;
        }
    }

    if (hard) return LimitAccel(panic, cfg);
    if (!any) return desired;

    // For mates the closing component of the command is cancelled first (D8).
    // Unknown traffic is still a blend: under saturation it can close.
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
