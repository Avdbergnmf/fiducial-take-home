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

/// How far we can divert in time `t` at accel `a`, capped by `vmax`.
float Reach(float t, float a, float vmax) {
    if (t <= 0.0f || a < 1e-6f) return 0.0f;
    if (vmax < 1e-3f) return 0.5f * a * t * t;
    const float t_v = vmax / a;
    if (t <= t_v) return 0.5f * a * t * t;
    return vmax * t - 0.5f * vmax * vmax / a;
}

/// Position after constant accel `a` for time `t`, coasting once |v|
/// hits `vmax`. Used to score an intercept, not as a second plant.
Vec3 PredictedPosition(const Vec3& p, const Vec3& v, const Vec3& a,
                       float t, float vmax) {
    const float a2 = swarm::LengthSq(a);
    const float v2 = swarm::LengthSq(v);
    const float vmax2 = vmax * vmax;
    float t_boost = t;
    if (vmax > 1e-3f) {
        if (v2 >= vmax2 - 1e-4f) {
            t_boost = 0.0f;
        } else if (a2 > 1e-8f) {
            const float qb = 2.0f * swarm::Dot(v, a);
            const float qc = v2 - vmax2;
            const float disc = qb * qb - 4.0f * a2 * qc;
            if (disc >= 0.0f) {
                const float root = (-qb + std::sqrt(disc)) / (2.0f * a2);
                if (root >= 0.0f && root < t_boost) t_boost = root;
            }
        }
    }
    const Vec3 boosted = p + v * t_boost + a * (0.5f * t_boost * t_boost);
    if (t_boost >= t - 1e-6f) return boosted;
    const Vec3 v_coast = v + a * t_boost;
    const float sp = swarm::Length(v_coast);
    if (sp < 1e-3f) return boosted;
    float coast = sp;
    if (vmax > 1e-3f && coast > vmax) coast = vmax;
    return boosted + v_coast * ((t - t_boost) * (coast / sp));
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
    //    intercept time — ram along the LOS instead of a tiny fake t_go
    //    that blows the gain and commands us away from them (D45).
    const float closing = -swarm::Dot(rel_velocity, unit);
    if (closing <= 1.0f) {
        Vec3 ram = unit * cfg.max_accel;
        ram = LimitAccel(ram, cfg);
        const float along = swarm::Dot(ram, unit);
        if (along < 0.0f) ram = ram - unit * along;
        return LimitAccel(ram, cfg);
    }
    float t_go = range / closing;
    // Floor so N/t_go² cannot explode in the last metres and saturate
    // *away* from the target after LimitAccel splits xy and z.
    if (t_go < 0.35f) t_go = 0.35f;

    // 4. Zero-effort miss, then augmented ZEM if we have their acceleration:
    //    where they pass us if nobody (else) steers, plus ½ At t_go².
    const Vec3 zem = los + rel_velocity * t_go
                     + target_accel * (0.5f * t_go * t_go);

    // 5. Component normal to the LOS. Along-LOS is closing, not a steer.
    const Vec3 zemn = zem - unit * swarm::Dot(zem, unit);

    // 6. ProNav: a = N * ZEMn / t_go². Same as a = N * Vc * ω.
    Vec3 accel = zemn * (navigation_gain / (t_go * t_go));

    // 7. A missile already has closing speed. A picket starts at rest, so
    //    add close along the LOS. In the last few kill-radii take the full
    //    lateral budget — a ram that brakes along the LOS misses under them.
    float along_close = cfg.lateral_limit * 0.6f;
    if (range < cfg.kill_radius * 4.0f)
        along_close = cfg.lateral_limit;
    accel = accel + unit * along_close;
    accel = LimitAccel(accel, cfg);

    // LimitAccel splits xy and z. That can flip the along-LOS sign and
    // command us *up* off a diving intercept. A kill is a ram: never brake
    // along the line of sight (D45).
    const float along = swarm::Dot(accel, unit);
    if (along < 0.0f) {
        accel = accel - unit * along;
        accel = LimitAccel(accel, cfg);
    }
    return accel;
}

