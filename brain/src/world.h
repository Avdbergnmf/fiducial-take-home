// world.h -- shared small types. No policy, no protocol, no guidance.
//
// DETERMINISM RULES, enforced by what lives in this file:
//   * FixedVec instead of std::vector/map/unordered_map. Node containers put
//     iteration order at the mercy of allocation addresses, which is the most
//     common source of a --replay mismatch.
//   * Rng wraps host->random and is the ONLY randomness in the brain.
//   * Nothing here reads a clock. Time comes from obs.time, always.
#ifndef SWARM_WORLD_H
#define SWARM_WORLD_H

#include "swarm.hpp"

#include <cstdint>
#include <cstring>

namespace sw {

using swarm::Vec3;
using swarm::Quat;

// ---------------------------------------------------------------------------
// Fixed-capacity vector. No heap after construction, deterministic order.
// ---------------------------------------------------------------------------

template <class T, uint32_t N>
class FixedVec {
public:
    uint32_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ >= N; }
    static constexpr uint32_t capacity() { return N; }

    void clear() { size_ = 0; }

    T& operator[](uint32_t i) { return data_[i]; }
    const T& operator[](uint32_t i) const { return data_[i]; }

    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

    T& back() { return data_[size_ - 1]; }

    // Returns nullptr when full rather than growing, so capacity pressure is a
    // visible decision instead of a silent allocation.
    T* push(const T& v) {
        if (size_ >= N) return nullptr;
        data_[size_] = v;
        return &data_[size_++];
    }

    T* emplace() {
        if (size_ >= N) return nullptr;
        data_[size_] = T{};
        return &data_[size_++];
    }

    // Order-preserving, so iteration order stays a function of insertion order
    // and nothing else.
    void erase(uint32_t i) {
        // The `k < N` term is redundant at runtime (size_ <= N always) but lets
        // the compiler prove the access is in range when N is small.
        for (uint32_t k = i + 1; k < size_ && k < N; ++k) data_[k - 1] = data_[k];
        if (size_ > 0) --size_;
    }

private:
    T data_[N]{};
    uint32_t size_ = 0;
};

// ---------------------------------------------------------------------------
// Seeded randomness. host->random is derived from rng_seed, which is per drone
// and reproducible from the scenario id. Never call rand() or anything else.
// ---------------------------------------------------------------------------

class Rng {
public:
    Rng() = default;
    explicit Rng(const swarm::Host* host) : host_(host) {}

    uint32_t NextU32() {
        uint32_t v = 0;
        if (host_) host_->Random(&v, sizeof(v));
        return v;
    }

    /// [0, 1)
    float NextUnit() { return static_cast<float>(NextU32() >> 8) * (1.0f / 16777216.0f); }

    /// [-half, +half)
    float NextSpread(float half) { return (NextUnit() * 2.0f - 1.0f) * half; }

private:
    const swarm::Host* host_ = nullptr;
};

// ---------------------------------------------------------------------------
// Mission constants, resolved once from SwBootInfo.
//
// Everything the brain needs to size a manoeuvre or a message lives here, so no
// module reads SwBootInfo directly and nothing hardcodes a number that varies
// between missions.
// ---------------------------------------------------------------------------

struct Config {
    uint32_t drone_id = 0;
    uint32_t fleet_size = 1;
    uint32_t tier = 0;
    float dt = 0.01f;

    float sense_radius = 60.0f;
    float comm_radius = 90.0f;
    uint32_t mtu = 256;
    uint32_t tx_budget_per_s = 4096;

    float max_speed = 20.0f;
    float max_accel = 15.0f;
    float max_tilt = 0.6f;
    float kill_radius = 1.0f;   // s1 value; --dump-params drone.kill_radius=1

    Vec3 asset{};
    float asset_radius = 30.0f;
    Vec3 arena_min{}, arena_max{};

    /// THE number people get wrong. max_accel is the vertical/total bound;
    /// horizontal acceleration comes from tilting and is bounded by
    /// g*tan(max_tilt) -- about 6.7 m/s^2 where max_accel reads 15.
    /// A hostile has exactly the same bound, so a stern chase never converges.
    float lateral_limit = 6.7f;

    /// How close two aircraft may get before we treat it as a loss. The kill
    /// radius applies to friendly-friendly with no exceptions, so this needs a
    /// real margin over it, not a token one.
    float separation_margin = 0.0f;

    /// Floor keep-out around an identified mate. Sized to arrest cruise with
    /// the lateral bound, plus a few kill radii. Must stay below ring neighbour
    /// spacing or the picket permanently repels itself. EnforceSeparation
    /// raises this to v_close²/(2a)+4·kill when a pair is closing faster
    /// than cruise (two interceptors). D8.
    float friendly_margin = 0.0f;

