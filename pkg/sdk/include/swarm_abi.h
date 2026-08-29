/* ===========================================================================
 * swarm_abi.h -- Distributed Drone Swarm Challenge, candidate interface.
 *
 * This is the ONLY header you need. Your library links against nothing:
 *
 *   - The simulator finds your code through the single exported factory
 *     function swarm_brain_v1(), resolved at runtime.
 *   - Everything your code needs from the simulator is handed to you as
 *     function pointers in SwHost.
 *
 * Therefore your library has no unresolved external symbols and builds
 * standalone:
 *
 *   g++    -shared -fPIC -std=c++17 -I sdk/include brain.cpp -o brain.so
 *   clang++ -shared      -std=c++17 -I sdk/include brain.cpp -o brain.dll
 *   cl /LD /std:c++17 /I sdk\include brain.cpp
 *
 * CONVENTIONS (see CHALLENGE.md section 4 for the full statement)
 *   World frame  NED  : x = North, y = East, z = Down. Altitude is -z.
 *   Body frame   FRD  : x = forward, y = right, z = down.
 *   Attitude          : unit quaternion rotating body FRD -> world NED.
 *   Angular rates     : body FRD, (p, q, r), rad/s.
 *   Units             : metres, seconds, radians, kilograms, Newtons.
 *                       No degrees appear anywhere in this interface.
 *
 * RULES
 *   - Pointers inside SwObservation are borrowed. They are valid only for
 *     the duration of the tick() call that supplied them. Copy anything
 *     you intend to keep.
 *   - Do not allocate memory that the simulator must free, or vice versa.
 *   - Do not spawn threads, read the wall clock, or touch files, sockets
 *     or environment variables. Simulator time, a seeded random source
 *     and logging are provided. Runs must be reproducible.
 *   - Do not attempt to share state between brain instances. In the graded
 *     configuration each instance runs in its own process.
 *   - tick() must return. A hang stalls the simulation. Tick cost is
 *     measured against a CPU budget of 2000 us per tick and reported in
 *     the score report, but it is not scored: it is a wall-clock reading
 *     and the score has to mean the same thing on your machine and ours.
 * ===========================================================================
 */
#ifndef SWARM_ABI_H
#define SWARM_ABI_H

#include <stdint.h>

#define SWARM_ABI_VERSION 1u

#if defined(_WIN32)
#  define SWARM_EXPORT __declspec(dllexport)
#else
#  define SWARM_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SW_MAX_TRACKS = 64,  /* per-tick cap on reported neighbours          */
    SW_MAX_RX     = 128, /* per-tick cap on received frames              */
    SW_MTU        = 256, /* max payload bytes in one broadcast           */
    SW_KEY_BYTES  = 32,
    SW_SIG_BYTES  = 64,
    SW_HASH_BYTES = 32,

    /* Keys you can have registered with key_import at once, per drone. There is
     * no way to release a handle, so a scheme that imports a fresh key per peer
     * per epoch will run out; one that re-imports the same bytes will not, since
     * that returns the handle it gave you before. */
    SW_MAX_KEYS = 64,

    /* What seal() adds to a plaintext: a 12-byte nonce in front, chosen for you,
     * and a 16-byte tag behind. Size a sealed buffer as pt_len + this. */
    SW_SEAL_OVERHEAD = 28
};

/* Bits in SwBootInfo::capability_flags, one per SwHost service group. A set bit
 * and a non-NULL function pointer say the same thing; the flags exist so you can
 * branch once at construction rather than testing a pointer on every call. */
enum {
    SW_CAP_RADIO   = 1u << 0, /* broadcast                                */
    SW_CAP_DECLARE = 1u << 1, /* declare_track, declare_identity          */
    SW_CAP_RANDOM  = 1u << 2, /* random                                   */
    SW_CAP_LOG     = 1u << 3, /* log                                      */
    SW_CAP_SIGN    = 1u << 4, /* sign                                     */
    SW_CAP_VERIFY  = 1u << 5, /* verify                                   */
    SW_CAP_AGREE   = 1u << 6, /* agree                                    */
    SW_CAP_KDF     = 1u << 7, /* kdf                                      */
    SW_CAP_AEAD    = 1u << 8, /* key_import, seal, unseal                 */
    SW_CAP_HASH    = 1u << 9  /* hash                                     */
};

/* -------------------------------------------------------------------------
 * Math types. Interpret as NED (world) or FRD (body) as documented per use.
 * ---------------------------------------------------------------------- */

typedef struct SwVec3 {
    float x, y, z;
} SwVec3;

