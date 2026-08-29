/* ===========================================================================
 * hover_relay_brain.cpp -- the starting point.
 *
 * This example does the smallest thing that touches every part of the
 * interface, so you can confirm your toolchain and your understanding before
 * writing anything real. It:
 *
 *   1. climbs to a fixed altitude and holds position there;
 *   2. broadcasts a heartbeat naming itself, twice a second;
 *   3. re-broadcasts heartbeats it hears from others, up to a hop limit,
 *      skipping ones it has already seen;
 *   4. reports every neighbour it can see as SW_CLASS_UNKNOWN.
 *
 * It will score approximately zero. Every enemy will reach the asset. Point
 * three is a deliberately naive flood, and point four is where situational
 * awareness is supposed to go.
 *
 * Note what this file does NOT do, because it is instructive: it trusts every
 * heartbeat it receives without checking whether the claimed position is
 * anywhere near where the frame actually came from.
 *
 * Build:
 *   clang++ -shared -std=c++17 -I ../sdk/include hover_relay_brain.cpp -o brain.dll
 *   g++ -shared -fPIC -std=c++17 -I ../sdk/include hover_relay_brain.cpp -o brain.so
 * ===========================================================================
 */

#include "swarm_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

/* --- tuning ------------------------------------------------------------- */

constexpr float kHoldAltitude   = 30.0f; /* metres above ground            */
constexpr float kPosGain        = 0.8f;  /* position -> accel              */
constexpr float kVelGain        = 1.6f;  /* velocity damping               */
constexpr float kHeartbeatHz    = 2.0f;
constexpr uint8_t kMaxHops      = 4;
constexpr uint32_t kSeenHistory = 256;

/* --- wire format -------------------------------------------------------- */

/* A real solution needs an explicit, versioned, endian-defined encoding.
 * This is the crudest thing that works: a packed struct copied byte for
 * byte. It is also completely unauthenticated, which is fine here and not
 * fine later. */
constexpr uint8_t kMagic = 0xA7;

#pragma pack(push, 1)
struct Heartbeat {
    uint8_t  magic;
    uint8_t  origin;    /* drone_id of whoever first sent this  */
    uint8_t  hops;      /* incremented by each relay            */
    uint8_t  reserved;
    uint16_t seq;       /* per-origin sequence number           */
    float    sent_time; /* sim time at the original sender      */
    float    pos[3];    /* claimed position, NED                */
};
#pragma pack(pop)

static_assert(sizeof(Heartbeat) == 22, "unexpected padding");

/* --- helpers ------------------------------------------------------------ */