Vec3 CollisionCourse(const Vec3& self_p, const Vec3& self_v,
                     const Vec3& tgt_p, const Vec3& tgt_v,
                     const Config& cfg, const Vec3& tgt_a) {
    const Vec3 los = tgt_p - self_p;
    const float range = swarm::Length(los);
    if (range < 1e-3f) return Vec3();

    // Vector ZEM: ZEM = r + v_rel t + ½ At t². Not ZEMn, not range/closing.
    // N=2 arrives at I. LimitAccel is the 5.4 cylinder (D51). Do not add g.
    static constexpr float kTs[] = {
        0.25f, 0.40f, 0.55f, 0.70f, 0.85f, 1.00f,
        1.20f, 1.45f, 1.70f, 2.00f, 2.40f, 2.80f,
        3.30f, 3.80f, 4.50f, 5.50f, 7.00f, 9.00f, 12.00f};

    const float kill = cfg.kill_radius > 0.1f ? cfg.kill_radius : 0.1f;
    float best_miss = 1.0e9f;
    Vec3 best_a = LimitAccel(los * (cfg.max_accel / range), cfg);
    Vec3 best_I = tgt_p;

    for (float t : kTs) {
        const Vec3 I = tgt_p + tgt_v * t + tgt_a * (0.5f * t * t);
        const Vec3 zem = I - self_p - self_v * t;
        const Vec3 arrive = LimitAccel(zem * (2.0f / (t * t)), cfg);
        const float miss = swarm::Length(
            PredictedPosition(self_p, self_v, arrive, t, cfg.max_speed) - I);
        if (miss < best_miss) {
            best_miss = miss;
            best_a = arrive;
            best_I = I;
        }
        if (miss <= kill) break;
    }

    Vec3 accel = best_a;

    // Never command away from the intercept (D45, along I not the body).
    const Vec3 to_I = best_I - self_p;
    const float dI = swarm::Length(to_I);
    if (dI > 1e-3f) {
        const Vec3 unit = to_I / dI;
        const float along = swarm::Dot(accel, unit);
        if (along < 0.0f) {
            accel = accel - unit * along;
            accel = LimitAccel(accel, cfg);
        }
    }
    return accel;
}

bool CatchableRam(const Vec3& self_p, const Vec3& self_v,
                  const Vec3& tgt_p, const Vec3& tgt_v, const Config& cfg) {
    const float kill = cfg.kill_radius;
    const float obvious = 2.0f * kill;
    const Vec3 rel_p = tgt_p - self_p;
    const float range = swarm::Length(rel_p);
    // Still in the merge bubble: keep going. 2·kill is the obvious-miss
    // threshold, not a "already hitting" test.
    if (obvious > 0.0f && range <= obvious) return true;

    const Vec3 rel_v = tgt_v - self_v;
    const float v2 = swarm::LengthSq(rel_v);
    if (v2 < 1e-8f) return false;

    const float t_cpa = -swarm::Dot(rel_p, rel_v) / v2;
    if (t_cpa <= 0.0f) return false;

    const Vec3 miss_vec = rel_p + rel_v * t_cpa;
    if (swarm::Length(miss_vec) <= obvious) return true;

    // Still slamming in: the intercept has the shot. Do not abort a 13 m,
    // 22 m/s merge because leftover lateral is 5 m and ½ a t² looks small.
    const float closing = -swarm::Dot(rel_v, rel_p) / range;
    if (closing >= 5.0f) return true;

    auto reachable = [&](float t) {
        if (t < 0.05f) return false;
        const Vec3 sep = rel_p + rel_v * t;
        const float d = swarm::Length(sep);
        if (d <= obvious) return true;
        const Vec3 unit = sep / d;
        float a = swarm::Dot(
            LimitAccel(unit * (cfg.max_accel + cfg.lateral_limit), cfg), unit);
        if (a < 0.1f) return false;
        return Reach(t, a, cfg.max_speed) >= d - kill;
    };

    if (reachable(t_cpa)) return true;
    // A cut-off a couple of seconds past CPA is still a ram. A 12 s
    // stern chase is not — that is not-closing.
    const float t_hi = t_cpa + 2.0f;
    if (reachable(t_hi)) return true;
    static constexpr float kTs[] = {
        0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f};
    for (float t : kTs) {
        if (t > t_hi) break;
        if (reachable(t)) return true;
    }
    return false;
}

