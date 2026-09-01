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

/// Facing ring slot for a world position. Same angle convention as RingSlot.
uint32_t FacingSlot(const Vec3& position, const Vec3& asset, uint32_t fleet_size);

/// True if this slot is still treated as on station. Self is always alive.
/// Never-heard is assumed alive so we do not steal sectors before the first
/// heartbeat. 1.5 s of silence (three missed 2 Hz beats) is death.
bool SlotAlive(uint32_t drone_id, uint32_t self_id, const float* heard, float now);

/// First live drone clockwise from `facing`, including facing. One owner,
/// not both neighbours (D15 / G4).
uint32_t UniqueOwner(uint32_t facing, uint32_t fleet_size, uint32_t self_id,
                     const float* heard, float now);

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
    /// Local tracks only (D9). Friendly and Hostile; everything else UNKNOWN.
    void Declare(const swarm::Host& host, const TrackStore& store) const;

    /// Queue whatever is worth saying this tick. One broadcast per tick, so
    /// this only queues -- Pump() does the sending. Non-const because it
    /// records when each track was last reported, which is how the rate limit
    /// survives across ticks.
    void Compose(Outbox<24>& outbox, TrackStore& store,
                 const swarm::Observation& obs);

    /// Send at most one frame, respecting the remaining byte budget.
    void Pump(const swarm::Host& host, Outbox<24>& outbox, const swarm::Observation& obs);

    /// A heartbeat from this origin. UniqueOwner treats 1.5 s of silence as
    /// death and walks clockwise to the next live slot.
    void NoteAlive(uint8_t drone_id, float now);

private:
    bool ShouldCommit(const Track& t, const swarm::Observation& obs) const;
    bool OwnsInbound(const Track& t, float now) const;
    bool CloserChaser(const Track& hostile, const TrackStore& store,
                      const swarm::Observation& obs) const;
    Vec3 PicketGoal(const TrackStore& store, float now) const;
    const char* AbortReason(const Track& t, const swarm::Observation& obs) const;
    void LogAbort(const char* why, uint32_t trk, const Track* t,
                  const swarm::Observation& obs);

    Config cfg_;
    Rng rng_;                 // unused today; the hook for jittering send times

    Stance stance_ = Stance::Forming;
    Track* target_ = nullptr;
    uint32_t target_id_ = 0;
    float committed_at_ = 0.0f;
    uint32_t last_abort_id_ = 0;
    float last_abort_at_ = -1.0e9f;
    char last_log_[192]{};

    uint16_t next_seq_ = 0;
    float last_heartbeat_ = -1.0e9f;
    float ring_radius_ = 60.0f;
    float ring_altitude_ = 30.0f;
    Vec3 picket_goal_{};
    float heard_[kMaxFleet]{};
};

}  // namespace sw

#endif  // SWARM_POLICY_H