inline SwVec3 sub(const SwVec3& a, const SwVec3& b) {
    return SwVec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline float norm(const SwVec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

inline SwVec3 clampNorm(SwVec3 v, float limit) {
    const float n = norm(v);
    if (n > limit && n > 1e-6f) {
        const float s = limit / n;
        v.x *= s; v.y *= s; v.z *= s;
    }
    return v;
}

/* --- the brain ---------------------------------------------------------- */

struct Brain {
    const SwHost* host = nullptr;
    SwBootInfo    boot{};

    SwVec3   hold_position{};
    bool     have_hold_position = false;
    bool     announced          = false;
    uint16_t next_seq          = 0;
    float    last_heartbeat_at = -1.0e9f;

    /* Ring buffer of (origin, seq) pairs we have already relayed. A real
     * solution wants something better behaved than this. */
    uint32_t seen[kSeenHistory] = {};
    uint32_t seen_head          = 0;

    /* We may only broadcast once per tick, so relays queue up here. */
    Heartbeat outbox[32]{};
    uint32_t  outbox_count = 0;

    bool alreadySeen(uint8_t origin, uint16_t seq) const {
        const uint32_t key = (static_cast<uint32_t>(origin) << 16) | seq;
        for (uint32_t i = 0; i < kSeenHistory; ++i) {
            if (seen[i] == key) return true;
        }
        return false;
    }

    void markSeen(uint8_t origin, uint16_t seq) {
        const uint32_t key = (static_cast<uint32_t>(origin) << 16) | seq;
        seen[seen_head] = key;
        seen_head       = (seen_head + 1) % kSeenHistory;
    }

    void enqueue(const Heartbeat& hb) {
        if (outbox_count < (sizeof(outbox) / sizeof(outbox[0]))) {
            outbox[outbox_count++] = hb;
        }
    }

    /* ---- lifecycle ---- */

    void init(const SwHost* h, const SwBootInfo* b) {
        host = h;
        boot = *b;

        /* Where to hold is not knowable yet: create() is called before the
         * first observation, so we do not have a position. It is captured on
         * the first tick instead. Sending every drone to the same point would
         * be worse than useless, because the kill radius applies to your own
         * side too. */

        /* Note the seen[] table starts full of zeroes, which collides with
         * (origin 0, seq 0). Harmless here, a bug in anything real. */
    }

    /* Nothing may call a host service from create(), not even log: in the
     * graded configuration each brain is a separate process and the channel is
     * not up yet. The banner therefore goes out on the first tick. */
    void announce() {
        if (announced || !host->log) return;
        announced = true;
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "drone %u of %u up, holding %.1f m, dt=%.4f s",
                      boot.drone_id, boot.fleet_size, kHoldAltitude, boot.dt);
        host->log(host->ctx, msg);
    }

    /* ---- inbound ---- */

    void consumeFrames(const SwObservation& obs) {
        for (uint32_t i = 0; i < obs.rx_count; ++i) {
            const SwRxFrame& f = obs.rx[i];
            if (f.len != sizeof(Heartbeat)) continue;

            Heartbeat hb{};
            std::memcpy(&hb, f.data, sizeof(hb));
            if (hb.magic != kMagic) continue;
            if (hb.origin == boot.drone_id) continue; /* our own, relayed  */
            if (alreadySeen(hb.origin, hb.seq)) continue;

            markSeen(hb.origin, hb.seq);

            /* Everything you would need in order to be suspicious is right
             * here and we are ignoring all of it. The frame arrived from a
             * measured direction f.bearing at a measured distance f.range,
             * and it claims to have originated at hb.pos. For a
             * single-hop frame those two statements can be compared. */
            if (hb.hops + 1u <= kMaxHops) {
                Heartbeat relayed = hb;
                relayed.hops      = static_cast<uint8_t>(hb.hops + 1u);
                enqueue(relayed);
            }
        }
    }

    /* ---- outbound ---- */

    void sendSomething(const SwObservation& obs) {
        const float heartbeat_period = 1.0f / kHeartbeatHz;
        const bool  due = (obs.time - last_heartbeat_at) >= heartbeat_period;

        /* Our own heartbeat takes priority over relaying other people's. */
        if (due) {
            Heartbeat hb{};
            hb.magic     = kMagic;
            hb.origin    = static_cast<uint8_t>(boot.drone_id);
            hb.hops      = 0;
            hb.seq       = next_seq++;
            hb.sent_time = obs.time;
            hb.pos[0]    = obs.self.position.x;
            hb.pos[1]    = obs.self.position.y;
            hb.pos[2]    = obs.self.position.z;

            const int rc = host->broadcast(
                host->ctx, reinterpret_cast<const uint8_t*>(&hb), sizeof(hb));
            if (rc >= 0) {
                last_heartbeat_at = obs.time;
                markSeen(hb.origin, hb.seq);
            }
            return; /* one broadcast per tick, so we are done */
        }

        if (outbox_count > 0) {
            const int rc = host->broadcast(
                host->ctx, reinterpret_cast<const uint8_t*>(&outbox[0]),
                sizeof(Heartbeat));
            if (rc >= 0) {
                /* Pop the front. A priority queue would serve you better
                 * than first-in-first-out once bandwidth gets tight. */
                for (uint32_t i = 1; i < outbox_count; ++i) {
                    outbox[i - 1] = outbox[i];
                }
                --outbox_count;
            }
        }
    }

    /* ---- situational awareness ---- */

    void declareBeliefs(const SwObservation& obs) {
        if (!host->declare_track) return;

        /* This is the hook the scorer reads. Replace UNKNOWN with what you
         * actually think each track is, and you start earning points for
         * situational awareness whether or not you get the intercept. */
        for (uint32_t i = 0; i < obs.track_count; ++i) {
            host->declare_track(host->ctx, obs.tracks[i].track_id,
                                SW_CLASS_UNKNOWN);
        }
    }

    /* ---- flight ---- */

    void fly(const SwObservation& obs, SwCommand& cmd) {
        /* Hold station above where we started, at the target altitude.
         * Remember that NED has z pointing down, so climbing means making z
         * more negative. */
        if (!have_hold_position) {
            hold_position      = obs.self.position;
            hold_position.z    = -kHoldAltitude;
            have_hold_position = true;
        }

        /* Proportional-derivative position hold, emitting an inertial
         * acceleration. Gravity is compensated by the simulator's inner
         * loop, so commanding zero here holds the current velocity. */
        const SwVec3 error = sub(hold_position, obs.self.position);

        SwVec3 accel;
        accel.x = kPosGain * error.x - kVelGain * obs.self.velocity.x;
        accel.y = kPosGain * error.y - kVelGain * obs.self.velocity.y;
        accel.z = kPosGain * error.z - kVelGain * obs.self.velocity.z;

        cmd.mode = SW_CMD_ACCEL_NED;
        /* max_accel is the right cap for a station hold, which is mostly
         * vertical. Do not copy it into anything that manoeuvres hard: the
         * sideways limit is the tilt, g*tan(boot.max_tilt), and it is well
         * under half of this. */
        cmd.vec  = clampNorm(accel, boot.max_accel);
        cmd.yaw  = 0.0f; /* face North */
    }

    void tick(const SwObservation& obs, SwCommand& cmd) {
        announce();
        consumeFrames(obs);
        sendSomething(obs);
        declareBeliefs(obs);
        fly(obs, cmd);
    }
};

/* --- ABI glue ----------------------------------------------------------- */

void* brain_create(const SwHost* host, const SwBootInfo* boot) {
    if (!host || !boot) return nullptr;
    if (boot->abi_version != SWARM_ABI_VERSION) return nullptr;

    Brain* b = new (std::nothrow) Brain();
    if (!b) return nullptr;
    b->init(host, boot);
    return b;
}

void brain_destroy(void* self) {
    delete static_cast<Brain*>(self);
}

void brain_tick(void* self, const SwObservation* obs, SwCommand* cmd) {
    if (!self || !obs || !cmd) return;
    static_cast<Brain*>(self)->tick(*obs, *cmd);
}

const SwBrain kBrain = {
    sizeof(SwBrain),
    SWARM_ABI_VERSION,
    &brain_create,
    &brain_destroy,
    &brain_tick,
};

} /* anonymous namespace */

extern "C" SWARM_EXPORT const SwBrain* swarm_brain_v1(void) {
    return &kBrain;
}