    static Config From(const SwBootInfo& b) {
        Config c;
        c.drone_id = b.drone_id;
        c.fleet_size = b.fleet_size;
        c.tier = b.scenario_id;
        c.dt = b.dt;

        c.sense_radius = b.sense_radius;
        c.comm_radius = b.comm_radius;
        c.mtu = b.mtu;
        c.tx_budget_per_s = b.tx_budget_bytes_per_s;

        c.max_speed = b.max_speed;
        c.max_accel = b.max_accel;
        c.max_tilt = b.max_tilt;
        c.kill_radius = b.kill_radius;

        c.asset = b.asset_position;
        c.asset_radius = b.asset_radius;
        c.arena_min = b.arena_min;
        c.arena_max = b.arena_max;

        c.lateral_limit = 9.81f * std::tan(b.max_tilt);
        c.separation_margin = b.kill_radius * 4.0f;   // unknown/civilian; see DESIGN.md
        // Arrest 14 m/s (our cruise, brain.cpp) with lateral_limit, then four
        // kill radii. On s1 that is ~19 m; 16 drones on a 75 m ring sit 29 m
        // apart, so the picket does not sit inside this bubble. D8.
        constexpr float kSepSpeed = 14.0f;
        c.friendly_margin = (kSepSpeed * kSepSpeed) / (2.0f * c.lateral_limit)
                            + 4.0f * c.kill_radius;
        return c;
    }
};

// ---------------------------------------------------------------------------
// One aircraft we have seen, in our own frame of reference.
// ---------------------------------------------------------------------------

enum class Belief : uint8_t {
    Unknown = 0,
    Friendly = 1,
    Hostile = 2,
    Civilian = 3,
    Wreckage = 4,     // ours only; declared as NEUTRAL, it is not an aircraft
};

inline SwClass ToSwClass(Belief b) {
    switch (b) {
        case Belief::Friendly: return SW_CLASS_FRIENDLY;
        case Belief::Hostile: return SW_CLASS_ENEMY;
        case Belief::Civilian: return SW_CLASS_NEUTRAL;

        // Wreckage is NOT an aircraft. MEASURED, not assumed: the trace header
        // lists "wreckage" as entity_classes[3] but belief_classes has no
        // wreckage entry, so no declaration about it can ever match the truth
        // label. Confirmed by running s1 twice with only this line changed --
        // NEUTRAL scored 10 more wrong_declarations and 0 more correct, with an
        // identical state_hash. Declaring anything here costs -2 each. Take the
        // zero. See notes/fixture-findings.md.
        case Belief::Wreckage: return SW_CLASS_UNKNOWN;

        default: return SW_CLASS_UNKNOWN;
    }
}

inline const char* BeliefName(Belief b) {
    switch (b) {
        case Belief::Friendly: return "friendly";
        case Belief::Hostile:  return "hostile";
        case Belief::Civilian: return "civilian";
        case Belief::Wreckage: return "wreck";
        default:               return "unknown";
    }
}

struct Track {
    uint32_t track_id = 0;

    /// True only for tracks our own sensors created. Peer-reported tracks have
    /// no local id, and Find() must not match them -- otherwise a peer track
    /// sitting at id 0 collides with a real sensor track at id 0. This is the
    /// same bug the example brain has in its seen[] table.
    bool has_local_id = false;

    Vec3 position{};
    Vec3 velocity{};
    Quat attitude{};

    float first_seen = 0.0f;
    float last_seen = 0.0f;
    float last_update = 0.0f;

    // Evidence accumulated over time. Classification needs sustained
    // observation, so single-tick geometry is never enough on its own.
    float closing_score = 0.0f;    // integrated approach toward the asset
    float ballistic_score = 0.0f;  // integrated "falling like wreckage"
    Vec3 last_velocity{};
    float miss_at_first = -1.0f;   // CPA miss when first classified; -1 = unset

    Belief belief = Belief::Unknown;
    float belief_since = 0.0f;
    Belief logged_belief = Belief::Unknown;  // last class we wrote to host->log
    uint8_t near_band = 0;                   // 0 far, 1 <12 m, 2 <6 m, 3 <3 m
    float friendly_until = -1.0e9f;          // heartbeat hold; Classify will not demote before this

    /// Last time we put this on the radio. Without it, Compose queues a report
    /// every tick for a second, which is 100 duplicates at 100 Hz.
    float last_reported = -1.0e9f;
};

constexpr uint32_t kMaxTracks = SW_MAX_TRACKS;
constexpr uint32_t kMaxFleet = 64;

}  // namespace sw

#endif  // SWARM_WORLD_H
