// brain.cpp -- ABI glue and the tick order. No tactics, no maths, no wire
// format. If logic appears in this file it belongs in one of the four modules.
//
// Build:
//   g++ -shared -fPIC -std=c++17 -I ../sdk/include src/*.cpp -o brain.so

#include "belief.h"
#include "flight.h"
#include "policy.h"
#include "protocol.h"
#include "world.h"

#include <cstdio>
#include <cmath>

namespace {

class SwarmBrain : public swarm::Brain {
public:
    SwarmBrain(const SwHost* host, const SwBootInfo* boot) : swarm::Brain(host, boot) {
        // NOTHING may call a host service here, not even log: in the graded
        // configuration each brain is a separate process and the channel is not
        // up yet. Allocation and pure setup only.
        cfg_ = sw::Config::From(this->boot());
        store_.Configure(cfg_);
        policy_.Configure(cfg_, sw::Rng(&this->host()));
    }

    swarm::Command Tick(const swarm::Observation& obs) override {
        Announce(obs);

        // 1. BELIEF: what our own sensors say. First, because our sensors are
        //    exact and a peer's report is hearsay that has to be matched
        //    against them.
        store_.Update(obs);

        // 2. INBOUND: what did we hear, folded in on top
        ConsumeFrames(obs);
        LogBeliefChanges(obs);

        // 3. POLICY: what should we do about it
        policy_.Decide(store_, obs);
        LogPicketRadius();
        LogLink(obs);
        if (policy_.last_log()[0] != '\0')
            host().Log(policy_.last_log());
        policy_.LogRing(host());
        policy_.LogMode(host());
        policy_.Declare(host(), store_);

        // 4. OUTBOUND: what is worth saying, then say at most one thing
        policy_.Compose(outbox_, store_, obs);
        policy_.Pump(host(), outbox_, obs);

        // 5. FLIGHT: turn the decision into an acceleration
        return Fly(obs);
    }

private:
    void Announce(const swarm::Observation& obs) {
        if (announced_) return;
        announced_ = true;
        host().Logf("drone %u/%u up, lateral limit %.2f m/s^2, kill r %.1f, tier %u",
                    cfg_.drone_id, cfg_.fleet_size, cfg_.lateral_limit,
                    cfg_.kill_radius, cfg_.tier);
        host().Logf("params sense=%.1f comm=%.1f maxv=%.1f maxa=%.1f tilt=%.2f rate=%.1f lat=%.1f sep=%.1f fsep=%.1f ring=%.1f alt=%.1f fix=%.2f",
                    cfg_.sense_radius, cfg_.comm_radius, cfg_.max_speed, cfg_.max_accel,
                    cfg_.max_tilt, cfg_.max_body_rate, cfg_.lateral_limit, cfg_.separation_margin,
                    cfg_.friendly_margin,
                    policy_.ring_radius(), policy_.ring_altitude(),
                    obs.self().fix_sigma);
        logged_ring_radius_ = policy_.ring_radius();
        logged_ring_alt_ = policy_.ring_altitude();
    }

    void LogPicketRadius() {
        const float radius = policy_.ring_radius();
        const float alt = policy_.ring_altitude();
        if (logged_ring_radius_ >= 0.0f &&
            radius == logged_ring_radius_ &&
            std::fabs(alt - logged_ring_alt_) < 0.5f)
            return;
        if (policy_.inbound_ray().ready()) {
            host().Logf("params ring=%.1f alt=%.1f ray h0=%.1f slope=%.3f w=%.1f",
                        radius, alt, policy_.inbound_ray().h0(),
                        policy_.inbound_ray().slope(),
                        policy_.inbound_ray().weight());
        } else {
            host().Logf("params ring=%.1f alt=%.1f", radius, alt);
        }
        logged_ring_radius_ = radius;
        logged_ring_alt_ = alt;
    }

    void LogLink(const swarm::Observation& obs) {
        if (!links_.ready()) return;
        const float now = obs.time();
        if (logged_link_at_ >= 0.0f && now - logged_link_at_ < 2.0f) return;
        logged_link_at_ = now;
        host().Logf("link loss=%.3f lat=%.3f n=%u src=%u",
                    links_.Loss(), links_.MeanLatency(),
                    links_.got() + links_.miss(), links_.origins());
    }