/* Unit quaternion rotating a vector from body FRD into world NED.
 * Always normalised; canonical sign, so w >= 0. */
typedef struct SwQuat {
    float w, x, y, z;
} SwQuat;

/* -------------------------------------------------------------------------
 * Your own state.
 *
 * Note that this struct offers two independent sources of information about
 * your motion. They are listed separately for a reason.
 * ---------------------------------------------------------------------- */

typedef struct SwSelfState {
    SwVec3 position;   /* absolute fix, NED, m                            */
    SwVec3 velocity;   /* absolute fix, NED, m/s                          */
    float  fix_sigma;  /* reported 1-sigma of the fix, m                  */
    float  fix_age;    /* seconds since the fix last updated              */

    SwQuat attitude;   /* body FRD -> world NED                           */
    SwVec3 gyro;       /* body FRD angular rate (p, q, r), rad/s          */
    SwVec3 accel;      /* body FRD specific force, m/s^2. This is what an
                          accelerometer reads: it includes the reaction to
                          gravity, so a stationary level vehicle reports
                          approximately (0, 0, -9.81).                    */
} SwSelfState;

/* -------------------------------------------------------------------------
 * A sensed neighbour.
 *
 * Every entity within sense_radius appears here, whatever it is. There is
 * no class field, no name and no identifier that means anything outside
 * your own drone.
 *
 * "Whatever it is" includes collision wreckage, which is a physical object
 * and is sensed like any other. Wreckage is in unpowered ballistic flight,
 * so it is identifiable from its kinematics but not from any field here.
 * It is lethal on contact, exactly like an aircraft.
 * ---------------------------------------------------------------------- */

typedef struct SwTrack {
    /* Observer-local handle. Two drones observing the same entity will
     * assign it different track_ids. A track_id is retired once the entity
     * has been out of range for a while, and a DIFFERENT id is issued if
     * that entity is reacquired later. How long "a while" is varies by
     * mission and is not published; it is a few seconds.
     *
     * A destroyed entity does not age out: it vanishes from this list on
     * the tick after it dies, with no notification of any kind. */
    uint32_t track_id;

    SwVec3 position;   /* NED, m                                          */
    SwVec3 velocity;   /* NED, m/s                                        */
    SwQuat attitude;   /* body FRD -> world NED                           */

    float first_seen;  /* sim time this track_id was created, s           */
    float last_seen;   /* sim time of the most recent update, s           */
} SwTrack;

/* -------------------------------------------------------------------------
 * A received radio frame.
 *
 * The payload is whatever bytes some transmitter passed to broadcast().
 * The simulator does not inspect, validate or interpret it in any way.
 * The remaining fields are measurements made by your own receiver.
 * ---------------------------------------------------------------------- */

typedef struct SwRxFrame {
    const uint8_t* data; /* borrowed for this tick only                   */
    uint32_t       len;

    float  rx_time;       /* sim time of reception, s                     */
    float  range;         /* measured distance to the transmitter, m      */
    float  range_sigma;   /* 1-sigma of the above, m                      */
    SwVec3 bearing;       /* unit vector from receiver toward transmitter,
                             expressed in world NED                       */
    float  bearing_sigma; /* 1-sigma of the above, rad                    */
} SwRxFrame;

/* -------------------------------------------------------------------------
 * Everything you are given, once per tick.
 * ---------------------------------------------------------------------- */

typedef struct SwObservation {
    uint32_t struct_size;

    float time;  /* sim time since scenario start, s                      */
    float dt;    /* control step, s. Constant for the whole episode.      */

    SwSelfState self;

    const SwTrack* tracks;     /* ascending track_id                      */
    uint32_t       track_count;

    const SwRxFrame* rx;       /* ascending (rx_time, transmitter)        */
    uint32_t         rx_count;

    uint32_t tx_budget_bytes;  /* bytes you may still broadcast right now */
    uint32_t flags;            /* reserved, currently zero                */
} SwObservation;

/* -------------------------------------------------------------------------
 * What you command.
 *
 * The simulator runs the inner control cascade
 *   velocity -> acceleration -> attitude -> body rate -> motor mix
 * and you may enter it at any level. Every level saturates against the
 * limits published in SwBootInfo, so a high-level command can never
 * achieve more than a low-level one could.
 *
 * Hover thrust is handled for you in the VEL_NED and ACCEL_NED modes:
 * commanding zero acceleration holds your current velocity. In ATTITUDE
 * and BODY_RATE modes, thrust is yours to set.
 * ---------------------------------------------------------------------- */

