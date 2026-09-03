#include "flight.h"

#include <cmath>

namespace sw {
namespace flight {
namespace {

constexpr float kPi = 3.14159265358979f;

float ArrestDistance(float closing, const Config& cfg) {
    if (closing <= 0.0f) return 0.0f;
    return (closing * closing) / (2.0f * cfg.lateral_limit)
           + 4.0f * cfg.kill_radius;
}

}  // namespace

float TimeToClose(const Vec3& p, const Vec3& v, const Vec3& q, const Vec3& w,
                  float radius) {
    const Vec3 offset = p - q;
    const float d = swarm::Length(offset);
    if (d <= radius) return 0.0f;
    if (d < 1e-4f) return 0.0f;
    const float closing = -swarm::Dot(v - w, offset / d);
    if (closing < 0.1f) return 1.0e6f;
    return (d - radius) / closing;
}

Vec3 AimAhead(const Vec3& position, const Vec3& velocity, float distance) {
    if (distance <= 0.0f) return position;
    const float speed = swarm::Length(velocity);
    if (speed < 1e-3f) return position;
    return position + velocity * (distance / speed);
}

Vec3 EstimatedAccel(const Vec3& velocity, const Vec3& last_velocity,
                    float dt, float cap) {
    if (dt < 1e-3f) return Vec3();
    Vec3 accel = (velocity - last_velocity) / dt;
    const float mag = swarm::Length(accel);
    if (cap > 0.0f && mag > cap) accel = accel * (cap / mag);
    return accel;
}

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
            const Config& cfg, const Vec3& target_accel,
            float lead_kill_radii, float navigation_gain) {
    // 1. Pretend they have already travelled `lead` metres along the
    //    trajectory we can predict, then intercept that virtual state.
    //    Not "0.5 kr past the ProNav intercept of the real body".
    const float lead = lead_kill_radii > 0.0f
        ? lead_kill_radii * cfg.kill_radius
        : 0.0f;
    Vec3 aimed = target_position;
    Vec3 aimed_v = target_velocity;
    if (lead > 0.0f) {
        const float speed = swarm::Length(target_velocity);
        if (speed > 1e-3f) {
            const float t_lead = lead / speed;
            aimed = target_position + target_velocity * t_lead
                    + target_accel * (0.5f * t_lead * t_lead);
            aimed_v = target_velocity + target_accel * t_lead;
        }
    }

    // 2. Line of sight and relative velocity (inertial, target minus us).
    const Vec3 los = aimed - self_position;
    const float range = swarm::Length(los);
    if (range < 1e-3f) return Vec3();
    const Vec3 unit = los / range;
    const Vec3 rel_velocity = aimed_v - self_velocity;

    // 3. Closing speed and time-to-go. If they are not closing, there is no
    //    intercept time — use our dash speed so the law still has a horizon.
    const float closing = -swarm::Dot(rel_velocity, unit);
    const float t_go = (closing > 1.0f)
        ? range / closing
        : range / (cfg.max_speed > 1.0f ? cfg.max_speed : 1.0f);

    // 4. Zero-effort miss, then augmented ZEM if we have their acceleration:
    //    where they pass us if nobody (else) steers, plus ½ At t_go².
    const Vec3 zem = los + rel_velocity * t_go
                     + target_accel * (0.5f * t_go * t_go);

    // 5. Component normal to the LOS. Along-LOS is closing, not a steer.
    const Vec3 zemn = zem - unit * swarm::Dot(zem, unit);

    // 6. ProNav: a = N * ZEMn / t_go². Same as a = N * Vc * ω.
    Vec3 accel = zemn * (navigation_gain / (t_go * t_go));

    // 7. A missile already has closing speed. We start at rest, so add a
    //    modest close along the LOS. Sized under the lateral cap so ZEMn
    //    still has authority against a weave.
    accel = accel + unit * (cfg.lateral_limit * 0.6f);
    return LimitAccel(accel, cfg);
}

Vec3 EnforceSeparation(const Vec3& desired, const Vec3& position, const Vec3& velocity,
                       const FixedVec<Track, kMaxTracks>& tracks,
                       const Config& cfg, const Track* exempt, bool intercepting) {
    Vec3 avoid;
    Vec3 panic;
    bool any = false;
    bool hard = false;

    const float kill = cfg.kill_radius * 2.0f;
    float t_target = 1.0e6f;
    if (intercepting && exempt)
        t_target = TimeToClose(position, velocity, exempt->position,
                               exempt->velocity, kill);

    for (const Track& t : tracks) {
        const bool mate = t.belief == Belief::Friendly;
        // Never exempt a mate: ramming one costs two drones. The intercept
        // target is the only track we are allowed to close on. D38: unless
        // we hit the target first or together — then braking misses.
        if (!mate && exempt && t.track_id == exempt->track_id) continue;
        if (mate && intercepting && t_target < 1.0e5f) {
            const float t_mate = TimeToClose(position, velocity, t.position,
                                             t.velocity, kill);
            if (t_target <= t_mate) continue;
        }

        const Vec3 offset = position - t.position;
        const float d = swarm::Length(offset);
        if (d < 1e-4f) continue;

        const Vec3 away = offset / d;
        const Vec3 rel_velocity = velocity - t.velocity;
        const float closing = -swarm::Dot(rel_velocity, away);

        // Floor: mates get cruise-arrest (~19 m) so the ring still fits;
        // unknown gets 4·kill for drift. When a pair is closing, both grow
        // to v_close²/(2a)+4·kill so we start in time. D8/D12.
        float margin = mate ? cfg.friendly_margin : cfg.separation_margin;
        if (closing > 0.0f) {
            const float stop = ArrestDistance(closing, cfg);
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

        if (mate && !intercepting) {
            const float closing_cmd = -swarm::Dot(desired, away);
            if (closing_cmd > 0.0f)
                avoid += away * closing_cmd;
        }
    }

    if (hard) return LimitAccel(panic, cfg);
    if (!any) return desired;

    // For mates the closing component of the command is cancelled first (D8).
    // Unknown traffic is a blend of desired + avoid, sized to actually
    // arrest (D12). Under saturation it can still close.
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

Vec3 RingSlot(uint32_t index, uint32_t count, const Vec3& centre,
              float radius, float altitude) {
    const uint32_t n = count > 0 ? count : 1;
    const float angle = (2.0f * kPi * static_cast<float>(index)) / static_cast<float>(n);
    return Vec3(centre.x + radius * std::cos(angle),
                centre.y + radius * std::sin(angle),
                -altitude);   // NED: altitude is -z
}

}  // namespace flight
}  // namespace sw