    void ConsumeFrames(const swarm::Observation& obs) {
        const float now = obs.time();

        for (const SwRxFrame& f : obs.rx()) {
            if (!radio_logged_) {
                radio_logged_ = true;
                host().Logf("radio range_sigma=%.2f bearing_sigma=%.3f",
                            f.range_sigma, f.bearing_sigma);
            }
            sw::Reader r(f.data, f.len);
            sw::Header h;
            if (!h.Read(r)) continue;                    // bad version or truncated
            if (h.origin == cfg_.drone_id) continue;     // our own, relayed back
            if (seen_.SeenAndMark(h.origin, h.seq)) continue;

            // Loss and latency are unpublished. Hop-0 seq gaps are loss on
            // the link to that neighbour; now − sent_time is delay. Relays
            // are not that measurement (D53).
            if (h.hops == 0)
                links_.Observe(h.origin, h.seq, h.sent_time, now);

            // TODO(tier 3): everything needed to be suspicious is right here.
            // f.range and f.bearing are measurements OUR receiver made, with
            // published sigmas -- not claims the sender made. For a single-hop
            // frame, the claimed position and the measured range can be
            // compared, and a replay from the wrong side of the arena fails
            // that test with no crypto involved.
            //
            // TODO(tier 3): freshness is measured (D53). A hopped report
            // older than the hop-0 stale window is dropped below.
            //
            // TODO(tier 5): the residual between a peer's claimed position and
            // our measured range to it is the one thing an insider cannot lie
            // about, because range is physical. Accumulate it per peer here.

            switch (h.type) {
                case sw::MsgType::Heartbeat: {
                    sw::HeartbeatMsg m;
                    m.Read(r);
                    if (!r.ok()) break;
                    float age = now - h.sent_time;
                    if (age < 0.0f) age = 0.0f;
                    if (h.hops > 0 && age > links_.StaleAfter()) break;
                    const sw::Vec3 predicted = m.position + m.velocity * age;
                    NotePeer(h.origin, predicted, now);
                    // Measured range is to the transmitter. Only hop-0 is
                    // the origin; a relay would fail HeartbeatPlausible and
                    // smear identity onto the neighbour (D56).
                    if (h.hops == 0) {
                        store_.MarkFriendly(predicted, obs.position(),
                                            f.range, f.range_sigma, now);
                    }
                    if (h.hops + 1u <= sw::kMaxHops) {
                        uint8_t relayed[SW_MTU];
                        if (sw::RelayCopy(f.data, f.len, relayed, sizeof(relayed),
                                          static_cast<uint8_t>(h.hops + 1u))) {
                            outbox_.Push(relayed, f.len, sw::kPrioRelay, now);
                        }
                    }
                    break;
                }
                case sw::MsgType::TrackReport: {
                    sw::TrackReportMsg m;
                    m.Read(r);
                    if (!r.ok()) break;
                    // Same trick as the heartbeat: payload is as-sent, we
                    // associate against where they are now. Age is measured
                    // (now − sent_time); latency is unpublished. D14.
                    float age = now - h.sent_time;
                    if (age < 0.0f) age = 0.0f;
                    if (age > links_.StaleAfter()) break;   // stale or replayed
                    const sw::Vec3 predicted = m.position + m.velocity * age;
                    store_.MergePeerReport(predicted, m.velocity, m.belief,
                                           m.confidence, now, h.origin, h.hops);
                    if (h.hops + 1u <= sw::kMaxHops) {
                        uint8_t relayed[SW_MTU];
                        if (sw::RelayCopy(f.data, f.len, relayed, sizeof(relayed),
                                          static_cast<uint8_t>(h.hops + 1u))) {
                            outbox_.Push(relayed, f.len, sw::kPrioRelay, now);
                        }
                    }
                    break;
                }
                case sw::MsgType::Ray: {
                    sw::RayMsg m;
                    m.Read(r);
                    if (!r.ok()) break;
                    float age = now - h.sent_time;
                    if (age < 0.0f) age = 0.0f;
                    if (age > links_.StaleAfter()) break;
                    policy_.NoteRay(m.h0, m.slope, m.weight, h.hops);
                    if (h.hops + 1u <= sw::kMaxHops) {
                        uint8_t relayed[SW_MTU];
                        if (sw::RelayCopy(f.data, f.len, relayed, sizeof(relayed),
                                          static_cast<uint8_t>(h.hops + 1u))) {
                            outbox_.Push(relayed, f.len, sw::kPrioRelay, now);
                        }
                    }
                    break;
                }
                case sw::MsgType::Claim: {
                    sw::ClaimMsg m;
                    m.Read(r);
                    if (!r.ok()) break;
                    // Allocation is UniqueOwner (D15), not claims. Composing
                    // these starved heartbeats (D11) and neighbours stacked.
                    break;
                }
                case sw::MsgType::Accuse:
                default:
                    break;
            }
        }
    }

