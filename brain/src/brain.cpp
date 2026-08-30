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

        // 3. POLICY: what should we do about it
        policy_.Decide(store_, obs);
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
        (void)obs;
    }

    void ConsumeFrames(const swarm::Observation& obs) {
        const float now = obs.time();

        for (const SwRxFrame& f : obs.rx()) {
            sw::Reader r(f.data, f.len);
            sw::Header h;
            if (!h.Read(r)) continue;                    // bad version or truncated
            if (h.origin == cfg_.drone_id) continue;     // our own, relayed back
            if (seen_.SeenAndMark(h.origin, h.seq)) continue;

            // TODO(tier 3): everything needed to be suspicious is right here.
            // f.range and f.bearing are measurements OUR receiver made, with
            // published sigmas -- not claims the sender made. For a single-hop
            // frame, the claimed position and the measured range can be
            // compared, and a replay from the wrong side of the arena fails
            // that test with no crypto involved.
            //
            // TODO(tier 3): freshness. now - h.sent_time against a window sized
            // from MEASURED latency, not a constant.
            //
            // TODO(tier 5): the residual between a peer's claimed position and
            // our measured range to it is the one thing an insider cannot lie
            // about, because range is physical. Accumulate it per peer here.

            switch (h.type) {
                case sw::MsgType::Heartbeat: {
                    sw::HeartbeatMsg m;
                    m.Read(r);
                    if (!r.ok()) break;
                    NotePeer(h.origin, m.position, now);
                    // TODO(next): a track sitting where a peer says it is, is a
                    // friendly. Free accuracy on a third of the airspace.
                    break;
                }
                case sw::MsgType::TrackReport: {
                    sw::TrackReportMsg m;
                    m.Read(r);
                    if (!r.ok()) break;
                    store_.MergePeerReport(m.position, m.velocity, m.belief,
                                           m.confidence, now);
                    break;
                }
                case sw::MsgType::Claim: {
                    sw::ClaimMsg m;
                    m.Read(r);
                    if (!r.ok()) break;
                    // TODO(B3): honour peer claims so two drones do not spend
                    // themselves on one target. Claims expire; never wait on an
                    // acknowledgement a dead drone will not send.
                    break;
                }
                case sw::MsgType::Accuse:
                default:
                    break;
            }

            // TODO(B4): multi-hop. On s2 the arrival is 220 m out and only one
            // drone can see it, so a frame that stops here never reaches the
            // drone that can act. Forward with a hop limit and a budget check,
            // not the example's flood.
        }
    }

    void NotePeer(uint8_t drone_id, const sw::Vec3& position, float now) {
        if (drone_id >= sw::kMaxFleet) return;
        peer_position_[drone_id] = position;
        peer_last_heard_[drone_id] = now;
    }

    swarm::Command Fly(const swarm::Observation& obs) {
        const sw::Vec3 position = obs.position();
        const sw::Vec3 velocity = obs.velocity();
        const sw::Track* target = policy_.target();

        sw::Vec3 accel;
        if (policy_.stance() == sw::Stance::Committed && target) {
            accel = sw::flight::ProNav(position, velocity, target->position,
                                       target->velocity, cfg_);
        } else {
            const sw::Vec3 goal = policy_.DesiredPosition(obs);
            const float range = swarm::Distance(position, goal);
            accel = (range > 25.0f)
                        ? sw::flight::Cruise(goal, position, velocity, 14.0f, cfg_)
                        : sw::flight::GoTo(goal, position, velocity, cfg_);
        }

        // Constraints last, and in this order: separation over everything
        // (a friendly-friendly collision costs two drones), then the arena.
        accel = sw::flight::EnforceSeparation(accel, position, velocity,
                                              store_.tracks(), cfg_, target);
        accel = sw::flight::EnforceArena(accel, position, velocity, cfg_);

        // Face where we are going: yaw is free and it makes the recording
        // readable in the viewer.
        float yaw = 0.0f;
        if (swarm::LengthSq(velocity) > 1.0f) yaw = std::atan2(velocity.y, velocity.x);

        return swarm::Command::Acceleration(accel, yaw);
    }

    sw::Config cfg_;
    sw::TrackStore store_;
    sw::Policy policy_;
    sw::Outbox<24> outbox_;
    sw::SeenSet<256> seen_;

    sw::Vec3 peer_position_[sw::kMaxFleet]{};
    float peer_last_heard_[sw::kMaxFleet]{};

    bool announced_ = false;
};

}  // namespace

SWARM_REGISTER_BRAIN(SwarmBrain)
