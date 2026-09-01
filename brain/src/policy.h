// policy.h -- what to do about what we believe.
//
// This is where judgement lives: whether to commit a drone, what to say and how
// often, what to declare. Deliberately separate from belief (which decides what
// things ARE) and from flight (which decides how to move). A new requirement
// should land in exactly one of the three.
#ifndef SWARM_POLICY_H
#define SWARM_POLICY_H

#include "belief.h"
#include "protocol.h"

namespace sw {

enum class Stance : uint8_t {
    Forming,      // take up a ring slot
    Picketing,    // hold the ring, watch
    Committed,    // running an intercept on a specific track
};

class Policy {
public:
    void Configure(const Config& cfg, Rng rng);

    /// Decide the stance and target for this tick.
    void Decide(TrackStore& store, const swarm::Observation& obs);

    /// Where flight should take us, given the stance.
    Vec3 DesiredPosition(const swarm::Observation& obs) const;

    Stance stance() const { return stance_; }
    const Track* target() const { return target_; }
    const char* last_log() const { return last_log_; }
    float ring_radius() const { return ring_radius_; }
    float ring_altitude() const { return ring_altitude_; }

    /// Report what we currently believe. Scored once a second on the MOST
    /// RECENT declaration per aircraft: correct +1, wrong -2, unknown 0.
    /// Cheap to call every tick and there is no reason not to.
    void Declare(const swarm::Host& host, const TrackStore& store) const;

    /// Queue whatever is worth saying this tick. One broadcast per tick, so
    /// this only queues -- Pump() does the sending. Non-const because it
    /// records when each track was last reported, which is how the rate limit
    /// survives across ticks.
    void Compose(Outbox<24>& outbox, TrackStore& store,
                 const swarm::Observation& obs);

    /// Send at most one frame, respecting the remaining byte budget.
    void Pump(const swarm::Host& host, Outbox<24>& outbox, const swarm::Observation& obs);

private:
    bool ShouldCommit(const Track& t, const swarm::Observation& obs) const;
    const char* AbortReason(const Track& t, const swarm::Observation& obs) const;

    Config cfg_;
    Rng rng_;                 // unused today; the hook for jittering send times

    Stance stance_ = Stance::Forming;
    Track* target_ = nullptr;
    uint32_t target_id_ = 0;
    float committed_at_ = 0.0f;
    char last_log_[192]{};

    uint16_t next_seq_ = 0;
    float last_heartbeat_ = -1.0e9f;
    float ring_radius_ = 60.0f;
    float ring_altitude_ = 30.0f;
};

}  // namespace sw

#endif  // SWARM_POLICY_H