    void NotePeer(uint8_t drone_id, const sw::Vec3& position, float now) {
        if (drone_id >= sw::kMaxFleet) return;
        peer_position_[drone_id] = position;
        peer_last_heard_[drone_id] = now;
        policy_.NoteAlive(drone_id, position, now);
    }

    swarm::Command Fly(const swarm::Observation& obs) {
        const sw::Vec3 position = obs.position();
        const sw::Vec3 velocity = obs.velocity();
        const sw::Mode mode = policy_.mode();
        const sw::Track* target = policy_.target();
        const sw::Track* focus = policy_.focus();

        const sw::Vec3 a_now = sw::flight::InertialAccel(obs.attitude(), obs.accel());
        sw::Vec3 accel;
        if (sw::Intercepting(mode) && focus) {
            const sw::Vec3 weave = sw::flight::EstimatedAccel(
                focus->velocity, focus->last_velocity, obs.dt(),
                cfg_.lateral_limit);
            const float prefer = last_t_go_ > 0.12f ? last_t_go_ - obs.dt() : 0.0f;
            const sw::flight::Course course = sw::flight::SolveCollisionCourse(
                position, velocity, focus->position, focus->velocity, cfg_,
                weave, a_now, prefer);
            accel = course.accel;
            last_t_go_ = course.t_go;
        } else {
            last_t_go_ = 0.0f;
            accel = sw::flight::DesiredAccel(
                mode, position, velocity, policy_.DesiredPosition(obs),
                focus, policy_.leashed(), obs.dt(), cfg_, a_now);
        }

        if (sw::Intercepting(mode))
            accel = sw::flight::SlewHorizontal(last_accel_, accel, obs.dt(), cfg_);

        accel = sw::flight::EnforceSeparation(accel, position, velocity,
                                              store_.tracks(), cfg_, target,
                                              sw::Intercepting(mode));
        // Leaving the arena is a wasted loss on station, and a ram that can
        // still hit must not be steered around a wall or the ceiling (D48).
        // The floor is not in that bargain (D64): no hostile is below the
        // dirt, so pulling up gives up no intercept that was still live.
        // Uncatchable aborts first.
        accel = sw::flight::EnforceArena(accel, position, velocity, cfg_,
                                         sw::Intercepting(mode));

        last_accel_ = accel;
        LogProximity(obs, target);

        const float yaw = sw::flight::DesiredYaw(mode, position, velocity,
                                                 cfg_.asset, focus);
        return swarm::Command::Acceleration(accel, yaw);
    }

    static int NearBand(float range) {
        if (range < 3.0f) return 3;
        if (range < 6.0f) return 2;
        if (range < 12.0f) return 1;
        return 0;
    }

    void LogBeliefChanges(const swarm::Observation& obs) {
        (void)obs;
        for (sw::Track& t : store_.tracks()) {
            if (t.belief == t.logged_belief) continue;
            const float miss = sw::ClosestApproachDistance(t.position, t.velocity, cfg_.asset);
            const float align = sw::ApproachAlignment(t.position, t.velocity, cfg_.asset);
            const float closing = -sw::RangeRate(t.position, t.velocity, cfg_.asset);
            const char* verb = "drop";
            if (t.belief == sw::Belief::Wreckage) verb = "wreck";
            else if (t.belief != sw::Belief::Unknown) verb = "call";
            char extra[72]{};
            if (!t.has_local_id) {
                std::snprintf(extra, sizeof(extra),
                              " peer origin=%u hops=%u n=%.0f e=%.0f",
                              t.last_origin, t.last_hops,
                              t.position.x, t.position.y);
            }
            host().Logf("%s trk=%u %s miss=%.1f first=%.1f score=%.2f align=%.2f close=%.1f%s",
                        verb,
                        t.has_local_id ? t.track_id : 0,
                        sw::BeliefName(t.belief),
                        miss, t.miss_at_first < 0.0f ? miss : t.miss_at_first,
                        t.closing_score, align, closing,
                        extra);
            t.logged_belief = t.belief;
        }
    }