typedef enum SwCommandMode {
    SW_CMD_IDLE      = 0, /* motors idle, you will fall                   */
    SW_CMD_VEL_NED   = 1, /* vec = velocity m/s,      yaw = heading rad   */
    SW_CMD_ACCEL_NED = 2, /* vec = acceleration m/s^2, yaw = heading rad  */
    SW_CMD_ATTITUDE  = 3, /* attitude + thrust N                          */
    SW_CMD_BODY_RATE = 4, /* vec = (p, q, r) rad/s + thrust N             */
    SW_CMD_MOTOR_PWM = 5  /* motor_pwm[4], each in [0, 1]                 */
} SwCommandMode;

typedef struct SwCommand {
    uint32_t      struct_size;
    SwCommandMode mode;

    SwVec3 vec;          /* meaning depends on mode, see above            */
    SwQuat attitude;      /* SW_CMD_ATTITUDE                              */
    float  thrust;        /* SW_CMD_ATTITUDE, SW_CMD_BODY_RATE: N         */
    float  yaw;           /* SW_CMD_VEL_NED, SW_CMD_ACCEL_NED: rad        */
    float  motor_pwm[4];  /* SW_CMD_MOTOR_PWM                             */
} SwCommand;

/* -------------------------------------------------------------------------
 * Belief classes, used only by the declare_* scoring hooks.
 * ---------------------------------------------------------------------- */

typedef enum SwClass {
    SW_CLASS_UNKNOWN     = 0,
    SW_CLASS_FRIENDLY    = 1,
    SW_CLASS_ENEMY       = 2,
    SW_CLASS_NEUTRAL     = 3,
    SW_CLASS_COMPROMISED = 4
} SwClass;

/* -------------------------------------------------------------------------
 * Constants handed to you once, at construction.
 * ---------------------------------------------------------------------- */

typedef struct SwBootInfo {
    uint32_t struct_size;
    uint32_t abi_version;

    uint32_t drone_id;         /* your own index, 0 .. fleet_size-1       */

    /* The ORIGINAL roster size. This never changes, and losses are never
     * reported, so a peer that has gone quiet may be dead, out of range,
     * or being interfered with. Telling those apart is your problem. */
    uint32_t fleet_size;

    uint64_t rng_seed;         /* yours alone, derived from the run seed  */
    uint32_t scenario_id;      /* threat tier, 0..5                      */
    uint32_t capability_flags; /* which SwHost services are populated     */

    float dt;                  /* control step, s                        */

    float    sense_radius;     /* m                                      */
    float    comm_radius;      /* m, nominal                             */
    uint32_t mtu;              /* max broadcast payload, bytes           */
    uint32_t tx_budget_bytes_per_s;

    float mass;                /* kg                                     */
    float max_total_thrust;    /* N, all four motors at full             */
    float max_speed;           /* m/s                                    */
    float max_accel;           /* m/s^2                                  */
    float max_tilt;            /* rad                                    */
    float max_body_rate;       /* rad/s                                  */
    float arm_length;          /* m, centre to motor                     */
    /* Hard radius, applied to EVERY pair of physical objects with no
     * exceptions: hostile, friendly, civilian, wreckage. Two of your own
     * drones this close destroy each other. */
    float kill_radius;         /* m                                      */

    SwVec3 asset_position;     /* NED, m                                 */
    float  asset_radius;       /* m                                      */
    SwVec3 arena_min;          /* NED, m                                 */
    SwVec3 arena_max;          /* NED, m                                 */

    uint8_t own_pubkey[SW_KEY_BYTES];
    uint8_t authority_pubkey[SW_KEY_BYTES];

    /* fleet_size * SW_KEY_BYTES bytes, indexed by drone_id. Borrowed for
     * the lifetime of your brain instance. */
    const uint8_t* fleet_pubkeys;
} SwBootInfo;

/* -------------------------------------------------------------------------
 * Services the simulator provides. Call these through the pointer you were
 * given in create(); never store the SwHost struct itself, store the
 * pointer. Unavailable services are NULL, so check capability_flags or the
 * pointer before calling.
 * ---------------------------------------------------------------------- */

