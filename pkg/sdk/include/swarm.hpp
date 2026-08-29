// swarm.hpp - optional C++ convenience layer over swarm_abi.h.
//
// Header-only, no dependencies beyond the standard library, and entirely
// optional: everything here is expressible in the C interface, and a brain
// written straight against swarm_abi.h is not at any disadvantage. What this
// buys you is the vtable boilerplate, bounds-checked views over the borrowed
// arrays, and small vector maths so you are not writing dot products by hand.
//
// Usage:
//
//     #include <swarm.hpp>
//
//     class MyBrain : public swarm::Brain {
//     public:
//         using swarm::Brain::Brain;
//         swarm::Command Tick(const swarm::Observation& obs) override {
//             for (const SwTrack& t : obs.tracks()) { ... }
//             return swarm::Command::Velocity({0, 0, -2});
//         }
//     };
//     SWARM_REGISTER_BRAIN(MyBrain)
//
// The borrowed-pointer rule still applies: an Observation and the views it hands
// out are valid for the duration of Tick and not one instruction longer. Copy
// what you need to keep.

#ifndef SWARM_HPP
#define SWARM_HPP

#include "swarm_abi.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace swarm {

// ---------------------------------------------------------------------------
// Vector maths. World is NED, body is FRD; see swarm_abi.h.
// ---------------------------------------------------------------------------

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3(const SwVec3& v) : x(v.x), y(v.y), z(v.z) {}

    operator SwVec3() const { return SwVec3{x, y, z}; }

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float LengthSq(const Vec3& v) { return Dot(v, v); }
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3& v, const Vec3& fallback = Vec3(1, 0, 0)) {
    const float n = Length(v);
    return (n < 1e-9f) ? fallback : v / n;
}
inline Vec3 ClampNorm(const Vec3& v, float limit) {
    const float n = Length(v);
    return (n <= limit || n < 1e-9f) ? v : v * (limit / n);
}
inline float Distance(const Vec3& a, const Vec3& b) { return Length(a - b); }

// Horizontal range only. Altitude rarely matters for a threat assessment and
// including it is a common source of confusion in NED, where up is negative.
inline float GroundRange(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

struct Quat {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;

    Quat() = default;
    Quat(float w_, float x_, float y_, float z_) : w(w_), x(x_), y(y_), z(z_) {}
    Quat(const SwQuat& q) : w(q.w), x(q.x), y(q.y), z(q.z) {}

    operator SwQuat() const { return SwQuat{w, x, y, z}; }

    Quat operator*(const Quat& o) const {
        return {w * o.w - x * o.x - y * o.y - z * o.z,
                w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w};
    }
    Quat Conjugate() const { return {w, -x, -y, -z}; }

    // Rotates a body-frame vector into the world frame.
    Vec3 Rotate(const Vec3& v) const {
        const Vec3 u(x, y, z);
        const Vec3 t = Cross(u, v) * 2.0f;
        return v + t * w + Cross(u, t);
    }
    Vec3 RotateInverse(const Vec3& v) const { return Conjugate().Rotate(v); }

    // Heading about the world down axis, from North toward East.
    float Yaw() const {
        const Vec3 nose = Rotate(Vec3(1, 0, 0));
        return std::atan2(nose.y, nose.x);
    }

    static Quat FromYaw(float yaw) {
        return {std::cos(0.5f * yaw), 0.0f, 0.0f, std::sin(0.5f * yaw)};
    }
};

// ---------------------------------------------------------------------------
// Views over the borrowed arrays
// ---------------------------------------------------------------------------

template <class T>
class Span {
public:
    Span() = default;
    Span(const T* data, uint32_t size) : data_(data), size_(size) {}

    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }
    uint32_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    const T& operator[](uint32_t i) const { return data_[i]; }

private:
    const T* data_ = nullptr;
    uint32_t size_ = 0;
};

class Observation {
public:
    explicit Observation(const SwObservation& raw) : raw_(&raw) {}

    float time() const { return raw_->time; }
    float dt() const { return raw_->dt; }
    const SwSelfState& self() const { return raw_->self; }
    Vec3 position() const { return raw_->self.position; }
    Vec3 velocity() const { return raw_->self.velocity; }
    Quat attitude() const { return raw_->self.attitude; }
    Vec3 gyro() const { return raw_->self.gyro; }
    Vec3 accel() const { return raw_->self.accel; }

    Span<SwTrack> tracks() const { return {raw_->tracks, raw_->track_count}; }
    Span<SwRxFrame> rx() const { return {raw_->rx, raw_->rx_count}; }
    uint32_t tx_budget() const { return raw_->tx_budget_bytes; }
    uint32_t flags() const { return raw_->flags; }