Vec3 DesiredAccel(Mode mode, const Vec3& position, const Vec3& velocity,
                  const Vec3& goal, const Track* focus, bool leashed,
                  float dt, const Config& cfg) {
    auto weave_of = [&](const Track& t) {
        return EstimatedAccel(t.velocity, t.last_velocity, dt,
                              cfg.lateral_limit);
    };

    switch (mode) {
        case Mode::Scrambling:
        case Mode::Ramming:
            if (focus) {
                return CollisionCourse(position, velocity, focus->position,
                                       focus->velocity, cfg, weave_of(*focus));
            }
            break;
        case Mode::Stalking:
            if (focus && leashed) {
                return ProNav(position, velocity, focus->position,
                              focus->velocity, cfg, weave_of(*focus));
            }
            return GoTo(goal, position, velocity, cfg);
        case Mode::Forming:
        case Mode::Picketing:
        case Mode::Watching:
            break;
    }

    // Must match kCruise in policy.cpp.
    constexpr float kStationCruise = 14.0f;
    const float range = swarm::Distance(position, goal);
    return (range > 25.0f)
               ? Cruise(goal, position, velocity, kStationCruise, cfg)
               : GoTo(goal, position, velocity, cfg);
}

float DesiredYaw(Mode mode, const Vec3& position, const Vec3& velocity,
                 const Vec3& asset, const Track* focus) {
    const bool along_velocity = Intercepting(mode) || mode == Mode::Stalking;
    if (!along_velocity) {
        if (focus && (mode == Mode::Watching || mode == Mode::Forming)) {
            return std::atan2(focus->position.y - position.y,
                              focus->position.x - position.x);
        }
        const float dx = position.x - asset.x;
        const float dy = position.y - asset.y;
        if (dx * dx + dy * dy > 1.0f)
            return std::atan2(dy, dx);
    }
    if (swarm::LengthSq(velocity) > 1.0f)
        return std::atan2(velocity.y, velocity.x);
    return 0.0f;
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
    constexpr float kSlack = 2.0f;
    Vec3 push;

    auto wall = [&](float p, float lo, float hi, float v, float bound, float& out) {
        float stop_lo = kSlack;
        float stop_hi = kSlack;
        if (bound > 0.1f) {
            if (v < -0.1f) stop_lo = (v * v) / (2.0f * bound) + kSlack;
            if (v > 0.1f) stop_hi = (v * v) / (2.0f * bound) + kSlack;
        }
        if (p < lo + stop_lo) out += (lo + stop_lo - p) * 0.5f - v * 0.8f;
        else if (p > hi - stop_hi) out -= (p - (hi - stop_hi)) * 0.5f + v * 0.8f;
    };

    wall(position.x, cfg.arena_min.x, cfg.arena_max.x, velocity.x,
         cfg.lateral_limit, push.x);
    wall(position.y, cfg.arena_min.y, cfg.arena_max.y, velocity.y,
         cfg.lateral_limit, push.y);
    // NED: z is down. arena_min.z is the ceiling, arena_max.z is the ground.
    wall(position.z, cfg.arena_min.z, cfg.arena_max.z, velocity.z,
         cfg.max_accel, push.z);

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