typedef struct SwHost {
    uint32_t struct_size;
    uint32_t abi_version;
    void*    ctx; /* pass this back as the first argument every time      */

    /* Broadcast to every drone within comm_radius. Returns the number of
     * bytes accepted, or a negative value if len exceeds the MTU, the
     * byte budget is exhausted, or you have already sent this tick.
     * At most one successful broadcast per tick. */
    int (*broadcast)(void* ctx, const uint8_t* data, uint32_t len);

    /* Report what you currently believe about an entity. These are scoring
     * hooks: they have no effect on the world, are not visible to any other
     * drone, and cannot be read back. Use them liberally; they are how your
     * situational awareness is measured independently of your kill count. */
    void (*declare_track)(void* ctx, uint32_t track_id, SwClass believed);
    void (*declare_identity)(void* ctx, const uint8_t pubkey[SW_KEY_BYTES],
                             SwClass believed);

    /* Deterministic, seeded from your rng_seed. Use this instead of rand(). */
    void (*random)(void* ctx, uint8_t* out, uint32_t len);

    /* Free-text diagnostics, surfaced in the log and the front end. */
    void (*log)(void* ctx, const char* text);

    /* --------------------------------------------------------------------
     * Cryptographic services. NULL in scenarios that do not need them.
     * You design the protocol; these are only the primitives.
     *
     * Every one of these returns the number of bytes it wrote for you, or a
     * negative value if it refused - a bad pointer, a length that does not add
     * up, an unknown key handle, or a tag that did not authenticate. verify()
     * writes nothing, so success is zero and failure is negative; key_import()
     * hands back a handle rather than bytes, and zero means it could not.
     * ----------------------------------------------------------------- */

    /* Ed25519 over your own private key, which you never see. */
    int (*sign)(void* ctx, const uint8_t* msg, uint32_t len,
                uint8_t sig[SW_SIG_BYTES]);
    int (*verify)(void* ctx, const uint8_t pubkey[SW_KEY_BYTES],
                  const uint8_t* msg, uint32_t len,
                  const uint8_t sig[SW_SIG_BYTES]);

    /* X25519 between your own static key and a peer public key. Both drones
     * agreeing on the same pair get the same 32 bytes; put them through kdf()
     * before using them as a key. */
    int (*agree)(void* ctx, const uint8_t peer_pubkey[SW_KEY_BYTES],
                 uint8_t shared[SW_KEY_BYTES]);

    /* HKDF-SHA256. */
    int (*kdf)(void* ctx, const uint8_t* ikm, uint32_t ikm_len,
               const uint8_t* info, uint32_t info_len,
               uint8_t out[SW_KEY_BYTES]);

    /* Register raw key bytes and get an opaque handle for seal/unseal.
     * Returns 0 on failure. Importing the same bytes twice returns the same
     * handle, and the table holds SW_MAX_KEYS of them. */
    uint32_t (*key_import)(void* ctx, const uint8_t key[SW_KEY_BYTES]);

    /* ChaCha20-Poly1305. seal() writes a nonce, the ciphertext and the tag, so
     * `out` needs pt_len + SW_SEAL_OVERHEAD bytes; unseal() takes that back
     * apart, verifies the tag, and returns a negative value if authentication
     * fails. Both return the number of bytes written. The nonce is chosen for
     * you and is unique per key per drone; you never supply one. */
    int (*seal)(void* ctx, uint32_t key_handle,
                const uint8_t* pt, uint32_t pt_len,
                uint8_t* out, uint32_t out_cap);
    int (*unseal)(void* ctx, uint32_t key_handle,
                  const uint8_t* ct, uint32_t ct_len,
                  uint8_t* out, uint32_t out_cap);

    /* SHA-256. */
    int (*hash)(void* ctx, const uint8_t* d, uint32_t len,
                uint8_t out[SW_HASH_BYTES]);
} SwHost;

/* -------------------------------------------------------------------------
 * Your brain. Implement these three functions and return a pointer to a
 * static SwBrain describing them.
 *
 * create()  is called once per drone, before the episode starts. Return an
 *           opaque pointer to your own state; it is passed back to you in
 *           every subsequent call. Return NULL to signal failure.
 *           Do not call host services from create().
 * tick()    is called once per control step. Read obs, fill cmd. The cmd
 *           struct is pre-zeroed with struct_size set and mode IDLE.
 * destroy() is called once at the end. Release whatever you allocated.
 * ---------------------------------------------------------------------- */

typedef struct SwBrain {
    uint32_t struct_size;
    uint32_t abi_version;

    void* (*create)(const SwHost* host, const SwBootInfo* boot);
    void  (*destroy)(void* self);
    void  (*tick)(void* self, const SwObservation* obs, SwCommand* cmd);
} SwBrain;

/* The one and only symbol your library must export. */
SWARM_EXPORT const SwBrain* swarm_brain_v1(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SWARM_ABI_H */