    const SwObservation& raw() const { return *raw_; }

private:
    const SwObservation* raw_;
};

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

struct Command {
    SwCommand raw{};

    Command() {
        raw.struct_size = sizeof(SwCommand);
        raw.mode = SW_CMD_IDLE;
    }

    static Command Idle() { return {}; }

    static Command Velocity(const Vec3& velocity_ned, float yaw = 0.0f) {
        Command c;
        c.raw.mode = SW_CMD_VEL_NED;
        c.raw.vec = velocity_ned;
        c.raw.yaw = yaw;
        return c;
    }

    static Command Acceleration(const Vec3& accel_ned, float yaw = 0.0f) {
        Command c;
        c.raw.mode = SW_CMD_ACCEL_NED;
        c.raw.vec = accel_ned;
        c.raw.yaw = yaw;
        return c;
    }

    static Command Attitude(const Quat& attitude, float thrust_newtons) {
        Command c;
        c.raw.mode = SW_CMD_ATTITUDE;
        c.raw.attitude = attitude;
        c.raw.thrust = thrust_newtons;
        return c;
    }

    static Command BodyRate(const Vec3& rate, float thrust_newtons) {
        Command c;
        c.raw.mode = SW_CMD_BODY_RATE;
        c.raw.vec = rate;
        c.raw.thrust = thrust_newtons;
        return c;
    }

    static Command MotorPwm(float m0, float m1, float m2, float m3) {
        Command c;
        c.raw.mode = SW_CMD_MOTOR_PWM;
        c.raw.motor_pwm[0] = m0;
        c.raw.motor_pwm[1] = m1;
        c.raw.motor_pwm[2] = m2;
        c.raw.motor_pwm[3] = m3;
        return c;
    }

    operator SwCommand() const { return raw; }
};

// ---------------------------------------------------------------------------
// Host services
// ---------------------------------------------------------------------------

class Host {
public:
    Host() = default;
    Host(const SwHost* host, const SwBootInfo* boot) : host_(host), boot_(boot) {}

    // Returns bytes accepted, or a negative value if the frame was refused for
    // being over the MTU, over budget, or a second send in the same tick.
    int Broadcast(const void* data, uint32_t len) const {
        if (!host_ || !host_->broadcast) return -1;
        return host_->broadcast(host_->ctx, static_cast<const uint8_t*>(data), len);
    }

    template <class T>
    int Broadcast(const T& message) const {
        static_assert(sizeof(T) <= SW_MTU, "message does not fit in one frame");
        return Broadcast(&message, sizeof(T));
    }

    void DeclareTrack(uint32_t track_id, SwClass believed) const {
        if (host_ && host_->declare_track) {
            host_->declare_track(host_->ctx, track_id, believed);
        }
    }

    void DeclareIdentity(const uint8_t pubkey[SW_KEY_BYTES], SwClass believed) const {
        if (host_ && host_->declare_identity) {
            host_->declare_identity(host_->ctx, pubkey, believed);
        }
    }

    void Random(void* out, uint32_t len) const {
        if (host_ && host_->random) {
            host_->random(host_->ctx, static_cast<uint8_t*>(out), len);
        }
    }

    float RandomUnit() const {
        uint32_t bits = 0;
        Random(&bits, sizeof(bits));
        return static_cast<float>(bits >> 8) * (1.0f / 16777216.0f);
    }

    void Log(const char* text) const {
        if (host_ && host_->log) host_->log(host_->ctx, text);
    }