    void LogProximity(const swarm::Observation& obs, const sw::Track* target) {
        const sw::Vec3 position = obs.position();
        const sw::Vec3 velocity = obs.velocity();
        const float now = obs.time();
        const bool chasing = sw::Intercepting(policy_.mode()) && target;
        if (!chasing) last_aim_log_at_ = -1.0f;

        for (sw::Track& t : store_.tracks()) {
            const float d = swarm::Distance(position, t.position);
            const int band = NearBand(d);
            if (band == 0) t.near_band = 0;

            const bool intercept = target != nullptr &&
                                   ((t.has_local_id && target->has_local_id &&
                                     t.track_id == target->track_id) ||
                                    target == &t);

            const sw::Vec3 offset = position - t.position;
            float closing = 0.0f;
            if (d > 1e-4f)
                closing = -swarm::Dot(velocity - t.velocity, offset / d);

            const bool band_step = band > 0 && band > t.near_band;
            if (band_step) t.near_band = static_cast<uint8_t>(band);

            // Last 1 s of a chase: refresh believed n/e/alt every 0.1 s so
            // the Aim cue tracks instead of sitting on the commit pose.
            const float ttg = sw::flight::TimeToClose(
                position, velocity, t.position, t.velocity, cfg_.kill_radius);
            const bool terminal_aim = chasing && intercept && ttg <= 1.0f &&
                (last_aim_log_at_ < 0.0f || now - last_aim_log_at_ >= 0.1f);

            if (!band_step && !terminal_aim) continue;

            // "ram" is reserved for the last metres of a committed intercept.
            // A 12 m pass of a civilian we are trying not to hit is "near".
            const char* verb = (intercept && band >= 3) ? "ram" : "near";
            if (intercept) {
                const sw::Vec3 weave = sw::flight::EstimatedAccel(
                    t.velocity, t.last_velocity, obs.dt(), cfg_.lateral_limit);
                const sw::flight::Course course = sw::flight::SolveCollisionCourse(
                    position, velocity, t.position, t.velocity, cfg_, weave,
                    sw::flight::InertialAccel(obs.attitude(), obs.accel()),
                    last_t_go_);
                host().Logf("%s trk=%u class=%s rng=%.1f close=%.1f n=%.1f e=%.1f alt=%.1f vn=%.1f ve=%.1f in=%.1f ie=%.1f ialt=%.1f",
                            verb,
                            t.has_local_id ? t.track_id : 0,
                            sw::BeliefName(t.belief), d, closing,
                            t.position.x, t.position.y, -t.position.z,
                            t.velocity.x, t.velocity.y,
                            course.meeting.x, course.meeting.y, -course.meeting.z);
            } else {
                host().Logf("%s trk=%u class=%s rng=%.1f close=%.1f n=%.1f e=%.1f alt=%.1f vn=%.1f ve=%.1f",
                            verb,
                            t.has_local_id ? t.track_id : 0,
                            sw::BeliefName(t.belief), d, closing,
                            t.position.x, t.position.y, -t.position.z,
                            t.velocity.x, t.velocity.y);
            }
            if (terminal_aim) last_aim_log_at_ = now;
        }
    }

    sw::Config cfg_;
    sw::TrackStore store_;
    sw::Policy policy_;
    sw::Outbox<24> outbox_;
    sw::SeenSet<256> seen_;

    sw::DirectLinkStats links_;
    sw::Vec3 peer_position_[sw::kMaxFleet]{};
    float peer_last_heard_[sw::kMaxFleet]{};

    bool announced_ = false;
    float logged_ring_radius_ = -1.0f;
    float logged_ring_alt_ = -1.0f;
    float logged_link_at_ = -1.0f;
    bool radio_logged_ = false;
    float last_aim_log_at_ = -1.0f;
    sw::Vec3 last_accel_{};
    float last_t_go_ = 0.0f;
};

}  // namespace

SWARM_REGISTER_BRAIN(SwarmBrain)
