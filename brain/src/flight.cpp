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

/// Seconds until the pair is inside `radius`, using relative closing.
/// 0 if already inside; large if not approaching.
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

}  // namespace

/// Time until we and a constant-velocity target can occupy the same point, if
/// we fly at `speed`. |d + w*t| = speed*t squares to a quadratic in t:
///     (|w|^2 - speed^2) t^2 + 2 (d.w) t + |d|^2 = 0
/// Returns -1 when no positive root exists -- the target outruns us and is
/// opening, so there is no lead point and the caller falls back to pursuit.
float TimeToIntercept(const Vec3& d, const Vec3& w, float speed) {
    const float c = swarm::Dot(d, d);
    if (c < 1e-6f) return 0.0f;
    const float a = swarm::Dot(w, w) - speed * speed;
    const float b = 2.0f * swarm::Dot(d, w);
    if (std::fabs(a) < 1e-3f) {          // same speed: the quadratic is linear
        return b < -1e-6f ? -c / b : -1.0f;
    }
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return -1.0f;
    const float root = std::sqrt(disc);
    const float t1 = (-b - root) / (2.0f * a);
    const float t2 = (-b + root) / (2.0f * a);
    float best = -1.0f;
    if (t1 > 1e-3f) best = t1;
    if (t2 > 1e-3f && (best < 0.0f || t2 < best)) best = t2;
    return best;
}

Vec3 BarrierAim(const Vec3& self, const Vec3& target_p, const Vec3& target_v,
                float speed, float lead) {
    if (lead < 0.0f) lead = 0.0f;
    const Vec3 v(target_v.x, target_v.y, 0.0f);
    const float vlen = swarm::Length(v);
    if (vlen < 0.5f) {
        const Vec3 los = target_p - self;
        const float tau = TimeToIntercept(los, target_v, speed);
        return (tau >= 0.0f) ? target_p + target_v * tau : target_p;
    }
    const Vec3 dir = v / vlen;

    auto at_along = [&](float s) {
        return Vec3(target_p.x + dir.x * s, target_p.y + dir.y * s, target_p.z);
    };

    const Vec3 los = target_p - self;
    const float tau = TimeToIntercept(los, target_v, speed);
    Vec3 meet = (tau >= 0.0f) ? target_p + target_v * tau : at_along(lead);
    const float meet_along = swarm::Dot(
        Vec3(meet.x - target_p.x, meet.y - target_p.y, 0.0f), dir);
    // Slightly in front of the meeting, hence of them (D39). Early → we
    // arrive on the chord and they fly into us; late → still ahead of
    // current position, not abeam. Never behind `lead`.
    float s = meet_along + lead;
    if (s < lead) s = lead;
    return at_along(s);
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
            const Config& cfg, float navigation_gain) {
    const Vec3 los = target_position - self_position;
    const float range = swarm::Length(los);
    if (range < 1e-3f) return Vec3();
    const Vec3 unit = los / range;

    const Vec3 rel_velocity = target_velocity - self_velocity;
    const float closing = -swarm::Dot(rel_velocity, unit);

    // --- terminal: zero-effort miss (D25) -------------------------------
    // Where the target would pass us if neither of us accelerated again. That
    // vector IS the miss, so steer to null it: a = N * ZEM / t_go^2.
    //
    // Classic proportional navigation nulls the line-of-sight rotation rate
    // instead, which is the same thing only while the closing rate is steady.
    // Ours is not: the midcourse leg hands over still accelerating, so PN was
    // solving a slightly wrong problem exactly when it mattered. Measured over
    // 20 unseen ids, swapping the law converted 2 breaches into kills and took
    // the tier-2 mean from -84.1 to -38.5 at no cost on the fixed set.
    const float rate = closing > 1.0f ? closing : 1.0f;
    const float t_go = range / rate;
    const Vec3 zem = los + rel_velocity * t_go;
    const Vec3 zem_perp = zem - unit * swarm::Dot(zem, unit);
    Vec3 terminal = zem_perp * (navigation_gain / (t_go * t_go));
    if (closing < 12.0f) terminal = terminal + unit * (cfg.lateral_limit * 0.6f);

    // --- midcourse: close the range, do not wait ------------------------
    // PN alone commands acceleration only ACROSS the line of sight. A picket
    // already sitting on the hostile's inbound bearing sees almost no LOS
    // rotation, so it commands almost nothing: measured on s1, a committed
    // drone held 0.2-0.4 m/s for four seconds while the hostile closed 86 m
    // and rammed it at our own ring radius. That is the whole score --
    // reward is W_kill*(1 - t_engage/t_free) and we were banking 16% of it.
    // So solve the predicted center intersection in closed form and fly there
    // flat out. Nothing
    // is held back for later: a kill is a ram, and the report credits the
    // drone we spend (losses_by_cause pair_hostile, wasted 0).
    const float speed = cfg.max_speed;
    const float lead = kBarrierLeadKills * cfg.kill_radius;
    const Vec3 aim = BarrierAim(self_position, target_position, target_velocity,
                                speed, lead);
    const Vec3 to_aim = aim - self_position;
    const float aim_range = swarm::Length(to_aim);
    const Vec3 wanted = (aim_range > 1e-3f) ? (to_aim / aim_range) * speed
                                            : unit * speed;
    const Vec3 midcourse = (wanted - self_velocity) * 2.0f;

    // --- handover -------------------------------------------------------
    // The predicted intersection assumes constant target velocity, so it goes stale as
    // soon as the hostile turns, and at 35 m/s of closing there is no range
    // left to correct: flying the lead point all the way in missed by 1-3 m
    // against a 1 m kill radius on every scenario measured. Hand over while
    // there is still time to null the error -- 6.71 m/s^2 needs about a
    // second to move 3 m, which at this closing speed is a few tens of
    // metres of range.
    float w = (range - 25.0f) / (70.0f - 25.0f);
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    const Vec3 accel = midcourse * w + terminal * (1.0f - w);

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