    void Logf(const char* format, ...) const {
        if (!host_ || !host_->log) return;
        char buffer[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        host_->log(host_->ctx, buffer);
    }

    bool Hash(const void* data, uint32_t len, uint8_t out[SW_HASH_BYTES]) const {
        if (!host_ || !host_->hash) return false;
        return host_->hash(host_->ctx, static_cast<const uint8_t*>(data), len, out) ==
               SW_HASH_BYTES;
    }

    // Crypto. Each returns false when the service is absent, so a brain written
    // for a signed protocol degrades to an unsigned one on a scenario that has no
    // adversary rather than crashing on a NULL pointer.
    bool Sign(const void* msg, uint32_t len, uint8_t sig[SW_SIG_BYTES]) const {
        if (!host_ || !host_->sign) return false;
        return host_->sign(host_->ctx, static_cast<const uint8_t*>(msg), len, sig) ==
               SW_SIG_BYTES;
    }

    bool Verify(const uint8_t pubkey[SW_KEY_BYTES], const void* msg, uint32_t len,
                const uint8_t sig[SW_SIG_BYTES]) const {
        if (!host_ || !host_->verify) return false;
        return host_->verify(host_->ctx, pubkey, static_cast<const uint8_t*>(msg), len,
                             sig) == 0;
    }

    bool Agree(const uint8_t peer_pubkey[SW_KEY_BYTES],
               uint8_t shared[SW_KEY_BYTES]) const {
        if (!host_ || !host_->agree) return false;
        return host_->agree(host_->ctx, peer_pubkey, shared) == SW_KEY_BYTES;
    }

    bool Kdf(const void* ikm, uint32_t ikm_len, const char* info,
             uint8_t out[SW_KEY_BYTES]) const {
        if (!host_ || !host_->kdf) return false;
        const uint32_t info_len =
            info ? static_cast<uint32_t>(std::strlen(info)) : 0u;
        return host_->kdf(host_->ctx, static_cast<const uint8_t*>(ikm), ikm_len,
                          reinterpret_cast<const uint8_t*>(info), info_len, out) ==
               SW_KEY_BYTES;
    }

    uint32_t KeyImport(const uint8_t key[SW_KEY_BYTES]) const {
        if (!host_ || !host_->key_import) return 0;
        return host_->key_import(host_->ctx, key);
    }

    // Bytes written, or a negative value. Give Seal a buffer of at least
    // pt_len + SW_SEAL_OVERHEAD.
    int Seal(uint32_t key_handle, const void* pt, uint32_t pt_len, void* out,
             uint32_t out_cap) const {
        if (!host_ || !host_->seal) return -1;
        return host_->seal(host_->ctx, key_handle, static_cast<const uint8_t*>(pt),
                           pt_len, static_cast<uint8_t*>(out), out_cap);
    }

    int Unseal(uint32_t key_handle, const void* ct, uint32_t ct_len, void* out,
               uint32_t out_cap) const {
        if (!host_ || !host_->unseal) return -1;
        return host_->unseal(host_->ctx, key_handle, static_cast<const uint8_t*>(ct),
                             ct_len, static_cast<uint8_t*>(out), out_cap);
    }

    bool Has(uint32_t capability) const {
        return boot_ && (boot_->capability_flags & capability) != 0;
    }

    const SwHost* raw() const { return host_; }
    const SwBootInfo* boot() const { return boot_; }

private:
    const SwHost* host_ = nullptr;
    const SwBootInfo* boot_ = nullptr;
};

// ---------------------------------------------------------------------------
// Brain base class
// ---------------------------------------------------------------------------

class Brain {
public:
    Brain(const SwHost* host, const SwBootInfo* boot)
        : host_(host, boot), boot_(*boot) {}
    virtual ~Brain() = default;

    virtual Command Tick(const Observation& obs) = 0;

    const Host& host() const { return host_; }
    const SwBootInfo& boot() const { return boot_; }
    uint32_t id() const { return boot_.drone_id; }
    uint32_t fleet_size() const { return boot_.fleet_size; }
    Vec3 asset() const { return boot_.asset_position; }

    // The published key of a fleet member, or nullptr if there is no roster or the
    // id is not in it. Borrowed for as long as this brain lives.
    const uint8_t* PubkeyOf(uint32_t drone_id) const {
        if (!boot_.fleet_pubkeys || drone_id >= boot_.fleet_size) return nullptr;
        return boot_.fleet_pubkeys + static_cast<size_t>(drone_id) * SW_KEY_BYTES;
    }

private:
    Host host_;
    SwBootInfo boot_;  // copied, because the pointer is only valid during create
};

}  // namespace swarm

// Generates the C entry points and the factory. Put it at file scope, once.
#define SWARM_REGISTER_BRAIN(BrainType)                                            \
    extern "C" {                                                                   \
    static void* swarm_hpp_create(const SwHost* host, const SwBootInfo* boot) {     \
        return new (std::nothrow) BrainType(host, boot);                            \
    }                                                                              \
    static void swarm_hpp_tick(void* self, const SwObservation* obs,                \
                               SwCommand* out) {                                    \
        swarm::Observation view(*obs);                                             \
        *out = static_cast<BrainType*>(self)->Tick(view);                           \
    }                                                                              \
    static void swarm_hpp_destroy(void* self) {                                     \
        delete static_cast<BrainType*>(self);                                       \
    }                                                                              \
    SWARM_EXPORT const SwBrain* swarm_brain_v1(void) {                              \
        static const SwBrain kBrain = {sizeof(SwBrain), SWARM_ABI_VERSION,           \
                                       swarm_hpp_create, swarm_hpp_destroy,          \
                                       swarm_hpp_tick};                              \
        return &kBrain;                                                             \
    }                                                                              \
    }

#endif  // SWARM_HPP
